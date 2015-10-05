/*
    m5threads, a pthread library for the M5 simulator
    Copyright (C) 2009, Stanford University

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA

    Author: Daniel Sanchez
*/

#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <sys/errno.h>
#include <sched.h>
#include <linux/sched.h>
#include <sys/mman.h>
#include <string.h>
#include <malloc.h>
#include <sys/syscall.h>

//Spinlock assembly
#if defined(__x86) || defined(__x86_64)
  #include "spinlock_x86.h"
#elif defined(__alpha)
  #include "spinlock_alpha.h"
#elif defined(__sparc)
  #include "spinlock_sparc.h"
#elif defined (__arm__)
  #include "spinlock_arm.h"
#else
  #error "spinlock routines not available for your arch!\n"
#endif

#include "pthread_defs.h"
#include "tls_defs.h"
#include "profiling_hooks.h"

#define restrict 

//64KB stack, change to your taste...
#define CHILD_STACK_BITS 16
#define CHILD_STACK_SIZE (1 << CHILD_STACK_BITS)

//Debug macro
#ifdef __DEBUG
  #define DEBUG(args...) printf(args)
#else
  #define DEBUG(args...) 
#endif

//Size and alignment requirements of "real" (NPTL/LinuxThreads) thread control block
#define NPTL_TCB_SIZE 1184 // sizeof (struct pthread)
#define NPTL_TCB_ALIGN sizeof(double)
#define NPTL_TCBHEAD_T_SIZE (sizeof(tcbhead_t))

//Thread control structure
typedef struct {
  pthread_t tid;
  unsigned int is_detached; //0 if joinable, 1 if detached
  volatile int child_finished;
  void* result; //written by child on exit
  void *(*start_routine)(void*);
  void* arg;
  //thread block limits
  void* tls_start_addr;
  void* stack_start_addr;
} pthread_tcb_t;


//Information about the thread block (TLS, sizes)
static struct {
  size_t tls_memsz;
  size_t tls_filesz;
  void*  tls_initimage;
  size_t tls_align;
  size_t total_size;
  size_t stack_guard_size;
} thread_block_info;


/* Thread-local data */

//Pointer to our TCB (NULL for main thread)
__thread pthread_tcb_t* __tcb;

// Used for TSD (getspecific, setspecific, etc.)
__thread void** pthread_specifics = NULL; //dynamically allocated, since this is rarely used
__thread uint32_t pthread_specifics_size = 0;


/* Initialization, create/exit/join functions */

// Search ELF segments, pull out TLS block info, campute thread block sizes
static void populate_thread_block_info() {
  ElfW(Phdr) *phdr;

  //If there is no TLS segment...
  thread_block_info.tls_memsz = 0;
  thread_block_info.tls_filesz = 0;
  thread_block_info.tls_initimage = NULL;
  thread_block_info.tls_align = 0;

  /* Look through the TLS segment if there is any.  */
  if (_dl_phdr != NULL) {
    for (phdr = _dl_phdr; phdr < &_dl_phdr[_dl_phnum]; ++phdr) {
      if (phdr->p_type == PT_TLS) {
          /* Gather the values we need.  */
          thread_block_info.tls_memsz = phdr->p_memsz;
          thread_block_info.tls_filesz = phdr->p_filesz;
          thread_block_info.tls_initimage = (void *) phdr->p_vaddr;
          thread_block_info.tls_align = phdr->p_align;
          break;
      }
    }
  }

  //Set a stack guard size
  //In SPARC, this is actually needed to avoid out-of-range accesses on register saves...
  //Largest I have seen is 2048 (sparc64)
  //You could avoid this in theory by compiling with -mnostack-bias
  thread_block_info.stack_guard_size = 2048;

  //Total thread block size -- this is what we'll request to mmap
  #if TLS_TCB_AT_TP
  size_t sz = sizeof(pthread_tcb_t) + thread_block_info.tls_memsz + NPTL_TCBHEAD_T_SIZE + thread_block_info.stack_guard_size + CHILD_STACK_SIZE;
  #elif TLS_DTV_AT_TP
  size_t sz = sizeof(pthread_tcb_t) + thread_block_info.tls_memsz + NPTL_TCB_SIZE + NPTL_TCBHEAD_T_SIZE + thread_block_info.stack_guard_size + CHILD_STACK_SIZE;
  #else
  #error "TLS_TCB_AT_TP xor TLS_DTV_AT_TP must be defined"
  #endif
  //Note that TCB_SIZE is the "real" TCB size, not ours, which we leave zeroed (but some variables, notably errno, are somewhere inside there)

  //Align to multiple of CHILD_STACK_SIZE
  sz += CHILD_STACK_SIZE - 1;  
  thread_block_info.total_size = (sz>>CHILD_STACK_BITS)<<CHILD_STACK_BITS;
}

//Set up TLS block in current thread
// @param th_block_addr:  beginning of entire thread memory space
static void setup_thread_tls(void* th_block_addr) {
  size_t tcb_offset = 0;
  void *tlsblock = NULL;
  char *tls_start_ptr = NULL;

  #if TLS_DTV_AT_TP
  th_block_addr += NPTL_TCB_SIZE;
  #endif

  /* Compute the (real) TCB offset */
  #if TLS_DTV_AT_TP
  tcb_offset = roundup(NPTL_TCBHEAD_T_SIZE, NPTL_TCB_ALIGN);
  #elif TLS_TCB_AT_TP
  tcb_offset = roundup(thread_block_info.tls_memsz, NPTL_TCB_ALIGN);
  #else
  #error "TLS_TCB_AT_TP xor TLS_DTV_AT_TP must be defined"
  #endif

  /* Align the TLS block.  */
  tlsblock = (void *) (((uintptr_t) th_block_addr + thread_block_info.tls_align - 1)
                       & ~(thread_block_info.tls_align - 1));
  /* Initialize the TLS block.  */
  #if TLS_DTV_AT_TP
  tls_start_ptr = ((char *) tlsblock + tcb_offset);
  #elif TLS_TCB_AT_TP
  tls_start_ptr = ((char *) tlsblock + tcb_offset
                       - roundup (thread_block_info.tls_memsz, thread_block_info.tls_align ?: 1));
  #else
  #error "TLS_TCB_AT_TP xor TLS_DTV_AT_TP must be defined"
  #endif

  //DEBUG("Init TLS: Copying %d bytes from 0x%llx to 0x%llx\n", filesz, (uint64_t) initimage, (uint64_t) tls_start_ptr);
  memcpy (tls_start_ptr, thread_block_info.tls_initimage, thread_block_info.tls_filesz);

  //Rest of tls vars are already cleared (mmap returns zeroed memory)

  //Note: We don't care about DTV pointers for x86/SPARC -- they're never used in static mode
  /* Initialize the thread pointer.  */
  #if TLS_DTV_AT_TP
  TLS_INIT_TP (tlsblock, 0);
  #elif TLS_TCB_AT_TP
  TLS_INIT_TP ((char *) tlsblock + tcb_offset, 0);
  #else
  #error "TLS_TCB_AT_TP xor TLS_DTV_AT_TP must be defined"
  #endif
}

//Some NPTL definitions
int __libc_multiple_threads; //set to one on initialization
int __nptl_nthreads = 32; //TODO: we don't really know...

//Called at initialization. Sets up TLS for the main thread and populates thread_block_info, used in subsequent calls
//Works with LinuxThreads and NPTL
void __pthread_initialize_minimal() {
  __libc_multiple_threads = 1; //tell libc we're multithreaded (NPTL-specific)
  populate_thread_block_info();
  void* ptr = mmap(0, thread_block_info.total_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  setup_thread_tls(ptr + sizeof(pthread_tcb_t));
}


//Used by pthread_create to spawn child
static int __pthread_trampoline(void* thr_ctrl) {
  //Set TLS up
  pthread_tcb_t* tcb = (pthread_tcb_t*) thr_ctrl; 
  setup_thread_tls(tcb->tls_start_addr);
  __tcb = tcb;
  DEBUG("Child in trampoline, TID=%llx\n", tcb->tid);

  void* result = tcb->start_routine(tcb->arg);
  pthread_exit(result);
  assert(0); //should never be reached
}

int pthread_create (pthread_t* thread,
                    const pthread_attr_t* attr,
                    void *(*start_routine)(void*), 
                    void* arg) {
  DEBUG("pthread_create: start\n");

  //Allocate the child thread block (TCB+TLS+stack area)
  //We use mmap so that the child can munmap it at exit without using a stack (it's a system call)
  void* thread_block;
  size_t thread_block_size = thread_block_info.total_size;
  thread_block = mmap(0, thread_block_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  DEBUG("pthread_create: mmapped child thread block 0x%llx -- 0x%llx\n", thread_block, ((char*)thread_block) + CHILD_STACK_SIZE) ;
 
  //Populate the thread control block
  pthread_tcb_t* tcb = (pthread_tcb_t*) thread_block;
  tcb->tid = (pthread_t) thread_block; //thread ID is tcb address itself
  tcb->is_detached = 0; //joinable
  tcb->child_finished = 0;
  tcb->start_routine = start_routine;
  tcb->arg = arg;
  tcb->tls_start_addr = (void*)(((char*)thread_block) + sizeof(pthread_tcb_t)); //right after m5's tcb
  tcb->stack_start_addr = (void*) (((char*) thread_block) + thread_block_size - thread_block_info.stack_guard_size); //end of thread_block
  
  *thread=(pthread_t) thread_block;

  //Call clone()
  DEBUG("pthread_create: prior to clone()\n");
  clone(__pthread_trampoline, tcb->stack_start_addr, CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND|CLONE_THREAD, tcb);
  DEBUG("pthread_create: after clone()\n");
  return 0;
}

pthread_t pthread_self() {
    if (__tcb == NULL) return 0; //main thread
    return __tcb->tid;
}

int pthread_join (pthread_t thread, void** status) {
    DEBUG("pthread_join: started\n");
    pthread_tcb_t* child_tcb = (pthread_tcb_t*) thread;
    assert(child_tcb->tid == thread); // checks that this is really a tcb
    assert(!child_tcb->is_detached); // thread should be joinable
    volatile int child_done = 0;
    while (child_done == 0) { // spin until child done
        child_done = child_tcb->child_finished;
    }
    DEBUG("pthread_join: child joined\n");
    //Get result
    if (status) *status = child_tcb->result;

    //Deallocate child block
    //munmap(child_tcb, thread_block_info.total_size);   

    return 0;

}


void pthread_exit (void* status) {
    // TODO: The good way to solve this is to have the child, not its parent, free
    // its own stack (and TLS segment). This enables detached threads. But to do this
    // you need an extra stack. A way to do this is to have a global, lock-protected 
    // manager stack, or have the M5 exit system call do it... Anyhow, I'm deferring
    // this problem until we have TLS.

    //From point (XXX)  on, the thread **does not exist**,
    //as its parent may have already freed the stack. 
    //So we must call sys_exit without using the stack => asm

    // NOTE: You may be tempted to call exit(0) or _exit(0) here, but there call exit_group,
    // killing the whole process and not just the current thread

    //If the keys array was allocated, free it
    if (pthread_specifics != NULL) free(pthread_specifics);

    //Main thread
    if (__tcb == NULL) _exit(0);

    DEBUG("Child TID=0x%llx in pthread_exit...\n", pthread_self() );
    __tcb->result = status;
    //TODO mem barrier here...
    __tcb->child_finished = 1;
    //XXX
    syscall(__NR_exit,0);
    assert(0); //should never be reached

/*#if defined(__x86) or defined(__x86_64)
    __asm__ __volatile__  (
         "\nmov  $0x3c,%%eax\n\t" \
         "syscall\n\t" 
         ::: "eax");
#elif defined(__alpha)
    __asm__ __volatile__  (
         "\nldi  $0,1\n\t" \
         "callsys\n\t");
#elif defined(__sparc)
    // Since this part of the code is provisional, don't bother with asm for now
    syscall(__NR_exit,0);
#else
    #error "No pthread_exit asm for your arch, sorry!\n"
#endif

    assert(0);*/
}


// mutex functions

int pthread_mutex_init (pthread_mutex_t* mutex, const pthread_mutexattr_t* attr) {
  DEBUG("%s: start\n", __FUNCTION__);
    mutex->PTHREAD_MUTEX_T_COUNT = 0;
    return 0;
}

int pthread_mutex_lock (pthread_mutex_t* lock) {
  DEBUG("%s: start\n", __FUNCTION__);
    PROFILE_LOCK_START(lock); 
    spin_lock((int*)&lock->PTHREAD_MUTEX_T_COUNT);
    PROFILE_LOCK_END(lock);
    return 0;
}

int pthread_mutex_unlock (pthread_mutex_t* lock) {
  DEBUG("%s: start\n", __FUNCTION__);
    PROFILE_UNLOCK_START(lock);
    spin_unlock((int*)&lock->PTHREAD_MUTEX_T_COUNT);
    PROFILE_UNLOCK_END(lock);
    return 0;
}

int pthread_mutex_destroy (pthread_mutex_t* mutex) {
  DEBUG("%s: start\n", __FUNCTION__);
    return 0;
}

int pthread_mutex_trylock (pthread_mutex_t* mutex) {
  DEBUG("%s: start\n", __FUNCTION__);
    int acquired = trylock((int*)&mutex->PTHREAD_MUTEX_T_COUNT);
    if (acquired == 1) {
	//Profiling not really accurate here...
	PROFILE_LOCK_START(mutex);
	PROFILE_LOCK_END(mutex);
        return 0;
    }
    return EBUSY;
}

// rwlock functions

int pthread_rwlock_init (pthread_rwlock_t* lock, const pthread_rwlockattr_t* attr) {
  DEBUG("%s: start\n", __FUNCTION__);
    PTHREAD_RWLOCK_T_LOCK(lock) = 0; // used only with spin_lock, so we know to initilize to zero
    PTHREAD_RWLOCK_T_READERS(lock) = 0;
    PTHREAD_RWLOCK_T_WRITER(lock) = -1; // -1 means no one owns the write lock

    return 0;
}

int pthread_rwlock_destroy (pthread_rwlock_t* lock) {
  DEBUG("%s: start\n", __FUNCTION__);
    return 0;
}

int pthread_rwlock_rdlock (pthread_rwlock_t* lock) {
  DEBUG("%s: start\n", __FUNCTION__);
    PROFILE_LOCK_START(lock);
    do {
        // this is to reduce the contention and a possible live-lock to lock->access_lock
        while (1) {
            pthread_t writer = PTHREAD_RWLOCK_T_WRITER(lock);
            if (writer == -1) {
                break;
            }
        }

        spin_lock((int*)&(PTHREAD_RWLOCK_T_LOCK(lock)));
        if ((pthread_t)PTHREAD_RWLOCK_T_WRITER(lock) == -1) {
            PTHREAD_RWLOCK_T_READERS(lock)++;
            spin_unlock((int*)&(PTHREAD_RWLOCK_T_LOCK(lock)));
	    PROFILE_LOCK_END(lock);
            return 0;
        }
        spin_unlock((int*)&(PTHREAD_RWLOCK_T_LOCK(lock)));
    } while (1);
    PROFILE_LOCK_END(lock);
    return 0;
}

int pthread_rwlock_wrlock (pthread_rwlock_t* lock) {
  DEBUG("%s: start\n", __FUNCTION__);
    PROFILE_LOCK_START(lock);
    do {
        while (1) {
            pthread_t writer = PTHREAD_RWLOCK_T_WRITER(lock);
            if (writer == -1) {
                break;
            }
            int num_readers = PTHREAD_RWLOCK_T_READERS(lock);
            if (num_readers == 0) {
                break;
            }
        }

        spin_lock((int*)&(PTHREAD_RWLOCK_T_LOCK(lock)));
        if ((pthread_t)PTHREAD_RWLOCK_T_WRITER(lock) == -1 && PTHREAD_RWLOCK_T_READERS(lock) == 0) {
            PTHREAD_RWLOCK_T_WRITER(lock) = pthread_self();
            spin_unlock((int*)&(PTHREAD_RWLOCK_T_LOCK(lock)));
	    PROFILE_LOCK_END(lock);
            return 0;
        }
        spin_unlock((int*)&(PTHREAD_RWLOCK_T_LOCK(lock)));
    } while (1);
    PROFILE_LOCK_END(lock);
    return 0;
}

int pthread_rwlock_unlock (pthread_rwlock_t* lock) {
  DEBUG("%s: start\n", __FUNCTION__);
    PROFILE_UNLOCK_START(lock);
    spin_lock((int*)&(PTHREAD_RWLOCK_T_LOCK(lock)));
    if (pthread_self() == PTHREAD_RWLOCK_T_WRITER(lock)) {
        // the write lock will be released
        PTHREAD_RWLOCK_T_WRITER(lock) = -1;
    } else {
        // one of the read locks will be released
        PTHREAD_RWLOCK_T_READERS(lock) = PTHREAD_RWLOCK_T_READERS(lock) - 1;
    }
    spin_unlock((int*)&(PTHREAD_RWLOCK_T_LOCK(lock)));
    PROFILE_UNLOCK_END(lock);
    return 0;
}


// key functions
#ifndef PTHREAD_KEYS_MAX
#define PTHREAD_KEYS_MAX 1024
#endif

typedef struct {
  int in_use;
  void (*destr)(void*);
} pthread_key_struct;

static pthread_key_struct pthread_keys[PTHREAD_KEYS_MAX];
static pthread_mutex_t pthread_keys_mutex = PTHREAD_MUTEX_INITIALIZER;

int pthread_key_create (pthread_key_t* key, void (*destructor)(void*)) {
  int i;
  DEBUG("%s: start\n", __FUNCTION__);

  pthread_mutex_lock(&pthread_keys_mutex);
  for (i = 0; i < PTHREAD_KEYS_MAX; i++) {
    if (! pthread_keys[i].in_use) {
      /* Mark key in use */
      pthread_keys[i].in_use = 1;
      pthread_keys[i].destr = destructor;
      pthread_mutex_unlock(&pthread_keys_mutex);
      *key = i;
      return 0;
    }
  }
  pthread_mutex_unlock(&pthread_keys_mutex);
  return EAGAIN;
}

int pthread_key_delete (pthread_key_t key)
{
  DEBUG("%s: start\n", __FUNCTION__);
  pthread_mutex_lock(&pthread_keys_mutex);
  if (key >= PTHREAD_KEYS_MAX || !pthread_keys[key].in_use) {
    pthread_mutex_unlock(&pthread_keys_mutex);
    return EINVAL;
  }
  pthread_keys[key].in_use = 0;
  pthread_keys[key].destr = NULL;

  /* NOTE: The LinuxThreads implementation actually zeroes deleted keys on
     spawned threads. I don't care, the spec says that if you are  access a
     key after if has been deleted, you're on your own. */

  pthread_mutex_unlock(&pthread_keys_mutex);
  return 0;
}

int pthread_setspecific (pthread_key_t key, const void* value) {
  int m_size;
  DEBUG("%s: start\n", __FUNCTION__);
  if (key < 0 || key >= PTHREAD_KEYS_MAX) return EINVAL; 
  if (pthread_specifics_size == 0) {
     pthread_specifics = (void**) calloc(PTHREAD_KEYS_MAX + 1, sizeof(void*));
     DEBUG("pthread_setspecific: malloc of size %d bytes, got 0x%llx\n", m_size, pthread_specifics);
     pthread_specifics_size = key+1;
  }
  pthread_specifics[key] = (void*) value;
  return 0;
}

void* pthread_getspecific (pthread_key_t key) {
  if (key < 0 || key >= pthread_specifics_size) return NULL;
  DEBUG("pthread_getspecific: key=%d pthread_specifics_size=%d\n", key, pthread_specifics_size);
  return pthread_specifics[key]; 
}

// condition variable functions

int pthread_cond_init (pthread_cond_t* cond, const pthread_condattr_t* attr) {
  DEBUG("%s: start\n", __FUNCTION__);
    PTHREAD_COND_T_FLAG(cond) = 0;
    PTHREAD_COND_T_THREAD_COUNT(cond) = 0;
    PTHREAD_COND_T_COUNT_LOCK(cond) = 0;
    return 0;    
}

int pthread_cond_destroy (pthread_cond_t* cond) {
  DEBUG("%s: start\n", __FUNCTION__);
    return 0;
}

int pthread_cond_broadcast (pthread_cond_t* cond) {
  DEBUG("%s: start\n", __FUNCTION__);
    PTHREAD_COND_T_FLAG(cond) = 1;
    return 0;
}

int pthread_cond_wait (pthread_cond_t* cond, pthread_mutex_t* lock) {
  DEBUG("%s: start\n", __FUNCTION__);
    PROFILE_COND_WAIT_START(cond);
    volatile int* thread_count  = &(PTHREAD_COND_T_THREAD_COUNT(cond));
    volatile int* flag = &(PTHREAD_COND_T_FLAG(cond));
    volatile int* count_lock    = &(PTHREAD_COND_T_COUNT_LOCK(cond));

    // dsm: ++/-- have higher precedence than *, so *thread_count++
    // increments *the pointer*, then dereferences it (!)
    (*thread_count)++;

    pthread_mutex_unlock(lock);
    while (1) {
        volatile int f = *flag;
        if (f == 1) {
            break;
        }
    }

    spin_lock(count_lock);

    (*thread_count)--;

    if (*thread_count == 0) {
        *flag = 0;
    }
    spin_unlock(count_lock);
    pthread_mutex_lock(lock);
    PROFILE_COND_WAIT_END(cond);
    return 0;
}

int pthread_cond_signal (pthread_cond_t* cond) {
  DEBUG("%s: start\n", __FUNCTION__);
    //Could also signal only one thread, but this is compliant too
    //TODO: Just wake one thread up
    return pthread_cond_broadcast(cond);
}


//barrier functions

//These funny tree barriers will only work with consecutive TIDs starting from 0, e.g. a barrier initialized for 8 thread will need to be taken by TIDs 0-7
//TODO: Adapt to work with arbitrary TIDs
/*int pthread_barrier_init (pthread_barrier_t *restrict barrier,
                          const pthread_barrierattr_t *restrict attr, unsigned count)
{
    assert(barrier != NULL);
    //assert(0 < count && count <= MAX_NUM_CPUS);

    PTHREAD_BARRIER_T_NUM_THREADS(barrier) = count;

    // add one to avoid false sharing
    tree_barrier_t* ptr
        = ((tree_barrier_t*)malloc((count + 1) * sizeof(tree_barrier_t))) + 1;
    for (unsigned i = 0; i < count; ++i) {
      ptr[i].value = 0;
    }

    PTHREAD_BARRIER_T_BARRIER_PTR(barrier) = ptr;

    return 0;
}

int pthread_barrier_destroy (pthread_barrier_t *barrier)
{
    free(PTHREAD_BARRIER_T_BARRIER_PTR(barrier) - 1);
    return 0;
}

int pthread_barrier_wait (pthread_barrier_t* barrier)
{
    int const num_threads = PTHREAD_BARRIER_T_NUM_THREADS(barrier);
    int const self = pthread_self(); 
    tree_barrier_t * const barrier_ptr = PTHREAD_BARRIER_T_BARRIER_PTR(barrier);

    int const goal = 1 - barrier_ptr[self].value;

    int round_mask = 3;
    while ((self & round_mask) == 0 && round_mask < (num_threads << 2)) {
      int const spacing = (round_mask + 1) >> 2;
      for (int i = 1; i <= 3 && self + i*spacing < num_threads; ++i) {
        while (barrier_ptr[self + i*spacing].value != goal) {
          // spin
        }
      }
      round_mask = (round_mask << 2) + 3;
    }

    barrier_ptr[self].value = goal;
    while (barrier_ptr[0].value != goal) {
      // spin
    }

    return 0;
}*/

int pthread_barrier_init (pthread_barrier_t *restrict barrier,
                          const pthread_barrierattr_t *restrict attr, unsigned count)
{
    assert(barrier != NULL);
  DEBUG("%s: start\n", __FUNCTION__);

    PTHREAD_BARRIER_T_NUM_THREADS(barrier) =  count;
    PTHREAD_BARRIER_T_SPINLOCK(barrier) = 0;
    PTHREAD_BARRIER_T_COUNTER(barrier) = 0;
    PTHREAD_BARRIER_T_DIRECTION(barrier) = 0; //up

    return 0;
}

int pthread_barrier_destroy (pthread_barrier_t *barrier)
{
  DEBUG("%s: start\n", __FUNCTION__);
    //Nothing to do
    return 0;
}

int pthread_barrier_wait (pthread_barrier_t* barrier)
{
  DEBUG("%s: start\n", __FUNCTION__);
    PROFILE_BARRIER_WAIT_START(barrier);
    int const initial_direction = PTHREAD_BARRIER_T_DIRECTION(barrier); //0 == up, 1 == down

    if (initial_direction == 0) {
       spin_lock(&(PTHREAD_BARRIER_T_SPINLOCK(barrier)));
       PTHREAD_BARRIER_T_COUNTER(barrier)++; 
       if (PTHREAD_BARRIER_T_COUNTER(barrier) == PTHREAD_BARRIER_T_NUM_THREADS(barrier)) {
           //reverse direction, now down
           PTHREAD_BARRIER_T_DIRECTION(barrier) = 1;
       }
       spin_unlock(&(PTHREAD_BARRIER_T_SPINLOCK(barrier)));
    } else {
       spin_lock(&(PTHREAD_BARRIER_T_SPINLOCK(barrier)));
       PTHREAD_BARRIER_T_COUNTER(barrier)--;
       if (PTHREAD_BARRIER_T_COUNTER(barrier) == 0) {
          //reverse direction, now up
          PTHREAD_BARRIER_T_DIRECTION(barrier) = 0;
       }
       spin_unlock(&(PTHREAD_BARRIER_T_SPINLOCK(barrier)));
   }

   volatile int direction = PTHREAD_BARRIER_T_DIRECTION(barrier);
   while (initial_direction == direction) {
      //spin
      direction = PTHREAD_BARRIER_T_DIRECTION(barrier);
   }
   PROFILE_BARRIER_WAIT_END(barrier);
   return 0;
}

//misc functions

static pthread_mutex_t __once_mutex = PTHREAD_MUTEX_INITIALIZER;
int pthread_once (pthread_once_t* once,
                  void (*init)(void))
{
  DEBUG("%s: start\n", __FUNCTION__);
  //fast path
  if (*once != PTHREAD_ONCE_INIT) return 0;
  pthread_mutex_lock(&__once_mutex);
  if (*once != PTHREAD_ONCE_INIT) {
    pthread_mutex_unlock(&__once_mutex);
    return 0;
  }
  *once = PTHREAD_ONCE_INIT+1;
  pthread_mutex_unlock(&__once_mutex);
  init();
  return 0;
}

#ifndef __USE_EXTERN_INLINES
int pthread_equal (pthread_t t1, pthread_t t2)
{
    return t1 == t2; //that was hard :-)
}
#endif

// Functions that we want defined, but we don't use them
// All other functions are not defined so that they will cause a compile time
// error and we can decide if we need to do something with them

// functions really don't need to do anything

int pthread_yield() {
  DEBUG("%s: start\n", __FUNCTION__);
    // nothing else to yield to
    return 0;
}

int pthread_attr_init (pthread_attr_t* attr) {
  DEBUG("%s: start\n", __FUNCTION__);
    return 0;
}

int pthread_attr_setscope (pthread_attr_t* attr, int scope) {
  DEBUG("%s: start\n", __FUNCTION__);
    return 0;
}

int pthread_rwlockattr_init (pthread_rwlockattr_t* attr) {
  DEBUG("%s: start\n", __FUNCTION__);
    return 0;
}

int pthread_attr_setstacksize (pthread_attr_t* attr, size_t stacksize) {
  DEBUG("%s: start\n", __FUNCTION__);
    return 0;
}

int pthread_attr_setschedpolicy (pthread_attr_t* attr, int policy) {
  DEBUG("%s: start\n", __FUNCTION__);
    return 0;
}

// some functions that we don't really support

int pthread_setconcurrency (int new_level) {
  DEBUG("%s: start\n", __FUNCTION__);
    return 0;
}

int pthread_setcancelstate (int p0, int* p1)
{
  DEBUG("%s: start\n", __FUNCTION__);
    //NPTL uses this
    return 0;
}

//and some affinity functions (used by libgomp, openmp)
int pthread_getaffinity_np(pthread_t thread, size_t size, cpu_set_t *set) {
  DEBUG("%s: start\n", __FUNCTION__);
    char *p = (char*)set;
    while ( size-- ) *p++ = 0;
  return 0;
}

int pthread_setaffinity_np(pthread_t thread, size_t size, cpu_set_t *set) {
  DEBUG("%s: start\n", __FUNCTION__);
  return 0;
}

int pthread_attr_setaffinity_np(pthread_attr_t attr, size_t cpusetsize, const cpu_set_t *cpuset) {
  DEBUG("%s: start\n", __FUNCTION__);
  return 0;
}

int pthread_attr_getaffinity_np(pthread_attr_t attr, size_t cpusetsize, cpu_set_t *cpuset) {
  DEBUG("%s: start\n", __FUNCTION__);
  return 0;
}


// ... including any dealing with thread-level signal handling
// (maybe we should throw an error message instead?)

int pthread_sigmask (int how, const sigset_t* set, sigset_t* oset) {
  DEBUG("%s: start\n", __FUNCTION__);
    return 0;
}

int pthread_kill (pthread_t thread, int sig)  {
    assert(0);
}

// unimplemented pthread functions

int pthread_atfork (void (*f0)(void),
                    void (*f1)(void),
                    void (*f2)(void))
{
    assert(0);
}

int pthread_attr_destroy (pthread_attr_t* attr)
{
    assert(0);
}

int pthread_attr_getdetachstate (const pthread_attr_t* attr,
                                 int* b)
{
    assert(0);
}

int pthread_attr_getguardsize (const pthread_attr_t* restrict a,
                               size_t *restrict b)
{
    assert(0);
}

int pthread_attr_getinheritsched (const pthread_attr_t *restrict a,
                                  int *restrict b)
{
    assert(0);
}

int pthread_attr_getschedparam (const pthread_attr_t *restrict a,
                                struct sched_param *restrict b)
{
    assert(0);
}

int pthread_attr_getschedpolicy (const pthread_attr_t *restrict a,
                                 int *restrict b)
{
    assert(0);
}

int pthread_attr_getscope (const pthread_attr_t *restrict a,
                           int *restrict b)
{
    assert(0);
}

int pthread_attr_getstack (const pthread_attr_t *restrict a,
                           void* *restrict b,
                           size_t *restrict c)
{
    assert(0);
}

int pthread_attr_getstackaddr (const pthread_attr_t *restrict a,
                               void* *restrict b)
{
    assert(0);
}

int pthread_attr_getstacksize (const pthread_attr_t *restrict a,
                               size_t *restrict b)
{
    assert(0);
}

int pthread_attr_setdetachstate (pthread_attr_t* a,
                                 int b)
{
   return 0; //FIXME
}
int pthread_attr_setguardsize (pthread_attr_t* a,
                               size_t b)
{
    assert(0);
}

int pthread_attr_setinheritsched (pthread_attr_t* a,
                                  int b)
{
    assert(0);
}

int pthread_attr_setschedparam (pthread_attr_t *restrict a,
                                const struct sched_param *restrict b)
{
    assert(0);
}

int pthread_attr_setstack (pthread_attr_t* a,
                           void* b,
                           size_t c)
{
    assert(0);
}

int pthread_attr_setstackaddr (pthread_attr_t* a,
                               void* b)
{
    assert(0);
}

int pthread_cancel (pthread_t a)
{
    assert(0);
}

void _pthread_cleanup_push (struct _pthread_cleanup_buffer *__buffer,
                            void (*__routine) (void *),
                            void *__arg) 
{
    assert(0);
}

void _pthread_cleanup_pop (struct _pthread_cleanup_buffer *__buffer,
                           int __execute) 
{
    assert(0);
}

int pthread_cond_timedwait (pthread_cond_t *restrict a,
                            pthread_mutex_t *restrict b,
                            const struct timespec *restrict c)
{
    assert(0);
}

int pthread_condattr_destroy (pthread_condattr_t* a)
{
    assert(0);
}

int pthread_condattr_getpshared (const pthread_condattr_t *restrict a,
                                 int *restrict b)
{
    assert(0);
}

int pthread_condattr_init (pthread_condattr_t* a)
{
    assert(0);
}

int pthread_condattr_setpshared (pthread_condattr_t* a,
                                 int b)
{
    assert(0);
}

int pthread_detach (pthread_t a)
{
    assert(0);
}


int pthread_getconcurrency ()
{
    assert(0);
}

int pthread_getschedparam(pthread_t a,
                          int *restrict b,
                          struct sched_param *restrict c)
{
    assert(0);
}

int pthread_mutex_getprioceiling (const pthread_mutex_t *restrict a,
                                  int *restrict b)
{
    assert(0);
}

int pthread_mutex_setprioceiling (pthread_mutex_t *restrict a,
                                  int b,
                                  int *restrict c)
{
    assert(0);
}

int pthread_mutex_timedlock (pthread_mutex_t* a,
                             const struct timespec* b)
{
    assert(0);
}

int pthread_mutexattr_destroy (pthread_mutexattr_t* a)
{
    //assert(0);
    //used by libc
    return 0;
}

int pthread_mutexattr_getprioceiling (const pthread_mutexattr_t *restrict a,
                                      int *restrict b)
{
    assert(0);
}

int pthread_mutexattr_getprotocol (const pthread_mutexattr_t *restrict a,
                                   int *restrict b)
{
    assert(0);
}

int pthread_mutexattr_getpshared (const pthread_mutexattr_t *restrict a,
                                  int *restrict b)
{
    assert(0);
}

int pthread_mutexattr_gettype (const pthread_mutexattr_t *restrict a,
                               int *restrict b)
{
    assert(0);
}

int pthread_mutexattr_init (pthread_mutexattr_t* a)
{
    //assert(0);
    //used by libc
    return 0;
}

int pthread_mutexattr_setprioceiling (pthread_mutexattr_t* a,
                                      int b)
{
    assert(0);
}

int pthread_mutexattr_setprotocol (pthread_mutexattr_t* a,
                                   int b)
{
    assert(0);
}

int pthread_mutexattr_setpshared (pthread_mutexattr_t* a,
                                  int b)
{
    assert(0);
}

int pthread_mutexattr_settype (pthread_mutexattr_t* a,
                               int b)
{
    //assert(0);
    //used by libc
    //yeah, and the freaking libc just needs a recursive lock.... screw it
    //if (b == PTHREAD_MUTEX_RECURSIVE_NP) assert(0);
    return 0;
}

int pthread_rwlock_timedrdlock (pthread_rwlock_t *restrict a,
                                const struct timespec *restrict b)
{
    assert(0);
}

int pthread_rwlock_timedwrlock (pthread_rwlock_t *restrict a,
                                const struct timespec *restrict b)
{
    assert(0);
}

int pthread_rwlock_tryrdlock (pthread_rwlock_t* a)
{
    assert(0);
}

int pthread_rwlock_trywrlock (pthread_rwlock_t* a)
{
    assert(0);
}

int pthread_rwlockattr_destroy (pthread_rwlockattr_t* a)
{
    assert(0);
}

int pthread_rwlockattr_getpshared (const pthread_rwlockattr_t *restrict a,
                                   int *restrict b)
{
    assert(0);
}

int pthread_rwlockattr_setpshared(pthread_rwlockattr_t* a,
                                  int b)
{
    assert(0);
}

int pthread_setcanceltype (int a,
                           int* b)
{
    assert(0);
}

int pthread_setschedparam (pthread_t a,
                           int b,
                           const struct sched_param* c)
{
    assert(0);
}

int pthread_setschedprio (pthread_t a,
                          int b)
{
    assert(0);
}

void pthread_testcancel ()
{
    assert(0);
}


/* Stuff to properly glue with glibc */

// glibc keys

//For NPTL, or LinuxThreads with TLS defined and used
__thread void* __libc_tsd_MALLOC;
__thread void* __libc_tsd_DL_ERROR;
__thread void* __libc_tsd_RPC_VARS;
//__thread void* __libc_tsd_LOCALE; seems to be defined in my libc already, but your glibc might not dfine it...
//Defined in libgomp (OpenMP)
//__thread void* __libc_tsd_CTYPE_B;
//__thread void* __libc_tsd_CTYPE_TOLOWER;
//__thread void* __libc_tsd_CTYPE_TOUPPER;

//If glibc was not compiled with __thread, it uses __pthread_internal_tsd_get/set/address for its internal keys
//These are from linuxthreads-0.7.1/specific.c

//FIXME: When enabled, SPARC/M5 crashes (for some weird reason, libc calls a tsd_get on an uninitialized key at initialization, and uses its result). Are we supposed to initialize these values??
//libc can live without these, so it's not critical
#if 0
enum __libc_tsd_key_t { _LIBC_TSD_KEY_MALLOC = 0,
                        _LIBC_TSD_KEY_DL_ERROR,
                        _LIBC_TSD_KEY_RPC_VARS,
                        _LIBC_TSD_KEY_LOCALE,
                        _LIBC_TSD_KEY_CTYPE_B,
                        _LIBC_TSD_KEY_CTYPE_TOLOWER,
                        _LIBC_TSD_KEY_CTYPE_TOUPPER,
                        _LIBC_TSD_KEY_N };
__thread void* p_libc_specific[_LIBC_TSD_KEY_N]; /* thread-specific data for libc */

int
__pthread_internal_tsd_set (int key, const void * pointer)
{
  p_libc_specific[key] = (void*) pointer;
  return 0;
}

void *
__pthread_internal_tsd_get (int key)
{
  return  p_libc_specific[key];
}

void ** __attribute__ ((__const__))
__pthread_internal_tsd_address (int key)
{
  return &p_libc_specific[key];
}
#endif //0


//Aliases for glibc
int __pthread_mutex_init (pthread_mutex_t* mutex, const pthread_mutexattr_t* attr)  __attribute__ ((weak, alias ("pthread_mutex_init")));
int __pthread_mutex_lock (pthread_mutex_t* lock) __attribute__ ((weak, alias ("pthread_mutex_lock")));
int __pthread_mutex_trylock (pthread_mutex_t* lock) __attribute__ ((weak, alias ("pthread_mutex_trylock")));
int __pthread_mutex_unlock (pthread_mutex_t* lock) __attribute__ ((weak, alias ("pthread_mutex_unlock")));

int __pthread_mutexattr_destroy (pthread_mutexattr_t* a) __attribute__ ((weak, alias ("pthread_mutexattr_destroy")));
int __pthread_mutexattr_init (pthread_mutexattr_t* a) __attribute__ ((weak, alias ("pthread_mutexattr_init")));
int __pthread_mutexattr_settype (pthread_mutexattr_t* a, int b) __attribute__ ((weak, alias ("pthread_mutexattr_settype")));

int __pthread_rwlock_init (pthread_rwlock_t* lock, const pthread_rwlockattr_t* attr) __attribute__ ((weak, alias ("pthread_rwlock_init")));  
int __pthread_rwlock_rdlock (pthread_rwlock_t* lock) __attribute__ ((weak, alias ("pthread_rwlock_rdlock")));
int __pthread_rwlock_wrlock (pthread_rwlock_t* lock) __attribute__ ((weak, alias ("pthread_rwlock_wrlock")));
int __pthread_rwlock_unlock (pthread_rwlock_t* lock) __attribute__ ((weak, alias ("pthread_rwlock_unlock")));
int __pthread_rwlock_destroy (pthread_rwlock_t* lock) __attribute__ ((weak, alias ("pthread_rwlock_destroy")));
/*
int   __pthread_key_create(pthread_key_t *, void (*)(void *)) __attribute__ ((weak, alias ("pthread_key_create")));
int   __pthread_key_delete(pthread_key_t) __attribute__ ((weak, alias ("pthread_key_delete")));
void* __pthread_getspecific(pthread_key_t) __attribute__ ((weak, alias ("pthread_getspecific")));
int   __pthread_setspecific(pthread_key_t, const void *) __attribute__ ((weak, alias ("pthread_setspecific")));
*/
int __pthread_once (pthread_once_t* once, void (*init)(void))  __attribute__ ((weak, alias ("pthread_once")));


//No effect, NPTL-specific, may cause leaks? (TODO: Check!)
void __nptl_deallocate_tsd() {}


/*
    m5threads, a pthread library for the M5 simulator
    Copyright (C) 2009, Stanford University

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
*/



#ifndef __PTHREAD_DEFS_H__
#define __PTHREAD_DEFS_H__


/*typedef struct {
    volatile int value;
    long _padding[15]; // to prevent false sharing
} tree_barrier_t;*/

// old LinuxThreads needs different magic than newer NPTL implementation
// definitions for LinuxThreads
#ifdef __linux__

//XOPEN2K and UNIX98 defines to avoid for rwlocks/barriers when compiling with gcc...
//see <bits/pthreadtypes.h>
#if !defined(__USE_UNIX98) && !defined(__USE_XOPEN2K) && !defined(__SIZEOF_PTHREAD_MUTEX_T)
/* Read-write locks.  */
typedef struct _pthread_rwlock_t
{
  struct _pthread_fastlock __rw_lock; /* Lock to guarantee mutual exclusion */
  int __rw_readers;                   /* Number of readers */
  _pthread_descr __rw_writer;         /* Identity of writer, or NULL if none */
  _pthread_descr __rw_read_waiting;   /* Threads waiting for reading */
  _pthread_descr __rw_write_waiting;  /* Threads waiting for writing */
  int __rw_kind;                      /* Reader/Writer preference selection */
  int __rw_pshared;                   /* Shared between processes or not */
} pthread_rwlock_t;


/* Attribute for read-write locks.  */
typedef struct
{
  int __lockkind;
  int __pshared;
} pthread_rwlockattr_t;
#endif
#if !defined(__USE_XOPEN2K) && !defined(__SIZEOF_PTHREAD_MUTEX_T)
/* POSIX spinlock data type.  */
typedef volatile int pthread_spinlock_t;

/* POSIX barrier. */
typedef struct {
  struct _pthread_fastlock __ba_lock; /* Lock to guarantee mutual exclusion */
  int __ba_required;                  /* Threads needed for completion */
  int __ba_present;                   /* Threads waiting */
  _pthread_descr __ba_waiting;        /* Queue of waiting threads */
} pthread_barrier_t;

/* barrier attribute */
typedef struct {
  int __pshared;
} pthread_barrierattr_t;

#endif


#ifndef  __SIZEOF_PTHREAD_MUTEX_T
#define PTHREAD_MUTEX_T_COUNT __m_count

#define PTHREAD_COND_T_FLAG(cond) (*(volatile int*)(&(cond->__c_lock.__status)))
#define PTHREAD_COND_T_THREAD_COUNT(cond) (*(volatile int*)(&(cond-> __c_waiting)))
#define PTHREAD_COND_T_COUNT_LOCK(cond) (*(volatile int*)(&(cond->__c_lock.__spinlock)))

#define PTHREAD_RWLOCK_T_LOCK(rwlock)  (*(volatile int*)(&rwlock->__rw_lock))
#define PTHREAD_RWLOCK_T_READERS(rwlock)  (*(volatile int*)(&rwlock->__rw_readers))
#define PTHREAD_RWLOCK_T_WRITER(rwlock)  (*(volatile pthread_t*)(&rwlock->__rw_kind))

//For tree barriers
//#define PTHREAD_BARRIER_T_NUM_THREADS(barrier)  (*(int*)(&barrier->__ba_lock.__spinlock))
//#define PTHREAD_BARRIER_T_BARRIER_PTR(barrier) (*(tree_barrier_t**)(&barrier->__ba_required))

#define PTHREAD_BARRIER_T_SPINLOCK(barrier)  (*(volatile int*)(&barrier->__ba_lock.__spinlock))
#define PTHREAD_BARRIER_T_NUM_THREADS(barrier) (*((volatile int*)(&barrier->__ba_required)))
#define PTHREAD_BARRIER_T_COUNTER(barrier) (*((volatile int*)(&barrier->__ba_present)))
#define PTHREAD_BARRIER_T_DIRECTION(barrier) (*((volatile int*)(&barrier->__ba_waiting)))

// definitions for NPTL implementation
#else /* __SIZEOF_PTHREAD_MUTEX_T defined */
#define PTHREAD_MUTEX_T_COUNT __data.__count

#define PTHREAD_RWLOCK_T_LOCK(rwlock)  (*(volatile int*)(&rwlock->__data.__lock))
#define PTHREAD_RWLOCK_T_READERS(rwlock)  (*(volatile int*)(&rwlock->__data.__nr_readers))
#define PTHREAD_RWLOCK_T_WRITER(rwlock)  (*(volatile int*)(&rwlock->__data.__writer))

#if defined(__GNUC__) && __GNUC__ >= 4
#define PTHREAD_COND_T_FLAG(cond) (*(volatile int*)(&(cond->__data.__lock)))
#define PTHREAD_COND_T_THREAD_COUNT(cond) (*(volatile int*)(&(cond-> __data.__futex)))
#define PTHREAD_COND_T_COUNT_LOCK(cond) (*(volatile int*)(&(cond->__data.__nwaiters)))

//For tree barriers
//#define PTHREAD_BARRIER_T_NUM_THREADS(barrier)  (*((int*)(barrier->__size+(0*sizeof(int)))))
//#define PTHREAD_BARRIER_T_BARRIER_PTR(barrier) (*(tree_barrier_t**)(barrier->__size+(1*sizeof(int))))

#define PTHREAD_BARRIER_T_SPINLOCK(barrier) (*((volatile int*)(barrier->__size+(0*sizeof(int)))))
#define PTHREAD_BARRIER_T_NUM_THREADS(barrier) (*((volatile int*)(barrier->__size+(1*sizeof(int)))))
#define PTHREAD_BARRIER_T_COUNTER(barrier) (*((volatile int*)(barrier->__size+(2*sizeof(int)))))
#define PTHREAD_BARRIER_T_DIRECTION(barrier) (*((volatile int*)(barrier->__size+(3*sizeof(int)))))

//Tree barrier-related
#if 0
#ifndef __SIZEOF_PTHREAD_BARRIER_T
#error __SIZEOF_PTHREAD_BARRIER_T not defined
#endif
#if ((4/*fields*/*4/*sizeof(int32)*/) > __SIZEOF_PTHREAD_BARRIER_T)
#error barrier size __SIZEOF_PTHREAD_BARRIER_T not large enough for our implementation
#endif
#endif

#else // gnuc >= 4
//gnuc < 4
#error "This library requires gcc 4.0+ (3.x should work, but you'll need to change pthread_defs.h)"
#endif // gnuc >= 4

#endif // LinuxThreads / NPTL

// non-linux definitions... fill this in?
#else // !__linux__
  #error "Non-Linux pthread definitions not available"
#endif //!__linux__

#endif //  __PTHREAD_DEFS_H__
/*
    m5threads, a pthread library for the M5 simulator
    Copyright (C) 2009, Stanford University

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
*/



#ifndef __SPINLOCK_ALPHA_H__
#define __SPINLOCK_ALPHA_H__

// routines adapted from /usr/src/linux/include/asm-alpha/spinlock.h

static __inline__ void spin_lock (volatile int* lock) {
        long tmp;
        __asm__ __volatile__(
         "1:     ldl_l   %0,%1\n"
         "       bne     %0,2f\n"
         "       lda     %0,1\n"
         "       stl_c   %0,%1\n"
         "       beq     %0,2f\n"
         "       mb\n"
         ".subsection 2\n"
         "2:     ldl     %0,%1\n"
         "       bne     %0,2b\n"
         "       br      1b\n"
         ".previous"
         : "=&r" (tmp), "=m" (*lock)
         : "m"(*lock) : "memory");
}

static __inline__ void spin_unlock (volatile int* lock) {
   __asm__ __volatile__ ("mb\n");
   *lock = 0;
}

static __inline__ int trylock (volatile int* lock) {


	long regx;
	int success;

	__asm__ __volatile__(
	"1:	ldl_l	%1,%0\n"
	"	lda	%2,0\n"
	"	bne	%1,2f\n"
	"	lda	%2,1\n"
	"	stl_c	%2,%0\n"
	"	beq	%2,6f\n"
	"2:	mb\n"
	".subsection 2\n"
	"6:	br	1b\n"
	".previous"
	: "=m" (*lock), "=&r" (regx), "=&r" (success)
	: "m" (*lock) : "memory");

	return success;


}

#endif  // __SPINLOCK_H__
/*
    m5threads, a pthread library for the M5 simulator
    Copyright (C) 2009, Stanford University

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
*/


#ifndef __SPINLOCK_ARM_H__
#define __SPINLOCK_ARM_H__

static __inline__ void spin_lock (volatile int* lock) {
    unsigned long tmp;
  
         __asm__ __volatile__(
"1:     ldrex   %0, [%1]\n"
"       cmp     %0, #0\n"
"       strexeq %0, %2, [%1]\n"
"       cmpeq   %0, #0\n"
"       bne     1b\n"
"       dmb\n"
        : "=&r" (tmp)
        : "r" (lock), "r" (1)
        : "cc");

}

static __inline__ void spin_unlock (volatile int* lock) {


     __asm__ __volatile__(
"       dmb\n"
"       str     %1, [%0]\n"
        :
        : "r" (lock), "r" (0)
        : "cc");
}


static __inline__ int trylock (volatile int* lock) {
        unsigned long tmp;

        __asm__ __volatile__(
"       ldrex   %0, [%1]\n"
"       cmp     %0, #0\n"
"       strexeq %0, %2, [%1]\n"
"       eor     %0, %0, #1\n"
"       bne     fail\n"
"       dmb\n"
"fail:     nop\n"
        : "=&r" (tmp)
        : "r" (lock), "r" (1)
        : "cc", "memory");

        return tmp;
}

#endif  // __SPINLOCK_ARM_H__
/*
    m5threads, a pthread library for the M5 simulator
    Copyright (C) 2009, Stanford University

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
*/


#ifndef __SPINLOCK_SPARC_H__
#define __SPINLOCK_SPARC_H__

// routines from /usr/src/linux/include/asm-sparc/spinlock_64.h
// Note: these work even with RMO, but a few barriers could be eliminated for TSO

static __inline__ void spin_lock(volatile int* lock)
{
	unsigned long tmp;

	__asm__ __volatile__(
"1:	ldstub		[%1], %0\n"
"	membar		#StoreLoad | #StoreStore\n"
"	brnz,pn		%0, 2f\n"
"	 nop\n"
"	.subsection	2\n"
"2:	ldub		[%1], %0\n"
"	membar		#LoadLoad\n"
"	brnz,pt		%0, 2b\n"
"	 nop\n"
"	ba,a,pt		%%xcc, 1b\n"
"	.previous"
	: "=&r" (tmp)
	: "r" (lock)
	: "memory");
}

static __inline__ int trylock(volatile int* lock)
{
	unsigned long result;

	__asm__ __volatile__(
"	ldstub		[%1], %0\n"
"	membar		#StoreLoad | #StoreStore"
	: "=r" (result)
	: "r" (lock)
	: "memory");

	return (result == 0);
}

static __inline__ void spin_unlock(volatile int* lock)
{
	__asm__ __volatile__(
"	membar		#StoreStore | #LoadStore\n"
"	stb		%%g0, [%0]"
	: // No outputs 
	: "r" (lock)
	: "memory");
}


#endif  // __SPINLOCK_SPARC_H__
/*
    m5threads, a pthread library for the M5 simulator
    Copyright (C) 2009, Stanford University

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
*/


#ifndef __SPINLOCK_X86_H__
#define __SPINLOCK_X86_H__

// routines from /usr/src/linux/include/asm-x86/spinlock.h

static __inline__ void spin_lock (volatile int* lock) {
    char oldval;
    __asm__ __volatile__
        (
         "\n1:\t" \
         "cmpb $0,%1\n\t" \
         "ja 1b\n\t" \
         "xchgb %b0, %1\n\t" \
         "cmpb $0,%0\n" \
         "ja 1b\n\t"
         :"=q"(oldval), "=m"(*lock)
         : "0"(1)
         : "memory");
}

static __inline__ void spin_unlock (volatile int* lock) {
	__asm__ __volatile__
        ("movb $0,%0" \
         :"=m" (*lock) : : "memory");
}

static __inline__ int trylock (volatile int* lock) {
    char oldval;
    __asm__ __volatile__
        (
         "xchgb %b0,%1"
         :"=q" (oldval),
          "=m" (*lock)
         :"0" (1) 
         : "memory");
    return oldval == 0;
}

#endif  // __SPINLOCK_X86_H__
/*
    m5threads, a pthread library for the M5 simulator
    Copyright (C) 2009, Stanford University

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
*/

#ifndef __TLS_DEFS_H__
#define __TLS_DEFS_H__

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>


//These are mostly taken verbatim from glibc 2.3.6

//32 for ELF32 binaries, 64 for ELF64
#if defined(__LP64__)
#define __ELF_NATIVE_CLASS 64
#else
#define __ELF_NATIVE_CLASS 32
#endif

//Seems like all non-ARM M5 targets use TLS_TCB_AT_TP (defined in
//  platform-specific 'tls.h')
#if defined(__arm__)
#define TLS_DTV_AT_TP 1
#else
#define TLS_TCB_AT_TP 1
#endif

/* Standard ELF types.  */

#include <stdint.h>

/* Type for a 16-bit quantity.  */
typedef uint16_t Elf32_Half;
typedef uint16_t Elf64_Half;

/* Types for signed and unsigned 32-bit quantities.  */
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;

/* Types for signed and unsigned 64-bit quantities.  */
typedef uint64_t Elf32_Xword;
typedef int64_t  Elf32_Sxword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

/* Type of addresses.  */
typedef uint32_t Elf32_Addr;
typedef uint64_t Elf64_Addr;

/* Type of file offsets.  */
typedef uint32_t Elf32_Off;
typedef uint64_t Elf64_Off;

/* Type for section indices, which are 16-bit quantities.  */
typedef uint16_t Elf32_Section;
typedef uint16_t Elf64_Section;

/* Type for version symbol information.  */
typedef Elf32_Half Elf32_Versym;
typedef Elf64_Half Elf64_Versym;


typedef struct
{
  Elf32_Word    p_type;                 /* Segment type */
  Elf32_Off     p_offset;               /* Segment file offset */
  Elf32_Addr    p_vaddr;                /* Segment virtual address */
  Elf32_Addr    p_paddr;                /* Segment physical address */
  Elf32_Word    p_filesz;               /* Segment size in file */
  Elf32_Word    p_memsz;                /* Segment size in memory */
  Elf32_Word    p_flags;                /* Segment flags */
  Elf32_Word    p_align;                /* Segment alignment */
} Elf32_Phdr;

typedef struct
{
  Elf64_Word    p_type;                 /* Segment type */
  Elf64_Word    p_flags;                /* Segment flags */
  Elf64_Off     p_offset;               /* Segment file offset */
  Elf64_Addr    p_vaddr;                /* Segment virtual address */
  Elf64_Addr    p_paddr;                /* Segment physical address */
  Elf64_Xword   p_filesz;               /* Segment size in file */
  Elf64_Xword   p_memsz;                /* Segment size in memory */
  Elf64_Xword   p_align;                /* Segment alignment */
} Elf64_Phdr;


#define ElfW(type) _ElfW (Elf, __ELF_NATIVE_CLASS, type)
#define _ElfW(e,w,t)       _ElfW_1 (e, w, _##t)
#define _ElfW_1(e,w,t)     e##w##t


#define PT_TLS              7               /* Thread-local storage segment */


# define roundup(x, y)  ((((x) + ((y) - 1)) / (y)) * (y))

extern ElfW(Phdr) *_dl_phdr;
extern size_t _dl_phnum;

//Architecture-specific definitions

#if defined(__x86_64) || defined(__amd64)

/* Type for the dtv.  */
typedef union dtv
{
  size_t counter;
  void *pointer;
} dtv_t;

typedef struct
{
  void *tcb;            /* Pointer to the TCB.  Not necessary the
                           thread descriptor used by libpthread.  */
  dtv_t *dtv;
  void *self;           /* Pointer to the thread descriptor.  */
  int multiple_threads;
} tcbhead_t;

#include <asm/prctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

/* Macros to load from and store into segment registers.  */
# define TLS_GET_FS() \
  { int __seg; __asm ("movl %%fs, %0" : "=q" (__seg)); __seg; }
# define TLS_SET_FS(val) \
  __asm ("movl %0, %%fs" :: "q" (val))

# define TLS_INIT_TP(thrdescr, secondcall) \
  { void *_thrdescr = (thrdescr);                                            \
     tcbhead_t *_head = (tcbhead_t *) _thrdescr;                              \
     int _result;                                                             \
                                                                              \
     _head->tcb = _thrdescr;                                                  \
     /* For now the thread descriptor is at the same address.  */             \
     _head->self = _thrdescr;                                                 \
                                                                              \
     /* It is a simple syscall to set the %fs value for the thread.  */       \
     asm volatile ("syscall"                                                  \
                   : "=a" (_result)                                           \
                   : "0" ((unsigned long int) __NR_arch_prctl),               \
                     "D" ((unsigned long int) ARCH_SET_FS),                   \
                     "S" (_thrdescr)                                          \
                   : "memory", "cc", "r11", "cx");                            \
                                                                              \
    _result ? "cannot set %fs base address for thread-local storage" : 0;     \
  }

#elif defined (__sparc)

register struct pthread *__thread_self __asm__("%g7");

/* Code to initially initialize the thread pointer.  */
# define TLS_INIT_TP(descr, secondcall) \
  (__thread_self = (__typeof (__thread_self)) (descr), NULL)

#elif defined (__arm__)

typedef struct
{
  void *dtv;
  void *private;
} tcbhead_t;

#define INTERNAL_SYSCALL_RAW(name, err, nr, args...)        \
  ({ unsigned int _sys_result;                  \
     {                              \
       register int _a1 asm ("a1");             \
       LOAD_ARGS_##nr (args)                    \
           asm volatile ("mov r7, #0xf0000\n"    \
                     "add r7, r7, #0x0005\n"  \
         "swi   #0  @ syscall " #name       \
             : "=r" (_a1)               \
             : "i" (name) ASM_ARGS_##nr         \
             : "memory");               \
       _sys_result = _a1;                   \
     }                              \
     (int) _sys_result; })

#undef INTERNAL_SYSCALL_ARM
#define INTERNAL_SYSCALL_ARM(name, err, nr, args...)        \
    INTERNAL_SYSCALL_RAW(__ARM_NR_##name, err, nr, args)

#define LOAD_ARGS_0()

#define ASM_ARGS_0

#define LOAD_ARGS_1(a1)             \
  int _a1tmp = (int) (a1);          \
  LOAD_ARGS_0 ()                \
  _a1 = _a1tmp;

#define ASM_ARGS_1  ASM_ARGS_0, "r" (_a1)

# define TLS_INIT_TP(descr, secondcall) \
    INTERNAL_SYSCALL_ARM(set_tls, 0, 1, (descr))

#else
  #error "No TLS defs for your architecture"
#endif

#endif /*__TLS_DEFS_H__*/

#ifndef _ISP_HEADER_
#define _ISP_HEADER_

#define TRUE 1
#define FALSE 0

typedef int _isp_platform_id;
typedef int _isp_device_id;

typedef _isp_platform_id*	isp_platform_id;
typedef enum { ISP_DEVICE_TYPE_STORAGE } isp_device_type;
typedef unsigned int isp_uint;
//typedef	_isp_device_id* isp_device_id;
typedef	int isp_device_id;
typedef int isp_int;

typedef struct 
{
	int start[1000],end[1000];
	int size;
} isp_lba_list;

/* in-storage-processing API's */
isp_int ispGetDeviceIDs(isp_platform_id platform,
        isp_device_type device_type,
        isp_uint num_entries,
        isp_device_id *devices,
        isp_uint* num_devices);

// download a script string to a storage. It is appended to the existing scripts
isp_int ispAddScript(isp_device_id device_id,
        const char* script_string);

// remove whole scripts in the storage
isp_int ispClearScript(isp_device_id device_id);

// ispSetActorArgument provides data values to actors.
// For instance, it provides LBA list.
// Maybe some actors receive these data through callback functions.
isp_int ispSetActorArgument(isp_device_id device_id,
        const char* instance_name,      // actor instance name
        const char* argument_name,
        void* argument,
        int size_of_argument);

// callback function is called by actor instance
void ispRegisterCallbackFunction(isp_device_id device_id,
        const char* instance_name,                                      // actor instance name
        const char* func_name,                                          // callback function name in the actor
        void(*func)(isp_device_id,void* data, int* data_size));       // callback function pointer in host

// It supports two modes of execution: release and debug.
// If debug mode is true then print callback function is called when a message is arrived.
// After the callback function is resolved, execution of the script continues.
isp_int ispRunScript(isp_device_id device_id,
        int isp_debug_mode,
        void(*printFunction)(char* message));

isp_int ispExit(isp_device_id device_id);

// utility functions
isp_int ispGetLBAList(isp_device_id device_id, 
        const char* fileName, 
        isp_lba_list* lba_list);

// isp binary handling functions
// download a binary file to ISSD
//isp_int ispSendBinary(isp_device_id device_id, const char* program_name, const char* program_file_name);

// run the downloaded binary program in ISSD
//isp_int ispRunBinary(isp_device_id device_id, const char* program_name, const char* program_argument, const char* program_output_file);

// run the downloaded binary program in ISSD
isp_int ispRunBinaryFile(isp_device_id device_id, const char* program_file_name, const char* program_argument, const char* program_output_file);

isp_int ispRunBinaryFileEx(isp_device_id device_id, const char* program_file_name, const char* program_argument, const char* program_output_file, const int numprocs, const char* clocks);

#endif
#ifndef _ISP_HEADER_
#define _ISP_HEADER_

#define TRUE 1
#define FALSE 0

typedef int _isp_platform_id;
typedef int _isp_device_id;

typedef _isp_platform_id*	isp_platform_id;
typedef enum { ISP_DEVICE_TYPE_STORAGE } isp_device_type;
typedef unsigned int isp_uint;
//typedef	_isp_device_id* isp_device_id;
typedef	int isp_device_id;
typedef int isp_int;

typedef struct 
{
	int start[1000],end[1000];
	int size;
} isp_lba_list;

/* in-storage-processing API's */
isp_int ispGetDeviceIDs(isp_platform_id platform,
        isp_device_type device_type,
        isp_uint num_entries,
        isp_device_id *devices,
        isp_uint* num_devices);

// download a script string to a storage. It is appended to the existing scripts
isp_int ispAddScript(isp_device_id device_id,
        const char* script_string);

// remove whole scripts in the storage
isp_int ispClearScript(isp_device_id device_id);

// ispSetActorArgument provides data values to actors.
// For instance, it provides LBA list.
// Maybe some actors receive these data through callback functions.
isp_int ispSetActorArgument(isp_device_id device_id,
        const char* instance_name,      // actor instance name
        const char* argument_name,
        void* argument,
        int size_of_argument);

// callback function is called by actor instance
void ispRegisterCallbackFunction(isp_device_id device_id,
        const char* instance_name,                                      // actor instance name
        const char* func_name,                                          // callback function name in the actor
        void(*func)(isp_device_id,void* data, int* data_size));       // callback function pointer in host

// It supports two modes of execution: release and debug.
// If debug mode is true then print callback function is called when a message is arrived.
// After the callback function is resolved, execution of the script continues.
isp_int ispRunScript(isp_device_id device_id,
        int isp_debug_mode,
        void(*printFunction)(char* message));

isp_int ispExit(isp_device_id device_id);

// utility functions
isp_int ispGetLBAList(isp_device_id device_id, 
        const char* fileName, 
        isp_lba_list* lba_list);

// isp binary handling functions
// download a binary file to ISSD
//isp_int ispSendBinary(isp_device_id device_id, const char* program_name, const char* program_file_name);

// run the downloaded binary program in ISSD
//isp_int ispRunBinary(isp_device_id device_id, const char* program_name, const char* program_argument, const char* program_output_file);

// run the downloaded binary program in ISSD
isp_int ispRunBinaryFile(isp_device_id device_id, const char* program_file_name, const char* program_argument, const char* program_output_file);

isp_int ispRunBinaryFileEx(isp_device_id device_id, const char* program_file_name, const char* program_argument, const char* program_output_file, const char* numprocs, const char* clocks);

#endif
#ifndef _ISP_HEADER_
#define _ISP_HEADER_

#define TRUE 1
#define FALSE 0

typedef int _isp_platform_id;
typedef int _isp_device_id;

typedef _isp_platform_id*	isp_platform_id;
typedef enum { ISP_DEVICE_TYPE_STORAGE } isp_device_type;
typedef unsigned int isp_uint;
//typedef	_isp_device_id* isp_device_id;
typedef	int isp_device_id;
typedef int isp_int;

typedef struct 
{
	int start[1000],end[1000];
	int size;
} isp_lba_list;

/* in-storage-processing API's */
isp_int ispGetDeviceIDs(isp_platform_id platform,
        isp_device_type device_type,
        isp_uint num_entries,
        isp_device_id *devices,
        isp_uint* num_devices);

// download a script string to a storage. It is appended to the existing scripts
isp_int ispAddScript(isp_device_id device_id,
        const char* script_string);

// remove whole scripts in the storage
isp_int ispClearScript(isp_device_id device_id);

// ispSetActorArgument provides data values to actors.
// For instance, it provides LBA list.
// Maybe some actors receive these data through callback functions.
isp_int ispSetActorArgument(isp_device_id device_id,
        const char* instance_name,      // actor instance name
        const char* argument_name,
        void* argument,
        int size_of_argument);

// callback function is called by actor instance
void ispRegisterCallbackFunction(isp_device_id device_id,
        const char* instance_name,                                      // actor instance name
        const char* func_name,                                          // callback function name in the actor
        void(*func)(isp_device_id,void* data, int* data_size));       // callback function pointer in host

// It supports two modes of execution: release and debug.
// If debug mode is true then print callback function is called when a message is arrived.
// After the callback function is resolved, execution of the script continues.
isp_int ispRunScript(isp_device_id device_id,
        int isp_debug_mode,
        void(*printFunction)(char* message));

isp_int ispExit(isp_device_id device_id);

// utility functions
isp_int ispGetLBAList(isp_device_id device_id, 
        const char* fileName, 
        isp_lba_list* lba_list);

// isp binary handling functions
// download a binary file to ISSD
//isp_int ispSendBinary(isp_device_id device_id, const char* program_name, const char* program_file_name);

// run the downloaded binary program in ISSD
//isp_int ispRunBinary(isp_device_id device_id, const char* program_name, const char* program_argument, const char* program_output_file);

// run the downloaded binary program in ISSD
isp_int ispRunBinaryFile(isp_device_id device_id, const char* program_file_name, const char* program_argument, const char* program_output_file);

#endif

#ifndef _S4LIB_
#define _S4LIB_

#include <stdio.h>


// simulation related variables and functions
extern int s4_tick_time;
void s4_spend_time(int theTick);

void s4_init_simulation();
void s4_wrapup_simulation();

// File related data structure
#define S4_PAGE_SIZE 1024  
#define S4_NUM_BUFFERS 1
char s4_buffer[S4_PAGE_SIZE*S4_NUM_BUFFERS];

// File related functions
FILE *
s4_fopen(const char * filename, const char * mode);

int
s4_fseek(FILE *stream, long offset, int whence);

void
s4_rewind(FILE *stream);

size_t
s4_fread(void * ptr, size_t size, size_t nitems, FILE * stream);

size_t
s4_pageread(size_t pageStartNumber, size_t numPages, FILE * stream);

size_t
s4_fwrite(const void * ptr, size_t size, size_t nitems, FILE * stream);
#endif
#define GEM5_EXECFILE	"./gem5/build/ARM/gem5.opt"
#define GEM5_PLATFORM	"./gem5/configs/example/se.py"
#define GEM5_NUMPROCS	4

#define GEM5_EXECFILE	"./gem5/build/ARM/gem5.opt"
#define GEM5_PLATFORM	"./gem5/configs/example/se.py"
#define GEM5_NUMPROCS	256

       HO                 J                  BCEFS              BCEHLNPQ           BCQ         	       GHJLMOPST          CELN               LN          	       BCDEHJRST          ACEGIKP     	       BDEFGIJKN          AGILQ              DGRST       	       BCEJKNPST          BEFGJKMN           BGKMNPQR           BEGKLMOP           H                  HR                 BDFHIJNS           GKN         	       ACEIKLPRT          BCKP               OQ                 Q                  IT          	       ABEHINOST          BDHIKNOR    
       ADEIJLPQST         CFJNP              DHIO               S           	       ABCJMQRST          R           	       CGILMNOPQ   
       BCEJLMQRST         ABDJKM      	       BCDFGIJPR          BFIJMOPQ           ABFJM       	       DGHJNPQRT          BCFGIOPQ           DFGLMNP     	       BCEFGMNQS          BCGJKNP            EMNOT              F           
       CFGHILNORS         EMNOT              H                  DFIKMNRS           AHN                FHT                BDEM               DM                 CFGLMN             AHKNO              P                  AHLQ               AEKOPT             CGLOQS      	       AEGHLMNST          R                  P                  CDEGHJS            FHIJOR             ACDQ               ABCEFKPQ           AEHIKLN            ACJL               N           	       ADEFHIRST          ABHK        	       AEFKLMNPQ          FGQR               BCJLMQR            BDFJMPQT           CEMO               DHIJLMPS           BCLO               BFGKOT             BDKNST             FNOQR              ABI                BCLP               ADEMNPQS           KRS                JMP                AI                 FJKNOR             AE          	       BDEFIJMPT          JQR                BH          
       ABDEMNOPRT         I                  BDEGHKPS           DGKL        
       ACEFKLMNST         CDEHST      	       EFGJLMNPS          FJT                AFIKS              DM                 HKM                ABDH        
       ABCDFHIJPR         OS          	       ADGHIJKPQ          DEFIJNS     
       BEFGJLPQRS         CENPQ       	       BEFGJMNPQ          CMOQS              BFKLPR             CIKMNOQ            DNO                DEIQR              AIJS        	       BCDEIJKPQ          ANO                DJL         	       BDEFILMNT   	       BCIJKORST          JPRS               L                  KMPRT       	       ABCEFKLOP          IKNRST             JNQ         	       ABDEFIKLS          HILNP              GJKNQS             Q           
       ABCEFGHIQT         EJ                 BCDEFN             GI          	       BCEHJKMOQ          FIKMNOQ     	       CEJLMOPST   
       CDFGMNQRST         ACHIKMST           ACP                FIJKLQRS    	       ABCHJMOPS          DGIJOPS            BFIKOP             BCEJR       
       DEFKLNOQRS  
       BDEFIKMNPR         GOQ                ADJLMOT            JT                 EFJKPQ             ABJMNOP     	       CGIKLMOST          ELP                ACEOP              BCH                BDFJKMOP           IMT                HIP         	       ADEFGHOST          HMQ                BCDHL              ACDILQS            FKQ                DGJOR              DGHILMQR           FHIS               BDIJKLMN           BFHJN              BKMO               CKR                BDKRT              CGHJKS             ADGIKLNS           IKM                BDFHS       	       BDHJKOQRS          AFGKNP      	       AGIKMNPRT          CIM         
       BDEHIJMOPR  
       ADIKLMNORT         S                  AE                 ADFLMNST           AFJLR              JKS         
       ACGKLNQRST  	       ADEHIJMRT          H                  BCKLOQT            BDMQ               ACNPQ              AMQST              ABCEGJKR           BFPS               HJKQRST            PS                 DEFIMNT     
       CEGIJLNOPR         GJLMNOP     
       ACDGHLMPQR         ABFNR       	       AFHJKLNPS          CHKMOQ             HL                 BHILST             CGIJPQT            ADINR              CL                 BGMPQR             CENOPQST           ADHIMN             BDKPQST            BFNO               N                  HT                 D                  BHJKMQ             FM                 AFJ                NS                 AGILQRS            EIJRS              EGIKRS      
       AFGHKLNPST         AEGHILMO           EFLR               ACGINPRS    
       BCEFGHMNPR         ADILNQT     	       ADFLNOPQT          P                  Q                  CFILQT             AFP                ACF                ACINQRS            G                  AMNPR              H                  BFGIPQ      
       BCDIMOPQRS         K                  ADFJPT             BM                 CHJN               ADOS               DEJKM              ADGHOQ      
       BDFIJKLPST         CHINPQT            BCFK               HJLORS             DEIMRS             BDEHK              BCDFKLMP           LNP         	       CEFHIKOST   
       BDEFGHKNOS         DFJKQS             EIJKOR             M                  BOPQ               BCDEMPT            FILPQRST           B                  AMPQ               BEJKLOQT           O                  HJL                T                  BDFN               BDINPST            BCDILST            CDJO               ABCGHLNR           ACGJKPR            ACDGOP      	       CDFHIJLRT          DGIL               Q           	       CDEFHLPQT   	       CDJKLMNPT          O           	       BCDFKLNRS   	       CDHIKMPST          BCIOST             BHR         
       ABCDEFJKNO  
       DEFJKLMNOS         CDJLMNST    	       ACDKLMOQR          ADIPRST            ACK                BGN         	       BDFHJMRST          I                  JM                 M                  BHIKP              R                  QT                 AKNR               BCDKN              HLR                ADKR               ABDFGLT     
       BDEHKLPRST  
       ABCFIKOPST         BGKNPQRS           AM          	       BFGIMNOPT          DKNR               IO                 HIJLPQST    
       BDEFGHLNOQ         DEIM               DEHIJKLO    
       ADEGHJMOPS         K                  BDGHJLP            BCGIKP             CEFGIJMS           ABFS               AHLNOP             ABKLOQR            CFMNRS      	       CDFGHLMRT          FJMOST             ACMNRS             DIR                BCJOQS      	       ABDEGPRST          CI                 EHNQRT             DS          	       BDEJLMPRT          PQ                 ABHIJMNT    	       CDEFJKNOS          AEFHMN             BCEFKLOR    	       BEGIJOQRS          ACGIKNST           IMQST              AHIOT              CS                 BEG                BEFGIJN            ABC                D           	       BDHLMNOPQ          LT          	       BEHJKLMNS          ACDMNQ             GILP               BDJ         
       ADFHKLMNOT         HJS                L                  HO                 DGHJQ              GKT                ADN                CGHJLT             AFGIMNOQ    
       ACDHJLNOQS         H                  DKQR               AGLOR              EGO                AEMP               L                  BDFJMRT            BDEGI       	       BDEGKLORS          DLM                BL                 S           
       ABHILOPQRT         BDJN        
       BCDFGHKLQR         EFHLMPRS           AKLNPR      	       EGHIJKLST   
       AGJKMNOQST         O                  GIJLN       
       ABCFGIJKNS  
       ADGHJNOPRS         DNPS               AGLO               KLMNOPQR           J           	       ACEGHJMNO          CDEFLNOP           GJ          	       ACEHILNOP          ACKOPQRS           PR                 CHJL               AO                 BFIN               AKLP               ALP                AGIOT              CEHLMQ             BCIKMNQT           EGKLP              ABDIPST     
       CGIJMNOPRT         DIMN        	       BCFIKLNPR   	       ADEFJNPQS          AFIO               BFIM               ABCGKMP            HR                 ILNP               CLOT               BKN                DGILS              CEFHNOPT           AGJKRST            ACDFLPR            BEFGIPS            DMRS               ACDGLOT            HKNOQS             BGK                DEJNOP             ABHT               ABHMNO             DFILNRST    	       DEGHKMNOQ          BEFGLST            GHILNRT            BCEHIMRT           ABCIQ              EFOST              DJOPT              LOR                KLRT               EGMS        
       CDEHIJLNOR         KLMNST             N                  FJNOS              DLMS        	       ABFGIJLRS          CDFGJLQS           KQRS               GK                 ADEL               FS          
       CEFGHIOPQR  	       CJKLMPQST   
       BFIKLMNPQR         FS                 CEGIQST            GI                 ACFGHIJT    
       BEFGHJLOQR         AFKPQR             DFGIJKPR           ACIJKNOP           T                  EFHMNORT           CDL         
       ABDEIKMNRT         DJ                 ADHKMPR            EGJMNOPS           CEMNP              AEN                CNR                R                  FGKLQRST           EFHMPQ      	       CFGHINPRS          EFGJMOT     
       BHJKMNOPQS         DEGNQR      
       AEFGIJKLRS  	       ABDEFHIKQ          ADEKL              BFGHIKQ            KLOPRST            QRS                H                  BCGKOPQT           EQR                S                  ABE                D                  BHIKMST     
       CEHJLNPQRT         BO          
       BCEFJMQRST         CHLN               Q           	       BDEJLMNST          JNOPT              BQR                FLS                H                  BCEHIT             BIMOQ              BFGMNOPS           ABEGHOQ            ABFLST             CDEFGPQR           BDFGJMN            FGJKQ              DKMNPQ             KPQ                AE          
       BCEFGIKOQR         K                  BNS                AHNPS              F                  FKNQ               DJT                ABFJLMNT    	       DEHJLNOQR          CEHLPRS            FGK                F                  ACEKOPR            G                  CFHLNPQ            BIMORT             ACJLMRST           Q                  BFMNP       
       BEFGHIJOPT         JO          	       BDEFHNPRS          PQRT               EKMO        
       ABGIKLMOPQ         ABKORS             BCKLMNST           BCIS               CDEFGHOR           F                  DIKNR              BCGMQS             CDEGKMP            KP          	       ABDEFHIMS          DST                MQ                 BDI                DFQRST             ACHJKPR            ABGJNR             CDFNPT             ABCFIMQ            FGIMQST            HQ                 CF                 EMT                AHKLNOST           DKMPQT             CGHPQRS            AEFIMOS            FJKNRST            FL          
       BCFHIJKNPT         BDJLMOT            EIKPQST            BCDHLNO            DFGKNPRT           DFGLMNT            HLPQT              MNR                T                  BFGIJNPT    	       DEFJLOPQS          LPR                CDLPS              AIKM               ABCIRS             ACHMNOS            DEFGHKOP           ALT         	       AFGHILPST          AHLOQ       	       BEJKMOQST          DET         
       DEGHJOPQRT         GJMPQRS     	       CEIJMNOPQ   
       CEFGMOPQRT         BCET               ANPR               BCFIP              AIJLMNR     	       CDJKMNOQR   
       CDGIJMNOST         HNRS               BEHMQT             IJLMPQ             AEGJMQS            DEFILOS            CDENR              CFL                GIJKP              BCDEJT             BMT                AFIJKLST           Q           	       AEFGKMPQS   
       CEFHIMNQST         CEFHJST     	       ABDEGHIKQ          AFGKMNR     	       AEFHIMRST          CJKLM              AEFHKL             CFHIR              IPRT        
       ABEGJKLNOT         FJKMOQRT           T                  ACFLNOQR           KOQ                BFHM               ET                 FHIKOST            APT         	       CDJMPQRST          L                  AGR                DFGMPS      	       ABEFGJNRS          BIJNPQT            CDJLT              ADHIJKS            ADFGJQS            P                  DGIJKPT     
       ABFLMOPQRS  	       AEFGHKNQS          JK          	       CEJLNPQRT          ABDIOQRT           JL          
       BDEFGJKPRT         EMN                P           	       ADEFIKOQS          HR          	       ACEGKLMPT   	       EFGHIMQST          KLQ                DFGIJKOQ           BOQS               OP                 CET                P                  AEFJLPQR    	       EGHKLOPST          AGMOQS             LNR                FHIJOT             I                  IKMRS              BCDHKNT     
       ABCGLMNPQR         ABCFGKLP           BDLMPR             BCEIJKMT           BELO               AEIR               CJNS        	       BDFGHJKPS          P                  ACLO               JP                 O                  KPQ                CNO         	       ACDEFHJPT   	       ACGHILNST          CIJNQ       	       CDEFGKLPT   	       BDFIJKOQR          BCHIMPS            ACGJPT             BCDFKMQ            BKM                AFIK               B                  EJKMOPT            EK                 BLP                DGKMOPS            EFM                BP                 F                  CH                 HKNT        	       AEFJKLMOP          ABDFHJT            ACHIJLP            BEFJKLT            BDEFIJKO           ER                 FH                 T                  ABHIMT             BEFINPR            HO                 OQS                EI                 ABDEJQR            ACHMQ       	       DGHMNQRST          ABDMN              M                  EFNR               AGH                I                  DEIJKNT            H                  H                  DHN                FT                 BS          	       BCDEHMQRS          S                  BMOT               FLN                AL          	       ACDEIKLOP   
       ADIJKLMPRT         BCELMOQR           FM          	       BFGIJMORS   
       CDFHIJMNPQ  
       BIJKLMNQRS         GJLMST      
       DFHILMOQST  
       BDFHIMOPQT         GHJLMNOR    
       BCDHJLMOQR         DGQ                BCEFMOPQ           CDJM               JRT                FKMNOT             JK                 FIKRT       	       CEFIJLOPR          OT                 G                  H                  BGKS               BGOS               CF                 AGNOPQT            FMPS               Q                  CEFGMP             ABGKLNO            ACJR               CIMOPS             AMR                BKLT               CEFGOT      
       CFIJKLNOQT         R           	       BDFHILNRS          BPS                KOS                DF                 FPRS        
       CEGJLMOQST         BCIJKM             EFGIPRST           AG                 HI                 HL                 R                  ADFGJOQT    	       CDEHKLNQS          DIJOT              BDFIJMPR           DEFHJKLS           ACJLMNR     	       ACDEGJLOT          BEFHIJO            DFKMNOR            DMT         	       DFHIJKMPT          ABDMS              ACDFGMRT           EKMPQRT            ABFGHMPS           CDFJL              C                  AJQ                LPQ                BCJLOR             BCEFNPR     
       ACDEJNOPST         BEGIJLM            ENOQS              EIQR               GIKNQ              ABGIJLP            BL                 IJKNOS             NT          	       CHIJKNOQR   
       ABCDEFGHJM         ADL                DKT         
       DEFGILMNQS         BDEHKOQT           ABDMNOP     	       ACDILNORT          GIK                BFGKLOPS           DEIKNOPT    	       AEGHKMNQS   
       CFGKMNOPRT         BDGOQST            DEJOQR             DEGHM              JP          
       BEGIJKLNQT         DGKLNQT            DFHJLMNP           DIJLPRS            DO                 BHKPQS             QT          
       BDIJKLMOPQ  
       ACDEFIKLOR         ACDGJO      	       ACDIKLQST   	       CEFJKLMNP   
       ACEGHJMNQS         CLNOQ       
       ACFHIJKOQT         FNP         	       BDEGHIMRS          KPS                FGJKLN             EH                 N           
       ABCDEGIKOQ         ACEIST             ABO                CGKMOQR            BJ                 BCDFIJKS           BGHKL              BNS                LNQ                BEFMNR             AIJKR              BDLQ               I                  LMNO        
       ABCFJKMOPQ         LR                 BDFG               N                  CEFNQ       
       BDGHIJKLNP  
       ABCDEFKPRS         EGM                EFJMO       
       ABDGJKMNQT         ABHJKR      
       BDEFNOPRST         DG                 KLPR               ABP                H                  BLNO        	       ABDHJKMPT          DKLRS              BS          	       DEFIJKMNP          BCEFIPT     	       AEFGHJLNP          ABGLS              EFHKMOPT           BCEFNPT            EJLM        
       CDEFGKLNRS         GST                GT                 FHST               AFKO               NP                 AEHILMNS           BIMNPRT            ABCDFLMQ           DF                 I                  CJMNQR             BQT                ALS                A                  BCIJNO             C                  A                  BFNQT       
       DEFGHIKORT  	       DEGJKPRST          HO                 AFHJ               EOPQS              BHIMPQRT           BEJNT              DFKMQT      
       ABDEFGHJKS         JNPST              BDF                GH                 CFKL               CIKMNQ             EHKNPRS            DFIT               AR          
       ADEFGHJKNO  
       CDFHIMNPQR  
       ABFGJKNPQT         BT                 ADEHMNPT    	       DEGJKPRST   	       ABFGHJKST          FKMPS              R                  BDEJK              EH                 KQ                 ADKLS              DFJOP              KRS                BKN                FPR                FS                 BGIK               FHIPR              C                  EFKST              CEHJL              M                  KOR                BH                 BDFHIOPR           CJP                KOP                I           	       ACDEFGKNO   	       BCDFGLPST          BHILMST            O                  CS                 A                  ADGKT       	       AEFIJOPST          AEIKMNRT           C                  ACHKQT             CFHLT              Q                  GNQR               DLMNORS            AJ                 BDHILNR     
       BCDGHKLNOS         CDIORT             BEFGJOS            ACFGLNQS           DGJNP              BJKMS              FJKOPR      
       ABDGIJMNOT  
       BCEJKLPQRT         DLMO        	       ABDFGJQRS   	       BEJKLMOPS          AEFNPT             BHJLORST    
       CEFGHIJOQT         DQR                BI                 EFIO               ABCEL              AC                 F                  L                  S                  ACEFJMOS           ABEGLP             ELMQ        
       ABDFHKLPQR         RS                 O                  JN                 I                  I                  BKQ                EJPS               ABFIS              FL                 MQ          
       CEFGHIKLMR  
       DFIKLNPRST         BDGHNST            AQS                ABINORST           BFJMQT      	       BCFHIKQST   
       CDFGHINPQT  
       ACEHILMOST         GM                 EIR                IJOQ        	       BDFGHIKRS   
       AFGILMOQRT         EHIJKMS     
       ABCGHINOPT  	       ABDIJLNPQ          CEK                BIR                GNOT               CMNQ               CEFMNQ      	       ABFHKLNRS   
       BFGLMNOQRS         BJR                CIM                ABEGJRS            HK          	       ABDFHIOQT          AEFRT              FGJOR              CKLPRS             C                  BI                 AQS                IL                 IKS                ACNPRS             EFMNP              EI                 L           	       CGHJLMORT          D                  GL                 CDLOPRST           CDILMORS           JKL                HP          	       BDGHIJOPR          JLT                ABCFGKMT    	       ACEFGJKLN   	       CFGHJKLOQ          P                  DFINOQ             CGHR        
       AFGHIKNPQR         DFHKLMP            GKOPQ              BCFGHMST           BILM               DEGIMRS            ABJKLORT           R                  BGJMNRST           LOQS        	       ABDGJLPQT          EGIMOPT     	       ACDGIJLNO          AEGHJKS     
       ABDEHJKLNS         FHK                ACJ                FP                 DFGHJST            ABCDGRS            CJMPRT             ACDFMPQR           J                  DFGHPQ             DP                 ABQR               DEIQ               BCGIP              AEOR               ELQ                DFGLNQS     	       CEFHKLOPT   
       ABCGIKNPST         GIMS               CHKLOT      
       ABDFGHINQR         AD                 ABCDEGJL    
       ACDFHJKLPS         EP          
       AEFGHIKQRS         DIMQ               PQRS               G                  LN          
       BFGHIKLNRT         GIMOQ       	       CDGHIKQST          ABDEFKNO           BEJ                BCDH        	       ABFKMNOQR          GIJKLMPT    
       CEHIJKMNPQ         DFGHKLOQ           CGJT               H                  G                  FHKN        
       ABCFILMPRS         L                  HIK                KRT                AMNQ        
       ACDEFGJMNQ         HIJNPT             CDEKQ              CFGIJT             AHN                BGKNPQR            AJNQ               ABDPQRS     	       FGIKMOPRT          AINRS       	       ACEFGLMRS          DQR                AEGNPQS     
       BCDGIKMNQS         CLPRS              ABEHRS             FGLOQ              AEFGJ              HLR                DGHJLMPR           BGILPQT            E                  ADFHIOPR           ADILQ              AN                 DHJM        	       BDIKNORST          EGHIM              AFHKLR             CGIRST             FKLS               R                  DIJ                ADGIJKR     
       ACJKMNPQRS         AEKPQR      	       DFGHJKNOR          BEFMP       	       AFGHJKLNP          ADGHINOT    
       ADFGHJKLNQ         DS          	       ABCFGINST          EHJNOQT     	       BDEFHKMNP          FKR                KLQ                BFLQS              ACEGJLM            AHIMRT             EN          	       AHIKLQRST          Q                  O           	       EFHINPQRT          DGIL               BHNT               ADHILMNP           JKO         	       ADFHKMNPS          NQ                 ABIQ        
       CGHJKMNRST         CGHMOR             DEFIO              KP                 AHIJLMOS           AMO                ADEFHKPR           ACEPS              D           	       ADEHKLPRT          AEFHIJNS    
       ACFGHIKMNQ         BE                 F                  HNO         	       CGHILMNOR          ABP                BDGIMR             CGHIJNOP           GLO         	       ABDEFILPS   
       ABDEFIJKMO         EP                 DOPQRST            NQS                BDFGHPT     	       CEFJLMQRT          CHILMOPT           JK          	       BDEFHIJPQ   
       ACDEFGJLOS         ACI                MR                 ABFIKLPT    	       ABCEFHLPR   
       BCDEFHJLNP         EFHJMP      	       BCDHIJPQT          LQ          	       BCGJLNRST          PS          
       ABELMNOQST         AGMQ               BCDEJMN            CEJ         
       AFGIKMNOPQ         BFGIJQS     	       BCDIJNPQT          E                  HT                 CJ                 DFIJKL             CFGHIKPS           ABGNQS             DEFGIQRS           DEMPT              AGHR               ACIJOPQS           BHMST              J                  BDGINOS            DHQ                CDFGLMOQ           AGJK               ACFQS              BEIN               DP                 DFMQ               BEFGHJKM           H                  BCFJNS      	       ADEGHKNOQ          ABEFHO             JRS                GJST               CEGHLR             B                  FGHINR             ERST               AFLNR              L                  AEGKLNRT    	       BCDFGKOPS          DN          	       CEGHMOQST   	       BCEGHKMPT   	       BCEIKNPQT          CDKOP              ABEIM              ABCNQ              PT          	       EFGHIJNOT   	       CHIKLMNRS          BEFGJRT     	       BCFHJKNOT   	       BCHIKMOQT          EFHN        	       BDEGLMNOP          BGIKLQ             N                  BDGJKLMO           CEFIJKLR           ABKLPT             CEGHJMOP           KLNQT              FHR         	       AEGIJMORT   
       ACDEGHIORS         KLMO        
       ACDFILNOPR         BEFGKNQT           M                  FGQR               AEFNOQT     	       ACDGJMOQT   
       ABDKMNPQRS         BG                 BDGJNPQR           EGHMNOP            IJ                 HL                 BCFGHKS            BCEFGORT           Q           	       ABEILMOPT          FQ                 DGIJ               F                  L                  F                  ADJLNPQS           A                  AFGIKNQ            BDGNOPQT           CEIJKNQR           K                  IJ                 S                  BCL                EFKNRST     
       DEFGJKMNST  	       AEFGIMQRS          BCDHJLR            COPRS              BCDHMST            BILPT              DFGHIPST           AHKMOQ             ADIJNQT            ABHJKL             NOR                DQ                 ACEIMNP            GIN                EK                 FJLMS              IJ                 HIOQS              AEFJ               ADHIPS      
       ABCDGHMNRT  
       ABDFHNOPQR         EFKMQ              AGHLOQ             DHIJLMOQ           ABDFJNQ            D           	       ABCDIKLNT          ACFILOPR    
       CDEJLNPQST         F                  ACJKP       
       ABEJLMNPQR         ABDEHIS            CDGKMPS            AB                 BEJR               DHT                ACGIKNT            AGHLMPQR           DEN                ACT                DFHJKPS            BRS                AHR                NR                 DH          	       CFIMNOQRS          HL                 JMR                BDEFKLMS           HJMNO              EGILPR             G                  BFM         	       FGHLNORST          S                  ABDFJMQT           O           	       AEGHIKNOT          OPR                N                  AIST               AFGKMNP            ABDGORT            BFIKMOST           CEHIKMO            AEFHO              EGIT               AIKP        	       ADEFKLOST          ADIJM              BFO                K           
       BEFGJMNOPR  
       AEGIJKLQRS         ADGILNOS    	       ACDJMQRST          BF                 EMNPS              JORST              ABFILMRT           T                  L                  ABGM               CDEFJQT            HI          	       ABJKLMNOR          C                  EINP        
       ACDEHIJMOR         G                  AMRS               GJKMPR             DFIKMORT           BDENO       	       ABEFHIKQS          AFIJOP             PR                 BK                 BILMOPQS           DEIJLMOQ    
       BDFGHJLMNQ         IMT                R           	       BFIJLMPRT          GM                 AELMNST     	       ABCEFHOQT   
       ADHIMOQRST         BGJKQ       
       BEILMNPQST         FJLOP              ABEGLP             ADOQRT      
       ACDGHJLQRS         EILP        	       EFIJLMNPS          PQS                QRS                EFNPS              AJNOS              MS                 ADGLMRT     
       BDFKOPQRST         ABDHKOP     	       BEFGKMOQR          JRT                EKS                CDHJKPR            GLMN               MS                 CDIP               CEFHNR             EHJNT              IT                 CGMPQS      	       ABGHIJLNT          BS          
       BEFHIJKNOQ         BEFIKP             BILQST             JQRS               FJKQS              BJRT               CEHNP              CFQ         
       ACDEJKLNRS         D                  BHIMNS             BHQS        	       DHIMNPQRT          AEIK               EILMOPQR           CDEGIKNS           FJ                 CDGPQ              DHKOPR             D                  EGHJMOS            KO                 ACEGKT      	       BCDGKLOQT          BEF                ACDELM             ABHJKLOS           GK                 BFHILP      	       BFGHJNOPQ          HL                 CLM                ABFHKL             BDELMPRS    	       ABCGIJLST          BIRT               N                  G                  CEFGJKNP           EGIKLNT            BDEINOT            AR                 DFGILMOP    	       BCEHKLQRS          EMNOT              ACGHLPQT           N                  ES                 DHIMS              CEFO               PT                 CDOQ               GK                 ACEHKS             H                  C                  KMNR               BEGJLO             CEHJKR             BEFLMNS            S                  BRT                EGJKNOP            CEFGIST            CO                 CHJQR       
       AEFHKLNPRT         BGILPQT            GIKO               DEFIMS             HOQ                AKL         	       BCIKMNPRS   	       EGIJKLOST          ACFMNOPR           F                  CDGHL              BIO         
       ADELMNOPST         BLS         
       BCHIJLNOST         ANP         
       ACDFHILMNQ         ABCEGMN            CHLNT       	       BDEHKMNOQ   
       ABDEFHIMOP         ADLMR              CFIMPS             DGO                BHKL        	       FJKLMOPQT          ABHST              EGHK               M                  AFHLRT             DGIKPR             C                  FIK                H                  ACEGHJ             EPQS               HIOS               BIJKMNP     
       BCGLMNOPRS         GHIOR              BCDJO              IJNST              BHLMNPST           CEQT               GHNS               EGJLMS      
       ABCDKMOPQS         G                  DEGKLPS            AHIQST             BGJKOQRT           BPR                DFMQST             LM                 EFIPQ              CF                 A                  JKST               AFPS        
       BDFHIJQRST         BE                 FHMN        	       BCGHIKLMQ   	       CEGHJKLOT   	       BCEFIJKST          BDHJKLR            GJKP               EPT                AFGHO       
       ABEFGINQST         FMNOQRT            EKLMOT      	       BCDEHMOQS   	       EFHJKLPRT          ABHIKQR     	       BCHIJMNPS          NST                DEKOPQS            BG                 CKQ         
       BDEGKLMOQR         DGIJKMP            BDEFIMPR           ACEHKOS            HPQT               CGOP               BFOQT              ACFINOS            CGPT        	       ACDFIKLRS          ABEGHJOQ           CFQ                H                  AEKNPR             GQ                 BDMORT             KR                 AEHKT              MOPQ               CEGPQ              HOP                BLMOQT             JS                 ABCEFR             CDK                CHI                AEFKQT             CDL                BEMPRT      
       CDEHILMNPQ         GJKLT              EHOQ        	       DFGHIKLOS          AHOST              ADEFHNP            BDINS       	       DFHILNPST          BCJNO              T                  EJM                CKPR               J                  ADGR               M                  CFHO               BIJKMPS            AFJLM              F                  BCFGNOPS           EIJT        
       BDFHILNOQT         DFHLOS             FGKOPT             AGLQ        	       CDGILNRST          FHIMQ              FS                 DGJMNOPQ           O                  AKN         	       BDEFHILQR   
       ACDEFGHJQT         FS                 G                  FHNT               ENOPQ              BMO                MR                 AEHJMNT            ACELP              AEFKLNPQ           BF                 ABCIKLPS           ACDFLMST           ACFMPQRS    
       ABDFHJKMRT         BGHJ               AEPT               CGHMQR      
       CDIJKLOQST         DFGINRS            EFKR               CFIKLMRT    
       DEFGJKMPST  
       ADEFLMNPRS  
       ADGHIJLMQT         DFIJLPRS    
       ACFJKLMOPR         ADFR               CMRT               BFIKNP             ADFGHLM            ADFJLMOR           CGHILOQS           T           	       DFHJKLORS          CIO                EMNQRT      
       EFJKMOPQST         BFIMNOPQ           AGLORS             KT                 GHIMO       	       AFGJLMOPR          O                  CR                 CMT         	       DHIJNOQRS          EGOS               J                  AER                DJM                DEIMRT             BMT                BFIR               BCLPQS             FGHOT              DGMO               C           	       ABCDHIPQT          N                  ACJT        
       BCGJKLORST  	       ADEGHJKRT          GHMQST             BFGJOPRS           FT                 BEHJNOST           PS                 IJK                T                  DHJPT              ABFIPQRS           AEF                AKPQRS             KN                 BCMNORS     
       BDEIMOPRST         Q                  HIQR               BGKMOPQS    
       EFGHKLNPQS  	       AEFGHLMPT   	       ADFHIOPST   	       BCEFGJNOR          DHINO              E                  BCJMNPQT           DGHMNOR            QT                 CKPQ        
       EFGHJKLOQT         FGKLP              ACNO        	       DEGIKLMNT          ADEHOPQT           L                  FGJOS              CGIKOP      	       ACHIMNORS          GKM                ALMNOPRS           DHIKLORS           BEFHJMPS    
       ADFHJLNOPR         CDGNS              OP                 BCEFGIJS           BDHS               BIJKLNO     
       BCFGHJMPQS         FHN                EMOPT              CDJKLORT           JP                 E                  CET                CFOQ               JKMN               BEFIMNOS           KM                 DFOPR              EKLQT              OQS                G                  BQS                E                  G                  GL                 FKO                CEFMS              BCDLNRST           CDPQT              GO          
       ABDEGJPQRT  
       ACDIKOQRST  	       BCDHKNQRS          AS                 B                  GHIQT              AEKLMPRT           AM                 BEGINOQR           GJNQR              FNR         	       BFIKLNOQS          AFN                ACFHP       	       ABDHNPQRS          M                  AFHJOP             BDHNOQST           GHJP               DJ                 ALNOP       
       CDEKLMOQST         BFK                DL                 CEHNPR             CO                 ACHIKLOP           CDFGHJNT           AFJOT              L           
       ABCDGHJRST         DEJMPT             AELMNO             ABFHLPS     
       ACDGIJLMPQ         EGIP               ACJP               DEJKQRS     	       DEGILORST   	       ABDGHJKPS          ACHKLPRT           ALT                IKPQT       
       ADFGHKPRST  
       ABDEHIPQRT         GMNR               BDEFLO             BDR                GK                 EJNPQ              R                  AF                 FI          	       ABCFIKLQT          ELS                CNO                AEGHMOQT           ABCDEMOQ           BFQR        	       CDFGHIJNO   
       ABCFIJKMPS         N                  BDL                BGNPS              FKNP        
       ABDEGIJKNP         IOPQ        
       DEFGHIJOPS         ADFIJQST           BIJLMQT            P                  FJKLO              ACHLOPRS           AIRS               DGNQS              BILN               BHNQST             JKNQT              S                  PQT                BHQ                DIMNQS      	       BDFHKPQRT          Q                  CHS                AFGLMPQT           ILP         
       EFIMNOPQST         F           	       BDEFKMNOS          AR          
       BEGIJNOPQT         HR                 AEFGMNQR           DOQST              CEQ                O                  BDFHNPQT           S                  KNQR               HILMPQ      	       BDGJLMNPT   	       EFGHIJNOQ   
       ACGHIMPQRS         GJNQ               H                  CHIN               ACIMNQ             ABCHJOPR           F                  DEGHIKNS           C                  ADFGJ              P                  LP                 BIOP               IS                 N           
       BCEFIKNQST         HQT         	       EGIJLMNOR          ACEFHQRS           HIR                DFJ                CGLS               BGJOT              CDEFO       	       ABEHIKLST          AE          
       ACDHKMOPRT         ALRT               BDFLS              ABEF               DL                 EMQ                O                  FGILMOP     
       CDEFHJKNRT         FGI                F                  NP                 ACFPQT             AGHMN              DFMNOQT     
       ACDHIJLNOP         ACGHJQT     	       BCEFHIMOS          HPT         	       ABDFHJKMO          DE                 CEKMNQRT           J           
       ABCEGINPRT         AIJMORST           R                  CJLMN       	       BGIKLQRST   	       BDFGIKMNT          CDEFR              EFJLPS             CGILOQS     	       HIJKNPQST          F                  CJLP               BEKLT       	       ADEGHIMPQ          CFHILQ             AGHKQ              CEFGKRT            EFJOQRT            ABDGLMOS           BCDFLRS     
       CEGHKLMOPQ  
       BGHIKLNPQS         BGJP               AFGHNPQ            DIMNPT             ABDJQRST           HJKNQST            BMP                DLNOS       
       ACEGHIJKRT  	       CDHKNOPQS          CDLNRT      	       ADGIJKORT          BNOP               AFHIKLST           ABDEFMNQ    
       FHJKLMOQST  	       ACHLNPQRS          EFGHJK             OR                 BDEJKORT    
       ABCDEJMOQT         MNOR               BCEFMQST    
       ABCDEFIJLO         M                  JMN                CDFKMQT            S                  CKST               CHLPQST            CFGNOPQ            ABCFJMNQ    
       AEGILNOQRS         GQ                 EIJLQ              ACELMST     	       ACHIJKPQS          FHJKMNT            BFGIJLRT           I                  DIKLRT             AEFJT              EGJKLMPS           ADJL               BHN                BKNO               BFHLNOT            ET                 BCHJR       	       ACDFKNOPQ          HIJOQ              ABEGIQRT    	       ACEFJKMQR          H                  DEIKMP             JOQ         
       BCGIKLNPRT  
       ABCEKMNPST         ABCGHNOP           ABH                BDEFHLM            ADFGKPT            CJLNRST     
       DEGHKMOPRS         EF                 EJLQS              AI          	       DGHKMOPQT          ABFHILPQ           CDEP               J                  BKT                ACGLN              L           	       AEGMOPQST   	       DEJMOPQRT          DEFJKLNQ           CEHNOPQT           BGHJLO             CDGLPR             EGHNQ       	       ABCDGIKLR          ABCEFGIS           AHKLMOS            CIJOR       	       ABDEGHKPQ          CGQ         
       ACDGMNPQRS         D                  I                  EI                 AJ          	       FGHJLMOQR   	       FHILMNQRT          N                  ACILNP             AIK                ENS                CL                 BFIKLNOR    
       CDFIJKLMNT         Q                  T                  KNT                BCFJKLOP           GHR                DHR                AGHKNPT     	       ABFGKNOQR   	       ABCDGIJMR   	       ADEFGJMNO          P                  BCEJNOS            DGOPT              GK                 AGJKOST            BDELQS             DEHIKLQR           KLMO               EKS         
       ABCDEFMNOQ         EILM        	       ABGMNPRST          CFIMNR             S           
       ACFHIJKNPQ         KNS                T                  FGMST              CEQ                NO                 ACDQ               DGP                IJST               G                  BEHLMNP            IPT         
       ABFHIJLOPQ         BKOPQST            F           	       ACDJKLMOQ          BCGI        	       BDEFJMOPS          ABCFHI             FLMNQRST           C                  L                  CLMQT              EHJLNQ             J                  BDJK               CHKLPRST           DEOQ               ADFKORST           AEFGINR            T           	       ABEGIJKLT          J           	       ACGIJLMNO   
       BDGHIJNOPS         ACFS               BEFO               CDHJPR             O                  CDEHJL             GM          	       AHIKLMNQT          KO                 IM                 BDLNPQT            CEIJ        
       ABDEFIKLQS  
       BHILMNOPQR         CIQS               ABFGHT             E                  BCEFGINS    	       ABDFGIJPQ          J                  CJMRT              AHLMQST            BJKMNPT            A                  FHJO               FGHIJQR            DEFOS              BDIMNS             CDNOR       
       BCEFHJMNPS         BFIMRT             KS                 GJT                BEFO        	       BCEFGIJLT          DJNOR       	       BIJKMPRST          BHJO               JQS                CDEGJRT            ACELMPR            AEIJL              FHOPRT             ADGIMNOQ           DS                 AGHLOP             CDFGJNO            FQ                 GLOR        	       BCDFGHLNR          HMNQS       	       ABEHIOPRS          CHIKLQRT    
       CDEFILMOPR         CEFKLMNO           CHMST              ADP                JQST        
       BCGMOPQRST         AMPST              BCGLQ              ACGIJLMS           AFH                BCEFIJMR           ABCHJKLN           BGIS               CEG                BCEGRS             BCDGHPRT    
       ACIJKNOQST         NR                 CGIJR              M                  BHKLQR             GKNQST      
       ACDHJKOQST         ADEMNOQ            AJM                P                  DH                 BJLMOT             AHP                ADFLT       
       BEFHIJKLMQ         BCGJMOP            BEIM               ABEHJMNP           EJ                 B                  M                  CDEFGJMN           DEFINQS     
       BCFHIJLNQS         R                  M                  EL                 EHN                ABHJNQRS           BPRS               FILMQR             BCEKNR      	       ACDLMNPST   	       ABCFJLNOP          ALOQ               LT                 BFJO               JMNPQR             AHJQS              GHJOR              CDJKLMOR           DGJKP              CDEGIJOR    
       ABCDFJLMPS         GKP                JLOS        
       BDEGHIJLNT         ABCHJOPR           EFGJKMN            DFILMNRS           ILOQ               ILOQ               ACDEFPR            GHIP        
       ABEFHLMOPS  
       ABDEGHLNRT         CFIJ               EIQ                AD                 AFMPQT             CLMQR              BJR                GS                 CGIJKPS            ABDEST             DILOPRT            DH          	       DGIKNPRST          DIOPRST            GIQS               CJQT               GIJLO              ABFGKLN            S                  R                  DJQT               DFNO               GQ          	       EFGHJLPRT          D           
       CEGKLNOPST         ACEJLN             N           	       BCDFGHLPT          C                  CEHJKLOR           DEFGIJP     
       AEGHIJKMPS         CK                 CEFGMS             AJR                CEFIJNST           BILPS              EF                 L                  ABDFT              Q                  C                  DEHIPQRS           BFJKL              KRS         	       BFGKNPQRS          BJKT               FQ                 BCDOQ       
       DGHIJMNOQR         BI                 ABCFILN            AFHLOQR     
       CEILMNPRST         IMNPRS             BNQ                LMOS        	       CGIKNOQRS   	       BCDHLNRST          BGHOS              BOP                R           	       AEFIJLNOR          DFGKLRT            GHM                F                  L                  ENP                GILNQ              ELQRS              BDIN               O                  G                  CFST               C                  AEFIOPR            BEIJPQR            AIK                CKM                E                  JKN                FHJLMNS            AFHIMPT            FJ                 AFJLMT             OQ                 HS          
       ABCEGHINPR  	       BDJLOPQST          CD                 T                  EP                 CHQRS              BE          
       ACDFGIKQST         FGIJPS             ACKPQST            EHOPR       	       ABEHINQRS   	       ADFHKPQRS          HKL                B                  K                  R           
       DEFKMNOQRT         ADEIKMQ            DH                 EHJMP              IRT                ALP                GNO                AEGJPT             IJMN               FHL                EIO                FGT                N           	       EFGJNOPRT   
       DFGHJMNPQT         F                  EGPRT              EFMN               BCE                CEFNOPQT           EGKO               I                  DT                 ADEFNPR            BHLPT              BDGOQST            BLM         
       AEKLMNOPRT         N                  CFHJNT             ACER               BCDEIMST           BDFM               BCDEFILQ           EQ                 F           	       CDGIJKMPT          BEP                FGIPT              FS                 ABEFLMS     
       BDEIKLNOQT         BDLNST             CEJMNOQT           CDGMOR      	       BDEGIMOQT          BT                 BCDFJKNP           ADFHJKNR           BFIJKP             CEHOPT             IR                 DGIR               AFR         
       DFGHJKLPQS         I                  DEFJKNQT           K                  BN                 AJKS               LMNQ               BFJL               EFHJKOR     	       BEIJLNQRS   	       GHJKMOPQT          BDLQRT             BCFJQR             AIPST       
       CDFHLNPQST  	       ABEGLOQRT   	       ABEIJLMNP          EHMPT       	       GHIJMOPRT   
       BEFIKLOPQR         DHNO               GHIMNPQS           EGP                GHM                R                  AEFJOR             DEHQ               BFGJLOT            GILS               DJKMN              ABGHOP             H                  NR                 DIJLMS             BDEGOQS            ABDQ               GLNPQ              BEFK               K                  LS                 AELNOPT            AGHRT              DEO                PQ          	       ADFHILQRS          FGKM               CNR                A                  AR                 G                  ACGLOPQT    
       CEFHKLNRST         EIKLMPR            JNOP               IPR                C           	       CEFHJKLNR          BEGJKOQR           BEFHJQT            DGHLNQ             DHILQ              AFHOQRS            JKNP               FKS                DIO                CIJ                CEMNQRT            CMPQ               KMR                EFM         
       CEGHIJOPQS         BEFLMNPR    	       ABCGIKLRT          ABIMS              B           
       CDFIJKMQST         BDEHJLPR           BGHIK              CHJLQ              ABFJT       	       GHIJKMQRT   
       BDEGJNORST         CGHLMNOP           HLN         	       ABDIKNORT   	       CGHJLMNOS   	       BCEGHIMOP   	       ABFGHJNPR          T           
       CDFGHLMOPR         EFHIKQT            ABEHK              AGL                S           	       ACDFJPQRT          DHLPT       	       BFGKLNQST          DFJNS              DHIJLNPR           H           	       ABHKLNPQS          EFGHJS             O                  FHKLMO             AGHIJKNQ           NO                 BJ                 BDGIJMT            EHJPQ              IM                 BJMN               BCHQRT             EIKOS              GHMNORT     
       BDEFHIJMRT         CDHKMN             GI                 IKOQT              AGILNOQ            DGJR        
       AFGJKNORST         DELNPQS            BFJKLP             E                  LMNQR       
       ACDFGJLOQS         NOPQS              DIQ                E                  DIQ                BEGJKMT            EHNT               BFGJNO             OT                 BGHKOQRS    
       BDHKMNPQRT         C           	       ABCFGILMR          ELOQ               FIKMT              FGIJKNOR           BMP         	       BCEFIMNQT          BEGKLNQS           DFHIKMN            I                  DGPR               T                  BDFHKMN            ABEGKT             JPR                G                  GJO                PRS                H           	       ACDEJMNOR   	       ABGHIPQRT          MP                 EP          	       CEFGHORST   	       ACDFGILNP          EIJOQT             GHKNQST            MQ                 CJST               ADFHJLOP    	       FGIKLOQST   	       BCEGHIMQR   	       DEKLMNQRT          C                  ABHNR       
       EFGHJKNOQT         ABCLMP             ACDGLQS            AK                 BDE                CDFIJ              BDHKOPQT    	       FGHIJLNOQ          EQ                 CK                 H                  DEGHILOQ           COR                ABJMNP             EHJPS              BCFIK       
       ACGIKMOPQS         MNT                L                  BCKLOQ             EGK                B                  M                  CDPQST             HJNOST             BCGHLMOQ           CIQS               ABEKLNRS    	       FHIJMOQRT          ACDEHKNQ           DGJQ               ACLOT              BCDHILO     
       ADEFGLMQST         KNST               JN                 BEFJMO             FPQ         	       ABEFJLNQT          GKQS               CELMP              ABDEFLMO           CDHIKNQS           JMOR               ABDJORT            ABCEFMPQ           IJKM               I                  BCEKQT             IJPS               ADEFINPS           BCGP               BFGMQS             ABEJLNOQ           ADFJOS             ADIKL       	       CGJKLNOPT          Q                  A                  IJM                CK          	       CEGIKMNOS          ABJN               IM                 ABDIKOPQ    
       BCEGHIJMPQ         BGHIOPT            CFHOQRS     
       AHJLMNOQRS         GJL                DEFHINQ            HIKM               GLS                AKLT               EFGMPS             EFHKLNPS           FGIQS       
       ABDEHIJMNR         AO          
       ADEFHNOPQS         BDJL               ABJKNPQT           CDEFGIP            BLMPST             DEKLOT             CHQ                AEF         
       ACDEGIMNQT         DJQS               DGHJQRS            AKPQ               BCDELNR            R                  BFT                GHPQ               BJOPQ       	       CEFHKNPQT          AFG                ACDENRS            CDFL               KQT                BHNR        	       GJKLMNORS   
       ACDGJKLMPQ         LMOPQ              AFHT               DHIJLT             EHJKMQ             AIMT               BCDFNOT            AHIKMNPR           C                  EGJLMNOP           AGRS               H           
       FHIKLMPQRT         ABDEFLT            FNP                IMP                BCFJKNQR           EJPR               FMR         	       DFGKNOQRT          DEIKQT             DEHJKOPQ           BCDIMOS            CHJRS              CJL                CHLQS              DGLNQT             KS                 BIJLMOQ            FJKO               AKNPS              A           	       ABFGHNOPT          ADFGJNR            FOPRT       
       BCEHLMNQRT         EIJKMNOT           BEHLM              C                  BT                 AKRS               BEFGKLOQ    	       BDFGHIJLQ          EKMPST             GHIJLMO            AELR        	       AFILMNORS          CEGJKQ             ADNT               BCDGHMQT    
       DFIJKLOPQT  
       ABCFGIKMRT         FGJMOS             GPRT               AF                 CLOQS              GHQRS              CFGRS              AC                 ABDILPRT           MQ                 AJLNOP             BDQ                BDKM               L           	       ADEHLMPRS          B           
       DEFGKLNPQS         EINOPS             DHQS               N                  DFIO               CLQR               AGKNO              FJNT               BDEHINQ            BGJQR              BDEL               ILMNT              G                  AFJM               AJKO        	       ADFHKOQRS          A                  N                  DEFLPRS            GQT                DEFKLNPR           IN          
       DEGHKMNOPQ         AN                 FJM                JK                 DEFKNQRS           I                  BGKLMRT            CFHLMOQR           IKQS               AL          
       ABCEFIKLPT         HKQRS              BG          
       BDFGHLNQRS         FGIKLOP     	       ADGHJKLMR          GK                 ABDEHKLM           R                  IS          
       EFGHJKLNRS         CHINOPRT           CDJOPQR     
       ABEGHIJMOT         CGN                HINR               BDHNQRST    
       BCEHIKLOST  
       BCGHIJOQRT         CNOPR              ADEHMS             BHPQ               AJMQR              CDEGILPT           B           	       BCFJKLMOR          AGMOR              CL          
       ADEGHIJNPR         HKQR        
       ABCDEJKQRS  
       ABDFIJLPRS         DHLP               BDFORST            AP                 ABIKPQR            CFGHJKLN           ABIPRT             DFGIJKN            DHNPT              EJR                A                  CH                 G                  MQ                 CDLNOQT            ACGHIN      
       CEFJKLNOPR  	       ACDLMNORT          AJMOPRS            ADEFIO             IJL         	       ABHIJLOQT          GR                 R                  GJL         	       BCDFKMOPT          P                  A                  ILS                CFGKNO             DHIMNS             KQ                 BJST               ACFIJKL     	       BFMNOPQST          ABFHIJP            GHIK               FIMNQRT            GIMR        
       EHJKLMNOQR  	       BGJNOPRST          EFIMP              GJMRT              IQ                 DFHRS              ABLP        
       FGIJLMOQST         C                  CDEFS              CHLO               DT          	       BFHMNOPST          RST                FI                 BQ                 ABEFIJLN           AOR                CEHIJMQT           GNOQ               GS          
       DFGHKLNPST  
       ABEHLMOPQS         CLM                BCOT        
       ADFGHLNQST  	       ACGHJKLPS          BHJLOP             ADEGJKPQ           EGHJMP             BEFGRT             HLOPQT             DEHKLOS            BDEJKR      	       DFKMNOPST          FINP               CDGJLMNQ           MOT                AR                 B           	       EIKLMNQST          LQR         	       ABDGIJNST          BIJKPQST           FNO                CHLP               BCL                EGIJOPT            AB          	       DEIKLOQRS   	       ABCDGHNOR          BH          	       ACFHMNORS   
       ABEFHIKNQS         N           	       BCGJMORST          I                  EOP         
       ABDEFIJNPR         BDGMNP             NOQ                ADGIJMPR    
       ABDGKMOPST         BCDGLR             EO                 O                  CT                 EGM                GIL                CDJKS              ABCGJKOR           ABD         
       BDIJMNPQRS  	       BEFKNOQST          J                  AF                 BH                 ADEMNQRS           BCDEHKNO    	       ADFGJMNPS          ABFGPQT     	       AFGJKLMST          ADFNPST            MQ                 ACHLOT      	       BFIJKMNQS   
       BDFGHKNOPR         BEK                R           
       ABKLMNPQRS         BDIMNQ             BFGLST             ABEGHLS     
       ACDFHLNOQR         ACIK               CGS                CHP                C                  DLMT        
       BDEFGJKMPS         LM                 DHT                AH                 BDEHMN             M           
       ABDFHIJMNR         DH                 BCGILMN            BCHKN              CGLQRT             EFJLM              M                  GR          
       AEGHJMNQRS         BG                 HLP                HJP         
       ACDEHINORT  	       ABDGJKMPR   
       CDGLMNORST         D                  DNQ                ABR                MR          	       ACGIKLPQR          BDJKPS             BEKQ               ACM         	       DGIJLMNOQ          CDILMNRS           DJNORS             DFJMQRT            C           
       BCDHIKLPQR         N           
       ADEFOPQRST         ADEGIMS            BJMNQRS            ACDFHQT     	       EFHKNPQRT   	       BCEINOQST          IN                 CGN         	       ADGILOQST          CIJKQRS            GMOQR              GI                 BCDOQST     	       DFHIKNPQR          FR          	       DFGIJKLOP          CDFHIORT           LNQ                BEFGQR             EHJMNPT            A           
       ABCDEGILNT         IPT                L                  FJ                 ABFGHINS    
       ADEFKMNQST         EGPS               CE                 ADN                ADEFQS             ACFJOT             IJOR               BCFKLMN            GJ          	       ADEFJKNRT          FJMNOQRT           GKLMR              BGH                AIOQ               ACN                Q                  BDGMOR             ADHLPS      
       CDEFHJPRST         BIJL               EFLR               BDJOPQRT           T                  S           	       AEFJMNQRT          GIJPQT             AR                 IJKLM       	       BFGLNOPST          ACGI        
       ADGILMPQRS  
       BDFGHILNPS  	       ADFKNPQRT   
       ABDEHJKMOP  	       CEFGJNPQT          AEIKLMQ     	       ABCFGMQRT          CLMST              BDEJKLOR           ABK                BKQST              GHIKNOP     	       ABCEGHMPS          DGKLRS             BCDEHLPS    	       BDHIKMNPQ   	       ADGIJMOPS          BOT                IJLMPRS     
       ABDGHIKOPQ         EN                 EKS                BM                 AGHMQS             JM          
       CFHILOPQST         NRT                HKMORST            AE                 GIJO               O                  BCGJMT             ABHJ        
       BCEFIJLOPS         R                  BCEIJ       
       ADGJKLOQST         Q                  DE                 LPT                CK                 R                  HMNOPQ      
       CDFGHIJLOT         BEF                CDGIRS      	       AFGHJKNPT          ACDEHJ             FGIS               OQ                 BDEMQS             ABQ                BFKOPQ             BFOT               AJOR               GHPT               DJMP               JMNST              BFIOR              DGQS               A                  ACHIMNT     	       FGHILMNOP          BGHNOQ      
       BEGIMNOPRS         GJKMORS            O                  DEN                AN                 EGMO               DFHT               BCDE               AFLOQRS            B                  BCFJMNRS           AGHM               BCDHJKLS           GHN         
       AEHIKMNQRT         ABDEHMPT           ACFJKLMR           A                  ABGKLORS           L                  AGLR               ABGH               AIOP               BKT         	       BEGHIKNPR          BJS                FS                 CEFHINRT           BFKLPQS            GJPT               ABDEGOP            AHJKMO             CDINQ              FIJ         
       ACFGILMOQT         BCFGHMQ            IT          	       ABDGILOPS          BDEHIRS            ACGHJNPT           E                  S                  DKN                BGHJLMQT           CDHKNO             CFKMNOS            JT                 ABMP               EFIJLOQR           EFGIJOS            ABCFGLNQ           AI                 ABIRT              GKNOPRST           AFHKMST            AG                 EFILMNR     	       CFGHINPST          BCHP        	       AHJKLOPQS   	       BCJKLMRST   	       CDHIKMOPR          IS          	       BCEFGILMN          CGILMNRT           LMOP        	       BCDEJKLNS   
       ACFIKLMPQR  
       DEGHIKMNST  	       BDFGHNORS          IT                 HLN                BJMNPRS     
       CDEFHJOQRS         HIJPS       	       CDEGHJNST          BDGIJLM            CIM         	       ABDHKLQST          GNOT               BCDIT       
       ABCDFHIPQS         ACEJKRS            CDHMNP      	       BCLMNOPQS   	       CDIJLMOQR          A           
       ABFGKLMOPT         CHIMNT      	       ADEGHIJNP          PQS                DFGHIJKP           DGILS              BP          
       ADFHIKMOQT         DHKMN       
       BDFGMPQRST         CEMT               FMS                DLMNOPT            BDEHI              DHN                ABEJK              BIJMNS             AFINOPRT           S                  AFLOS              OS                 CDFGHINT           DEFMP              S                  GJR                CGLNRS             ABCFKPR     	       ACDKMNPRT   
       EGHLMNOQRS         FNQR               BFHMNQT            EIJR               P                  RT                 EFGJPT             EO          
       FHKMNOPRST         AGIJLOS            FIKNQS             BHIKPQS            BCDJKNS            DHPRT              EFJRS              KLOR               N                  CD                 S                  CFJ                AFHIK              BDHILR             BEFGHKLO           BKOPRST            DJST               QT                 BIMNOP             CFJT               BHKLMO             KLMQR              DMOPR       
       CFHIJLNOQS         EGL                CEGHIJNP           CJS         
       DGHILMNORT         JS                 BPR         
       BCEGHJMOPQ         AFGNPQS            BEGIJST     	       ABEFGHJST          BJOP        
       BDEFIKLMOT         ACGJKMT     
       EGIJLMNOPS         DOQT               BF                 AGIJQ              A                  FIKT               ACFGINOQ           CM          	       BHIJKLMOT          EFIKMT             P                  JKLQS       	       FIKNOPQRT          DIJKNQS            NPT                L                  DGS                ABFGHRS            GMOPQ              BCFIKLNO           KMQS               ABKS               GT                 HILMPR      
       ADEFGHINPR         FHKLNQ      	       CEJKLNQRT          JKP                GHLPS              AHIKMPRS           B                  CKLMNRT            GM                 KS                 EOR                M                  ABS                NO                 ALR                S           
       BFJLMOPQRS         DGIJMNST           CFHJMOST           ADLMNPT            BG                 FIO         
       BDGHIJNPST         AP                 BLNO               FG                 BHKMN              R                  AFJKMNQS    	       BCEFGLOST   	       ABFGJMNPQ          ABEGIMRT           ENQT               AGJS               GJQ                CJNOQ              CF                 M                  ABDEGK             CEK                DMR                DEG                CFRT               AMQT        	       ABCEFLMQS          LMNQRS             ELR                ACEKMP             F                  BDGKMQRT           EHN                GRT                BDHJLOQR           DFJPR       
       ABCDEFKMPT         H                  AGLST       	       BJKMNOPRS          IN                 AJKLPS             BEHIMT             FIR                HKRS               CGIOR              ACGJR              BHJQ               DGMOQRT            AEIR               DFGIMNQ     	       CDEFKNORS          FKLMQT             BDJKLMPT           HK                 ABDKPQS     	       CEFHKLORT   	       ACFIKNOPQ          EJMNPRS            H                  R                  FQ                 CHINR              D                  EINOPR             ADILMRT            HMOT               LN                 HLPRT              CIMOPQ             IJ          
       AEFGHIMNRS         MNQ                FJKOP       
       ADEGHINORS         ADHLQS             EQR         	       BDEFINPST          JL          
       ABEFHKLNOQ         ABDOPT      
       ABCFGHJMPQ         DGJKLNT     	       ABHIJNOPR   	       BEGHJMNPQ   	       CGHKMOPQR          FMO                HIJLPQR            CDGJMOQT           N           
       AEHJLOPQRT         BFHILPRS           BCEKO       	       ABCEGIKNS          BFGKNT             BFHLR              FKLST              ACEIJ              BPQRST             CDEJLMNP           O           	       CDEFGHMRT   	       ADEGHKLPT   	       CDFGJNPRT   
       ADFGLNOQRT         HN                 IJKMT              GK                 ACJO               E                  DEKMOPST           DEPS               J           	       BFGIKLPST   	       BCFGHJQRS          JLMO        	       CFIJMNQRT          HIN                EGHOS              DKQ                DFNRS              ALMQS              DFHILM             FOQ         	       ACDFGIOQS          PT                 CFJLMQRS    
       ADFGKMPQRT         GNOPT              CMPR        	       BEFGHLMPS          R                  DLPRS              ABCDEMNP           MNQT               GI                 BEFGIJNO           GLT                BNQ                N                  DO                 AE                 DKLMPT             ABGIJMOS           J                  AGOP               BDNR               BELN               DFGJNPST    
       BIJKLMOPRT         AGIKL              FMNOQR             L           	       ABDGHMNPR          ABCHIJMN    
       CDEFGHLMNO         CEHIQRST    
       BCEGHIMOPQ         HIN         	       BCFJMOQRT   	       CEGIMOPST          IS          
       ABDGINPQST  
       EFGIJMNQRS         CIKPQR             G           
       CEGHILMNOS         GKMR        
       ABCHLNOQST         ADNRT              GP          	       BCEFIMOQS          EGPQ               DKLMOPR            ABHIMOT            D           	       ABCDEFIKQ          I                  L                  INOT               BDLNOPQR    	       DGHIKLMQT   	       AEKLMPQRS          IT                 DJOQST      
       ABDGILNOPQ         BNOT        	       BDEGMOPQT          CGHORST     	       DFGHIJLRS   	       ACFGHINQS          EFJLPQR            Q                  JM                 BJPQS              AC          	       BEFHIKMPR   	       DEHIJKLOR   
       AHJMNOPQST         Q                  BG                 I                  BCEJ        
       CFGHIKOQRT         CQ                 ACFJKNQ     
       ACDFHJLOPR         CGIQR              DEGMQS             CJKNS              BHJLNO             BDEGQT      	       BFHIKLQST          CDGHKMR            JM                 CJLPS              ABCIPS             DGMNPRS            R           	       BDHIKNQRT          EHMO        
       AEFHIJLMST         BGILR              D                  ACGS               EGINPS             N                  CEGHKOPT           EGIJKMPR    
       BEGIKNQRST         HJKOPQ             ACFHJNP            ART                M           
       ACEFGLNOQT         ACGJPRST           ABDFGQR            DEOQS              ABCJLMOS           O                  JLNRS       	       BGHIKMNOR          BFHNRT             K                  EHJKNOS     
       AEGHKNORST         P                  EI                 CHIJKMQS           KNT                ADEHPQR            CDGJKLPQ           DFIKMRS            IJ          
       ACDFGHKLNS         BHIOPT             JKNO               T                  DN                 CEHILMO     	       ABDEJMNQT          MS                 AEIMT       
       ADFGIJLMOP  	       BCGHIKLNR          HS          	       CEGHJKQRT          DGNOP       
       CFHIJLMNOQ  	       ACKLNPQST          GHMNO       
       CDFGHIKNPT         ACEGMT             EFGQR              BEFGHIKS           KLOR               EJLMOPQR           Q           
       BCDGHIMNST         CDGMQR             DELPQ              BFGHPRT            ACDIST      
       CDGIJKNORT         AEIKMNOR           IMS         	       BCHKLMORS          ABEFHIMR    
       ACDJLNPQRT  
       ADEFGJKNOT         AJKP               GHMT               BDGHIMOS           CHIMNQ             FGQT               ABFGOPS            JLN                CQT                AGNST              EGKLOQT            P                  HQ                 AIQ                JNOQR              BOR                CDFHT              ADKR               ACHILS             FQ                 FLOQ               BDOP               F                  CLN                EFKLNPRT           IN                 BCDJLMQS    
       BDFGHIMPQT         ABDLS              DIKLOPRS           AHIKMOPS    
       ABDFJMNORS         BGHQ               ADIKLMNO           CKNO               CHIOQST            F                  ACDGLMPS    	       DHIJKLMNO          IORT               L                  JKQ                EK                 BEKNORS            DEFHJOQS           C                  DHMNPS             E           
       ACDFHLMQST         DN                 ACDGJP      	       ABFIMNQRT          AH                 C                  A                  BIKMR              AHJKRT             HIJLMOT     	       DFGHIKLQT          FHMPR              CDHLMN             BFJNOP             BGHLO              CEFHL              CEGILMS            HPT                EGIKT              GIJKLNP            BHJLMNPT    
       DEFIJOPQST         DMQ         	       DEFHIKOQS          EHI                L                  DEGHILNQ           GR                 AGHLPRS            DHNOS              ADHQR              KNOPR              HJKMQ              HJK                DJ                 GMN                BCHIMRST           IJLMP       
       AEFHKLMRST         C                  CEKLMT             ABDKMN             BHIJNOST           DFHIPS             B                  DFLNOQS            ABFIJLPT    
       DEFGIJLPQT         BLN                ES          	       DGHJMNPRS          HLR                PT                 ACHJNP             BEFGJNOQ           D                  CL                 BG                 BEFJLPST           F           	       ABCEHNORS          CJLOS              DT                 ANS                IJL                AHLMORST           EFN                ABDEFHJL    	       GHIJKLMNT          CFJLP              ACFIKMOS    
       ABCGHJNOPT         FGHIPQR     	       BDFHKNPQT          AENOP              LM          	       CEFGHIMOP          EHNR               ACDFJOT            ADEHJMPR           DIKMO              DEIQS              EFHMOQ             ACKQ               HIMQ               DKLPST             AEJ                CFKPQS             GS                 Q                  BEHR               M                  BCDFIKMR           BMRS               FKLMPQ             AKMQ        	       ABFHJKPQT          JO                 R                  ABC                EGMS               DO          	       AEFGJMNOR          R                  BGMQ               GMS         	       CDJKNOQRT          BDHKQ              EJLPQRS            CFO                IJQ                ABEGJMO            BNQRT       	       BCEGLOPQS          ABHKN              BCEFHMPQ           FT                 O                  ADFJMPQR           ADFGJ       	       AEFHLNPQS          EHJM               CIKPQ       	       ACDFHKMOR          ACFGJK             JN                 C                  DOT         
       BCDEIKLOQS  
       DEGHJMNPRT  
       ACDJKLNORT         CDHJLMRT    
       ADFGHIKMNO         O                  N                  AKMQT              DEGKMQRT           AHJNOQS            DEGHKLMO           CHKS               EGIN               EHQT               CGJLOPQS           BFIJKS      	       ABDEFIKNQ          DEFGKQ             FHIKT              CDGMOP      	       ACFHIJQRT   
       ACDGHJMQST         JK                 AEHIKOQS           AEIKMT      
       BDFHIJLNQT         ABKNQRT            EGJPR              FIKLOPS            BDEIMN             IQ          	       AEFGHJMNP          DEGIJMO            ADFGHMQT           AGPST              M                  ACGHIJQT    
       BCDFKLOPQR         GR                 N                  BIKOP       
       ABGHJKOQRT         BDEGO              FG                 BDENOPQS    
       ACEGIJLMPR         BCDELM             ACIKNR             EFHNT       
       ABEGKLMORS  
       BCDGHIJKMN         ACEFMR             ABCGJKOQ           DHMR               CDGILN             D                  JKQST              DFILQ       	       BEGHKLNST          BHJNPQ             CFILNOT            ABDGINST           H           
       ACEJMNOPRT         BDEGO              F                  AHKPQRST           CDKOPRT            AFIKMT             DNQ                DKMOPQ             BLR                AHLMS              BFKNOT             CDEGHJPT           CLOP               E                  IP                 ABLORT             ADHIJK      
       AFJKLMNPQR         PS                 FG                 AHKMNOT            CJN                BHJNPS             OQ                 CNQ                AEMPQS      
       BDGINOPQST         GMNORS      
       BCDEJKLMPS  
       ABCDGJLQST         BCFGOST            GJLOT              EGHPS              CDKMS              CT                 ABC                CHOQT       
       AEHKLMOQRS         BCJMPQS            QR          	       AEFGHOPST          COR                FMOQRS             JMOPQ       	       ABCEHLNQS          BEL                GLNRST             BC          	       BCHJMNOPT          APT                BPQ                AKS         	       AEGNPQRST          AFHMNPR            EFO                K                  AER                BEIKPQS            GM                 ADLMP              AF                 CHN                CDEHKS             AEGJOQST           ABCGT              I                  DILP               DLNOR              FLMS               HJLMNQT            DFHJ               CILOQR             ACHIKM             DEKT               ACP                ACFKMOR     	       BCDNOPQST          BIJP               ADFGJKMQ           T                  HILQS       	       DEFIJKNPQ          BGJQT              BCIKMNOT           HJKN               AKMQT       
       CDEFGHJLNT         GQ                 AEFGJPT            BFGIKPQS           AEGJNOQR           CEKLMRT            ABFJKS             FR                 AEHLMNS            FHNOPS             F                  DGIJNRT            I           	       DEFGIKOPS   	       ABCGIKLNS   	       BDEFJNQST          L           
       BEFGHIJOPS         S                  BHIKNO             BCDFJLOR           HIKQ        	       BDFGHIOST          ACDLPQR            ABDHMOPQ    	       DFGIJORST   
       CDFGIJLQRT         BEFKMS             DGINOR      	       ABDGKMORT   	       BDIKLMQRS          ABDEHIK            BDHIJLNS    	       AEGIKNRST          BKLT               FKLST              ADIJMNO     
       ACDFJLMOST         ABCDGHPS           I                  ABDGMPRT           GHJKORS            EKNOS              BDEJR              BCFGHM             CGJNRS             S                  BEJP               CDGINOPQ           JNP         	       ACEGJLOPR          LR          	       DEFGHKMRS          ABHOPRST           AC                 CGMNOQRS           EJNO        
       ABCDFJMOQS         INR                BGIJLP             ELST               HKP                BKPRT              BFPR               F                  AKMRST             ABCKNQR            BKLNP       	       EHIJKNPRT          AIKLNOPS    	       ABCDFHIJQ          AFNS               CT                 ACFQ               DIJKLNR     	       CEFKOPQST          AGKM               FJLOR              FHKMOPQS           BFLQ        	       ABGHLOQST          ACEFLQS            EK                 K                  ACGIST      	       ABDHKLMNQ          AEIP               ACDFGLRS    	       CDFGIJOPR          LQT                CIJKLP             CDEGJLOP           HLOS               BCFGIJR            BEKM               BCDGOPRT           BCDEJKL            ACMS        	       CFGMNOPQR          BMNT               BCGKM              ABJPR              NR          
       BEGHJKNRST  
       ABCFGIKMRT         ABCKLOQT           CIMNQST            ANS                ACFHJNPS    
       BDFHKMOPQR         ABFOP              BDHMORS            BJMPQRST           T                  EFG                EGK                T           
       ACDEINOPRT         CGJOS       
       BCDEJKLMQT  
       DEGIJLOPRS         CDGILPS            CN          	       ABCDEFIOS          BDHIMOT            DFIPS              ACDIJKMQ           ADKLOS      
       CDEGILNOQS         CR                 DGI                CDIN               FQ                 ACEKMP             ABCLNOP            BCJS               MS                 EH                 OP                 CHOPR       	       BCEGKLMST          AGHQRT             LP          	       CDEHIJLNT          FMPRS              Q           	       CEGHKMOPQ   
       ACHIKLMNPQ  
       BDEFIJNQRS  
       ABFGIJKNQT         BIPS               IOT                AE                 CFIJKLMQ           CEIOPQ             CMO         	       FHJLMNPQS          CI                 BMQ         	       BEFIJLNOP          GJKMNP             GJN                ADFKQST            BO                 CDFHPQRS           HKNRS              FLQ                HRS                EGHIKRT            H                  CHOT               DEFGJLPT           CIJKO              DFHLPQRT    	       BCFHNPQRT          ACDEFORT           ABJPQ              FHOQS              ADGOQ              ABKPT              DQRT               G                  BIPRS              EKNQ               DEHIJLMO    
       AFGHKOPQRS         CDGHMNRS           DP                 BJL                AFILN              M                  BHIM        
       ABCHKOPQRS         ABCFHMPR           AEHMOT             BCEFMPT     
       AFGIJLNOQT         CFIKLQ             B           
       ADGHJKNPQR         BEFGKNPR           BC                 HO          	       ACFGKOQRT          ABEFHLNO           C                  O                  ABEFIQT            BH                 AHIKLMPS           ABDKLRT            AGJLNPQR           BFIJMT             GLMNPST            CEFHKPQS           ELNS               ABHJQS             DHI                DELNOPQT           N           
       ABCDGHKPRS         CLOT               JMT                PQS         	       ABGIJKOQS          E                  DEHT        
       BEFHJKLORT         H                  EFLMOPT            KMNOS              AEMOP       
       ABEHIMNQRT         ADJNQRT            T                  BCEFJMR            CEFIOPST           BDEHKPST           DM          
       DEGHIKNQRT         FMNOPRS            EFIT               DH          
       BCDEHILNST         ET                 MS                 H                  A                  ADEOPRST           KQ                 ACFKOQT            ABDGKMOQ           CDFIR              IOPR        	       DEGIKMOPT          G                  ACDEIKNS    	       ABDFHKQRT          EILOR              DFHINQT            ACGJK              AK                 D                  CR                 ACO                OT                 CFNRS              DLT                LN                 HLNORS      	       AEFGKMPQR          CJKMNQT            BCEST              CFN         
       BEHIKLOPQS         FGHJKNOR           BGNR               D                  ABDGLOPS           ABDEHOQ            BOR                ABGLNQ             EFGMRT             BCFHJLNR           EHJKLNP            GHR                CDHIOQ      
       AFGHJKLMNT         EJNOPS             R                  BCDHLPQS    	       BDEFGHOPQ          KLQR               BGIKMQT            T           	       BCGIOPQST          CDEFGNOT           BCFHIMOP           BJQ                AEHJLNR     	       AEHKNPQST          QR                 BI                 HIJT               CFILM       
       ACDEKLMNQR  
       ABCFHIKLMO         J                  C                  DFOS               ADGPST             ADHIJPQR           CDEGJKMT           G                  ACEGN       
       BDEGKOPQST         AEGNPQR     	       ABHIKOPRS          AFIO               DGKNO              BEGP        
       BCDEFHLNOP         DFK         
       ACFGJLNRST         G                  ACILMPT            ABP                JNT                P                  EGMP               J           	       ABFGHJLNS          BKORS              IJ                 BDEFKMNR           BJR                H           	       DEGHILNOS          FLNQ               KQ                 ADNOQ       	       ACDHJMNRS          I                  ENST               ACLNORS            HJT                BCIS               ALNQ               EFJKQST            ACDM               FGKLNP      
       ABCEHKMNRT         EMNOQ              CEHS               DI          
       ABCDHMNOPR         NT                 CHIMPQ             LPQS               BFIKMS             CFM         
       AEFIKLMOQR         J                  ABFIKLOP           ACHM        
       ACDGIJLNOR  
       BDEFHIJKNR         CI                 GIJQ               BGIORT      	       ABDEGHMOR          BDEGIJOR           P                  FKT                BIJOPQRS           C                  EN                 AFGHJKMS           CDHKPR             ACDEILPT    	       BCDFHJLQR          BDLM               AEGK        	       ABCHKMQST          ABHKPQ             D                  AE                 AFKMNOT            ABG                DGLNO              AEPRS              BHKOPT             DHIMNOQR    
       ABCDEGKMOS         G                  D                  BCGHN              FHOPQ              BCDEKOP     
       CFGHJLMNOT         J                  DGIKMT      	       ADEFJKOPQ          E           
       BCDEHIKMOS         BHIST       
       ABDEGIJKMR         ADEGQR             GHQ                BCELNT             BHJ                FHIJLNPR           K           
       BEFHJLOPRS         DGH                HJMOS              CEFJOPRT           FP          	       FJKLNOPQT          BDEGMOQS           BMP                BEHIKST            EFGJMPR            AC                 BGQT        	       CEFJLMNQS   
       BCDEGIJLOS         J           	       CDFILMOPT          KR                 FGHMS              LM                 T                  GT                 BGHKOQST           ACJMT              CGKT        	       EFGIKMPQS          BDFGIJLR           H                  FHJMOQ      
       ABCDEHJLOS  	       BEFGHKLOP          ADJLMRT            KLQ                CDJR               Q                  EKPS        
       CFGHJLQRST  
       AEFGIJMOST         ADFPQT      
       DEILNOQRST         BCEFILO     
       ACDFGIKNOS         EFHILOS     	       ABDEFKNPT          O                  ACGHIOPQ    
       BCDFHJKMST  	       BHJKLMNPS          ACFKMOP            BCFJKLST           HT                 DEHKQRST           AHOS               DNPRST             P           
       ABCEGIKLPR  
       BCDEHIMNST         S                  ADFHIJR            BEFJST             AHKMS       	       BEFLNPQRT          P                  BCEQRT             EHNP               CDGJLP             CIKLNQRS           BCDEFPT            CEFILM      	       CDEFMNPQS          HOS                ALQS               ACEFGRST           ACJKMNPR    
       BEFGJMNOST         AQ                 KLQRS              CFMQR       	       BCHIKLMPR   	       BFIJLMNRT          F                  AFHJMOPQ           M                  CDEIKNP     	       CGHIKLMPT          AP                 CDFQST             EJLNS              AG                 BEFKOQRT           I                  JLNO               DJLNPT             CDLMNPT            T                  HJKLOP             BFHKMQ             T           	       ADFJLMOPQ          BDQ                ABEGHJKN           FG                 CFILRS      
       BCGHIJKMOS         DKRT               CEF                AHIJKO             FL                 DEFGKLOP           CJT                CFKOR              CDFHKPQ            CHOS               DEHINOST           DEHNPQT     
       CDHLMNORST  
       CEFGHIKLNT         DHS                B           
       ABDGHJLMOP         ABDGHIJM    
       BDFHKLMNPT         CJPQ               R                  EKN                CEH                AGT         
       ABCHIKLNOQ         AIQ         	       AEGHJLNST          ACIJKMR            CK          
       ABDEGLNRST         HIJNOQST           HM                 FGIS               EJMNPT             PQT                CEIJQ              ABE                JR                 L                  L                  BCE                AH                 EHLMPRT            ACJKLPR            ACGKNQRT           DFHKLMST           DKNPT              ABCEFLNQ           F                  BNO                EHKPR       	       EFGHJLORS   
       DEFHJMOPST         EJQ                EGHMQT             DMNR        	       CGHJKLNOS          AC                 EFKLNOQT           AHIJLMOT           AQ                 BHJLNOT            C           
       DEGHIMNOQR         HKM                ADFILNT            CEGL        	       ACGHLNRST          ABEKMPR            BEGNQR             ACKQR              DELPQRST           BCILS              DFIRT              JMPR               BDGKQ              AIKLMOQS           KP          
       ABHIKLNPQS         CEFILPST           BDGLMNOQ           AEJN               BEFGHJMP           BCDGJMO            ABEIMNR            DILOQR             G                  I                  BCFJO              DFIKLPST    
       ACDGHJKLMQ  	       CDFGHJMQS          AHMPRS             DE                 FHIOQS             CEHJPS             EIMQR              ACJKNOT     	       ACHLMNOPQ          JKMNOQST           AJLRS              FHIKLNOR           HPQ                BDFIKLPR           IJMR               A                  BKMOS              EGIKMNPQ    	       BDEFGILQR          JO                 T                  T                  CDGLP              KR          	       BDFGIOQRT          OST                HIJK               ACDMNRST           EHJRST             HI                 BFGHJKOP           LM                 ANOPQS             G                  P                  FKOQR              GIK         	       ABDEFJLQR          EOS                DNS                ABOR               ADIJOQS            GKOQ               BCHKNOPS           GOPR               DS                 R           
       ABDFKMNRST  
       ABCDEJMNRT         HLMPST             ABFK               FGMOST             BGKLQRT            BHMP        
       ABDEFJKPQS         I           	       ABDGIQRST          BEGHIPR            CDFLNQRT           DEHINOQ            M                  D                  K                  AEL                GHJMN              CGR         
       ABCEHJKNQS         AFNT        
       BDFGKMPQRS         CEFGIJK            J                  FR          	       CDFGKLMQR          AKLO               C                  DEFJLMQR           DGNPQT      
       CIJLMNQRST         BCHMNPRT    
       BEFIKLNOPS  	       BCFILNORT          BFIJLN             GILMPS             A                  EPR                AKL                AIJ                CDFGHN             I                  FM                 GH                 GHJPR              BELNPRT     
       CDEFIKMNQT         GJKN        
       BCDEFGINPT         ADIMOPQT           JMPR               ABCLMNPQ           DGHIJLT            DNS                CGMNOPS     	       ABFGIOPRS   	       ABEFJKMRS          CDEHPQS            LQS                D           	       DFGHLMOQT   	       AEGHJMRST   	       BEGILMORS          EGOP               HMRS               EKQT               BGHIOPQT           AEKN               CFJNQ              CDGKLMRT           HJNRT       	       BEFHKMNQS          CDFHKNOS           A                  ACGNOQS            BCILNO             CGHILMST    	       FGHJMNORS          BFHLMQT            BEHIKMOT           ADM                NO                 GOQ                BCFHLQRS           O                  ACDIPRS            DEGJKNPT           CMN                BDELMNOR           ABDGHQS            DEIMPT      
       ABCDEIJNOS  	       BEFKLOPRS          T                  T           	       BCEFGJNQS          BFGKLPST           ACJP               DLOQ               H                  BFGILMOR           F                  ABCHIMQ            FGIKNPR            CFGHNPST           AENO               AJOQ               G                  CDLRT              ABLMST      	       EFGHIJKMT   
       BCEFHJNOQT         ABGKNOR            AEIT               EFINPT      
       BCEHKLMQRS         ABCEHOS            EHJKNPR            Q                  I                  ADGL               EI                 DKMT               BCEIJNPT           ACDHIN      	       CEKLMPQRS          ACGNOPT            ABDERST     	       ACFJLMQST          CEFLNQ             FT                 KLMPR       	       ACHLMPQRS          ABJPRT             AINOP              ADGIOT             J                  AIJPQST            DH                 R                  AJT                BCDFGHRS           CDFHKLT            FKNST              F                  DEGKLNQ     
       BEFGHLPQST         EILOQT             BCDEIJKM           ABEIJPR            BCDLQR             L           
       AGHIJLMOQS  	       ACDLMOPQT          RS                 AFHKMOR            BDFGIPS            DFGKOPS            IMO                BEGHIKPR           AGMPR              OQS         	       ABCDHKLNP          DJMQST             DIMNO              Q                  C                  BNQS               HJLNO              KM                 GJN         	       IJKLMNOST   
       BDFHIJNOQR         BJLOT       
       CDFHIKMNPQ         GIOPS       
       AEFGJNPQRS         DEFHNOQ            ADGIKMO     	       CDEFHIJNR          FHRST       
       CDEFKMORST         BCFKLNR            EGLN        
       AEFIJLOPQS         NQ                 BEGLMP             C           	       AHIJMOPQR          CHNP               ACDEFIOT           M                  BKMS        	       ACIJMNOQT          CDFIKPS            ACEFGQ             DHJLMNPT           GL                 H           
       BCFHLMOQRS  	       CFKLNPRST          N                  EGKT               FJ          
       ABDEHKNOQR         F                  A           
       ADFGJKLRST  	       CDEHIKMQT          ABCDFHLN           GINQT       	       ACEHILNRS          ADEGKNQ     
       ABCEFHMPQS         BCEHLNOT           ABGIKLOP           CEGMO       
       ACDILMPQST  	       ACEJKNOST          FHQ                EFGIKNT            BJPT               AFJ                JLQRS       
       BIJKMOPQST         DHJNOQRS           AKLOPQ      	       AFJLMPRST   
       ABCDFIKNPQ         J                  DGLOPQS     
       ADEGHJKQST         EFHLNQS            JPQS               BLS                BI          
       DEFHIJLPQS         CDFGHIJQ           CFIJKLMP           ADFHIPQR           EKP                BFQ                GKLPQ       
       ABEFGIJMPQ         HLP                EJOT               BCDFGIRS           AKST        
       ABFGHKNPQS         BEFKMPQR           ADEFJLRS           BFGIJNPQ           GKNOPRT            CDGLQT             FGJLP              GHL                BIKNORST           I                  CDJPR              AI          
       ACHJKLMPQS         GRT         
       BCEJLMNORS         BCILPT             DEGPR       	       ABCFGIJPS          GIJLM              AEGNOP             R           	       FGHJKMNRS          FKORS              EHOPQR             CDGHNR      
       BFIJLMOPRT         EHILN              BDELR       	       BCDFHOPST          FLNOPS             AIJLNOST           K           	       BFHJMNOPS          ACDFLNPT           JP          
       AEFHIKMNOQ         KQT         	       ABDHLORST          AGOQ               AHIJKPQ            IJMQT              A           
       ABCDEHJMPT         ABNO               CFGJST             EGHJKT             CD                 CQ                 GKPS               GN          	       EFGIJMNPQ          ANT                DIMQ               AKQ                ACIJLMPT           DFGLMPQS    
       DEFHJKLMQT         CDFGM              BHKLQR             BCHJR              L                  CJLM        
       CDFGKLMNOP         EKLO               D                  EKQ                GKLQR              FIMPST      	       BDEFGHKQR          EGQ                DGKO        
       ABCDJNOQRS         AHL                BEGIJMR     
       BCDEGHINST         AHIOPT      
       BCEFGIJNOQ         EHLRT              BCEJS              DEOPR              FLQR               GLPR               EKLMNST            ADJK        	       ACEHJOPQS          ANOP        
       CDFHLOPQRS         BLP                E                  ABIJKOT            P                  HIRT               ABDF               BFLM               AEIPR       
       BCDFGLMNOS         BEFGKNRS           HM                 DF          
       CDEFHKLMQT         MT                 ACIJLOR            BFHKLMN            HIS                EHPR               E           	       ABCEIKNOQ          ACFGHPR            IJ                 ABNR               C                  BLQR               ADMR               AT          
       ABDFGHIJKL         HM                 CFHQ               KM                 CGHO        	       BHJLMNPQR          AEGIKMO            HIL                HL                 BCFGIT             ABDGHNPR           BN                 BGHO               CEHLMPRS           HIKLNOQT           BGJT               DHLPS              ABCEHJPT           GIJKNT             ER          
       ACEHKMNPRT         LNR                JKMNR              CGHIJLMS           BDEFMOPR           GHJ                BR                 ACJMO              ABILMPQ            LOR                AFGMNPQR           ABDHJLM            BIJMNP      
       BCDFKLOPQR         ABCKLP             FLPR               BJNO        	       ABDFJNOPR          AHJN               NO          
       ACEHLMNQRS         ABKM               DEHLRS      	       BCDKLMNRT          HIK                B                  BCEFMNOQ           BDEHJNOQ           IP                 O                  L                  AEFHT              BGHOQR             ADHOQ              CGKQST             CHJKLOPS           BIKNO       	       AGILMOPRT          H                  CFNQT              DIP                DEF                HLN                ACHQRS             CJP                FO                 L                  BEKLNRT            AFJMT              J                  CGKR        	       AEFIKLMNP          CGJLNOQ            ALO                DFI         
       FGJKLMOPRT  	       DGILMNPRT          BILMNQ             ABCDGMS     	       BDFGHJNPR          DN                 F                  LM                 CDEGHOQR           QS                 I           	       ACFGHORST          BGKNOPR            ACDFNP             E                  MS                 IJKR               DOPRS       
       ADFGHLNOQR         ABDEJPS            DK                 AFGHIJNR           DHR         	       ADEFIKOST          ACFIORT            HJ                 GMQ                DFK                DEFLNOQT           BILN               BFG                CFHIPR      
       ABCDGHKLMS         JMPT               ABFJKLMO           BEIOPRT            FMOQ        
       ACFGHLNPRS         BFLOPQR            AHMRT              CEHQT              JLPQ               ACDEIS             ABEFGHP            FHIJKOQT    	       AEGJLOPRT          AEHJMNQT           DEFQ               BDR                ABFHJN             ACDEHPT            CMNP        
       ABCFHIJPRT         CMNP               CFIMNQ      
       BDHJKNOQRT         FGHQ        
       ABDJKMOQRS         HIOP               ABCLMNT            ACEIR              CEP                AM                 EFJT               CDEJQ              AHN                ALMNT              BDEGJL             GJM         
       BDFHIKLPQS         K                  N                  ACFGKMOT    	       BCDFGIOST          P                  BKOQ               CEP         	       ACHILNRST          DHJ                P                  BCFIJMT            GNT                KM                 ACEGIJ             EQ                 EFGJLMN            AFJNPRS            ACHJKLRS           IR                 A                  EGIK               GHJMNPR     	       ACDEFKMRS          FHJS               AFIM               ACS                F                  O           	       FGHIMOQRT   
       BEFGHIKLRS         BGHIS       
       BDFGHKLQST         CELT               MQ          	       CDEGILOPS          AGQ                ILMOS              GLT         	       AFGHJMORT          ACDJS              ADHIT              BFGHIMOR           E                  CEHO               N                  CKMQT              F                  CDFOS              BCFIKM             GS          	       ADEFIJKLR          BFM                M                  BE                 ABDEJOQ            FQ          
       ABEHILMORS         CELOQRST           ANS         
       ADHIKMNPRT  
       ACDEGIJMRS         DIKPR              AEGJKS             BFHJN              DLQR        
       EFGJKLPQRS         CDFJLP             OQR                BCFGMNST           ABMPRT             FHJQ               N                  EK                 ADFGOT             GHJLP              ABEFJMRS           BCFPT              ABGMNOST           DGLMNOP     	       BDEFGHMNQ          AHJLMNS            AEHNOS             HILMOQR            K                  BCIKQRST           M                  E                  AFJP               ABEHKQT            I                  BFJLNPRT           ABDENPST           FIKQ               BHILMPQR           D                  ABFIJM             CDFHIKOS           H                  ABGNQRS            BFHIJLR            E                  ABGIMNT     	       ABEFGILMS          DFHR        	       CFHJMNOST   	       DEIJKMPQT   
       BCEFIMNPQR         BJ                 JKLNOPQ            GLMOS              AEHMRT      	       ABCGHKLOQ          MP                 ADKL               BHNP        	       ADELOPQST   	       FGHJKMNPR   	       ABFIKLMST          GS                 BCEKMNS            EJMPR              EFGMP              FIKN        
       ACHIJNPQRT  	       ABDFHLMQR          BDKMNQS            BEHOS              HN                 CDEIOPT            AGLN               EGKQ               IMPQS              ABDFKMO            ACFLMT             BEHIJQS     
       ACDFGHMQST         AHJRS              GS                 ACDFINQR           DEGHIS      
       ABCDEHJKMR  	       ADEGILOPS          EHKORST            BCDEQ              CT                 ENOQT              ACKMPR             CELNOPRS           F                  QS                 CHJLR              BFQRT              DGS         	       BDFIJLMRT          EGILP              HILQ               CDFIJR             BCDKNOT            ABCDHIJN           DOR                GJMRT       	       ABFJLMOST          DFH                DFHPQ       
       GHIJKLMOPQ         EI          	       ADEFGHLQS   
       ABFGHJLNPT         Q                  T                  JKPQ               EH                 HINPS              AKLNPQRS    
       DEFGJKNOQS         FIMP               J           	       DEFGKPRST          ABCEGHJR           EHIKNT             CDHIJLMR           AQ                 BGHKOP             ADEHNOP            BDEIKRT            C                  BEFJLORT           I                  DEHIMRT     	       CFGHLOPQS          ABHIOQST           BCLT               GN                 AJ                 GH          	       ADEFHNORS          AIL                BKR                AEGKMNOQ    
       BCFHIJOPQS         ABCFHIP            FR                 AFLR        
       ABCDEHMNOP  	       BDGIKNOQR   	       ABCFHJLNR          DMQST              AFLN               BGHLNRST           B           
       ABCGHIKMQR         E                  ABHLMNOS           M                  ACIKNST            FLNOQST            DL                 EKNPQRT            GPQ                AEFIMPR            AFKLPQS            BCEGOQR     	       ADEGJKLMS          PR                 EJM                EJO                L                  K                  N                  BFHIJKST           CFL         
       ADEGJLMPRT         AJKMNPR            BDFGLPS            DOP                DHJKMOQR           FIJKT              FHKLMQRS           DEFGHLM            HINT               AEHLMQT            BFHMNPS     
       BCFHIJKPRS  	       BCIJKLMPR   
       BEFGHLOQRT         AEIMPST            D                  BFQ                FGKT               CEGHMOPR           BDFHPS             DEFIJOT     	       AFJLMNOPS          AE                 T                  ACFNPS             KMS                GP                 AILQR              CDIPQR             DJ                 ABCFMNOS           AGHT               ABEIKL             ALMOPR             EGHKPQR            E                  EIOR               DEGNO              R                  ACEHILNO           BILMS              GIJLMNP            DENPS              FH                 AGK                BNRT               GIJLNQ             KLOQS       
       ACDEILNRST  	       CFIKLNOST          EGMQRST            HJLNOPS     	       ACHIKMNOR          DJP                BIM                Q                  FH          	       BCDEFIKNS          DOPR               DKMNOP             DIJOS              CFNQRST            BCEFILMS    	       BDFILOPRT   
       DEFGKLNOPS         DFGHJQR            BLM                AMOQ               CG                 AK                 EFOT               AFGHKNPR    
       ACEFGHJKLS         EJKN               BFHIKN             FHJO        	       BCFGHKPQS   	       DFHIKOQST          EQ                 HLN         	       BEJKLMOPR   
       BGILMNOPRS  	       AFJKNQRST          GJKMPQRS           HJ                 DHIJN              K                  S           	       ACDFGHLST          GI                 EIST               IQ                 CMS                CHILO       
       ACFGIMNOPS         JLMNQ              M           
       ADEGIMPRST         R                  EIP                AMPRT       	       BCDGHILRS   
       BCDHIKMORT  
       ABCEHKLMOS  	       ACEILMNOQ          DI                 FPRS               BEHKNS      	       ABDEGHKMS          AIJ                ACDGNS             GIJPQ              EIOPT       	       DEFGLNQRT          AILNO              ST                 BDFGLNR            ACDMT              AILNRT      	       CDEIMOPST          FLNQ               H                  BI                 BEFIQT             EF                 CEIKQ       
       BDEHJKLMOT         DGKLN              CEGKLMR            R                  ILO                BCHKS              FHNOQRST           ABCDHK             CHIJKPT            ABDFIJLS           P                  DFGHINPR           ACDHLOP            ADFHIP      	       IJKLMNPRT          ABEK               DS          
       BDEFGHLMOR         DEFKNOR            M                  AC          	       BCFHJLMOP          BEIKLNQT           AGHJKQT            CDGL               BGLPQRS            AEHNP              IN                 J           	       BDEHIKLMQ          IMN                CDGHMPT            AELMQ              KNOQ               BIMN               DFMOT              CEFLMOT            IJR                ACDJKPS            DJ                 ABLMNPR            BCJLOS             APR         
       BDFGKLMOQS         ACHJKLMP    	       ABCDJNPRS          H                  DFHLT              DFINOR             AFOP        	       DHIKLNORS          DEHJNOPS           BDFLNPR            FGKLNR             J                  BCFLPRT            CFGILRST           DKP                DGJNPRT            CGKOT       
       BCDFGHIJOQ         AF                 BILS        	       ACDEFMNRS          ABHKN              CO                 CDEJMST            BJKNQR      	       ACEFGLMNQ          DEFJPQ             BCFGIL             BCFLO              ABDGQR             GJLPRT             BCEQ               BGLMNR             AEHILPRS           AHIJRS             ACENRT      	       ADFGILOQS          BDEGJOPT           CRT                ABHOQ              ABFJKL             ADJM        	       ACEFHIJMO          FM                 BJT                BEIMOP      	       CDEHIMQRS          ABFKOQRS    	       DEFGHLQST          BELN        	       CDEJKNORT          CFLMOS             BKLNORS            EFKMRT             CGMNO              IJLOP              DL                 BKM                BHMOR              BEHJK              DFNPQ              Q                  IMOPQ              CP                 M                  EFKR               DGHJNOQR           HJMOP              HJKLNR             DKL         
       ACDEFGHPST         ADHJKPS            AEHJKLNT           DGHLP       
       ADFGJOPQST         AEGO        
       ACEHIJNPST         AFKMT              FGJNORS            GMOS               DHIJMOQ     	       ABDFHLNQT          FIJMNOPS           ACGIKMQT           M                  L                  BCFJLOQS           BFHKOPRT           M                  ABFH               ACLMPST     
       CILNOPQRST  
       CFGHMPQRST  
       BCEGHMOPST         ACE                CFNP               ACDFKLOP    
       BEHJKLMPRS         O                  ANPR               ACFHKMRT           CGIKOQT            ABEHNPR            AEMNR       	       ABCDFIJLO          BCDG               BGJLP              CEHNS       	       BFGIJLRST          L                  F                  GO                 DKOS               HKOR               ABFHIL             J                  C                  EGNO        	       CFGHIMRST          L                  N                  AGQ                BFIPT              INO                ABEFHLQ            KMNP               CEL                BCEFJLQR           L                  EHLPS       	       BEFIJMOQT          CEGJPQST           JS                 DFGJOPQT           AEFLMQS            S           
       BDFGHLMNRT         ABCFJMQ     	       ABCEFGIJM          BEKLMPQT           D           
       BCDEHJKMNO         DFHNS              BDMNQ              DHKPT       	       BGHIJKLNR   
       DHIKLMORST  
       ABFJLMNOQS         EGMRT              BN                 DEFJNPS            BMQ                FINOR              EJST               S           	       ABDIJKMOT          EGILPS             FKLST              ACELMNPS           H                  DFGIKNST    	       CFGIMNOPT          GIS                CDFHKS             DFHR        	       ACEFKLMQR   
       CFGHIKMPRS         ACDGKQT            FOS                ACGILOR            DEFIJNPQ           ABM                ABDEHP      	       BCDEFJMRT          EIQR               DFJKP              CDIMPQ             BPST        
       ACDFHIJOST         KLQT               EFGJS              FK                 J                  FG                 BGHIJLT            AM                 FJ                 HP                 CGI         	       ADHILMNOQ          ABCFHIJ            KMN                ABDPR              H                  AEFR        	       DEFIJLNRS   
       BDFGIJMPQT         G                  GL                 BMQS               DR                 F                  DEKNQR             DEMP               FM                 APQ         
       ABDFIKLMNQ         DGMORT             C                  HIKLMNT            LNOQRS             AB                 DEFT               K                  ADJN               JKNO        
       ABEGHNPQRS         EIJQ               DGJKLM             B                  FPT                DFHKLNP            BCDGIKQ            BIJPRS      
       DEHKMNPQST  
       BGHIJKLNRS         ABCEIJMQ           FGHJS              L                  ABDHJ              ABDGPRS     	       CDEFIKQRS          PT                 DGJQR              DIOR        
       CDFGHIJNOS  	       CHJKLMPQS          N                  FGK                FKPQR       	       CEFJKLQRT          G                  BHKNQT             BCEIKMPR           LM                 BHIKRST            KNOPQT             FHKNP              ADEFNP             ALQ         
       ADGJLMNQRS         CIMO               EGJK               ACGIJPS            ABDEFHJR           EGLQRS             DJ          	       BCGHILPQR          BDHLOT             FIJKMNQ     	       ABELMNOPS          J                  AP                 ACJKMNRT           BCFS               EHJ                FILMQ              ADIJNQT            BGQT               CEFJ        	       AJKMNOPQT          IJMS               ACEIJS             CDIJMNS            EMRS               AIMOQS             CFHJKLT     
       ABDFGIJNQS         DS          
       BCEFJLMPQT         BGJKNO             FGJNOPS            FJLMNOPS    
       BDGJKLMNPQ  
       ABDELOPQST         EMR                ACFLMRST           FNQRT       	       BCEJKPQRT          N           	       ACEHIJLMT          O                  CDEIKMQT           FT                 HIN                CEKQR              EJMPR              HRT         	       BCDGMNPQS          CDM                AFNQS              E                  CFL                HNPR               CH                 CT                 O                  LP                 ADEHKNQT           EHIKMN      
       ABCIJLPQRS  	       ACHJKNOPR          PQ          
       ABEGHJLOST         HKS                KLPR        	       BFILNORST          HJP                EO                 BIJNQT             CT                 BEKMNOQS    
       ABCEFKMPRT         A                  HNO                GHKLP              S           	       ACGIJMNQS          CDEHJM      
       CEJKMOPRST         AK                 CKPT               ANO                IM                 MST                J                  IKM                T                  A                  AHMNOP             BEKR        
       ACDEIJNOPR         IKL                BCJLNPR     	       BDHKLNPQS          CGIP               LO                 CGLNQRST    
       ABCFGHJMOP         LN                 D                  EGQT               CDOP        	       ABCDEGNPR          ALST               BDGLNOQR           HIPT               M                  BDKMNPR     
       CEIJKNOPQT         AFHT               ACDEJKLR           CPQ         
       ACDGIMPQRT         EI                 L           
       BCDEFHJNOQ         KLOQR              F                  BDEHI              DG                 JNO                BDLQRS      	       CEHIJKMPQ   	       ABEFGKMOS          ACEGJKMO           AGMO               BFKS        
       ADEHIJKPRS  
       BCDEGHOPST  
       ADFGHKLQRT         FGJMRST            IMOS        	       CGHIKLMQT          EGR                DGJ                CGIP               AEMN               FO                 BOPRS              AEO                KOT                CGHO               ABEGJOS            GMT                R                  IKMR               H                  BEJKN       
       ABGKNOPQST         ACIKMPRT           GLNST       	       AFJMNPQST          BCEHJQ             QR          	       EFHJMNOPS   
       ABCDEFHLPT         BEHIKMNS           EIJLOPST           BHPS               BCDFNS             CDF                OT                 AR          	       BCFGHIOPR   
       ABDFILMOQR         AHJKPQT            GMPS               BFJKMOPQ    
       BFGIKLMOQT         DN                 CDFINT             NR                 ELOPQ              EFPQ        
       ADEFHJMNOQ  
       BDEFJKLORS  
       ADEFIJLPQR         AHINPS             PR          
       DFGKLNOQRT         CLPQ        
       CDEFGHIJPS         BCEO               EK                 R           
       AEGHIMNOPQ         RS                 E                  AEHJMQR     
       BCDEFHJORS         EFNT               ABOP               AEGHJKNT           AP                 ACFGIKT            DEGIJLQ            JKR         
       BCHIJKLMNR  	       ABEJKLMQS          DHLMO       	       DEFIJLNRT   
       ACEGKLMPQR         HJK                T                  BFHLMQ             CDJM        
       BCDFHIMOQS         ANOPQRT            DIKOPR             AEGMPR             C                  GPR                PQS                CGR                FGIMS       
       ABCHIJKNOS         IJLQS              DFHJKNRS           CPS                NT                 EFGIMR      
       ADFGJKNOPT         L                  KPRT               BIJKP              S                  EFIKLNOP           ACIMR       
       ABCFHJKOPT  	       ACDEFJNRT   	       ABEHIKNOT          AEGJKLST           CINOPQS     	       CGKMNOPRT          O           	       BCDFJMOST          IP                 AT                 EHKMNOP            FKS         	       EHILMORST          ACIKNST            ACENP              ADEMN              BEFGHJOQ           BDFGJKRT    
       HJKLMOPQRT         FIS                BGHL        
       BDEHJPQRST         BGJQS              B                  AJO                FGJNOS      
       AEGHJLNOQT         BCGKS              AEGJL              CPR                IJKNP              KORS               CHIMQT             CGI                EJMT               GMP         
       BFGHIJKORT  
       ACEGIJLMQR         BDFILRST           CDJKMNQ     	       CDFHILPRS          ACEH               HQ                 BDHKL              CEKLMR             O           
       BEFGHIKLMR         DHIKOST            EFM                BGHIJMNP           DKMNRS             CDJS               BFHKMNQS           NT          
       ABCEGHIJOR         BHJKNPST           EFJKLPST           CHLN               AEGIKLQ            GIJKMPST           BFGMPRST           FIMOQ              DEKLN              CDEHLN             CN          	       CDEIJLNOQ          ABGJKPR            S                  DLMOPQ             CDHJK              JKLR               BCK                DELN               P                  JMS                CEFIJ              AHJKPQS     
       BDFGHJKOPQ  	       DFHIMNQST          EHLPS              ABIMNR      	       BCDGHIMPT          CFGM               FGKLN              EGKMT              JST                S                  AFJ                ELMOS              AGLNQT      
       ABCDEFIMOQ         JN          
       ABFGJLMNOS         CJKLNRS            DJOPQR             AEFHMNPQ           DFJNOQRT           L           
       ABCDGHJLMT         CGKMQST            ACDHPRS     	       BEFIMNOPR          ABCFHIQ            ACGIL              T                  AGT                ADFKNQT            CGHIJKLN           EJ                 ABCJKOQS    
       ACEFHKNPRT         GN                 KMPR               ER          	       DEGJKLPQR          OPT                RS                 DEKLPS      	       CDEGHJKMT   
       ABEJKOPQRS         BKM                ABHP               BCEJMO      
       BCEGHJMNQS         GKQT               JR                 ABKO               ABCIK              R                  CEHIJLQT           CFMO               HNOT               CDGILP             DFIKR       	       ACDFLMNST          CFKNPT      	       ABEGNOQRT          NQT                HMPQT              DHMPT       
       ABCEGHLNOR         AIJLNR             DJMNST             FK                 KM                 AGKLNOPT    
       BDFIJNOPST         BCFIJQST           KLR                BCI                EFG         
       ABCFGHKMOT         IQR         
       ABCFGIKLMP         GHKOT       
       ABCEFJKLOS         BCEHLOT            HOR                AS          	       AGHKLOQST          AFGHIKLM           BDQS               BCDGMST            BM                 P                  ADHLO              FL                 FHMO               BCDFQRT            AFKPQT             ELOS               EFHLMNP            BMPS               ACDEFIOR           CLNST       
       BDGIJLPQRS         INS                CHKLPQ             CHJ         
       ADEHKLMQST  
       BDEGHJNRST         DGJKMNO            DGIJLS             CDELNOR            ACHIO              JST                EKOQR              BFKNRST            L                  DLOR        	       ABDFGMOQS          CDJMQST            CFIPQ              BJLNPQT            AJR                EMR                DEFGLPS            J                  EFGJOS             BCEGIJOR           AC          
       AEHLMOPQRT         CIKPT              BM                 C                  GJMOS              CEIJMQ      
       AEFHJMQRST         KO                 G                  ABDHKMNQ           HJL                AJ          
       BDEFHIJNST         CHLMOPQS           B           	       ADFGJLNQT          CFHIP              BHIO               DGPT               IJLNS              EGHK               F                  P                  ACDENOS            ALS                JNST               PT                 DEGJLMS            AMP         
       BDEIJLOQRT  
       BCDFJKLOPR         E                  AIJKMOT            AIOS               CGP                JS                 CDFIP              DGPR               AMQ                EGIPT              B                  ABHLS              AMP                ABEJMNR            AT                 GJQ                L                  AIJMNO             T                  CGIKMPQT           HP          
       ACEHILMNRT         DGPT        	       ABEGHIKOT          ACEIJMNO           J                  ADOS               FHK                BDEJLPQR           CFGILMRS           ADKLMOP            GIMNQ       	       CEFGLMOPQ          CEFHJMPQ           BIL                CDMNQS             CST                BJ                 BCDEJLMT           CEFGHMPS           BCFGHT             BDEILQS            GLPT        
       AEGIJKLNQR         F           	       CHJKNOPST          BDMN               ABEJNQRS           CHINOS      
       ABDGIJLOPQ         R                  CDFGIJ             DJST               EFGT               DFGOPQS            COS                ABCDMOPQ           MNPQ               BKR         
       DEFGJLMPQS         BS                 CDJMQT      	       ACDFGKLOT          AEKLPQ             AJ          
       BEFGHJKLQS  	       CDFHKLMPT          KM                 S                  EHIST              CFHJKLPS           ACDEOPR     	       ACDHIJKST          ABDKLRST           GIMO               AG                 CHMP               CFHJKQ             EFHIQT             AGIMPS             GMO                EHJNOQRS    
       AFGHIKMQRS  	       ADGIKLNPT          AINR               CIM                S                  AHJM               AEN                KN          
       CEFJMNPQST         GHIL        	       BCDEFHJPR          AFMP        
       ABCDJKOPQT         CDHJL       
       ACFHKLNOPR         HIKS               CILT               EFN                CGLR               AM                 ACDFJKPR           MS                 CDEFGIMR           DKQT               LN                 ACDJNOT            DE          
       DEFGHJLPRT         M                  EHMORS             BCDGJQ             G                  Q                  ER                 ENT         	       BDGILNOPT   
       ADFGIJLMOP         BDEINT             AEFHJLN            AFLP        
       ACDJLMNRST         DFKMPT      	       EGLMPQRST          FN                 GHIKNPQR           EKN         	       BCFKLMORT          ABIJKNS     
       DEGJKLORST         C                  CGIKNR             O                  Q                  P                  AFG                FJLP               GKPR               DT                 ACHJKM             CDGKLMR            EFH                GKO         	       ADEFJMPQR          INR                DENOR       
       BCDFIJLNQS  	       ACDGIMNQT          AFM                C                  BCDENOS            CDEGHJL     	       ACDGKMPQT          CFHKLOT            B                  ACEKOPR            AI                 GILNOR             ADFGILMN           HJT                A           	       ABCEFGHOR          AJOR        	       DEFGIJNPT          R                  EGIJPS             BKLST              JLM                EFG                KS                 CHS                M                  CFGIOQS            BKM                DFHIL              CFJKLPRT           ABCFGKT     
       BGHILNOPRT         AFHLNPQR           DEFGOPR            DJMST              EFJKMOQ            BDJN        
       BCHIMOPQRS         CFILOST            M                  AIOT               AGLMNORT    	       CDEFHMOQT          BCDP               BFJLST      	       DHIJKOPQR   
       ABCEJKLPRT         L                  CMP                HIO                ACINQR             S           
       CEFHIJKQST         CFKT               CDLOP              ABEF               ELPT               BEKNQ              BDJ         
       EFGIJKLOQR         M                  AGJKMPT            EHOPQRS            CEHNO              ADELPR             ACEKLMO            EFKLNOR            K                  GHIMNQRS           EL                 B           
       ABDEGJLMNO  
       DFHIKNPQRT         DHILPT             BHKNP              DKN                CDEILN             AHKLOQRS           EIK                BDJKPQRT           GHKLT              E                  BFGMNPR            DF                 ACFIOS      	       GIKLMNPRT   
       ABEGHJKORS         BCGHMQ             GPQ                BFHN        	       DFHJLMNPR          AOPT               INPQ               CFQ         	       CDEGLMNOS          M                  EIS                AB                 AEFKQT             P           	       AEFIKOQRS          BFJKS              DHIKT              FS                 EGHKNQR            BCFGJOPS           ET                 JLMOP              BCDGHIMP           M                  BEJKMOQR           AC                 L                  S                  FJP                PT                 KM                 EFKT               GMP                MNQ                IKLNOT             GIOPR              DFGIJOP            ACIJMPQR           ABDEFQRT    
       BEHIKNOPST         EIP                DGHT               ER                 BRST               AO                 AEGQ               ELM                F           	       ABCEGIMQR   
       CDEGIJKOPQ         BEFJNT      	       ADFGINPQS          BCFLMOP            EKLMOQRT           ACDFGJLS           D                  AKRT        
       ADEFGKLORS         R           
       ADFIJLMNPQ         ACEIMP             FGMPS              IK                 G           	       ABDFGKOPS   	       ACDEKLMST          BEGHJK             GHJKNOPT           DE                 DIJP        
       AEFGJKLMQR         BHJLNOQR           DEHIN              P                  KMS                P                  DP                 JM                 CFGIMNR            DHILQ              AFJKMST            ANOQT       	       BCGHKLNST          EHJKLMN            EGR                GR                 CHRST              ABEGHIKP           BCJKMPRS           GJMNOS             DJ                 DQ                 O                  HIJST              BJKLP              DFGHILRS    	       BEHIKNPQS          DGJMNO             CEIKPS             EFHJMNPQ    	       DEGIKMOPS          ABCHR              FMS                DMQR               ACFHIJKP           DKP                KL          	       CDEGHLQRT   
       ABCHJLMOPS         ABIT               BEFGMS             AIMNOQ             BDMOPQRT    
       BCHJMOPQRT         CMS         	       ADGLMNOPS          KN          	       ABCFHKLNP   	       DEHIJKOPS          ACEHKOPQ           CLNR               BDHJNOR            M                  ABLMS       
       ACDEFHJMNQ         DEJKR       	       ACDJLMPRS          EL          	       ADEIKMOPS   	       ABCFHJRST   	       AFGHKOPQS          ABEGIKR            ADEKMNPR           AFGJKMPS           AFHOS              ACEFGJLS           BQ          
       AGIJKLMNOQ  
       BDEFHIKPQT         JKRST       	       BEFHIJLOQ          BDEGHMQT           R           	       ABCDFGHNO          BINOQS             AD                 CI                 CEMP               BFKT               T                  ABDGKLPT    
       BCDEFGHJKM         CNS                FT                 B                  K                  MO          
       BCDGJKNOPR         GJMP               DG                 AOS                AEGIJKNR           CDKLOQ             ADM                BFHQ               J                  JPS                HOP                M           
       BDEGHLMPQR         I           	       ADEFKNOST          DKLMOPS     
       ABCFGIMOPR         EO                 ABIO               BCDFIJKM           ABFHLMNP           ABENOPRT           B                  FT                 AFGLNQST           BDGJST             AEHLOQ             ADFHLPQT           CDFJR       	       DEGHIJKOR          BCJOR       
       ABCHJLNPRT         EK          	       ADFGKOPRT          GQR                AEFIJLMO           BCEIMNOQ           EJOT               Q                  CDIS               CR                 BCJKNPST    	       BDFLMNOPQ          AFGJRT             RS                 CLPQ               DGHLMNS            CEFHIJOQ           Q                  ACDHLMOR           S                  AJKMNST            BCPRS              ADEHJKOT    
       AEFGHKMNRT         LNS         
       ACGHIKMOPQ         BCGLNPR            ABCDHMNO           BEO                AFNT               NR                 KLPQ               ADHIJN             O                  CEKMNORS           ABEFGIT            CGST        	       BDEIJMNQT          Q                  MNQRST             DNOR               FILPT              AGLPT              CGKL               DILMN       	       CDFHJKOPR          MNS         	       ACJLNOPST          K                  ACDJMPS            CJLOS              EQS                NQR                JKM                DEFR        
       BCHILNORST         L                  ACHKMNPR           FR                 BDEGJOQ            S                  DFHO               ADJLMP      	       ABDHIKNRT          BDFKMQS            ADJKLOQR           R                  BQ                 EH                 H                  FILPR              BDHNPQ             AD          	       BCDGHIOQT          CQRS               ADGIJQST           KLQR               BCEQ               ABCDGLOS           R                  FGJLMNO            CFHJMNPQ           ABDFHIQ     
       ABEHIJLOQR         C                  ADKS               BDJQ        	       CDEKLMOPQ          ST                 BDFHKLPS           G                  EH                 AC                 DEHLNQS     	       ABCDIKLPQ   	       HIJKMOQRT   
       BDEFHILNOP         L                  GI                 CDGNP              AJOS        
       ACEGHIKLMS         S           
       ABDEFHJORT         I                  CDFIMQS            NT                 JT                 IMR                BKP                EGLMPRT            CF                 R           	       ACDFILMQR          BEFKMQR     	       ACDGJKMNR          BFJ                KN          	       ABCEGNPRT          ABCFLOQS    
       AEFGIJKMNR         CLP         
       ABCEFGLORT  
       CFGHIJLQST         IJ                 GIMQ               EGHIMOP            KP                 FGLNQ       
       BCEGIKMNRS  	       ACDGMNORS   	       DFGJKLOST          K                  DFGS               FHLOST             S                  BFKLMOR            GILP        	       BCDGHINQS          CFHINO      
       ACDGLNPQRT         KRS         	       CEIJKLQRT          HPRST       	       ABDFILMPQ          BK                 E           	       BCDEFIJKR   
       ABDFJKMNPR         JLPQR              FQ                 CDIJLMNP           ABDKNORS    
       ACDFGJKNRS         HIJORS             BI                 FOT         	       ABDFIJQRT          EFGJKPQT           BDLPT              CGK         	       BDEHIJKNO          CDEGHJT            ILQ                AR                 AHJ                AIO                BDFGIOPT           CDEFP              JK                 AHMST              ADFIJNRT    
       BEGJLMNPRT         AGHKNPR            E                  BFGIKMNR           CEO                HNQ                ABHLNQT     	       AIJLNOQST          ABCIJOST    
       ABEGIJKQST         ABCFGKLT    	       BCEFGKLNP          CJP                BLOPQR             GNPQR              DG                 ACEHMOST    
       ADHIJMPQRT  	       BCFGIQRST   
       ABFGHJLMOS         BP                 BDFGJLMQ           EKO                BCMNOQT            CDGIJ              G                  EFJNT              AGHN        	       ABCFJKQRT          FJ                 ILOST       
       BCEGIKMPQR         B           
       ADFGKMNOQR  
       ACDEFINQRT         CFIKLOPT           AG          
       BFGJKLOPRS         CEJMOPR            CDGJKPT            GI          	       ADEGIKMPT   
       BEIKLNPRST         ACJP        
       ACDEKLMNRS         DFGNR              FHT                ADEGLNQT           CGKLO       
       HIJKLNOQST  	       CDFGHKMNR          ABDEF       	       BCEGIKLOT          CGPR               DFQRT              CEFKNT             BHIJM              HLOT               ABGIJKS            IMO                IJT                CFKMP              J                  BDFQS              LNP                FHIJNPQ            FKQ         
       AEGIKLNOQT  	       ABDEGLOPS          BCFJPRS            ALT                CL                 AB          
       ADEFIJLQST         ADIJ        	       BFIJNPRST   	       ACFGHJOST          GR          
       ACEFJKMNPQ  	       DFGHMNOPQ          JP          	       CDEGILMPQ          BIKMR       
       EGHJMNOQST         KLNQT              CDHJLQ             FJKO               CHIS               AEFGPRST           CFIMP              Q                  BH          
       AFHIKLMQRT  
       ABCDFHILPR  
       ABCDHKLMOQ         EFHLMNS            FLM                DEINPT             CJLMNPQT           N                  D                  IT                 CHJNO       
       ABGIJLOQRT         CIP                R                  P                  R                  ABGO               K                  ABCFHIRT    	       ABDIJKQRS          BCGJKNQ            CEPS               DMNPT              FHT                BCDNQT             DGLQR       	       DEGHJKLOR          AEFGPT      	       EFHKNQRST          AJQ                CFPT        
       AFGIJNOPST         CHLO               EGHOT              LPR                BCDFNORT           AEJO               DJ                 HST                FKMR               KL                 R                  I                  BEIJKM             NOT                ABGJR              FLNO               CP                 GJOR               AIJLOQT            BPRS               GHILNOS            BFGS               AFG                ADEHMPS     
       ADFGLMNORS         BCJKMNPR           ADEGOR             BQ                 BFGHLOQS           JLNO               BL                 BIKLMNQR           P                  EHMQT              AFGHN              ACP                GILQ               D                  BCGHJMQT           DP                 A                  BDFJKNOS           EM                 EHJKRST            BLR                AMOR        
       BCEGJLMOPR         GLMPQT             HIMOR              DIJLS              BCDPT              AIKLNPQ            BCEFLORT           CPT         	       BCGIJLMNO          IKO                MQT         	       BCDIKOPQT          AEF         
       ADEFGHLNOP  
       ACEFKLMNRS         DGHPQ       	       ABDFHKMNR   
       CDEGJKMPRT  
       CDGHKMNORT  
       ABDHKMNORT         BEHILMQ            AHIM               CLRT               EG                 GPQ                CIJKM              K           	       BCEFGIKPR          ABCEH       
       ABGIJLOPRT         LNQS               BFHMRS             AEFHKLN     
       BCEIJMNQST         CDJLN       
       ABDFIKLMNT         ABLMPT             I                  BHMPQT             S           	       ABHJKLMQR          AMOPS              DJQR               KNQS        	       ACHIKLORT          GS                 LPS                CEJL               C           	       BCDEJLNOS          BCDHJMQS           BCQS               JO                 EGPT               CHKMNPRS           I                  EFKLOPQS           JT                 DEMRS              FJNPQR      	       ADEJLMNPT          BDM                DJMPQRT            ABDGNR             GIJKMP             BDQ                J                  O                  AFHIKLOQ           ABEHNQ      	       ACFHILQRT   	       ADEJKMNQS          EGHIQRT            CEFGHKOP           CJ                 G                  HKMQ               FJ                 CDM         
       ABDFGJKLNR         CFIOQ              AFGIRST            FKMOPRS            AFKMOPQT           M                  EFIKMOQ            FGHILOPR           BEHNQR             HJL                FGLQ               F           	       AGHIKMPRT          BDS         
       BDEHKMNQRT         ABHJKM             GHIMOR             B                  AMNOQS      
       ABEGHJMRST         B           	       CEFJKLPQT          DKPT               EFJ                FIRST       	       AEFHMNPST   
       ACGJKLNOPS         BCEIKN             O                  CEFIKN             FHILQ              ET                 F           	       ADFHIMOPR          DKLOP              FLOR               AEJNPQR            DEGLT              INR                BDIPT              BJKNPQ             CFGLMPT            AOPR               BL                 NPST        
       BEHIKMOPQS         CHLMPQS            BDEIJKT            BFGHJLN            ADILQ              GKNOQRT            BCF                BCEFJO             AEGHJMNT           INOP               IN                 D                  D                  BDIQR              DN                 ABCDGHPT           BIJKNOPS           CEJKNOST    
       CFGIKLNOQT         BDEHIKMP           O           
       ACFHIJMQRT  	       CFJMNOPRS   
       CDGIJLMOPS  	       BCEGHJLRS   
       ABCDFGIKLT         O                  D                  BHK                NR                 FILNP              EHPQR       	       ABEHJMNQS          ADLMQT      
       AEFGJNPRST  
       CFGILNOPQT         EGHKMPQ            R                  EFHJQS             ABDHMNP            C                  BDHJST      	       ABCDEGHKO          JPS                INS                E           	       CFGHILMQS          GJM                EHPR               CNO                ET                 A                  ABMN        
       ADEHJKNPQR         AEHJMP             BCHP        	       ABDIJKLOQ          K           
       AFHIJKLNRT         ACFHKQ      
       DGHIJLMNRT         AH                 BHINOT             DIL                BCFHILMO           EHMNQRST    
       ABEFGHJKQT         CFGR        	       BEFGHJKLS          BGHIO       	       ABDFHJNRT   
       ABDGHIKRST         BCDENOP            ADILQ              DLORS              CEJP               ALOR               CFHIJKLT           DMS         	       BDGHMNOPR          ABEFHIN            CEILP              CDHLPR             E                  EILM               FGLQR              BDFHKLOT           AFRS               IM                 AIST               ADGKMNOP           EHJMNPS            FGHKQT             RT                 N                  ADGHNS             BDGIT              BFJQ        
       CDEFHILMPR         DHIRT       
       GHIKLMOPRS         E                  GM                 PQS                AEIOP              FJLNQT             E                  EHPQ               P           	       BDFHNOQST   	       BDHIJNPST          IKNP               J                  BEHKOST            BEKMRS             CH                 GQ                 E           
       EFJKLNPRST  
       ABDGIKMOQT         DK                 BCGNQR             AFGHIOQS    
       EFGHJLMNQS         DJ          	       ABCGJLOPR          EFGMS       
       ADFKLMNPST  	       BDEFHKNOP          CDGHJKS     
       BDFILNOQRS         KQT         	       BGHJKNPRS          FGIJLPT            EGHKQ              BFJLPQ             LNST        	       ACHJKMORS          CEFGIRS            ABFIKMQ            BFO                O                  AEHJK              DJR                DIKMST             AEFJKMNP    	       ADFHILMNR   	       ABCDFGILR          FKMN               BHM                DOQ                GMNRS              KOPQT              MR                 BCFMNQRS           NOT         
       ACEFHIMNQT         DEO                DFM                BJ                 AHIJKLP            CDEGIKQ            BDIJMPQ            DHNOPQ             ACLQT              JP                 BCFGHJPS           T                  DIKNPQR            CDFHJORT           BCEHK              EJLMNPR            BGIMNOQ            BDEGHJNS           ABFLP              BLPS               ABCELNQ            DK                 L                  BEFHIL      	       ADIJLNOST          GHJN               D           	       ADEFGJLNQ          ABCHNRS            CE                 DEJ         	       BCIJLOPST          BK                 AEIJKMRT           BDIKR              KLMR               H           
       ADFGIMNQRS  	       ABDGLMPQT          IJKPST             CFIKO              BDKOQ              AFIJNS             FI                 BG          	       GHIJMNRST          FGIJKOPT    	       ADEFGHKST          BFHJMN             M                  EFGMN       
       BCEFIJLOPR         DEFHNS             CFGIJLNP           R           
       CDEGHKLPQT  
       ABCDEGHJMS         LPT         
       AFJKLMOPRT  	       CDFGHILMS          CDGKM       
       FGHJOPQRST  
       ABCGIMNQST  
       ABCDGHILNT         BCDEGLST           CLS                BDNOPST     	       EILNPQRST   	       BCDGHPQST          DHILMS      
       CEFHINOPST  
       ACFHKLOPQR         DHS         
       ACDGHKLMQT         AFHJLMOR           BCDEGLQ            AO                 DJ                 BCGPT       
       DEFGHIJNQT         ELNS               CKLN        
       EFHJLNOQST         DL                 DFJLNT             GIOPT       	       BCFLMOPQT   
       ACEHJLMNOP  	       EGHIJNRST          HJNP        
       ACDGJKLMPR         FKNQ               ACHKPT             EGHOP              PT                 LM                 GP                 BEQR               LRT                BCIJKQ             FMNPQRS            CDENT              FN                 HNOQR              ACEGLQRT           AN                 R                  GHKLOR             BFHJNOQR    
       ADFIJLPQRS  
       BCDEHMPQRT         DF                 BCEJT              AB                 ACDGIRST           CDHJ        	       DEFIJKMPT          CD                 AEGHJLNR    
       ABCHJKMNQS         EMS         
       CFGHJKLMST         ADMR        	       BCEHNOPRS          BFOS               CGHIJNT            CFLMNOP     
       ACEGHMOPQS         ACFIRT      	       ABCEJKNOP   	       CDFGHIKRS          BMOPT              DEFIJNO     
       ABEIJKPQRS         AKLQS              JNT                CDHMRS             DKMOS       
       ABDFHIKMPS         FHL                EGIKNQRT           DK                 BHLPQ              AIKLMN             ALMR               EGNOQR             H                  HIN                BEKLMPRT           OQ                 GHLQR              RT                 EGPRT              ACDEFHT            P                  M                  AGLP               EGNOPRST           BFGHMO             BEFLMNT            HJKLNPT            AGO                CFJLPRST    	       BHIJKLNQS   
       CDEGHIJKOP         CDHJKR             PRS                HIJMP              KLR         
       ABCDHIMOPR         DILQST             FHJKM              BCEKOS             CKLOT              ACIMNOPS           BCFPST             AEMNOS             BGIJKM             Q                  AIKLMNOQ           CDEHMO      
       BEFGHIJMRT  
       ABFGILQRST         JKLM               EOQR               F                  AIKNT              BCDGHKPR           BDEFHKLT           AFGHLPQT           ACDELR      	       ADLMNOPQT          BF                 E           
       BCDEILNOPQ  
       ACDHJKOPRT  	       CDGHJKQST          CHJLMNQT    
       BEFHJKOPST         CIJKN              JP                 CDLORS             AFIQR              AGJKLMS            AJKOPST     	       CGHJKLPRS          ACGOP              GOS                BG          	       BCFGILMPT          AFHJLMNT    
       ADEFGJKMQS         FIPQS              EHKLNQT            DINS        	       DEHJKLNQR          DIKPQT             EFRS        	       CFHIKLORS          AIJPQ              ET          
       AGHIJKLNPQ         BCF                EJKMNRT            BEJLN              BCEJLMST           T                  AC                 AFHLMS      	       CDEFIJMQS          KMR         	       ADGIJKLPT          GP          	       AFHILMORT   	       BCEGLPQRT          EK                 EHLOP              AINOR              DE                 BEMNQST            BCDIJORT           CIKNT              CGH                ADFI               CFHKLMT            AOPQT              CGL         	       DEFGJMPQS          S           	       ADEGHNPQS          LT                 HINO               AKMOQ              BFKNOR             JQ                 HJOPQS             KO                 FOT                KMS                AEIOST             AEFJKNQT    	       AFIKOPQRS   
       BCDEFHKOPQ         BCDEGMRS    
       BEFGILOPRS  
       CDGHILMQST         CDEFKLNO           ABM                CDEGKMNQ           CGIMOS             ABEGJLQR           AG                 CINPQ       
       BDEFGHIMNT  	       BEFHIKOQR          DHIJL              A                  AM                 P                  EGMT               EFKMNT      
       CDEGHJKMNP         AJ                 ACDFMOS     
       ABCGKMQRST         DFHJS              BEMQ               CDGLMN             N                  E                  ABFIMNPS           S           	       ABDFHIPQS          QS                 BCEFGST            FGHIJKR            EGHKPST            FJK                ACDEFKLT           AEFIOT             ACEFGMOT    	       ABDFIJMRT          ACIPQST            CELNQST            BGIJS              JP                 AGHLNR             EGHJORT            EKN                G                  C                  HJLOP              BJNPQS      	       CDEJOQRST          FKP                AFHMORT            BDEFG              FJKL               FG                 FNQ                ABC                GHIJKLQT           CEHIORS            L                  EFIMPQS            P                  ABMQR              DGJKLNP     	       ABDHJKNOQ          BEFPR       
       ADGHJMNPRS         BIP                CDGIJKPS           ADEIJNQT           ACIKMNPQ           C           
       BFGIKLMPQR         G                  T                  HOT                NP                 AEFHJPR            ACEGLT             GHNT               AGJKMOQS           EHMNO              GIMT               HPT                JN                 AEMPS              CDGHNO      
       DEFGHLMNOT  
       BDEJKLMOQS         ABEIKLQR    
       BCDJKLMNPR         LT                 K                  ACFIKS             AEFJKLNS    
       DEFGIKLQRS         FJKMNP             JKORS              BHMNOPS            FMNP               EHLQ        	       ADEHKOPQT   	       ACGHKLNPQ   
       CFGIJLNOPR  
       CDEFGIJLNT  
       ACDEHJLOPS         J                  BCI                DOQT               A                  BGLST       
       BCDIJMNOQR         AHLT               BCEHJM             T           	       BDEFGKLRT   
       BDEGJKORST         AS          	       BFGHIMOQT          I                  CDFGL              BCFKLPS     
       CEGJKNOQST  
       CFGJKMQRST         DEHJMOPQ           CJ                 ACEGIJLP           GIJNQST            BILMNPRT           AR                 ACFHLOS            AGILNPT            DIKLS       	       EGHILMOQR          ABCFGHJR           EJ          
       BDFHIKMQRS         AFILNPST           EGHIJKNR           BCEHIJKR    
       BGHIJMNOPT         BEMQS              N           	       DFHKLNORS   
       AGHJKMNOQT         BFM                AEGIJ       
       ACDEFLMOPQ         ABDEFIJM    
       DGIKLNOPRS         CKP                CDFMR       	       CGIKLMOQS          ABKO               ACEJLORT           ABCFILM            CDEGI       	       AFHIKLQST          GORT        	       FHIJKMPQR   	       BEFHIMPRS          BCGHILMR    	       CDEGIMNPT          PS                 ABDFIR             K                  ABILRST            BCHJN              BCOPQ       	       ADFGKOPST          GS          
       AFHIMNPQRT         DKMNST             AJO         	       BDEIJKRST   	       ABGKMOPRT          FKLPQT             CFGHKLOT           COPR        	       CFGHIJQST          IQ                 CELS               BDGJNR             ABCFKNST           FJQS               ACM         	       ACEHIJNQS          BFLNQ              CDFGNST            ACHNRST            P           	       CDFGKLNPS   
       CDEGHJKMOQ         CEHJOP             EFMT        	       ACDGJMOQT          DFIKLOQR           DEJLOQST    
       CEGHIJKORT         DGST        
       ADGIJKMPQT         DLRST              AEKP               FHILMORT           ADGHJMO            BDFHJNT            DEINQRT            CEHIJLMT           GN          	       ADHIJLOQS          AGHNS              AFGJKPQT    
       CGJKLMNPQS  
       ACDHJLMORT  	       BEFKLORST          BHKLOS             A           
       CGHIJLMNPQ         AKQ                DFGHKLN            KMP                BHKOQT             IORS               AEGIKOQ            AKQS               CGHNQ              EIJOP              EN                 AFT         	       CFGHKMPST          ILMNPQ      	       DEFIJLORS          BDGILMT            M           	       AEGJOPRST   
       BCEFGMNOPR         S                  AJP                DG          
       ABEFHJKMST         BFHLNQS            HLR                BCGHPQST           I                  A                  M                  CHMOQS             KOT                COP                AFIJMORS           AFKS               DGKLT              CKL                CHT                DO                 R                  ACHLOQS            ACDEGI             DI          
       ABCDHIKOQT         CEFGINOR           BDHIKLM            ADJMNPQT    	       BCHKOPQRT          HMNOQ              CEFHJLS            KL          	       BEFGHMNPS   
       AEFIKLMNOT         HMNPR              CIKLOPT     	       AEGHIJLRT          LNQ                IK                 BGLMNT             JKNRST      	       ABCDHIKNQ          ACDGKN             DFIK               DLNPQ              CEFGHLMR           DJOR               BDHNQR             AJPQ               EHIKT              KPS                H           
       EGHJKLORST         GIKOPQST           NP                 BCHIJLMQ           DNOPQST            FIMNOT             CFIQR              DFGILMP            DHOQ               BH                 FMQT               ACFLMORS    	       ABDGHKLMP   
       CDFGJMNPRS         AEIJLNR            FLPQT              BEFIT              AN                 CDGHIKNQ           ABDKMQST    	       BCEGIMQST          ACMOQ              BFGILMR            KOPQ               GNQR               DFGIJLNQ           DIL                N                  AEFKMNOP           GOQ         	       BFGHJKNOQ          BDET               ACEJRST            CEFK               AHLMQ              J           	       ABFGHIKOS          ACEFMQRS           AELPRT             BEHINR      
       ADJKLMOPRS         ABCILRT            G                  DPR         
       AEIJKLMNQS  	       DGHKNOQST          L                  ACEGHKQ            DGKMNRT            BDJOPQST           EFJQR              ACIKP              CDHIKLST           CDEHMPST           DFP                P                  B                  EPRT               MS                 ACDJMNT     
       ABFHJLNOQR         AJKLRS             IJNOQT             CEHKQ              M           
       ACDEHKNOQT         F                  IKLMPQR            ADEGK              AQ                 FHIJKLMN           F                  CDEGIPQT           EFGIPT      
       DGHIKMOQRS  	       DEFJKMOPR   
       ABCDFGHLMO         ADFJKO             FGLMQR             DP                 AEKPS              MT                 LMOPS       	       BCLMNOPQT          ADEGHKMN           BEFJO              EFJKMOPQ    	       ABFHJKQRS          CELMNT             CHT         	       BEGJKLNOP   	       BHJKNOQRS          F                  D                  CDIKOPT            EHS         
       ABEHLMNPRT  
       AFGIJMNOQS  	       DFJKLMQRT          P                  EFJS        
       ABIJKLMOQS         DT          
       AEGJKMOPST         BFMNOPR     
       ACEGHLMPST         AEHKORT            ADQT        	       ABDFGHLOP          I                  DGJLQS             GKNS               ACGKT              M           
       ACEKNOQRST         JOS                BCEFJNOR           G           
       AFJKLMOPST  
       BCEHJKLPRS         CFNT               IJKL               HT                 CGHMQST            S           
       BCDEHKNOST  
       BDEGIKLRST         AGHIJMPR           GMPR               BIJQ               R                  C                  O                  BEIL               ACFIT              B                  ABCJLPRS           ABDLMNRS           ABCEJKM            DPT                FGHOT              BKLNPRST           HIKRS              FGOS               GHRT               ABCFP              JMO                FKLM        	       ABCHINPST          DFKLNQT            AHJPST             ABDFHJNT           ELMNPS      
       BCHLMNPQRT         ER                 GR                 CEFJKNP     	       ADHJKMOQS          BEH         
       DEHJKLMRST  
       ADHIJLMNST         BEIMO              BEJK               BIJLOT             DENPR              BCGIKLPR           ABGILR             DKLMNT      
       ABCFGKNQST  
       BCDEFIJMRT         DEIKT              CDFILMPT    
       BEFGHIKLOR         ADFGMQ             E                  ELOPQT             BCFILNPT           IMNT               DGHMPQ             CDGLRS             EJLOPR             E                  DFKMPQST    
       AEGHJKMPRT         BEIJKMR            EGIPS              IPS         	       ABEIJMPRT          ADIKNS             CFNP               CMPQR              KLQT               CDEINOQT           BCEFMNOP           ACGIKQST           ABDFJKNR           L           	       CEFGHIMQR   	       BDGKLMNOQ          JK                 CEFHKP             GJOT        
       ACDFLMNOPS         EQT         
       FHJLMNORST         BDFJKPQR    
       ABDFGHIKMP         ABNQ               ACEFGJK            HILPRT             GNQ                FJM                CGO         
       CEJKLNOPRT         BHKOR              BJT                E           
       ABHIJKLMPR         CFGJPQ             DE                 KS                 BM                 QS                 NT                 HKP                ADIKLR      	       CGIJLPRST          CGLMNOPR           ACGK        	       ACGHJLORS          AIJLMR             CEHQRS             AEGIKMT            FH                 DR                 ST                 FHIOQT             KOQS        
       CEGIKMNOPS         EPQ                AHJMS              DGJS               DJKLMO             ABEFJLOR           AK                 DFGHJQ             R                  DFIKLOT            BEGMOQST           J                  FJNS               B                  ADFI               P                  FI                 K                  DFLMS              IKLNPRT     	       ACDGHKMPT          DFHJKMR            HS                 BCDFIOPR           ACDELMPQ           DG                 DEFHLOQ            CDFGJT             LS                 DGILOQ      	       ABDEGHKOS          O                  JKP                CDJMR              DR          	       EFGJLOPRT          ADEGLPRS           AFLMNOQT    
       ABDEHIJKNR  
       ACDGIJKMPS         BENST              NR                 CDEHIMST           L                  BF                 AKO                IJMR               BCDEHP      
       CEFHIJKLST         M                  BKMP               ACET        	       DEFGIKNST          AEMNPT      	       EFGIJKORS          JK                 BCFJQ              H                  FJNQ               ABCGHKQ            ABIJOPS            IKOQ               CKMP               S           
       ABFIJKMNST         CDLOPS      
       ABCDFGJLMP         AEGILM      	       ABCEFHKST          FN                 JMR                ACDEFGNP           BEFGLMP            DGLOQ              IT                 CFGHJ       	       ACDEGKLNR          DHLMNS      
       BDEFHMNRST         JL          
       ABDFHJKMPT         S           
       ABFHJMOPRS         EIJKMPQ            GP                 AFIJLS             GHKLMOT     
       BEFHLMNOPQ         GI                 ACFIJOT            BDGJKMOS    	       ABHILMPQT          BK                 DEJQ               AFLQRS             AEGHIKMR           OT                 BCMNQRST           R                  FQR                CEFGHLR            JPQ                ADHIPQRT           ACEINORS           L           	       CEGIKNPRT          LQ                 F           
       ADKLMNOPRT         KMOQ               AEGHKPS            ACDEGLQ            CDFHIKL     	       FGHKNOPRS          CEGILMPQ           DILP               AJ                 DO          
       ABDEIMOPQS         D                  CHNPQR             AS                 AFHINPRT           FGJMORST           GIJMQR             AFLNPRS            DNR                CFRS               AEJKLMNT           DEFGLNPT           BEFHJLNT           CGIJ               BPQ                CGIQS       	       BCDHIJMNO          ABFHJNOT           BEGPR              LQ                 EFNOPT             JK                 AIS                CJK         
       BDEFLOPQST         DGILMRT            ABHIMR             BCEIJT             DIPR               I                  BDFGJ              GJMQ               DMPR               DEKMQRT            DN                 KLNOQ              FGHIL              BMR                ABHMS       
       CDGHJLQRST  
       ABCDEHIJLR         M                  ABFGT              ABKNR              BEFILOS            BHILPQRS           AJQ                CLOQ               AEKLR              AFKLOQST           HKMPS              ADFLQ              CGHKM       	       ACFJOPQST          BEKNRS             AGKLNQR            JM                 HPQRS              DEKOPT      
       BDEFIJKNST         AFGHKPQS           GJ                 CEINPQS            CEILMNO            CEFGMNP            CIKM               BK                 FIQ                DK                 AHIK               AJNR               BDEHLR             J           
       DFGIKLOQRS  	       BDEGHIJMT          AG                 ADLM        	       CEFIJNORS   	       CDKNOPRST          GKS         	       ADJMNPRST   
       BCFGIKLPRS         I                  LQS                ACDLNO             AL                 AHIOQRS            ABGLOT             DEHJKMNP    	       ABDEHKOST          FGKLOQT     	       BDEFGHKOQ          JQ                 ABGHIJMT    	       BDEGJMPST   
       EGHJMOPRST         C                  IM                 CKLQT              ABDEFRS            JL                 ACGKMRS            FG                 DEHLN              LO                 CFKMOPRS           JQ                 JK                 AFIKMQRT           AIOQRS             CDEGIOQT           ACEHKN             CFHJLPST           AFG                GLPRS              ACKMOQRS    
       BCDGHJPQRT         GIKLMNR            FIKNO              BEGIOQR            BCL                GKMPQR             BCEFGLMT           GP                 KR                 ABF                DPS                EJKNQRT            DJMNST             ABHJKOQT    
       ACFGHJNRST         AEFGMST     
       ADEJKMOPRS         DFKP               DEJ         
       DEFHIKLNST         CFGLOR             ABHIKNPQ           ABFGIPQT    
       BCDGIJLNQS         JMQS               MT                 E                  AEFILPQ            JMNT               AJMNRS             GIJMNS      
       CDGIJKLPQT         AS                 DHLP               DGS                AIP                AFGIMNPT    
       BGHJKLNPST         AHOPQ              F                  O                  DJLOP       	       DHIKLMNOS          O                  HO                 DEIOPST     
       ACGJMOPRST         DFGHJKLR           BHIMN              ABCEFIR            J                  BFGHMNOP           HI                 CEGT               DO                 ABHQR              HIMOS              OP          
       CDHJKMNPQR         BKPS               AFHINT             CDGJNOPT    
       BDEILOPQRS  
       BCDEILPRST         BCIJM              ACDGHK      
       AEFHJKLMOP         GHJNQRS     
       BEFHIOQRST         CGLNOPRS           F                  G                  ELMOR       	       ABCGIPQRT          CIQR        
       ACEFGHIJQS         CDENPQS            N                  BEHILNT     	       AEFHIKLQS          ABFN               BD                 ADHIKR             ABEHJKQR           CHIN        	       BCDEHKPST   
       DGHJKLPQST         BHIMNO             GJMN               AGH                P                  KN                 DGIMOQR            AEFKMQ             LR                 D                  KR                 F                  BR                 DEHLNR             DEFJNOPR           BFM         	       BFGJLNOQT   
       ABCDEJLMNR  	       FHKMNOQST          EHJKLNOR           NS                 BCN                I           
       ADHJKLMNPT  
       AEHJKLMNPR         DFHT               DGLOP              AFHNOQRT    
       ADFJMOPQST         O                  BDHIJKNQ           BFHKLT      	       CDEGHNQST          CHLOR       	       AEGJKNPQS          P                  B                  MP          
       AEGHIKNOPS         EPS                JN          
       CEGIJKLQRS         LQ                 CIQR        	       EFHJLNPRS          BNO                CDFJQRT            F                  CDEGJKL            BIMQ               AGILQRST           EGJ                CFIJT              MP                 CDFGLNPQ           BI                 DIR         	       DGHIJKLMO          BCFGMRS            ADEMPRT            DEILOP             BGKRS       
       BCDEFKLORT         KM                 AI                 H                  BDS         	       CDGHILMPS   
       ABDGHJNOST  
       EFGILMOQST         FGLN        
       ABDFGMNOQR         HK                 BCEFJLNO           DHJKLO             D                  H                  ADGJL       	       BFGHJKNQS   
       ADGIKLPRST         LMOQS              ACINR              DEHILNQS           GHIKLT             CDI         	       ABEFGHLNQ          ACHJOQS            ADHJKNPR           NR                 I                  BEL                HMPQT              AOQ         	       EFGHIMNST          BGJLMNR            BEFJLN             ACLO               EIP                ADGOPQS     
       ABCEGIJNOQ         AEFGINRT           BELNO              AEHILMOP           GIJKLMN            ACIOR              BFHJPQ             GR                 BCFMOPQR           AFGKLO             AJ          	       BGJKNOPQS          AEJK               ACEOQST            MQT         
       BEFHIJPQST  
       ABCFGHKLMT         FPRT               GHIMP       	       EFINOQRST   
       BCDFHMNOPR  
       DEFGHIJNRT         EIPST              AB                 AGIO        	       ACGHMNPQT   
       ABCDEFJKNP         ABCHJPT            ABFJLORS           GM          	       CEHILMNOS          CKS                EGHIPQT            GOS                BCMOPT      
       ABDEFMNPRS         CFG                E                  BHMN               CDEFGHLO           A                  ACGHIJKQ    
       ABCFJKNQRT  	       ACEMOPQRS          BEHLMO             EFGMO              CFJO               BEGN               T                  EFHKLNPQ           ABHNQRST           T           
       ABCEFLNQST         DJLRST             EFJM               AFGNRS      	       CDHKMPQST          GHPS               CLMPR              ADINQR             ADGIOS      
       ACDEFGHIKM         I                  QT                 R                  HIR                ABGHKLP     
       DEFHJKNOPR  	       BEFHMNOPS   	       BCIJNOQRT          S           
       ACEHIMNOQR         BFILMQ             JS                 ABCFILOR           BEO                FOP                DMNQT       
       GHIKLMNQRT         LMNQ               DFM                KRT         	       BCGHMOPRT          FHIKMNR            ABT                BEK                ADL         	       BIKMNQRST          BEJMOQS            D                  Q                  DMS         	       BCFGIJLNP          BHLNPR             O                  J           	       EFGHIJNPQ          I                  ACDMRS             HKNR               JMNR               BHILNQRT           BC                 AFIJPR             HOQS               CEFIL              DHIKLOR            DJKMPQR            FM                 FP                 ABCGKPS     
       CFGKLMOPRT  
       BCDHJKLOST         R                  N                  ABCEFHM            CGQST              AOR                OQS                BLN                L                  BDQ                BCDIKMNR           DHIKNQRT           ABEGIKT            ADELNOQT           M                  CGHLO              AN                 CELS               CFGMPQ             ABELNOQ            C                  CFPR        
       ADFJKLMOPT  	       ABFHMNPST          CDGHT       	       BCEFGHIJP          NQS                DJLOPS             GHLMS              J                  ABCQ               O                  DOP                D                  DEGKN       	       CDEFILNQT          ABEFGJMN           GQ                 CDEFH              ANO                EGKL               BDL         
       BCEFIKORST         AFGJKN             IKMR               CDGNR              BCGRS              I           	       ABCEFIJST          BEFHIMP            AFGHJQS     	       AEFGHORST          CJ                 HJLT               DEKMO       	       ABDJLMPRS          AHIJKPT            Q                  BIKS               ABCJLOQ            CDK         
       BEFGHLNPST         AFGHLNOR           LR          	       ABGHMOPRT   	       CDEFGIKLT          BGHJKS             CEIJKLPT           LT                 CGIQS              GOS                CGJLPT      
       ADEFHIJMNO         G                  DGJMNPQR           GMPT               BJ                 GIN         
       EFGHKMOQRT         BHNQS              ACMOT              Q                  IJLNPR             GHKLNS      
       ABDEHJKLMR         BDIN        
       BDFHIKOPQS         O                  AHJKMN             KRS                EKOQT              BCFKOP             CFNOPQRS    	       ABCKMOQRT          GHKLMNS            DELOR              IT                 BCFJKMS            J                  BDHJPST            KQT                DIR         
       BDGKLMNOPQ         G                  DEFJPT             DGIKOPT            FNRST              ADE                EGR                CGKNQRS            DEF                BCFHLNPQ           BIMNOPQ            G                  DH                 HPS                AFJNT              DFH         	       DEGHKNQRT          EKR                CEHLNO             KNR         
       BEFHMNOPQT         EILN               DGR                ACFHIJMS           CEFHNOR     	       ACEFHJNRS          DN                 CGT                CNO                FGKQRS             EFINO              BPST               R                  EHMNQ              ACDIKLMO           AEGHST             LNOPS              DK                 CDFMOQ             IO                 FHMS               EO                 ACFGIJNP    	       ACDHKOPRS   
       BEGILNOQRS         ACGJKQS     
       ACEFJKMOPS  
       ABCDHILMNQ  	       ABDEHKMNR   
       CFHIJKNPQT         HNO                AHKNRS             FGINT              AB                 AEKQ               CEGJMNR            G           	       CDFGKMNRT   
       BDHJKMNOPQ         BCDFHKMQ           AEFGINRS           GHMN               BFHKOT             ABGHJ              LS          
       ADEFHJLOQS  	       DFGJLPQST   
       BCFGJKNQRT         EGHJOPRT           BIKLM              ABIM               CGHJKOP     
       ACFHKMOPQR         AILNP              ABGLNOQT           CDHJS              FGHJOS             I                  CDEFOPQR           BKOQS              ABDGHNOS           BCQS               FHNT               ABCGLPR            GHIJKNQT           DFGIRT             GJRT               FJL                FIR                HIMPRT             EFGILPQ            CJPT               CDHINO             AFHL               AP                 ADMNPR             CNP                BEKNOPT            AFJKLMR     
       DEILMPQRST         GHJLP              CFKLM              CGHR               BHLMQRS            BCHIJKO     	       FJKLMQRST          CGKNQ              OQ                 GLPS               GJMS        
       ABDHIJLQRT         DJMNOR      
       BDEFIOPQRS         ACDJLPT            BEGMNPQR           ELNQT              AF                 FHKNOT             AFGHIQR            ES                 JM                 BCDKMST            CDIJOP      	       CFGHJKPQT          DKT                DEGIMOPT           HM          
       ACFHJLMOPT         CHILOPS            ACIJMOPT           ADKNT              FJKN               CELNQT      
       BEFGHIJNQR  	       CFHIJKNOQ          BDK                EFHIKMN            DFJKLMS            BGMRS              DK                 BCEKOST            DLPQ               GIKR               ABKLMNQR           ADFHKM             GHIKNRT            EN                 BDEIJNST    
       ABDHLMNOPS         CGHIMS             BCFIJKRS           AKRST       
       ABDFGHQRST         AFMNQRS            AHMT        
       CDEGHINOPS         CDGOS              ABEHIJLO    	       ABEHMNOST          LOT                FNPQ               BDIKMOQ            BEFHO              ABNPT              D           
       DEGHKLNOQR         M                  CEHJKST            AEK         
       BEGKLMNOQS         BG          
       BDEGHIKPRT         BGJL               D                  I                  JN                 KR                 MP          	       ABGIKMNPT          AQR                DEMR        	       BEKMOPRST          CGHJLQR     	       CFGIKOQST   	       BEFGJKNOP   	       ABFIMNORS          BIMOQST            MR                 CIQR        	       ABGHJLNOP          BFHJST             DFH                EJS                IMRT               CEJKMNR            EKPT        
       ABFHKLMOQT         CM                 G                  FGK                CEP                EFHJL              CJN                GI                 ADEIJQRT           HIK         	       ACDEHIJMP          ACDHKMOQ           APS                CDOPT              BEMNOT             FMOS               CEGOS              EFHILMS            DHIJLMT            LNP                H                  ABHS               DEOPQ              S           
       BCDEFHIORS  
       BCEFHIKOPT         AEJQ               CGMPRS             BEFGJ              EM                 DFLM               BEGLPQS            ACEK               J           	       EIKLPQRST          EHIKQRST    
       ABFGILPQST  	       BEGHKLPRS          FI          	       AEHIKOQST          B                  EJL                AEILMNQS    	       ADEFGIJLM          B                  GLOS               AELOT              EJKLPQT     
       BDEILMPQRS         ACDGOP             R                  R                  CORS               ADKNOP             FGHJ        	       ACDEGIJLS          AL          
       ABDIJLMPQS         FINPRS             BEIKLQ             CDFGJPT            EHJ                CFIKM              BFMO               FR          	       DEGJLMQRS          ILOST              EFHKPQS            DGIQT              GKMRS       
       AEFIJKLMOP         ACFGHJ             AFLT        	       ACJLMPQRS          ABCDGH      	       AFGHKOPQS          ABDGILOS           AEIJOPT            EFIKLOR            ER                 KP                 AEJPT              AHL                KP                 K                  R                  GHIR        	       BDFGIJLQT          ACDILQS            CHKR        
       BCEKLMNPRS         S           
       FHIJLMOQRS         GOT                ALNOQS             ADIQ        	       ACJKLMNOT   
       ABEHJKOPST  
       CDEJKLMNST         AIJO               JOQRT              CIJ         	       ACEFIJKMR          CFGJOPQR           BGHKPT             FKLQR              AFIOS              R                  LOPQ               DHLOPRT            CFHIORT            ABHIJLPR           BDELMNQT           GLOPRT             CDFHJLOS           CEHKQR             FHINO              EHM                ACDEGINO    
       ABCDINQRST         CEKQ        
       ACEJMNPQST         CFMT        	       AEIKLMOPT          BGLMNPQT           ACEILOT            DNORT              DHIJQ              KR                 DFKMT              IKLOR              CDFHJMN            ABCFIJMT           BCFGKLS     	       DFGJLNQRT          KS                 AIJKMN      	       ACIJKOPST          FGKM               GIJ                DGKMPQT            K           	       AEHIJKMOR   	       ABDEFJLOP   
       DEIJKNOPQR         BHLNOPST           GHIS               QS                 DFKPS       	       ACDHJLNRS          D                  C                  EKOPQST            HLMNPRS            ACIJR              BJKOPST            ABDJKMOT           CFGMQS             PS                 EIJMNRT            J                  ABCFOP             HN          
       ABDEIKLMQS         FGS                ENQ                BEFI        
       ABFHKMNPQR         GN                 DT          
       ADFHJMNPQS         ABHIJMO            JKLMPQS            ACFIKMNQ           QR                 G                  DHN                CIKO        	       BCFJKLNPS          B                  BGHT               J                  M                  AEFGM              EQ          
       BDFJKMOPQT         ABDLQS             P                  JS          	       ADFKMPQRT   	       AEFGILORT          HJKLQRT            EHJMPQ             HK                 GLR                DGPS               BCGHIKM            FGI                BCGQRST     	       EFGJLMQST          LN                 K           
       ADFHIMORST  	       ADGIKMNQT          FGST               FG                 AFHLNS             BDFIKQST           AGHLNRT     	       ABCFJOPRS          AJKLMOP            ACHLPS             CKLP               BJKQR              M                  AP                 HS          	       BCDEILOPQ          GHNPQST            BCGNOPQ     	       ABDEFKLNP          GHQ                NOQT               BH                 BEGJKORT           AIK                FJR                IS                 ABDR               BCEFGLNP           O                  FK                 AGIOQT             CDKPR              BFGO               HIO         	       AEFMOPQST   
       ABCDEGJNPT  
       ACDGKMNORS         EJNPRS      	       BCFHIKNOQ          CHJKLMPS           AS                 BF                 CEHS        
       CDEFHIKOPS         E           
       ABDEFJPRST         BCGLMNO            GIJQT              ACHNO              EL                 ACJNOP      	       CDGLNORST   	       CFGHJMNQR          EJ          
       AEFGKLMPRS         MQ                 DF                 KL                 ADH                ADEHLT             ABCFHNPQ           CDEGHJNR           E                  BH                 D           	       ADEFGIQRS          CDK         
       DGIKLNOQRS  	       ACEFKMQRT          BFT                CDEMST             CFN                ILNPQ              ABCFLNPR    	       CDFLOQRST          BCFJPQRT    	       ABCDEIKMN          FO                 T                  ABDGHS             R                  BDGKNST            ABCFJRT            BEHQRT             FGL                HJ                 DRST        
       CDHJKLOPQS         AN                 EJK                CDGIMOR            BFS         	       ACDHKMNQS          ACDFKLQ            DJ                 DG                 CDFJKMNP           DEIKMNOP           JQ          	       BCFKMNPST          KMNQR              CEFGIQRS           BCOQ               ACEILT             FGHKMP             GIKNST             AKPQRST            FHINPQRS           AIRT               HIKLPQST           FGH                BLMOT              DHT                NS          	       BCFGIMPRS          BDNT               BFHJLPQ            EQ                 CFMPQR      
       BEFIJLMORT         AB          
       ABCGHIKOQT  
       ACFHKLMOST         ANP         
       FHJKLNOPQR  
       ABCDFHJMPS         ABDFHM             AFIO               AJP                DELMQR             DJKLP              BK                 F           
       ACDHIJKLRT         L                  EGK                GHJLS              S                  MORT               Q                  AJKLMRT            A                  DEFGHN             Q                  AGIKLPS            DEFHKNQS           GKMST              N                  M                  ABEFJKM            CEHILOT            BFIKO              DMR                AGHIJKMR           DIPQT       	       ACFHIJLRT          BKNS               DF                 N                  ALNQ               ACDQ               AFHI               CDFHMNQT           BDEFIL             DEJNPS             DT                 BGLNQ       	       ACDFGMOQT          HJST               BGLMNR             BDFINOPR    	       ABHIKLNQR          BDILQRS            CGIJO              EGHINRST           N           	       ABCEHKNOP          D                  DHLO               ADNR               CELOQ              ABCFGQR     	       ADGJKMNQR          JR                 CEFHJLPQ           BCDEJKQS    
       BDEFGMNORS         BHKMOQ             ABEIJ              BDFJNORT           FGHIJKT            CS                 BDFMNRS            DGLMNO      
       ADGHIKLMQT         ABDHJQRS    
       AFGJKMNORT         DEFIKLMP           IKLN               ACFIKMOP           KNOP               K                  FKLM               AS                 BGKLOP             CEKNOS             ALS                DFJKPR             O                  HJKMQ              EFHIJP             FHJMQRST           CDEI        	       ABDEKLPST          HJ                 CGHKLNS            BGK                IO                 BEGJO              AQS                AGJMPR             FHIQT              LP                 LOQ                DHIMPQRS           CDKOPQS            FGHJLMN            CEHNS       
       ACGHJKMNOQ         BMS                B           
       EFHIKMNQRS         CDO                GIQR               CEKNP       
       ACDFGHJPQR         EN          	       BDIJLMOPR          BJPT               CJNT               L           	       ADFHJKLOP          EFH         	       ABHKLNOPQ          LQ                 BFJMNORS           C                  ENPRT       	       ADEHIJKMQ          HR                 ACGHIJST    	       ABDGJPQRS          ADFGIJLP           CDFNPQS            BCDGILOT           AIMP        	       AFGHIJLOR   	       BCEFGHJQS          DGT                D                  DGQST       
       ACEGHLMQRT         GQ                 FLPQRT             BEFGKMPQ           ADJMR              EHNPQRT            CFKN               AFHMOQT            CFGHLMOP    
       CDFGHKMNPQ         AKMPT              AOPR               GHJ         
       CDEFKLNOST         LMNQ               ADHLMS             ET                 BO                 ADEMOQ             DGIJKRT     	       AFJKMNRST          BJKMR              ACDEHIJQ    	       ACGKMOQRS   
       ABCDEJKNQT         ABEGNR             BFHJO              O                  BGHILPQ            LNRS               CGOR               BFMNO              BFHJMOP            EFGMRST            R           
       BDEGJKNORT         EHOQRS      
       ACDEHIKMOQ         HIKL               BCDHOR             BGILPQ             MOQT               FMS                DI          
       DEFIJLNQRS         CGMQ               BCEFGNT            ABHJKLPS           CGLMNOQR           Q                  CIQR               AEGKMNOS           BGLRT              AGLO               P                  KLN         
       ABFIJKNPQT         S                  JNR                EIL                AGJOPST     	       ADHIKMNOQ          LMS                CDFIJLR            ABDIKMRS    	       CJKLPQRST   
       BCFHILNOPQ         GQ                 C                  HI                 CDGHMNST           ACDHO              JP          
       BDFIKMPQRS         FGHPR              EP                 DGIKORT            HMOR        	       FIJLNPRST          KMQRST      
       ABEFGMNPST  	       CDGHIJKPT   	       ADGHIMORS   	       ACGILMPRT          EFHLNOQ            FGHIPR             FGJKLP             BEGPQ              BILMNRS     	       BCIOPQRST          IO          
       ACDEFNPQRT         CEGIMORT    
       CEHJKNQRST         CDFLNPQT           AHKNT              A                  BGKLPS             ABINOQRS           DEIKMNQ            DJKQ        
       BCEGLMQRST  
       DEJLNOQRST         EHLS        	       DEGIJLNPR   	       BGIJKLOPT          CFKLMOS            CHIO               DH          	       ADFJMORST          DFJKP       
       BFHJKOPRST         I                  CIKMNR             CFILMPS            DM          	       BCEGJKOST          DEGJMNT            N                  N                  ABFILMNR           BCFMNOP     
       ACDEILNQST         JT          	       ABCFKLMNS          OQ                 FHIN               DT                 L                  CLPST              ABDEHIM            BCHR        	       ABCFJLOPR          HILOPQST           ABDIJS             DEKO        
       ABHKLMNORS         MQR         	       ACDFGIMPT   	       ABDKMOQRS          ACKNQ       
       EGIJLMNOPQ         C                  BCEIOQ             ADMPR              CFIL               ABCFRS             HS                 JKLPT              ABDEFGMT           DFJL               KMPT               BCGM               BCDNR              EHJR        	       ADFGHNPQT   
       EFHJKMPQST         BCN                BEHKMPRS           AGT                AFIKLS      
       ABEFGHJKOR         DLPR               DRT         
       BCEHLNPQRS  
       CEHIJMNOPS         DR                 DKS                AEOR               CFN         	       BCEJKNOST          BGIJK              FJKLMNT            BCFHN              DKQ         	       ACHIJKLOP          CEGHKQRT           CEL         	       BEFGHJMNT          EHMS               BGJKMNPR           BDJKO              DFGJKOT            EIJKOPQ            B                  ACDGKNS            GHIMN              EGH                AEFJMQT     	       AGIJKMNQT          DEFGJNO            HMT         
       ADFKLMNOQS         BJLOQST     	       ABCMNOPQS          AG          
       BDEFHINORS         BEFLMOPQ           GHKMT              FIKS               EJOPS       
       ABDGHIKPQT         ACDIMNPQ           GOT                ABJQS              EFILNRST           HIJQS              I                  EFNS               CEJMR              J                  FK                 BHIJKRT            CHKNOR             CT          
       BCFIJKMRST         BNT         	       BCDILORST          CELMN       
       ACEGLNOPQR         MN          
       BCEFGLMNQR         AMN                CIJOPRST           EGHIJQST           CILQ        	       ABDHKNPQT          ACGJOPQR    	       AFGMNOPST          AEGS               CDLNRST     	       ABCENOQRT   	       AIJKLMOQS   
       DEFGIMNPRS         ADEGHQS     
       ADEKLNPQST         CGJKPR             ABHJK       	       BEFHKLMPQ          BDGHKNT            OR                 CKS                GK                 EGIJQT             DLMPRST            ACHILNRT           BCEGS              EMORT              ILQRT              JKLMNR             DJL                L                  AH                 MPRS        
       BIKLMNOPQT  
       ABCDEHKNPR         FIJMNR      
       ABCHJKNPQR         AFHPQ              D                  AEGHJLMN           BM                 DFO                CDER               CEHNOQ             BCEKMOQ            DPQ                GHT                GQ          
       BCDEGHOPRT         DHKMNORT           IKQS               GJLP               E                  BDGMOQT            CEGJMOPQ           DKMNPRT     
       ADEJKLNOQR         G           	       ADFKLMOPQ   	       ABEGJOPQS          AIKMPS             HI          
       ABFIJLMPRS         BCDFGNQ            EF          
       DGIJKLNOPT         DFHKMQRT           ABCEJMQ            AEGIKLO            BDFKMST            EFHO               JQ          	       ABDJKORST   	       CDEFGIJNT   	       ADGHLNORS          CEFIJLS     	       ADHIKLOQT   
       BDEFGKLORS  	       DHJKNOPQR   
       BCEGHIKMNS         DEMPQRT            HJM         
       BCFGIKMPRT         BDPR               CGIJMOST           DMN                BEMR               EKN                BNRS               BO                 BDFN               C                  DFGKMPQ     	       EFHKLNQRS          CGLMORST           K                  BF                 BGJ                CD                 CDEKLNOT    	       BCEGJOQST          CGHOPQR            BFLN               FGHIJPST           CFMS               ADGIJLMR           KQ                 FHIOST             AHQT               DFKNPQR     
       DEGHIJOPRT         EFGIKNR     	       DEFHJOPQR          GHP         	       ADEIKLOST          AI                 AEILOPR            KL                 AEHJ               BHKMQ              ABCINQS            L                  CDEM               BCDKNRT     
       CFIJLMNOST         EILMPT             BHKM               ABCIKLP            IJO                ACDEGLPR    
       ABDEFIJMOQ         CDEFHT             EGHJKNPR           DEFJOPR            AIOQS              LO          
       ACDEGINPST  	       BCFIJLMST          INT                DFINPQR            IP          	       ADEGNPRST          DNPT               GLRT               ABDHQT             Q                  AFJS               DMT         	       AFGHLOQST          HKT                CEJLMPT            AMS                ABEILNOQ    	       ACDGJMNQT   
       BCDEFOPQRS         FINOPRT     
       ABHILNOPST  	       BDEIJOPQT          DFHILM             CGLMNPT            FHL                KMNQ               CDP                FGOS               HKMOQS             AEFNOQ             AGLRT              CIKMRT             ACDMOPQ            AIRS               BDFGJRT            T                  FIMPR              J                  AHJKS       	       BDEGIKLRT          BFGHLNPT           AHM                DHJNR              KP                 CEO                HJT                BHJLT              ACHIKMT     	       CDFJKNOPR          BDLQR              CDKMO              FILMQ              DNQR               CFJT               ABKLMQT     	       AEHILNOPS          DIKMQST            BCDFGJLM           K           	       BCEFGLMPT   
       BFIJKOQRST  	       ABCDFGIKQ          ADEHNR      
       ACDEGJMNOP  
       ACDHIKLNOS         JO                 OQ                 BGR         	       BEHLOPQST          GJLP               LO          	       CEFHJKLNP          F                  FHJKMNPQ           ABDJPRT            BJKLPS             GP                 EHJOPQS            ABCDFGHJ           IMO                ACEHKOQR           DEIJLNS            BEFGILQ            ABFMOR             KQ                 AEFMT              ACGHM              MPT                BCDFHLP            ABMT        	       ACHJKLMST   
       ABCEGHMORT         JKLPQT             DEFOP              BHIJLOR            FNQ                ADEGOS             ACDJMS             AP          	       ACFHIMPQS          BCDJNQ             AFGHKOP     	       AFIJKLNPR   
       CHIJMOQRST         EGHMR              BCFG               DEFHKNOR           E                  NO                 BCFKLNPQ    
       BCEFJLMNOS         EFHJMNT            CRST               INR                ABEFIJLO           M           	       AEIJKMNOS   	       BCEFHIMOP          ABDGJKN            O           
       DEIJKMNOPR         DP                 BCEHL              HIJKLORT           DILPQRT            C           
       CFHIJMPRST         BCIP               FHPQ        
       BCEHIJKMNP         Q           	       BCGHIJPRS          CJKLNP             AFHIOR             H                  BDGHINT            MOPRS       
       BCDEFKLMPQ         Q                  ST                 DMPQ               ACFGNPQR           GLS         
       BCEFHJKMRT         ADEK               CJKLNRT            I                  O                  BFNOPQR            CFMOQ       	       ABDEGILNT          KQ                 EGIJMOQR           G                  FHKPQT      
       BEFGHKQRST  	       ABFGJLOPS          EILR               ACOS        	       ABDEGMNQS   	       BCEFGIOPQ          E                  AIJMPR             T                  AGIKOP             DMO                GLM                BHJS        	       ABFILNOPQ   
       DEGHJNOPRS         O                  FT                 DJO         
       BDFHKNOPRT  
       AFGIJKLOQR         IJLMOS             DGIOQ              CK          	       BCDEHILRT          DGHIKLNS           AFJLMNR            R                  KMP                HIJKORT            EILR               JNQST              F           
       BDEGHKMNPS         HL                 DFHJOST            AGJOQ              EFI                O                  BCFJMRT            C                  PR                 CEG                P                  BHLOT              BEFHIKQT    	       BCDEGJKQT          DNQ                ACEJKLQS    	       BCGHIJLMP          KLMO        	       ABDKLMNQR          ACDOPQT            GMST               HQ                 AHJMNPST           HIJMNP             BEFIOPST           EHLT        	       BCFGJLNOR          AFIKNO             CLQT               ACFOPR      	       EGHJLOPQR          EKOR               DFGOP              C                  ACEGL              T                  BCDEIJT            FLNT               EIORT              BDEFI              ET                 G                  IKLMN       
       AEGIKMNQST         AHJOP              ADEGIKMN           EHM                ACHJPRST           AEGHST      
       BCEFHKLMNO         ABCHI              ABDEHILN    
       ABCEIJLOQS         CDJKLNT            CFHLNORS           AFNPT              BDFHIJM            CEGHJKLN           O                  ABCEORT            EFGMOST            BDEIOQ             GJLQ               ADILT       	       ACILOPQST          FHJMNOT            AHJMNORS           CJT                CEFG               BKQRST             D                  A                  BEHLORST    
       CDKLMNPQRT  	       BCDEFORST   
       ADFHJKMPRS         ACDGHJL            GHOT               GJKLMS             BFKNR              CFIKLNPR           CEGM               ABKLP              DIK                HJKLMQR     	       ACEFHIMNT          ILR         	       ABDFGJKLM          ABKLO              CDFIPS             BCFNQR             DIKLNPRS           GJKLMOT     
       ACDGJNORST         HMNQ        	       BCEGIJLNO          P                  Q           	       BCEGHKOQS          ABFKR       
       GHIJLMNOQT         ACFPR              ACHKMN             CEG                BKMNOP      	       CFGKLMNQT   	       BGHIJOQRT          M                  GMN         
       DEFGHIMQST         ACGHJNR            KQR         
       ADFJKMPRST  #include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define TRAN 10000
#define ITEM 20
#define LENGTH 10
#define MIN 300

#define GEM5_NUMPROCS 4

typedef struct aprioriset{
	int length;
	int support;
	char value[LENGTH];
}aprioriset;

typedef struct aprioristruct{
	int num;
	aprioriset valuelist[10000];
}aprioristruct;

typedef struct gencreturnstruct{
	int num;
	aprioriset valuelist[ITEM];
	int proper[ITEM];
}gencreturnstruct;

typedef struct gencstruct{
	aprioristruct* l;
	aprioriset* s;
	gencreturnstruct ret;
	int num;
	int length;
}gencstruct;

typedef struct genlstruct{
	aprioriset* c;
	aprioristruct* data;
}genlstruct;

typedef struct aprioriassvalue{
	char left[LENGTH];
	char right[LENGTH];
	float support;
	float confidence;
}aprioriassvalue;

typedef struct aprioriassstruct{
	int num;
	aprioriassvalue aprioriasslist[10000];
}aprioriassstruct;

typedef struct associationstruct{
	aprioriassvalue* dest;
	aprioriset* left;
	aprioriset* right;
}associationstruct;

double readtime, makec1time, makectime, makeltime, mergetime, asstime, writetime;

void readapriorib(aprioristruct* data, FILE* fp){
	data->num=TRAN;
	fread(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void saveapriorib(aprioristruct* data, FILE* fp){
	fwrite(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void readapriorinnb(aprioristruct* data, FILE* fp){
	fread(data, sizeof(aprioristruct), 1, fp);
}

void saveapriorinnb(aprioristruct* data, FILE* fp){
	fwrite(data, sizeof(aprioristruct), 1, fp);
}

void saveassstructb(aprioriassstruct* dest, FILE* fp){
	fwrite(dest->aprioriasslist, sizeof(aprioriassvalue), dest->num, fp);
}

void saveassstructnnb(aprioriassstruct* dest, FILE* fp){
	fwrite(dest, sizeof(aprioriassstruct), 1, fp);
}

void insertion(char* buf, char val, int length){
	int i, j;
	for(i=0;i<length;i++){
		if(buf[i]>val){
			break;
		}
	}
	for(j=length;j>i;j--){
		buf[j]=buf[j-1];
	}
	buf[i]=val;
}

int checkbuf(char* buf, char val, int length){
	int a;
	for(a=0;a<length;a++){
		if(buf[a]==val){
			return 1;
		}
	}
	return 0;
}

void initaprioriset(aprioriset** data, int length){
	(*data)=(aprioriset*)malloc(sizeof(aprioriset));
	(*data)->support=0;
	(*data)->length=length;
}

void deleteaprioriset(aprioriset** data){
//	free((*data)->value);
//	free(*data);
}

int compareset(aprioriset* a, aprioriset* b){
	if(a->length<b->length)
		return 1;
	else if(b->length<a->length)
		return 0;
	else{
		if(strncmp(a->value, b->value, a->length)<0)
			return 1;
		else return 0;
	}
}

int numofmatch(aprioriset* a, aprioriset* b){
	int ret=0;
	int acount=0, bcount=0;
	while(1){
		if(acount==a->length||bcount==b->length)
			break;
		if(a->value[acount]==b->value[bcount]){
			acount++;
			bcount++;
			ret++;
		}
		else if(a->value[acount]>b->value[bcount])
			bcount++;
		else
			acount++;
	}
	return ret;
}

int isequal(aprioriset* a, aprioriset* b){
	if(a->length==b->length)
		if(numofmatch(a, b)==a->length)
			return 1;
	return 0;
}

void add(aprioristruct* data, aprioriset* value, int equal){
	int i;
	if(equal){
		for(i=0;i<data->num;i++){
			if(isequal(value, &data->valuelist[i])){
				deleteaprioriset(&value);
				return;
			}
		}
	}
	data->valuelist[data->num]=*value;
	data->num++;
}

void mergeset(aprioriset* res, aprioriset* a, aprioriset* b){
	int i;
	char newval;
	for(i=0;i<a->length;i++){
		res->value[i]=a->value[i];
	}
	for(i=0;i<b->length;i++){
		if(checkbuf(res->value, b->value[i], a->length)==0){
			newval=b->value[i];
			break;
		}
	}
	insertion(res->value, newval, b->length);
}

int issubset(aprioriset* large, aprioriset* small){
	int largecount=0;
	int smallcount=0;
	char cl, cs;
	if(small->length>large->length)
		return 0;
	while(smallcount<small->length){
		if(largecount>=large->length)
			return 0;
		cl=large->value[largecount];
		cs=small->value[smallcount];
		if(cl==cs){
			largecount++;
			smallcount++;
		}
		else if(cl<cs)
			largecount++;
		else
			return 0;
	}
	return 1;
}

int isproper(aprioriset* set, aprioristruct* str){
	aprioriset* tset;
	int strcount;
	int setcount=set->length-1;
	initaprioriset(&tset, setcount);
	int i;
	for(i=0;i<set->length-1;i++){
		if(i==setcount)
			continue;
		else if(i<setcount){
			tset->value[i]=set->value[i];
		}
		else{
			tset->value[i-1]=set->value[i];
		}
	}
	
	for(strcount=0;strcount<str->num;strcount++){
		if(isequal(tset, &str->valuelist[strcount])){
			if(setcount==0)
				return 1;
			else{
				setcount--;
				for(i=0;i<set->length;i++){
					if(i==setcount)
						continue;
					else if(i<setcount){
						tset->value[i]=set->value[i];
					}
					else{
						tset->value[i-1]=set->value[i];
					}
				}
			}
		}
	}
	return 0;
}

void loadapriorifromfile(aprioristruct* data, FILE* fp){
	aprioriset* nowdata;
	int len, i, j;
	for(j=0;j<TRAN;j++){
		fscanf(fp, "%*s %*s %*s %d %*s", &len);
		initaprioriset(&nowdata, len);
		for(i=0;i<len;i++){
			fscanf(fp, "%*c %c", &(nowdata->value[i]));
		}
		add(data, nowdata, 0);
	}
}

void loadapriorifromfileb(aprioristruct* dest, FILE* fp){
	fread(dest, sizeof(aprioristruct), 1, fp);
}

void saveaprioritofile(aprioristruct* data, FILE* fp){
	int length;
	int i;
	int j;
	for(i=0;i<data->num;i++){
		length=data->valuelist[i].length;
		fprintf(fp, "LEN %02d SUP %03d :", length, data->valuelist[i].support);
		for(j=0;j<length;j++){
			fprintf(fp, " %c", data->valuelist[i].value[j]);
		}
		fprintf(fp, "\n");
	}
}

void saveaprioritofileb(aprioristruct* dest, FILE* fp){
	fwrite(dest, sizeof(aprioristruct), 1, fp);
}

void makec1(aprioristruct* target, aprioristruct* data){
	char itemlist[ITEM];
	int i=0, j, k;
	aprioriset* nowdata;
	for(k=0;k<data->num;k++){
		if(i==ITEM) break;
		nowdata=&data->valuelist[k];
		for(j=0;j<nowdata->length;j++){
			if(checkbuf(itemlist, nowdata->value[j], ITEM)==0){
				if(i==0)
					itemlist[0]=nowdata->value[j];
				else{
					insertion(itemlist, nowdata->value[j], i);
				}
				i++;
			}
		}
	}
	for(j=0;j<i;j++){
		initaprioriset(&nowdata, 1);
		nowdata->value[0]=itemlist[j];
		add(target, nowdata, 1);
	}
}

void* genlthreadfunc(void* thearg){
	genlstruct* arg=(genlstruct*)thearg;
	int datacount;
arg->c->support=0;
	for(datacount=0;datacount<arg->data->num;datacount++){
		if(issubset(&arg->data->valuelist[datacount], arg->c)){
			arg->c->support++;
		}
	}
	return NULL;
}

void genL(aprioristruct* l, aprioristruct* c, aprioristruct* data, int minnum){
	int ccount=0, cdatacount=0, datacount, i, j;
	aprioriset* nowdata;
	pthread_t thread[GEM5_NUMPROCS];
	genlstruct* genlstructs=(genlstruct*)malloc(sizeof(genlstruct)*c->num);
	if(c->num<GEM5_NUMPROCS){
		for(ccount=0;ccount<c->num;ccount++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
		}
		for(ccount=0;ccount<c->num;ccount++){
			pthread_join(thread[ccount], NULL);
		}
		for(ccount=0;ccount<c->num;ccount++){
			nowdata=&c->valuelist[ccount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
			ccount++;
		}
		while(ccount<c->num){
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				pthread_join(thread[i], NULL);	
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				nowdata=&c->valuelist[cdatacount];
				if(nowdata->support<minnum){
					deleteaprioriset(&nowdata);
				}
				else{
					add(l, nowdata, 1);
				}
				cdatacount++;
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				genlstructs[ccount].c=&c->valuelist[ccount];
				genlstructs[ccount].data=data;
				pthread_create(&thread[i], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
				ccount++;
				if(ccount==c->num)
					break;
			}
		}
		i++;
		for(j=0;j<i;j++){
			pthread_join(thread[j], NULL);
		}
		for(j=0;j<i;j++){
			nowdata=&c->valuelist[cdatacount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
			cdatacount++;
			if(cdatacount==c->num)
				break;
		}
	}
	c->num=0;
	free(genlstructs);
}



void* gencthreadfunc(void* thearg){
	gencstruct* arg=(gencstruct*)thearg;
	gencreturnstruct* ret=&arg->ret;
	aprioriset tset;
	aprioriset* pset=&tset;

	int i, j=0, k, flag, ccount=0;

	ret->num=0;
	for(i=arg->num;i<arg->l->num;i++){
		flag=0;
		if(numofmatch(&arg->l->valuelist[i], arg->s)==arg->length-1){
			mergeset(pset, &arg->l->valuelist[i], arg->s);
				pset->length=arg->length+1;
			for(k=0;k<j;k++){
				if(isequal(pset, &ret->valuelist[k])){
					flag=1;
					break;
				}
			}
			if(flag){
				deleteaprioriset(&pset);
			}
			else{
				ret->valuelist[j]=*pset;
				ret->proper[j]=isproper(&ret->valuelist[j], arg->l);
				ret->num++;
				j++;
			}
		}
	}
}

void genC(aprioristruct* c, aprioristruct* l){
	pthread_t thread[GEM5_NUMPROCS];
	gencstruct strarg[GEM5_NUMPROCS];
	gencstruct strarg2[GEM5_NUMPROCS];
	gencstruct* parg;
	int length=l->valuelist[0].length;
	int threadrun[GEM5_NUMPROCS]={0,};
	int threadrun2[GEM5_NUMPROCS]={0,};
	int* pthreadrun;
	int i, j, count=0, toggle=0, ccount=0;

	if(l->num<GEM5_NUMPROCS){
		for(i=0;i<l->num;i++){
			strarg[i].l=l;
			strarg[i].s=&l->valuelist[i];
			strarg[i].length=length;
			strarg[i].num=i+1;
			pthread_create(&thread[i], NULL, gencthreadfunc, (void*)&strarg[i]);
		}
		for(i=0;i<l->num;i++){
			pthread_join(thread[i], NULL);
		}
		for(i=0;i<l->num;i++){
			for(j=0;j<strarg[i].ret.num;j++){
				if(strarg[i].ret.proper[j]){
					add(c, &strarg[i].ret.valuelist[j], 1);
				}
			}
		}
	}
	else{
		for(count=0;count<GEM5_NUMPROCS-1;count++){
			strarg[count].l=l;
			strarg[count].s=&l->valuelist[count];
			strarg[count].length=length;
			strarg[count].num=count+1;
			pthread_create(&thread[count], NULL, gencthreadfunc, (void*)&strarg[count]);
			ccount++;
		}
		toggle=1;
		while(count<l->num){
			if(ccount==GEM5_NUMPROCS-1){
				for(i=0;i<GEM5_NUMPROCS-1;i++){
					pthread_join(thread[i], NULL);
					
				}
				ccount=0;
			}
			parg=toggle?strarg2:strarg;
			pthreadrun=toggle?threadrun2:threadrun;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				(parg+i)->l=l;
				(parg+i)->s=&l->valuelist[count];
				(parg+i)->length=length;
				(parg+i)->num=count+1;
				pthread_create(&thread[i], NULL, gencthreadfunc, (void*)(parg+i));
				count++;
				ccount++;
				*(pthreadrun+i)=1;
				if(count==l->num)
					break;
			}
			parg=toggle?strarg:strarg2;
			pthreadrun=toggle?threadrun:threadrun2;
			toggle=!toggle;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				*(pthreadrun+i)=0;
				for(j=0;j<(parg+i)->ret.num;j++){
					if((parg+i)->ret.proper[j]){
						add(c, &(parg+i)->ret.valuelist[j], 1);
					}
				}
			}
		}
		pthreadrun=toggle?threadrun:threadrun2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			pthread_join(thread[i], NULL);
		}
		parg=toggle?strarg:strarg2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			for(j=0;j<(parg+i)->ret.num;j++){
				if((parg+i)->ret.proper[j]){
					add(c, &(parg+i)->ret.valuelist[j], 1);
				}
			}
		}
	}
}
void mergestruct(aprioristruct* target, aprioristruct* l){
	int i;
	for(i=0;i<l->num;i++){
		add(target, &l->valuelist[i], 1);
	}
}

void* getassociationrulefunc(void* thearg){
	associationstruct* assstruct=(associationstruct*)thearg;
	aprioriset* left=assstruct->left, *right=assstruct->right;
	int k=0, l=0, m=0;
	for(k=0;k<right->length;k++){
		if(checkbuf(left->value, right->value[k], left->length)){
			assstruct->dest->left[l]=right->value[k];
			l++;
		}
		else{
			assstruct->dest->right[m]=right->value[k];
			m++;
		}
	}
	assstruct->dest->support=((float)right->support)/((float)TRAN);
	assstruct->dest->confidence=((float)right->support)/((float)left->support);
}

void getassociationrule(aprioriassstruct* dest, aprioristruct* list){
	aprioriset* right, *left;
	int i, j, k;
	pthread_t thread[GEM5_NUMPROCS];
	associationstruct assstruct[GEM5_NUMPROCS];
	int proccount=0;
	int threadrun[GEM5_NUMPROCS]={0,};
	for(i=0;i<list->num;i++){
		right=&list->valuelist[i];
		if(right->length<2)
			continue;
		for(j=0;j<list->num;j++){
			left=&list->valuelist[j];
			if(right->length==left->length)
				break;
			if(issubset(right, left)){

				if(proccount==GEM5_NUMPROCS-1){
					for(k=0;k<proccount;k++){
						pthread_join(thread[k], NULL);
						threadrun[k]=0;
					}
					proccount=0;
				}
				assstruct[proccount].dest=&dest->aprioriasslist[dest->num];
				assstruct[proccount].left=left;
				assstruct[proccount].right=right;
				pthread_create(&thread[proccount], NULL, getassociationrulefunc, (void*)&assstruct[proccount]);
			
				threadrun[proccount]=1;
				dest->num++;
				proccount++;
			}
		}
	}
	for(i=0;i<proccount;i++){
		pthread_join(thread[i], NULL);
	}
}

int apriori(){
	aprioristruct result;
	aprioriassstruct ass={0,};
	FILE* input=fopen("merged", "rb");
	FILE* output=fopen("ass", "wb");
	readapriorinnb(&result, input);
getassociationrule(&ass, &result);

	saveassstructnnb(&ass, output);
fclose(output);
	fclose(input);
	return 0;
}

int main(){
	apriori();
	return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define TRAN 10000
#define ITEM 20
#define LENGTH 10
#define MIN 300

#define GEM5_NUMPROCS 4

typedef struct aprioriset{
	int length;
	int support;
	char value[LENGTH];
}aprioriset;

typedef struct aprioristruct{
	int num;
	aprioriset valuelist[10000];
}aprioristruct;

typedef struct gencreturnstruct{
	int num;
	aprioriset valuelist[ITEM];
	int proper[ITEM];
}gencreturnstruct;

typedef struct gencstruct{
	aprioristruct* l;
	aprioriset* s;
	gencreturnstruct ret;
	int num;
	int length;
}gencstruct;

typedef struct genlstruct{
	aprioriset* c;
	aprioristruct* data;
}genlstruct;

typedef struct aprioriassvalue{
	char left[LENGTH];
	char right[LENGTH];
	float support;
	float confidence;
}aprioriassvalue;

typedef struct aprioriassstruct{
	int num;
	aprioriassvalue aprioriasslist[10000];
}aprioriassstruct;

typedef struct associationstruct{
	aprioriassvalue* dest;
	aprioriset* left;
	aprioriset* right;
}associationstruct;

double readtime, makec1time, makectime, makeltime, mergetime, asstime, writetime;

void readapriorib(aprioristruct* data, FILE* fp){
	data->num=TRAN;
	fread(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void saveapriorib(aprioristruct* data, FILE* fp){
	fwrite(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void readapriorinnb(aprioristruct* data, FILE* fp){
	fread(data, sizeof(aprioristruct), 1, fp);
}

void saveapriorinnb(aprioristruct* data, FILE* fp){
	fwrite(data, sizeof(aprioristruct), 1, fp);
}

void saveassstructb(aprioriassstruct* dest, FILE* fp){
	fwrite(dest->aprioriasslist, sizeof(aprioriassvalue), dest->num, fp);
}

void insertion(char* buf, char val, int length){
	int i, j;
	for(i=0;i<length;i++){
		if(buf[i]>val){
			break;
		}
	}
	for(j=length;j>i;j--){
		buf[j]=buf[j-1];
	}
	buf[i]=val;
}

int checkbuf(char* buf, char val, int length){
	int a;
	for(a=0;a<length;a++){
		if(buf[a]==val){
			return 1;
		}
	}
	return 0;
}

void initaprioriset(aprioriset** data, int length){
	(*data)=(aprioriset*)malloc(sizeof(aprioriset));
	(*data)->support=0;
	(*data)->length=length;
}

void deleteaprioriset(aprioriset** data){
//	free((*data)->value);
//	free(*data);
}

int compareset(aprioriset* a, aprioriset* b){
	if(a->length<b->length)
		return 1;
	else if(b->length<a->length)
		return 0;
	else{
		if(strncmp(a->value, b->value, a->length)<0)
			return 1;
		else return 0;
	}
}

int numofmatch(aprioriset* a, aprioriset* b){
	int ret=0;
	int acount=0, bcount=0;
	while(1){
		if(acount==a->length||bcount==b->length)
			break;
		if(a->value[acount]==b->value[bcount]){
			acount++;
			bcount++;
			ret++;
		}
		else if(a->value[acount]>b->value[bcount])
			bcount++;
		else
			acount++;
	}
	return ret;
}

int isequal(aprioriset* a, aprioriset* b){
	if(a->length==b->length)
		if(numofmatch(a, b)==a->length)
			return 1;
	return 0;
}

void add(aprioristruct* data, aprioriset* value, int equal){
	int i;
	if(equal){
		for(i=0;i<data->num;i++){
			if(isequal(value, &data->valuelist[i])){
				deleteaprioriset(&value);
				return;
			}
		}
	}
	data->valuelist[data->num]=*value;
	data->num++;
}

void mergeset(aprioriset* res, aprioriset* a, aprioriset* b){
	int i;
	char newval;
	for(i=0;i<a->length;i++){
		res->value[i]=a->value[i];
	}
	for(i=0;i<b->length;i++){
		if(checkbuf(res->value, b->value[i], a->length)==0){
			newval=b->value[i];
			break;
		}
	}
	insertion(res->value, newval, b->length);
}

int issubset(aprioriset* large, aprioriset* small){
	int largecount=0;
	int smallcount=0;
	char cl, cs;
	if(small->length>large->length)
		return 0;
	while(smallcount<small->length){
		if(largecount>=large->length)
			return 0;
		cl=large->value[largecount];
		cs=small->value[smallcount];
		if(cl==cs){
			largecount++;
			smallcount++;
		}
		else if(cl<cs)
			largecount++;
		else
			return 0;
	}
	return 1;
}

int isproper(aprioriset* set, aprioristruct* str){
	aprioriset* tset;
	int strcount;
	int setcount=set->length-1;
	initaprioriset(&tset, setcount);
	int i;
	for(i=0;i<set->length-1;i++){
		if(i==setcount)
			continue;
		else if(i<setcount){
			tset->value[i]=set->value[i];
		}
		else{
			tset->value[i-1]=set->value[i];
		}
	}
	
	for(strcount=0;strcount<str->num;strcount++){
		if(isequal(tset, &str->valuelist[strcount])){
			if(setcount==0)
				return 1;
			else{
				setcount--;
				for(i=0;i<set->length;i++){
					if(i==setcount)
						continue;
					else if(i<setcount){
						tset->value[i]=set->value[i];
					}
					else{
						tset->value[i-1]=set->value[i];
					}
				}
			}
		}
	}
	return 0;
}

void loadapriorifromfile(aprioristruct* data, FILE* fp){
	aprioriset* nowdata;
	int len, i, j;
	for(j=0;j<TRAN;j++){
		fscanf(fp, "%*s %*s %*s %d %*s", &len);
		initaprioriset(&nowdata, len);
		for(i=0;i<len;i++){
			fscanf(fp, "%*c %c", &(nowdata->value[i]));
		}
		add(data, nowdata, 0);
	}
}

void loadapriorifromfileb(aprioristruct* dest, FILE* fp){
	fread(dest, sizeof(aprioristruct), 1, fp);
}

void saveaprioritofile(aprioristruct* data, FILE* fp){
	int length;
	int i;
	int j;
	for(i=0;i<data->num;i++){
		length=data->valuelist[i].length;
		fprintf(fp, "LEN %02d SUP %03d :", length, data->valuelist[i].support);
		for(j=0;j<length;j++){
			fprintf(fp, " %c", data->valuelist[i].value[j]);
		}
		fprintf(fp, "\n");
	}
}

void saveaprioritofileb(aprioristruct* dest, FILE* fp){
	fwrite(dest, sizeof(aprioristruct), 1, fp);
}

void makec1(aprioristruct* target, aprioristruct* data){
	char itemlist[ITEM];
	int i=0, j, k;
	aprioriset* nowdata;
	for(k=0;k<data->num;k++){
		if(i==ITEM) break;
		nowdata=&data->valuelist[k];
		for(j=0;j<nowdata->length;j++){
			if(checkbuf(itemlist, nowdata->value[j], ITEM)==0){
				if(i==0)
					itemlist[0]=nowdata->value[j];
				else{
					insertion(itemlist, nowdata->value[j], i);
				}
				i++;
			}
		}
	}
	for(j=0;j<i;j++){
		initaprioriset(&nowdata, 1);
		nowdata->value[0]=itemlist[j];
		add(target, nowdata, 1);
	}
}

void* genlthreadfunc(void* thearg){
	genlstruct* arg=(genlstruct*)thearg;
	int datacount;
arg->c->support=0;
	for(datacount=0;datacount<arg->data->num;datacount++){
		if(issubset(&arg->data->valuelist[datacount], arg->c)){
			arg->c->support++;
		}
	}
	return NULL;
}

void genL(aprioristruct* l, aprioristruct* c, aprioristruct* data, int minnum){
	int ccount=0, cdatacount=0, datacount, i, j;
	aprioriset* nowdata;
	pthread_t thread[GEM5_NUMPROCS];
	genlstruct* genlstructs=(genlstruct*)malloc(sizeof(genlstruct)*c->num);
	if(c->num<GEM5_NUMPROCS){
		for(ccount=0;ccount<c->num;ccount++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
		}
		for(ccount=0;ccount<c->num;ccount++){
			pthread_join(thread[ccount], NULL);
		}
		for(ccount=0;ccount<c->num;ccount++){
			nowdata=&c->valuelist[ccount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
			ccount++;
		}
		while(ccount<c->num){
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				pthread_join(thread[i], NULL);	
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				nowdata=&c->valuelist[cdatacount];
				if(nowdata->support<minnum){
					deleteaprioriset(&nowdata);
				}
				else{
					add(l, nowdata, 1);
				}
				cdatacount++;
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				genlstructs[ccount].c=&c->valuelist[ccount];
				genlstructs[ccount].data=data;
				pthread_create(&thread[i], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
				ccount++;
				if(ccount==c->num)
					break;
			}
		}
		i++;
		for(j=0;j<i;j++){
			pthread_join(thread[j], NULL);
		}
		for(j=0;j<i;j++){
			nowdata=&c->valuelist[cdatacount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
			cdatacount++;
			if(cdatacount==c->num)
				break;
		}
	}
	c->num=0;
	free(genlstructs);
}



void* gencthreadfunc(void* thearg){
	gencstruct* arg=(gencstruct*)thearg;
	gencreturnstruct* ret=&arg->ret;
	aprioriset tset;
	aprioriset* pset=&tset;

	int i, j=0, k, flag, ccount=0;

	ret->num=0;
	for(i=arg->num;i<arg->l->num;i++){
		flag=0;
		if(numofmatch(&arg->l->valuelist[i], arg->s)==arg->length-1){
			mergeset(pset, &arg->l->valuelist[i], arg->s);
				pset->length=arg->length+1;
			for(k=0;k<j;k++){
				if(isequal(pset, &ret->valuelist[k])){
					flag=1;
					break;
				}
			}
			if(flag){
				deleteaprioriset(&pset);
			}
			else{
				ret->valuelist[j]=*pset;
				ret->proper[j]=isproper(&ret->valuelist[j], arg->l);
				ret->num++;
				j++;
			}
		}
	}
}

void genC(aprioristruct* c, aprioristruct* l){
	pthread_t thread[GEM5_NUMPROCS];
	gencstruct strarg[GEM5_NUMPROCS];
	gencstruct strarg2[GEM5_NUMPROCS];
	gencstruct* parg;
	int length=l->valuelist[0].length;
	int threadrun[GEM5_NUMPROCS]={0,};
	int threadrun2[GEM5_NUMPROCS]={0,};
	int* pthreadrun;
	int i, j, count=0, toggle=0, ccount=0;

	if(l->num<GEM5_NUMPROCS){
		for(i=0;i<l->num;i++){
			strarg[i].l=l;
			strarg[i].s=&l->valuelist[i];
			strarg[i].length=length;
			strarg[i].num=i+1;
			pthread_create(&thread[i], NULL, gencthreadfunc, (void*)&strarg[i]);
		}
		for(i=0;i<l->num;i++){
			pthread_join(thread[i], NULL);
		}
		for(i=0;i<l->num;i++){
			for(j=0;j<strarg[i].ret.num;j++){
				if(strarg[i].ret.proper[j]){
					add(c, &strarg[i].ret.valuelist[j], 1);
				}
			}
		}
	}
	else{
		for(count=0;count<GEM5_NUMPROCS-1;count++){
			strarg[count].l=l;
			strarg[count].s=&l->valuelist[count];
			strarg[count].length=length;
			strarg[count].num=count+1;
			pthread_create(&thread[count], NULL, gencthreadfunc, (void*)&strarg[count]);
			ccount++;
		}
		toggle=1;
		while(count<l->num){
			if(ccount==GEM5_NUMPROCS-1){
				for(i=0;i<GEM5_NUMPROCS-1;i++){
					pthread_join(thread[i], NULL);
					
				}
				ccount=0;
			}
			parg=toggle?strarg2:strarg;
			pthreadrun=toggle?threadrun2:threadrun;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				(parg+i)->l=l;
				(parg+i)->s=&l->valuelist[count];
				(parg+i)->length=length;
				(parg+i)->num=count+1;
				pthread_create(&thread[i], NULL, gencthreadfunc, (void*)(parg+i));
				count++;
				ccount++;
				*(pthreadrun+i)=1;
				if(count==l->num)
					break;
			}
			parg=toggle?strarg:strarg2;
			pthreadrun=toggle?threadrun:threadrun2;
			toggle=!toggle;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				*(pthreadrun+i)=0;
				for(j=0;j<(parg+i)->ret.num;j++){
					if((parg+i)->ret.proper[j]){
						add(c, &(parg+i)->ret.valuelist[j], 1);
					}
				}
			}
		}
		pthreadrun=toggle?threadrun:threadrun2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			pthread_join(thread[i], NULL);
		}
		parg=toggle?strarg:strarg2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			for(j=0;j<(parg+i)->ret.num;j++){
				if((parg+i)->ret.proper[j]){
					add(c, &(parg+i)->ret.valuelist[j], 1);
				}
			}
		}
	}
}
void mergestruct(aprioristruct* target, aprioristruct* l){
	int i;
	for(i=0;i<l->num;i++){
		add(target, &l->valuelist[i], 1);
	}
}

void* getassociationrulefunc(void* thearg){
	associationstruct* assstruct=(associationstruct*)thearg;
	aprioriset* left=assstruct->left, *right=assstruct->right;
	int k=0, l=0, m=0;
	for(k=0;k<right->length;k++){
		if(checkbuf(left->value, right->value[k], left->length)){
			assstruct->dest->left[l]=right->value[k];
			l++;
		}
		else{
			assstruct->dest->right[m]=right->value[k];
			m++;
		}
	}
	assstruct->dest->support=((float)right->support)/((float)TRAN);
	assstruct->dest->confidence=((float)right->support)/((float)left->support);
}

void getassociationrule(aprioriassstruct* dest, aprioristruct* list){
	aprioriset* right, *left;
	int i, j, k;
	pthread_t thread[GEM5_NUMPROCS];
	associationstruct assstruct[GEM5_NUMPROCS];
	int proccount=0;
	int threadrun[GEM5_NUMPROCS]={0,};
	for(i=0;i<list->num;i++){
		right=&list->valuelist[i];
		if(right->length<2)
			continue;
		for(j=0;j<list->num;j++){
			left=&list->valuelist[j];
			if(right->length==left->length)
				break;
			if(issubset(right, left)){

				if(proccount!=GEM5_NUMPROCS-1){
					for(k=0;k<proccount;k++){
						pthread_join(thread[k], NULL);
						threadrun[k]=0;
					}
					proccount=0;
				}
				assstruct[proccount].dest=&dest->aprioriasslist[dest->num];
				assstruct[proccount].left=left;
				assstruct[proccount].right=right;
				pthread_create(&thread[proccount], NULL, getassociationrulefunc, (void*)&assstruct[proccount]);
			
				threadrun[proccount]=1;
				dest->num++;
				proccount++;
			}
		}
	}
	for(i=0;i<proccount;i++){
		pthread_join(thread[i], NULL);
	}
}

int apriori(){
	aprioristruct data;
	aprioristruct candidate;
	FILE* input=fopen("adata", "rb");
	FILE* output=fopen("c1", "wb");
	candidate.num=0;
	readapriorinnb(&data, input);
	makec1(&candidate, &data);
	saveapriorinnb(&candidate, output);

	fclose(output);
	fclose(input);
	return 0;
}

int main(){
	apriori();
	return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define TRAN 10000
#define ITEM 20
#define LENGTH 10
#define MIN 300

#define GEM5_NUMPROCS 4

typedef struct aprioriset{
	int length;
	int support;
	char value[LENGTH];
}aprioriset;

typedef struct aprioristruct{
	int num;
	aprioriset valuelist[10000];
}aprioristruct;

typedef struct gencreturnstruct{
	int num;
	aprioriset valuelist[ITEM];
	int proper[ITEM];
}gencreturnstruct;

typedef struct gencstruct{
	aprioristruct* l;
	aprioriset* s;
	gencreturnstruct ret;
	int num;
	int length;
}gencstruct;

typedef struct genlstruct{
	aprioriset* c;
	aprioristruct* data;
}genlstruct;

typedef struct aprioriassvalue{
	char left[LENGTH];
	char right[LENGTH];
	float support;
	float confidence;
}aprioriassvalue;

typedef struct aprioriassstruct{
	int num;
	aprioriassvalue aprioriasslist[10000];
}aprioriassstruct;

typedef struct associationstruct{
	aprioriassvalue* dest;
	aprioriset* left;
	aprioriset* right;
}associationstruct;

double readtime, makec1time, makectime, makeltime, mergetime, asstime, writetime;

void readapriorib(aprioristruct* data, FILE* fp){
	data->num=TRAN;
	fread(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void saveapriorib(aprioristruct* data, FILE* fp){
	fwrite(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void readapriorinnb(aprioristruct* data, FILE* fp){
	fread(data, sizeof(aprioristruct), 1, fp);
}

void saveapriorinnb(aprioristruct* data, FILE* fp){
	fwrite(data, sizeof(aprioristruct), 1, fp);
}

void saveassstructb(aprioriassstruct* dest, FILE* fp){
	fwrite(dest->aprioriasslist, sizeof(aprioriassvalue), dest->num, fp);
}

void insertion(char* buf, char val, int length){
	int i, j;
	for(i=0;i<length;i++){
		if(buf[i]>val){
			break;
		}
	}
	for(j=length;j>i;j--){
		buf[j]=buf[j-1];
	}
	buf[i]=val;
}

int checkbuf(char* buf, char val, int length){
	int a;
	for(a=0;a<length;a++){
		if(buf[a]==val){
			return 1;
		}
	}
	return 0;
}

void initaprioriset(aprioriset** data, int length){
	(*data)=(aprioriset*)malloc(sizeof(aprioriset));
	(*data)->support=0;
	(*data)->length=length;
}

void deleteaprioriset(aprioriset** data){
//	free((*data)->value);
//	free(*data);
}

int compareset(aprioriset* a, aprioriset* b){
	if(a->length<b->length)
		return 1;
	else if(b->length<a->length)
		return 0;
	else{
		if(strncmp(a->value, b->value, a->length)<0)
			return 1;
		else return 0;
	}
}

int numofmatch(aprioriset* a, aprioriset* b){
	int ret=0;
	int acount=0, bcount=0;
	while(1){
		if(acount==a->length||bcount==b->length)
			break;
		if(a->value[acount]==b->value[bcount]){
			acount++;
			bcount++;
			ret++;
		}
		else if(a->value[acount]>b->value[bcount])
			bcount++;
		else
			acount++;
	}
	return ret;
}

int isequal(aprioriset* a, aprioriset* b){
	if(a->length==b->length)
		if(numofmatch(a, b)==a->length)
			return 1;
	return 0;
}

void add(aprioristruct* data, aprioriset* value, int equal){
	int i;
	if(equal){
		for(i=0;i<data->num;i++){
			if(isequal(value, &data->valuelist[i])){
				deleteaprioriset(&value);
				return;
			}
		}
	}
	data->valuelist[data->num]=*value;
	data->num++;
}

void mergeset(aprioriset* res, aprioriset* a, aprioriset* b){
	int i;
	char newval;
	for(i=0;i<a->length;i++){
		res->value[i]=a->value[i];
	}
	for(i=0;i<b->length;i++){
		if(checkbuf(res->value, b->value[i], a->length)==0){
			newval=b->value[i];
			break;
		}
	}
	insertion(res->value, newval, b->length);
}

int issubset(aprioriset* large, aprioriset* small){
	int largecount=0;
	int smallcount=0;
	char cl, cs;
	if(small->length>large->length)
		return 0;
	while(smallcount<small->length){
		if(largecount>=large->length)
			return 0;
		cl=large->value[largecount];
		cs=small->value[smallcount];
		if(cl==cs){
			largecount++;
			smallcount++;
		}
		else if(cl<cs)
			largecount++;
		else
			return 0;
	}
	return 1;
}

int isproper(aprioriset* set, aprioristruct* str){
	aprioriset* tset;
	int strcount;
	int setcount=set->length-1;
	initaprioriset(&tset, setcount);
	int i;
	for(i=0;i<set->length-1;i++){
		if(i==setcount)
			continue;
		else if(i<setcount){
			tset->value[i]=set->value[i];
		}
		else{
			tset->value[i-1]=set->value[i];
		}
	}
	
	for(strcount=0;strcount<str->num;strcount++){
		if(isequal(tset, &str->valuelist[strcount])){
			if(setcount==0)
				return 1;
			else{
				setcount--;
				for(i=0;i<set->length;i++){
					if(i==setcount)
						continue;
					else if(i<setcount){
						tset->value[i]=set->value[i];
					}
					else{
						tset->value[i-1]=set->value[i];
					}
				}
			}
		}
	}
	return 0;
}

void loadapriorifromfile(aprioristruct* data, FILE* fp){
	aprioriset* nowdata;
	int len, i, j;
	for(j=0;j<TRAN;j++){
		fscanf(fp, "%*s %*s %*s %d %*s", &len);
		initaprioriset(&nowdata, len);
		for(i=0;i<len;i++){
			fscanf(fp, "%*c %c", &(nowdata->value[i]));
		}
		add(data, nowdata, 0);
	}
}

void loadapriorifromfileb(aprioristruct* dest, FILE* fp){
	fread(dest, sizeof(aprioristruct), 1, fp);
}

void saveaprioritofile(aprioristruct* data, FILE* fp){
	int length;
	int i;
	int j;
	for(i=0;i<data->num;i++){
		length=data->valuelist[i].length;
		fprintf(fp, "LEN %02d SUP %03d :", length, data->valuelist[i].support);
		for(j=0;j<length;j++){
			fprintf(fp, " %c", data->valuelist[i].value[j]);
		}
		fprintf(fp, "\n");
	}
}

void saveaprioritofileb(aprioristruct* dest, FILE* fp){
	fwrite(dest, sizeof(aprioristruct), 1, fp);
}

void makec1(aprioristruct* target, aprioristruct* data){
	char itemlist[ITEM];
	int i=0, j, k;
	aprioriset* nowdata;
	for(k=0;k<data->num;k++){
		if(i==ITEM) break;
		nowdata=&data->valuelist[k];
		for(j=0;j<nowdata->length;j++){
			if(checkbuf(itemlist, nowdata->value[j], ITEM)==0){
				if(i==0)
					itemlist[0]=nowdata->value[j];
				else{
					insertion(itemlist, nowdata->value[j], i);
				}
				i++;
			}
		}
	}
	for(j=0;j<i;j++){
		initaprioriset(&nowdata, 1);
		nowdata->value[0]=itemlist[j];
		add(target, nowdata, 1);
	}
}

void* genlthreadfunc(void* thearg){
	genlstruct* arg=(genlstruct*)thearg;
	int datacount;
arg->c->support=0;
	for(datacount=0;datacount<arg->data->num;datacount++){
		if(issubset(&arg->data->valuelist[datacount], arg->c)){
			arg->c->support++;
		}
	}
	return NULL;
}

void genL(aprioristruct* l, aprioristruct* c, aprioristruct* data, int minnum){
	int ccount=0, cdatacount=0, datacount, i, j;
	aprioriset* nowdata;
	pthread_t thread[GEM5_NUMPROCS];
	genlstruct* genlstructs=(genlstruct*)malloc(sizeof(genlstruct)*c->num);
	if(c->num<GEM5_NUMPROCS){
		for(ccount=0;ccount<c->num;ccount++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
		}
		for(ccount=0;ccount<c->num;ccount++){
			pthread_join(thread[ccount], NULL);
		}
		for(ccount=0;ccount<c->num;ccount++){
			nowdata=&c->valuelist[ccount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
			ccount++;
		}
		while(ccount<c->num){
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				pthread_join(thread[i], NULL);	
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				nowdata=&c->valuelist[cdatacount];
				if(nowdata->support<minnum){
					deleteaprioriset(&nowdata);
				}
				else{
					add(l, nowdata, 1);
				}
				cdatacount++;
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				genlstructs[ccount].c=&c->valuelist[ccount];
				genlstructs[ccount].data=data;
				pthread_create(&thread[i], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
				ccount++;
				if(ccount==c->num)
					break;
			}
		}
		i++;
		for(j=0;j<i;j++){
			pthread_join(thread[j], NULL);
		}
		for(j=0;j<i;j++){
			nowdata=&c->valuelist[cdatacount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
			cdatacount++;
			if(cdatacount==c->num)
				break;
		}
	}
	c->num=0;
	free(genlstructs);
}



void* gencthreadfunc(void* thearg){
	gencstruct* arg=(gencstruct*)thearg;
	gencreturnstruct* ret=&arg->ret;
	aprioriset tset;
	aprioriset* pset=&tset;

	int i, j=0, k, flag, ccount=0;

	ret->num=0;
	for(i=arg->num;i<arg->l->num;i++){
		flag=0;
		if(numofmatch(&arg->l->valuelist[i], arg->s)==arg->length-1){
			mergeset(pset, &arg->l->valuelist[i], arg->s);
				pset->length=arg->length+1;
			for(k=0;k<j;k++){
				if(isequal(pset, &ret->valuelist[k])){
					flag=1;
					break;
				}
			}
			if(flag){
				deleteaprioriset(&pset);
			}
			else{
				ret->valuelist[j]=*pset;
				ret->proper[j]=isproper(&ret->valuelist[j], arg->l);
				ret->num++;
				j++;
			}
		}
	}
}

void genC(aprioristruct* c, aprioristruct* l){
	pthread_t thread[GEM5_NUMPROCS];
	gencstruct strarg[GEM5_NUMPROCS];
	gencstruct strarg2[GEM5_NUMPROCS];
	gencstruct* parg;
	int length=l->valuelist[0].length;
	int threadrun[GEM5_NUMPROCS]={0,};
	int threadrun2[GEM5_NUMPROCS]={0,};
	int* pthreadrun;
	int i, j, count=0, toggle=0, ccount=0;

	if(l->num<GEM5_NUMPROCS){
		for(i=0;i<l->num;i++){
			strarg[i].l=l;
			strarg[i].s=&l->valuelist[i];
			strarg[i].length=length;
			strarg[i].num=i+1;
			pthread_create(&thread[i], NULL, gencthreadfunc, (void*)&strarg[i]);
		}
		for(i=0;i<l->num;i++){
			pthread_join(thread[i], NULL);
		}
		for(i=0;i<l->num;i++){
			for(j=0;j<strarg[i].ret.num;j++){
				if(strarg[i].ret.proper[j]){
					add(c, &strarg[i].ret.valuelist[j], 1);
				}
			}
		}
	}
	else{
		for(count=0;count<GEM5_NUMPROCS-1;count++){
			strarg[count].l=l;
			strarg[count].s=&l->valuelist[count];
			strarg[count].length=length;
			strarg[count].num=count+1;
			pthread_create(&thread[count], NULL, gencthreadfunc, (void*)&strarg[count]);
			ccount++;
		}
		toggle=1;
		while(count<l->num){
			if(ccount==GEM5_NUMPROCS-1){
				for(i=0;i<GEM5_NUMPROCS-1;i++){
					pthread_join(thread[i], NULL);
					
				}
				ccount=0;
			}
			parg=toggle?strarg2:strarg;
			pthreadrun=toggle?threadrun2:threadrun;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				(parg+i)->l=l;
				(parg+i)->s=&l->valuelist[count];
				(parg+i)->length=length;
				(parg+i)->num=count+1;
				pthread_create(&thread[i], NULL, gencthreadfunc, (void*)(parg+i));
				count++;
				ccount++;
				*(pthreadrun+i)=1;
				if(count==l->num)
					break;
			}
			parg=toggle?strarg:strarg2;
			pthreadrun=toggle?threadrun:threadrun2;
			toggle=!toggle;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				*(pthreadrun+i)=0;
				for(j=0;j<(parg+i)->ret.num;j++){
					if((parg+i)->ret.proper[j]){
						add(c, &(parg+i)->ret.valuelist[j], 1);
					}
				}
			}
		}
		pthreadrun=toggle?threadrun:threadrun2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			pthread_join(thread[i], NULL);
		}
		parg=toggle?strarg:strarg2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			for(j=0;j<(parg+i)->ret.num;j++){
				if((parg+i)->ret.proper[j]){
					add(c, &(parg+i)->ret.valuelist[j], 1);
				}
			}
		}
	}
}
void mergestruct(aprioristruct* target, aprioristruct* l){
	int i;
	for(i=0;i<l->num;i++){
		add(target, &l->valuelist[i], 1);
	}
}

void* getassociationrulefunc(void* thearg){
	associationstruct* assstruct=(associationstruct*)thearg;
	aprioriset* left=assstruct->left, *right=assstruct->right;
	int k=0, l=0, m=0;
	for(k=0;k<right->length;k++){
		if(checkbuf(left->value, right->value[k], left->length)){
			assstruct->dest->left[l]=right->value[k];
			l++;
		}
		else{
			assstruct->dest->right[m]=right->value[k];
			m++;
		}
	}
	assstruct->dest->support=((float)right->support)/((float)TRAN);
	assstruct->dest->confidence=((float)right->support)/((float)left->support);
}

void getassociationrule(aprioriassstruct* dest, aprioristruct* list){
	aprioriset* right, *left;
	int i, j, k;
	pthread_t thread[GEM5_NUMPROCS];
	associationstruct assstruct[GEM5_NUMPROCS];
	int proccount=0;
	int threadrun[GEM5_NUMPROCS]={0,};
	for(i=0;i<list->num;i++){
		right=&list->valuelist[i];
		if(right->length<2)
			continue;
		for(j=0;j<list->num;j++){
			left=&list->valuelist[j];
			if(right->length==left->length)
				break;
			if(issubset(right, left)){

				if(proccount!=GEM5_NUMPROCS-1){
					for(k=0;k<proccount;k++){
						pthread_join(thread[k], NULL);
						threadrun[k]=0;
					}
					proccount=0;
				}
				assstruct[proccount].dest=&dest->aprioriasslist[dest->num];
				assstruct[proccount].left=left;
				assstruct[proccount].right=right;
				pthread_create(&thread[proccount], NULL, getassociationrulefunc, (void*)&assstruct[proccount]);
			
				threadrun[proccount]=1;
				dest->num++;
				proccount++;
			}
		}
	}
	for(i=0;i<proccount;i++){
		pthread_join(thread[i], NULL);
	}
}

int apriori(){
	aprioristruct data;
	aprioristruct candidate;
	FILE* input=fopen("l1", "rb");
	FILE* output=fopen("c2", "wb");
	candidate.num=0;
	readapriorinnb(&data, input);
	genC(&candidate, &data);

	saveapriorinnb(&candidate, output);
	fclose(output);
	fclose(input);
	return 0;
}

int main(){
	apriori();
	return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define TRAN 10000
#define ITEM 20
#define LENGTH 10
#define MIN 300

#define GEM5_NUMPROCS 4

typedef struct aprioriset{
	int length;
	int support;
	char value[LENGTH];
}aprioriset;

typedef struct aprioristruct{
	int num;
	aprioriset valuelist[10000];
}aprioristruct;

typedef struct gencreturnstruct{
	int num;
	aprioriset valuelist[ITEM];
	int proper[ITEM];
}gencreturnstruct;

typedef struct gencstruct{
	aprioristruct* l;
	aprioriset* s;
	gencreturnstruct ret;
	int num;
	int length;
}gencstruct;

typedef struct genlstruct{
	aprioriset* c;
	aprioristruct* data;
}genlstruct;

typedef struct aprioriassvalue{
	char left[LENGTH];
	char right[LENGTH];
	float support;
	float confidence;
}aprioriassvalue;

typedef struct aprioriassstruct{
	int num;
	aprioriassvalue aprioriasslist[10000];
}aprioriassstruct;

typedef struct associationstruct{
	aprioriassvalue* dest;
	aprioriset* left;
	aprioriset* right;
}associationstruct;

double readtime, makec1time, makectime, makeltime, mergetime, asstime, writetime;

void readapriorib(aprioristruct* data, FILE* fp){
	data->num=TRAN;
	fread(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void saveapriorib(aprioristruct* data, FILE* fp){
	fwrite(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void readapriorinnb(aprioristruct* data, FILE* fp){
	fread(data, sizeof(aprioristruct), 1, fp);
}

void saveapriorinnb(aprioristruct* data, FILE* fp){
	fwrite(data, sizeof(aprioristruct), 1, fp);
}

void saveassstructb(aprioriassstruct* dest, FILE* fp){
	fwrite(dest->aprioriasslist, sizeof(aprioriassvalue), dest->num, fp);
}

void insertion(char* buf, char val, int length){
	int i, j;
	for(i=0;i<length;i++){
		if(buf[i]>val){
			break;
		}
	}
	for(j=length;j>i;j--){
		buf[j]=buf[j-1];
	}
	buf[i]=val;
}

int checkbuf(char* buf, char val, int length){
	int a;
	for(a=0;a<length;a++){
		if(buf[a]==val){
			return 1;
		}
	}
	return 0;
}

void initaprioriset(aprioriset** data, int length){
	(*data)=(aprioriset*)malloc(sizeof(aprioriset));
	(*data)->support=0;
	(*data)->length=length;
}

void deleteaprioriset(aprioriset** data){
//	free((*data)->value);
//	free(*data);
}

int compareset(aprioriset* a, aprioriset* b){
	if(a->length<b->length)
		return 1;
	else if(b->length<a->length)
		return 0;
	else{
		if(strncmp(a->value, b->value, a->length)<0)
			return 1;
		else return 0;
	}
}

int numofmatch(aprioriset* a, aprioriset* b){
	int ret=0;
	int acount=0, bcount=0;
	while(1){
		if(acount==a->length||bcount==b->length)
			break;
		if(a->value[acount]==b->value[bcount]){
			acount++;
			bcount++;
			ret++;
		}
		else if(a->value[acount]>b->value[bcount])
			bcount++;
		else
			acount++;
	}
	return ret;
}

int isequal(aprioriset* a, aprioriset* b){
	if(a->length==b->length)
		if(numofmatch(a, b)==a->length)
			return 1;
	return 0;
}

void add(aprioristruct* data, aprioriset* value, int equal){
	int i;
	if(equal){
		for(i=0;i<data->num;i++){
			if(isequal(value, &data->valuelist[i])){
				deleteaprioriset(&value);
				return;
			}
		}
	}
	data->valuelist[data->num]=*value;
	data->num++;
}

void mergeset(aprioriset* res, aprioriset* a, aprioriset* b){
	int i;
	char newval;
	for(i=0;i<a->length;i++){
		res->value[i]=a->value[i];
	}
	for(i=0;i<b->length;i++){
		if(checkbuf(res->value, b->value[i], a->length)==0){
			newval=b->value[i];
			break;
		}
	}
	insertion(res->value, newval, b->length);
}

int issubset(aprioriset* large, aprioriset* small){
	int largecount=0;
	int smallcount=0;
	char cl, cs;
	if(small->length>large->length)
		return 0;
	while(smallcount<small->length){
		if(largecount>=large->length)
			return 0;
		cl=large->value[largecount];
		cs=small->value[smallcount];
		if(cl==cs){
			largecount++;
			smallcount++;
		}
		else if(cl<cs)
			largecount++;
		else
			return 0;
	}
	return 1;
}

int isproper(aprioriset* set, aprioristruct* str){
	aprioriset* tset;
	int strcount;
	int setcount=set->length-1;
	initaprioriset(&tset, setcount);
	int i;
	for(i=0;i<set->length-1;i++){
		if(i==setcount)
			continue;
		else if(i<setcount){
			tset->value[i]=set->value[i];
		}
		else{
			tset->value[i-1]=set->value[i];
		}
	}
	
	for(strcount=0;strcount<str->num;strcount++){
		if(isequal(tset, &str->valuelist[strcount])){
			if(setcount==0)
				return 1;
			else{
				setcount--;
				for(i=0;i<set->length;i++){
					if(i==setcount)
						continue;
					else if(i<setcount){
						tset->value[i]=set->value[i];
					}
					else{
						tset->value[i-1]=set->value[i];
					}
				}
			}
		}
	}
	return 0;
}

void loadapriorifromfile(aprioristruct* data, FILE* fp){
	aprioriset* nowdata;
	int len, i, j;
	for(j=0;j<TRAN;j++){
		fscanf(fp, "%*s %*s %*s %d %*s", &len);
		initaprioriset(&nowdata, len);
		for(i=0;i<len;i++){
			fscanf(fp, "%*c %c", &(nowdata->value[i]));
		}
		add(data, nowdata, 0);
	}
}

void loadapriorifromfileb(aprioristruct* dest, FILE* fp){
	fread(dest, sizeof(aprioristruct), 1, fp);
}

void saveaprioritofile(aprioristruct* data, FILE* fp){
	int length;
	int i;
	int j;
	for(i=0;i<data->num;i++){
		length=data->valuelist[i].length;
		fprintf(fp, "LEN %02d SUP %03d :", length, data->valuelist[i].support);
		for(j=0;j<length;j++){
			fprintf(fp, " %c", data->valuelist[i].value[j]);
		}
		fprintf(fp, "\n");
	}
}

void saveaprioritofileb(aprioristruct* dest, FILE* fp){
	fwrite(dest, sizeof(aprioristruct), 1, fp);
}

void makec1(aprioristruct* target, aprioristruct* data){
	char itemlist[ITEM];
	int i=0, j, k;
	aprioriset* nowdata;
	for(k=0;k<data->num;k++){
		if(i==ITEM) break;
		nowdata=&data->valuelist[k];
		for(j=0;j<nowdata->length;j++){
			if(checkbuf(itemlist, nowdata->value[j], ITEM)==0){
				if(i==0)
					itemlist[0]=nowdata->value[j];
				else{
					insertion(itemlist, nowdata->value[j], i);
				}
				i++;
			}
		}
	}
	for(j=0;j<i;j++){
		initaprioriset(&nowdata, 1);
		nowdata->value[0]=itemlist[j];
		add(target, nowdata, 1);
	}
}

void* genlthreadfunc(void* thearg){
	genlstruct* arg=(genlstruct*)thearg;
	int datacount;
arg->c->support=0;
	for(datacount=0;datacount<arg->data->num;datacount++){
		if(issubset(&arg->data->valuelist[datacount], arg->c)){
			arg->c->support++;
		}
	}
	return NULL;
}

void genL(aprioristruct* l, aprioristruct* c, aprioristruct* data, int minnum){
	int ccount=0, cdatacount=0, datacount, i, j;
	aprioriset* nowdata;
	pthread_t thread[GEM5_NUMPROCS];
	genlstruct* genlstructs=(genlstruct*)malloc(sizeof(genlstruct)*c->num);
	if(c->num<GEM5_NUMPROCS){
		for(ccount=0;ccount<c->num;ccount++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
		}
		for(ccount=0;ccount<c->num;ccount++){
			pthread_join(thread[ccount], NULL);
		}
		for(ccount=0;ccount<c->num;ccount++){
			nowdata=&c->valuelist[ccount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
			ccount++;
		}
		while(ccount<c->num){
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				pthread_join(thread[i], NULL);	
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				nowdata=&c->valuelist[cdatacount];
				if(nowdata->support<minnum){
					deleteaprioriset(&nowdata);
				}
				else{
					add(l, nowdata, 1);
				}
				cdatacount++;
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				genlstructs[ccount].c=&c->valuelist[ccount];
				genlstructs[ccount].data=data;
				pthread_create(&thread[i], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
				ccount++;
				if(ccount==c->num)
					break;
			}
		}
		i++;
		for(j=0;j<i;j++){
			pthread_join(thread[j], NULL);
		}
		for(j=0;j<i;j++){
			nowdata=&c->valuelist[cdatacount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
			cdatacount++;
			if(cdatacount==c->num)
				break;
		}
	}
	c->num=0;
	free(genlstructs);
}



void* gencthreadfunc(void* thearg){
	gencstruct* arg=(gencstruct*)thearg;
	gencreturnstruct* ret=&arg->ret;
	aprioriset tset;
	aprioriset* pset=&tset;

	int i, j=0, k, flag, ccount=0;

	ret->num=0;
	for(i=arg->num;i<arg->l->num;i++){
		flag=0;
		if(numofmatch(&arg->l->valuelist[i], arg->s)==arg->length-1){
			mergeset(pset, &arg->l->valuelist[i], arg->s);
				pset->length=arg->length+1;
			for(k=0;k<j;k++){
				if(isequal(pset, &ret->valuelist[k])){
					flag=1;
					break;
				}
			}
			if(flag){
				deleteaprioriset(&pset);
			}
			else{
				ret->valuelist[j]=*pset;
				ret->proper[j]=isproper(&ret->valuelist[j], arg->l);
				ret->num++;
				j++;
			}
		}
	}
}

void genC(aprioristruct* c, aprioristruct* l){
	pthread_t thread[GEM5_NUMPROCS];
	gencstruct strarg[GEM5_NUMPROCS];
	gencstruct strarg2[GEM5_NUMPROCS];
	gencstruct* parg;
	int length=l->valuelist[0].length;
	int threadrun[GEM5_NUMPROCS]={0,};
	int threadrun2[GEM5_NUMPROCS]={0,};
	int* pthreadrun;
	int i, j, count=0, toggle=0, ccount=0;

	if(l->num<GEM5_NUMPROCS){
		for(i=0;i<l->num;i++){
			strarg[i].l=l;
			strarg[i].s=&l->valuelist[i];
			strarg[i].length=length;
			strarg[i].num=i+1;
			pthread_create(&thread[i], NULL, gencthreadfunc, (void*)&strarg[i]);
		}
		for(i=0;i<l->num;i++){
			pthread_join(thread[i], NULL);
		}
		for(i=0;i<l->num;i++){
			for(j=0;j<strarg[i].ret.num;j++){
				if(strarg[i].ret.proper[j]){
					add(c, &strarg[i].ret.valuelist[j], 1);
				}
			}
		}
	}
	else{
		for(count=0;count<GEM5_NUMPROCS-1;count++){
			strarg[count].l=l;
			strarg[count].s=&l->valuelist[count];
			strarg[count].length=length;
			strarg[count].num=count+1;
			pthread_create(&thread[count], NULL, gencthreadfunc, (void*)&strarg[count]);
			ccount++;
		}
		toggle=1;
		while(count<l->num){
			if(ccount==GEM5_NUMPROCS-1){
				for(i=0;i<GEM5_NUMPROCS-1;i++){
					pthread_join(thread[i], NULL);
					
				}
				ccount=0;
			}
			parg=toggle?strarg2:strarg;
			pthreadrun=toggle?threadrun2:threadrun;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				(parg+i)->l=l;
				(parg+i)->s=&l->valuelist[count];
				(parg+i)->length=length;
				(parg+i)->num=count+1;
				pthread_create(&thread[i], NULL, gencthreadfunc, (void*)(parg+i));
				count++;
				ccount++;
				*(pthreadrun+i)=1;
				if(count==l->num)
					break;
			}
			parg=toggle?strarg:strarg2;
			pthreadrun=toggle?threadrun:threadrun2;
			toggle=!toggle;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				*(pthreadrun+i)=0;
				for(j=0;j<(parg+i)->ret.num;j++){
					if((parg+i)->ret.proper[j]){
						add(c, &(parg+i)->ret.valuelist[j], 1);
					}
				}
			}
		}
		pthreadrun=toggle?threadrun:threadrun2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			pthread_join(thread[i], NULL);
		}
		parg=toggle?strarg:strarg2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			for(j=0;j<(parg+i)->ret.num;j++){
				if((parg+i)->ret.proper[j]){
					add(c, &(parg+i)->ret.valuelist[j], 1);
				}
			}
		}
	}
}
void mergestruct(aprioristruct* target, aprioristruct* l){
	int i;
	for(i=0;i<l->num;i++){
		add(target, &l->valuelist[i], 1);
	}
}

void* getassociationrulefunc(void* thearg){
	associationstruct* assstruct=(associationstruct*)thearg;
	aprioriset* left=assstruct->left, *right=assstruct->right;
	int k=0, l=0, m=0;
	for(k=0;k<right->length;k++){
		if(checkbuf(left->value, right->value[k], left->length)){
			assstruct->dest->left[l]=right->value[k];
			l++;
		}
		else{
			assstruct->dest->right[m]=right->value[k];
			m++;
		}
	}
	assstruct->dest->support=((float)right->support)/((float)TRAN);
	assstruct->dest->confidence=((float)right->support)/((float)left->support);
}

void getassociationrule(aprioriassstruct* dest, aprioristruct* list){
	aprioriset* right, *left;
	int i, j, k;
	pthread_t thread[GEM5_NUMPROCS];
	associationstruct assstruct[GEM5_NUMPROCS];
	int proccount=0;
	int threadrun[GEM5_NUMPROCS]={0,};
	for(i=0;i<list->num;i++){
		right=&list->valuelist[i];
		if(right->length<2)
			continue;
		for(j=0;j<list->num;j++){
			left=&list->valuelist[j];
			if(right->length==left->length)
				break;
			if(issubset(right, left)){

				if(proccount!=GEM5_NUMPROCS-1){
					for(k=0;k<proccount;k++){
						pthread_join(thread[k], NULL);
						threadrun[k]=0;
					}
					proccount=0;
				}
				assstruct[proccount].dest=&dest->aprioriasslist[dest->num];
				assstruct[proccount].left=left;
				assstruct[proccount].right=right;
				pthread_create(&thread[proccount], NULL, getassociationrulefunc, (void*)&assstruct[proccount]);
			
				threadrun[proccount]=1;
				dest->num++;
				proccount++;
			}
		}
	}
	for(i=0;i<proccount;i++){
		pthread_join(thread[i], NULL);
	}
}

int apriori(){
	aprioristruct data;
	aprioristruct candidate;
	FILE* input=fopen("l2", "rb");
	FILE* output=fopen("c3", "wb");
	candidate.num=0;
	readapriorinnb(&data, input);
	genC(&candidate, &data);

	saveapriorinnb(&candidate, output);
	fclose(output);
	fclose(input);
	return 0;
}

int main(){
	apriori();
	return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define TRAN 10000
#define ITEM 20
#define LENGTH 10
#define MIN 300

#define GEM5_NUMPROCS 4

typedef struct aprioriset{
	int length;
	int support;
	char value[LENGTH];
}aprioriset;

typedef struct aprioristruct{
	int num;
	aprioriset valuelist[10000];
}aprioristruct;

typedef struct gencreturnstruct{
	int num;
	aprioriset valuelist[ITEM];
	int proper[ITEM];
}gencreturnstruct;

typedef struct gencstruct{
	aprioristruct* l;
	aprioriset* s;
	gencreturnstruct ret;
	int num;
	int length;
}gencstruct;

typedef struct genlstruct{
	aprioriset* c;
	aprioristruct* data;
}genlstruct;

typedef struct aprioriassvalue{
	char left[LENGTH];
	char right[LENGTH];
	float support;
	float confidence;
}aprioriassvalue;

typedef struct aprioriassstruct{
	int num;
	aprioriassvalue aprioriasslist[10000];
}aprioriassstruct;

typedef struct associationstruct{
	aprioriassvalue* dest;
	aprioriset* left;
	aprioriset* right;
}associationstruct;

double readtime, makec1time, makectime, makeltime, mergetime, asstime, writetime;

void readapriorib(aprioristruct* data, FILE* fp){
	data->num=TRAN;
	fread(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void saveapriorib(aprioristruct* data, FILE* fp){
	fwrite(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void readapriorinnb(aprioristruct* data, FILE* fp){
	fread(data, sizeof(aprioristruct), 1, fp);
}

void saveapriorinnb(aprioristruct* data, FILE* fp){
	fwrite(data, sizeof(aprioristruct), 1, fp);
}

void saveassstructb(aprioriassstruct* dest, FILE* fp){
	fwrite(dest->aprioriasslist, sizeof(aprioriassvalue), dest->num, fp);
}

void insertion(char* buf, char val, int length){
	int i, j;
	for(i=0;i<length;i++){
		if(buf[i]>val){
			break;
		}
	}
	for(j=length;j>i;j--){
		buf[j]=buf[j-1];
	}
	buf[i]=val;
}

int checkbuf(char* buf, char val, int length){
	int a;
	for(a=0;a<length;a++){
		if(buf[a]==val){
			return 1;
		}
	}
	return 0;
}

void initaprioriset(aprioriset** data, int length){
	(*data)=(aprioriset*)malloc(sizeof(aprioriset));
	(*data)->support=0;
	(*data)->length=length;
}

void deleteaprioriset(aprioriset** data){
//	free((*data)->value);
//	free(*data);
}

int compareset(aprioriset* a, aprioriset* b){
	if(a->length<b->length)
		return 1;
	else if(b->length<a->length)
		return 0;
	else{
		if(strncmp(a->value, b->value, a->length)<0)
			return 1;
		else return 0;
	}
}

int numofmatch(aprioriset* a, aprioriset* b){
	int ret=0;
	int acount=0, bcount=0;
	while(1){
		if(acount==a->length||bcount==b->length)
			break;
		if(a->value[acount]==b->value[bcount]){
			acount++;
			bcount++;
			ret++;
		}
		else if(a->value[acount]>b->value[bcount])
			bcount++;
		else
			acount++;
	}
	return ret;
}

int isequal(aprioriset* a, aprioriset* b){
	if(a->length==b->length)
		if(numofmatch(a, b)==a->length)
			return 1;
	return 0;
}

void add(aprioristruct* data, aprioriset* value, int equal){
	int i;
	if(equal){
		for(i=0;i<data->num;i++){
			if(isequal(value, &data->valuelist[i])){
				deleteaprioriset(&value);
				return;
			}
		}
	}
	data->valuelist[data->num]=*value;
	data->num++;
}

void mergeset(aprioriset* res, aprioriset* a, aprioriset* b){
	int i;
	char newval;
	for(i=0;i<a->length;i++){
		res->value[i]=a->value[i];
	}
	for(i=0;i<b->length;i++){
		if(checkbuf(res->value, b->value[i], a->length)==0){
			newval=b->value[i];
			break;
		}
	}
	insertion(res->value, newval, b->length);
}

int issubset(aprioriset* large, aprioriset* small){
	int largecount=0;
	int smallcount=0;
	char cl, cs;
	if(small->length>large->length)
		return 0;
	while(smallcount<small->length){
		if(largecount>=large->length)
			return 0;
		cl=large->value[largecount];
		cs=small->value[smallcount];
		if(cl==cs){
			largecount++;
			smallcount++;
		}
		else if(cl<cs)
			largecount++;
		else
			return 0;
	}
	return 1;
}

int isproper(aprioriset* set, aprioristruct* str){
	aprioriset* tset;
	int strcount;
	int setcount=set->length-1;
	initaprioriset(&tset, setcount);
	int i;
	for(i=0;i<set->length-1;i++){
		if(i==setcount)
			continue;
		else if(i<setcount){
			tset->value[i]=set->value[i];
		}
		else{
			tset->value[i-1]=set->value[i];
		}
	}
	
	for(strcount=0;strcount<str->num;strcount++){
		if(isequal(tset, &str->valuelist[strcount])){
			if(setcount==0)
				return 1;
			else{
				setcount--;
				for(i=0;i<set->length;i++){
					if(i==setcount)
						continue;
					else if(i<setcount){
						tset->value[i]=set->value[i];
					}
					else{
						tset->value[i-1]=set->value[i];
					}
				}
			}
		}
	}
	return 0;
}

void loadapriorifromfile(aprioristruct* data, FILE* fp){
	aprioriset* nowdata;
	int len, i, j;
	for(j=0;j<TRAN;j++){
		fscanf(fp, "%*s %*s %*s %d %*s", &len);
		initaprioriset(&nowdata, len);
		for(i=0;i<len;i++){
			fscanf(fp, "%*c %c", &(nowdata->value[i]));
		}
		add(data, nowdata, 0);
	}
}

void loadapriorifromfileb(aprioristruct* dest, FILE* fp){
	fread(dest, sizeof(aprioristruct), 1, fp);
}

void saveaprioritofile(aprioristruct* data, FILE* fp){
	int length;
	int i;
	int j;
	for(i=0;i<data->num;i++){
		length=data->valuelist[i].length;
		fprintf(fp, "LEN %02d SUP %03d :", length, data->valuelist[i].support);
		for(j=0;j<length;j++){
			fprintf(fp, " %c", data->valuelist[i].value[j]);
		}
		fprintf(fp, "\n");
	}
}

void saveaprioritofileb(aprioristruct* dest, FILE* fp){
	fwrite(dest, sizeof(aprioristruct), 1, fp);
}

void makec1(aprioristruct* target, aprioristruct* data){
	char itemlist[ITEM];
	int i=0, j, k;
	aprioriset* nowdata;
	for(k=0;k<data->num;k++){
		if(i==ITEM) break;
		nowdata=&data->valuelist[k];
		for(j=0;j<nowdata->length;j++){
			if(checkbuf(itemlist, nowdata->value[j], ITEM)==0){
				if(i==0)
					itemlist[0]=nowdata->value[j];
				else{
					insertion(itemlist, nowdata->value[j], i);
				}
				i++;
			}
		}
	}
	for(j=0;j<i;j++){
		initaprioriset(&nowdata, 1);
		nowdata->value[0]=itemlist[j];
		add(target, nowdata, 1);
	}
}

void* genlthreadfunc(void* thearg){
	genlstruct* arg=(genlstruct*)thearg;
	int datacount;
arg->c->support=0;
	for(datacount=0;datacount<arg->data->num;datacount++){
		if(issubset(&arg->data->valuelist[datacount], arg->c)){
			arg->c->support++;
		}
	}
	return NULL;
}

void genL(aprioristruct* l, aprioristruct* c, aprioristruct* data, int minnum){
	int ccount=0, cdatacount=0, datacount, i, j;
	aprioriset* nowdata;
	pthread_t thread[GEM5_NUMPROCS];
	genlstruct* genlstructs=(genlstruct*)malloc(sizeof(genlstruct)*c->num);
	if(c->num<GEM5_NUMPROCS){
		for(ccount=0;ccount<c->num;ccount++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
		}
		for(ccount=0;ccount<c->num;ccount++){
			pthread_join(thread[ccount], NULL);
		}
		for(ccount=0;ccount<c->num;ccount++){
			nowdata=&c->valuelist[ccount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
			ccount++;
		}
		while(ccount<c->num){
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				pthread_join(thread[i], NULL);	
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				nowdata=&c->valuelist[cdatacount];
				if(nowdata->support<minnum){
					deleteaprioriset(&nowdata);
				}
				else{
					add(l, nowdata, 1);
				}
				cdatacount++;
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				genlstructs[ccount].c=&c->valuelist[ccount];
				genlstructs[ccount].data=data;
				pthread_create(&thread[i], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
				ccount++;
				if(ccount==c->num)
					break;
			}
		}
		i++;
		for(j=0;j<i;j++){
			pthread_join(thread[j], NULL);
		}
		for(j=0;j<i;j++){
			nowdata=&c->valuelist[cdatacount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
			cdatacount++;
			if(cdatacount==c->num)
				break;
		}
	}
	c->num=0;
	free(genlstructs);
}



void* gencthreadfunc(void* thearg){
	gencstruct* arg=(gencstruct*)thearg;
	gencreturnstruct* ret=&arg->ret;
	aprioriset tset;
	aprioriset* pset=&tset;

	int i, j=0, k, flag, ccount=0;

	ret->num=0;
	for(i=arg->num;i<arg->l->num;i++){
		flag=0;
		if(numofmatch(&arg->l->valuelist[i], arg->s)==arg->length-1){
			mergeset(pset, &arg->l->valuelist[i], arg->s);
				pset->length=arg->length+1;
			for(k=0;k<j;k++){
				if(isequal(pset, &ret->valuelist[k])){
					flag=1;
					break;
				}
			}
			if(flag){
				deleteaprioriset(&pset);
			}
			else{
				ret->valuelist[j]=*pset;
				ret->proper[j]=isproper(&ret->valuelist[j], arg->l);
				ret->num++;
				j++;
			}
		}
	}
}

void genC(aprioristruct* c, aprioristruct* l){
	pthread_t thread[GEM5_NUMPROCS];
	gencstruct strarg[GEM5_NUMPROCS];
	gencstruct strarg2[GEM5_NUMPROCS];
	gencstruct* parg;
	int length=l->valuelist[0].length;
	int threadrun[GEM5_NUMPROCS]={0,};
	int threadrun2[GEM5_NUMPROCS]={0,};
	int* pthreadrun;
	int i, j, count=0, toggle=0, ccount=0;

	if(l->num<GEM5_NUMPROCS){
		for(i=0;i<l->num;i++){
			strarg[i].l=l;
			strarg[i].s=&l->valuelist[i];
			strarg[i].length=length;
			strarg[i].num=i+1;
			pthread_create(&thread[i], NULL, gencthreadfunc, (void*)&strarg[i]);
		}
		for(i=0;i<l->num;i++){
			pthread_join(thread[i], NULL);
		}
		for(i=0;i<l->num;i++){
			for(j=0;j<strarg[i].ret.num;j++){
				if(strarg[i].ret.proper[j]){
					add(c, &strarg[i].ret.valuelist[j], 1);
				}
			}
		}
	}
	else{
		for(count=0;count<GEM5_NUMPROCS-1;count++){
			strarg[count].l=l;
			strarg[count].s=&l->valuelist[count];
			strarg[count].length=length;
			strarg[count].num=count+1;
			pthread_create(&thread[count], NULL, gencthreadfunc, (void*)&strarg[count]);
			ccount++;
		}
		toggle=1;
		while(count<l->num){
			if(ccount==GEM5_NUMPROCS-1){
				for(i=0;i<GEM5_NUMPROCS-1;i++){
					pthread_join(thread[i], NULL);
					
				}
				ccount=0;
			}
			parg=toggle?strarg2:strarg;
			pthreadrun=toggle?threadrun2:threadrun;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				(parg+i)->l=l;
				(parg+i)->s=&l->valuelist[count];
				(parg+i)->length=length;
				(parg+i)->num=count+1;
				pthread_create(&thread[i], NULL, gencthreadfunc, (void*)(parg+i));
				count++;
				ccount++;
				*(pthreadrun+i)=1;
				if(count==l->num)
					break;
			}
			parg=toggle?strarg:strarg2;
			pthreadrun=toggle?threadrun:threadrun2;
			toggle=!toggle;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				*(pthreadrun+i)=0;
				for(j=0;j<(parg+i)->ret.num;j++){
					if((parg+i)->ret.proper[j]){
						add(c, &(parg+i)->ret.valuelist[j], 1);
					}
				}
			}
		}
		pthreadrun=toggle?threadrun:threadrun2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			pthread_join(thread[i], NULL);
		}
		parg=toggle?strarg:strarg2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			for(j=0;j<(parg+i)->ret.num;j++){
				if((parg+i)->ret.proper[j]){
					add(c, &(parg+i)->ret.valuelist[j], 1);
				}
			}
		}
	}
}
void mergestruct(aprioristruct* target, aprioristruct* l){
	int i;
	for(i=0;i<l->num;i++){
		add(target, &l->valuelist[i], 1);
	}
}

void* getassociationrulefunc(void* thearg){
	associationstruct* assstruct=(associationstruct*)thearg;
	aprioriset* left=assstruct->left, *right=assstruct->right;
	int k=0, l=0, m=0;
	for(k=0;k<right->length;k++){
		if(checkbuf(left->value, right->value[k], left->length)){
			assstruct->dest->left[l]=right->value[k];
			l++;
		}
		else{
			assstruct->dest->right[m]=right->value[k];
			m++;
		}
	}
	assstruct->dest->support=((float)right->support)/((float)TRAN);
	assstruct->dest->confidence=((float)right->support)/((float)left->support);
}

void getassociationrule(aprioriassstruct* dest, aprioristruct* list){
	aprioriset* right, *left;
	int i, j, k;
	pthread_t thread[GEM5_NUMPROCS];
	associationstruct assstruct[GEM5_NUMPROCS];
	int proccount=0;
	int threadrun[GEM5_NUMPROCS]={0,};
	for(i=0;i<list->num;i++){
		right=&list->valuelist[i];
		if(right->length<2)
			continue;
		for(j=0;j<list->num;j++){
			left=&list->valuelist[j];
			if(right->length==left->length)
				break;
			if(issubset(right, left)){

				if(proccount!=GEM5_NUMPROCS-1){
					for(k=0;k<proccount;k++){
						pthread_join(thread[k], NULL);
						threadrun[k]=0;
					}
					proccount=0;
				}
				assstruct[proccount].dest=&dest->aprioriasslist[dest->num];
				assstruct[proccount].left=left;
				assstruct[proccount].right=right;
				pthread_create(&thread[proccount], NULL, getassociationrulefunc, (void*)&assstruct[proccount]);
			
				threadrun[proccount]=1;
				dest->num++;
				proccount++;
			}
		}
	}
	for(i=0;i<proccount;i++){
		pthread_join(thread[i], NULL);
	}
}

int apriori(){
	aprioristruct data;
	aprioristruct candidate;
	FILE* input=fopen("l3", "rb");
	FILE* output=fopen("c4", "wb");
	candidate.num=0;
	readapriorinnb(&data, input);
	genC(&candidate, &data);

	saveapriorinnb(&candidate, output);
	fclose(output);
	fclose(input);
	return 0;
}

int main(){
	apriori();
	return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define TRAN 10000
#define ITEM 20
#define LENGTH 10
#define MIN 300

#define GEM5_NUMPROCS 4

typedef struct aprioriset{
	int length;
	int support;
	char value[LENGTH];
}aprioriset;

typedef struct aprioristruct{
	int num;
	aprioriset valuelist[10000];
}aprioristruct;

typedef struct gencreturnstruct{
	int num;
	aprioriset valuelist[ITEM];
	int proper[ITEM];
}gencreturnstruct;

typedef struct gencstruct{
	aprioristruct* l;
	aprioriset* s;
	gencreturnstruct ret;
	int num;
	int length;
}gencstruct;

typedef struct genlstruct{
	aprioriset* c;
	aprioristruct* data;
}genlstruct;

typedef struct aprioriassvalue{
	char left[LENGTH];
	char right[LENGTH];
	float support;
	float confidence;
}aprioriassvalue;

typedef struct aprioriassstruct{
	int num;
	aprioriassvalue aprioriasslist[10000];
}aprioriassstruct;

typedef struct associationstruct{
	aprioriassvalue* dest;
	aprioriset* left;
	aprioriset* right;
}associationstruct;

double readtime, makec1time, makectime, makeltime, mergetime, asstime, writetime;

void readapriorib(aprioristruct* data, FILE* fp){
	data->num=TRAN;
	fread(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void saveapriorib(aprioristruct* data, FILE* fp){
	fwrite(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void readapriorinnb(aprioristruct* data, FILE* fp){
	fread(data, sizeof(aprioristruct), 1, fp);
}

void saveapriorinnb(aprioristruct* data, FILE* fp){
	fwrite(data, sizeof(aprioristruct), 1, fp);
}

void saveassstructb(aprioriassstruct* dest, FILE* fp){
	fwrite(dest->aprioriasslist, sizeof(aprioriassvalue), dest->num, fp);
}

void insertion(char* buf, char val, int length){
	int i, j;
	for(i=0;i<length;i++){
		if(buf[i]>val){
			break;
		}
	}
	for(j=length;j>i;j--){
		buf[j]=buf[j-1];
	}
	buf[i]=val;
}

int checkbuf(char* buf, char val, int length){
	int a;
	for(a=0;a<length;a++){
		if(buf[a]==val){
			return 1;
		}
	}
	return 0;
}

void initaprioriset(aprioriset** data, int length){
	(*data)=(aprioriset*)malloc(sizeof(aprioriset));
	(*data)->support=0;
	(*data)->length=length;
}

void deleteaprioriset(aprioriset** data){
//	free((*data)->value);
//	free(*data);
}

int compareset(aprioriset* a, aprioriset* b){
	if(a->length<b->length)
		return 1;
	else if(b->length<a->length)
		return 0;
	else{
		if(strncmp(a->value, b->value, a->length)<0)
			return 1;
		else return 0;
	}
}

int numofmatch(aprioriset* a, aprioriset* b){
	int ret=0;
	int acount=0, bcount=0;
	while(1){
		if(acount==a->length||bcount==b->length)
			break;
		if(a->value[acount]==b->value[bcount]){
			acount++;
			bcount++;
			ret++;
		}
		else if(a->value[acount]>b->value[bcount])
			bcount++;
		else
			acount++;
	}
	return ret;
}

int isequal(aprioriset* a, aprioriset* b){
	if(a->length==b->length)
		if(numofmatch(a, b)==a->length)
			return 1;
	return 0;
}

void add(aprioristruct* data, aprioriset* value, int equal){
	int i;
	if(equal){
		for(i=0;i<data->num;i++){
			if(isequal(value, &data->valuelist[i])){
				deleteaprioriset(&value);
				return;
			}
		}
	}
	data->valuelist[data->num]=*value;
	data->num++;
}

void mergeset(aprioriset* res, aprioriset* a, aprioriset* b){
	int i;
	char newval;
	for(i=0;i<a->length;i++){
		res->value[i]=a->value[i];
	}
	for(i=0;i<b->length;i++){
		if(checkbuf(res->value, b->value[i], a->length)==0){
			newval=b->value[i];
			break;
		}
	}
	insertion(res->value, newval, b->length);
}

int issubset(aprioriset* large, aprioriset* small){
	int largecount=0;
	int smallcount=0;
	char cl, cs;
	if(small->length>large->length)
		return 0;
	while(smallcount<small->length){
		if(largecount>=large->length)
			return 0;
		cl=large->value[largecount];
		cs=small->value[smallcount];
		if(cl==cs){
			largecount++;
			smallcount++;
		}
		else if(cl<cs)
			largecount++;
		else
			return 0;
	}
	return 1;
}

int isproper(aprioriset* set, aprioristruct* str){
	aprioriset* tset;
	int strcount;
	int setcount=set->length-1;
	initaprioriset(&tset, setcount);
	int i;
	for(i=0;i<set->length-1;i++){
		if(i==setcount)
			continue;
		else if(i<setcount){
			tset->value[i]=set->value[i];
		}
		else{
			tset->value[i-1]=set->value[i];
		}
	}
	
	for(strcount=0;strcount<str->num;strcount++){
		if(isequal(tset, &str->valuelist[strcount])){
			if(setcount==0)
				return 1;
			else{
				setcount--;
				for(i=0;i<set->length;i++){
					if(i==setcount)
						continue;
					else if(i<setcount){
						tset->value[i]=set->value[i];
					}
					else{
						tset->value[i-1]=set->value[i];
					}
				}
			}
		}
	}
	return 0;
}

void loadapriorifromfile(aprioristruct* data, FILE* fp){
	aprioriset* nowdata;
	int len, i, j;
	for(j=0;j<TRAN;j++){
		fscanf(fp, "%*s %*s %*s %d %*s", &len);
		initaprioriset(&nowdata, len);
		for(i=0;i<len;i++){
			fscanf(fp, "%*c %c", &(nowdata->value[i]));
		}
		add(data, nowdata, 0);
	}
}

void loadapriorifromfileb(aprioristruct* dest, FILE* fp){
	fread(dest, sizeof(aprioristruct), 1, fp);
}

void saveaprioritofile(aprioristruct* data, FILE* fp){
	int length;
	int i;
	int j;
	for(i=0;i<data->num;i++){
		length=data->valuelist[i].length;
		fprintf(fp, "LEN %02d SUP %03d :", length, data->valuelist[i].support);
		for(j=0;j<length;j++){
			fprintf(fp, " %c", data->valuelist[i].value[j]);
		}
		fprintf(fp, "\n");
	}
}

void saveaprioritofileb(aprioristruct* dest, FILE* fp){
	fwrite(dest, sizeof(aprioristruct), 1, fp);
}

void makec1(aprioristruct* target, aprioristruct* data){
	char itemlist[ITEM];
	int i=0, j, k;
	aprioriset* nowdata;
	for(k=0;k<data->num;k++){
		if(i==ITEM) break;
		nowdata=&data->valuelist[k];
		for(j=0;j<nowdata->length;j++){
			if(checkbuf(itemlist, nowdata->value[j], ITEM)==0){
				if(i==0)
					itemlist[0]=nowdata->value[j];
				else{
					insertion(itemlist, nowdata->value[j], i);
				}
				i++;
			}
		}
	}
	for(j=0;j<i;j++){
		initaprioriset(&nowdata, 1);
		nowdata->value[0]=itemlist[j];
		add(target, nowdata, 1);
	}
}

void* genlthreadfunc(void* thearg){
	genlstruct* arg=(genlstruct*)thearg;
	int datacount;
arg->c->support=0;
	for(datacount=0;datacount<arg->data->num;datacount++){
		if(issubset(&arg->data->valuelist[datacount], arg->c)){
			arg->c->support++;
		}
	}
	return NULL;
}

void genL(aprioristruct* l, aprioristruct* c, aprioristruct* data, int minnum){
	int ccount=0, cdatacount=0, datacount, i, j;
	aprioriset* nowdata;
	pthread_t thread[GEM5_NUMPROCS];
	genlstruct* genlstructs=(genlstruct*)malloc(sizeof(genlstruct)*c->num);
	if(c->num<GEM5_NUMPROCS){
		for(ccount=0;ccount<c->num;ccount++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
		}
		for(ccount=0;ccount<c->num;ccount++){
			pthread_join(thread[ccount], NULL);
		}
		for(ccount=0;ccount<c->num;ccount++){
			nowdata=&c->valuelist[ccount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
			ccount++;
		}
		while(ccount<c->num){
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				pthread_join(thread[i], NULL);	
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				nowdata=&c->valuelist[cdatacount];
				if(nowdata->support<minnum){
					deleteaprioriset(&nowdata);
				}
				else{
					add(l, nowdata, 1);
				}
				cdatacount++;
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				genlstructs[ccount].c=&c->valuelist[ccount];
				genlstructs[ccount].data=data;
				pthread_create(&thread[i], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
				ccount++;
				if(ccount==c->num)
					break;
			}
		}
		i++;
		for(j=0;j<i;j++){
			pthread_join(thread[j], NULL);
		}
		for(j=0;j<i;j++){
			nowdata=&c->valuelist[cdatacount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
			cdatacount++;
			if(cdatacount==c->num)
				break;
		}
	}
	c->num=0;
	free(genlstructs);
}



void* gencthreadfunc(void* thearg){
	gencstruct* arg=(gencstruct*)thearg;
	gencreturnstruct* ret=&arg->ret;
	aprioriset tset;
	aprioriset* pset=&tset;

	int i, j=0, k, flag, ccount=0;

	ret->num=0;
	for(i=arg->num;i<arg->l->num;i++){
		flag=0;
		if(numofmatch(&arg->l->valuelist[i], arg->s)==arg->length-1){
			mergeset(pset, &arg->l->valuelist[i], arg->s);
				pset->length=arg->length+1;
			for(k=0;k<j;k++){
				if(isequal(pset, &ret->valuelist[k])){
					flag=1;
					break;
				}
			}
			if(flag){
				deleteaprioriset(&pset);
			}
			else{
				ret->valuelist[j]=*pset;
				ret->proper[j]=isproper(&ret->valuelist[j], arg->l);
				ret->num++;
				j++;
			}
		}
	}
}

void genC(aprioristruct* c, aprioristruct* l){
	pthread_t thread[GEM5_NUMPROCS];
	gencstruct strarg[GEM5_NUMPROCS];
	gencstruct strarg2[GEM5_NUMPROCS];
	gencstruct* parg;
	int length=l->valuelist[0].length;
	int threadrun[GEM5_NUMPROCS]={0,};
	int threadrun2[GEM5_NUMPROCS]={0,};
	int* pthreadrun;
	int i, j, count=0, toggle=0, ccount=0;

	if(l->num<GEM5_NUMPROCS){
		for(i=0;i<l->num;i++){
			strarg[i].l=l;
			strarg[i].s=&l->valuelist[i];
			strarg[i].length=length;
			strarg[i].num=i+1;
			pthread_create(&thread[i], NULL, gencthreadfunc, (void*)&strarg[i]);
		}
		for(i=0;i<l->num;i++){
			pthread_join(thread[i], NULL);
		}
		for(i=0;i<l->num;i++){
			for(j=0;j<strarg[i].ret.num;j++){
				if(strarg[i].ret.proper[j]){
					add(c, &strarg[i].ret.valuelist[j], 1);
				}
			}
		}
	}
	else{
		for(count=0;count<GEM5_NUMPROCS-1;count++){
			strarg[count].l=l;
			strarg[count].s=&l->valuelist[count];
			strarg[count].length=length;
			strarg[count].num=count+1;
			pthread_create(&thread[count], NULL, gencthreadfunc, (void*)&strarg[count]);
			ccount++;
		}
		toggle=1;
		while(count<l->num){
			if(ccount==GEM5_NUMPROCS-1){
				for(i=0;i<GEM5_NUMPROCS-1;i++){
					pthread_join(thread[i], NULL);
					
				}
				ccount=0;
			}
			parg=toggle?strarg2:strarg;
			pthreadrun=toggle?threadrun2:threadrun;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				(parg+i)->l=l;
				(parg+i)->s=&l->valuelist[count];
				(parg+i)->length=length;
				(parg+i)->num=count+1;
				pthread_create(&thread[i], NULL, gencthreadfunc, (void*)(parg+i));
				count++;
				ccount++;
				*(pthreadrun+i)=1;
				if(count==l->num)
					break;
			}
			parg=toggle?strarg:strarg2;
			pthreadrun=toggle?threadrun:threadrun2;
			toggle=!toggle;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				*(pthreadrun+i)=0;
				for(j=0;j<(parg+i)->ret.num;j++){
					if((parg+i)->ret.proper[j]){
						add(c, &(parg+i)->ret.valuelist[j], 1);
					}
				}
			}
		}
		pthreadrun=toggle?threadrun:threadrun2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			pthread_join(thread[i], NULL);
		}
		parg=toggle?strarg:strarg2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			for(j=0;j<(parg+i)->ret.num;j++){
				if((parg+i)->ret.proper[j]){
					add(c, &(parg+i)->ret.valuelist[j], 1);
				}
			}
		}
	}
}
void mergestruct(aprioristruct* target, aprioristruct* l){
	int i;
	for(i=0;i<l->num;i++){
		add(target, &l->valuelist[i], 1);
	}
}

void* getassociationrulefunc(void* thearg){
	associationstruct* assstruct=(associationstruct*)thearg;
	aprioriset* left=assstruct->left, *right=assstruct->right;
	int k=0, l=0, m=0;
	for(k=0;k<right->length;k++){
		if(checkbuf(left->value, right->value[k], left->length)){
			assstruct->dest->left[l]=right->value[k];
			l++;
		}
		else{
			assstruct->dest->right[m]=right->value[k];
			m++;
		}
	}
	assstruct->dest->support=((float)right->support)/((float)TRAN);
	assstruct->dest->confidence=((float)right->support)/((float)left->support);
}

void getassociationrule(aprioriassstruct* dest, aprioristruct* list){
	aprioriset* right, *left;
	int i, j, k;
	pthread_t thread[GEM5_NUMPROCS];
	associationstruct assstruct[GEM5_NUMPROCS];
	int proccount=0;
	int threadrun[GEM5_NUMPROCS]={0,};
	for(i=0;i<list->num;i++){
		right=&list->valuelist[i];
		if(right->length<2)
			continue;
		for(j=0;j<list->num;j++){
			left=&list->valuelist[j];
			if(right->length==left->length)
				break;
			if(issubset(right, left)){

				if(proccount!=GEM5_NUMPROCS-1){
					for(k=0;k<proccount;k++){
						pthread_join(thread[k], NULL);
						threadrun[k]=0;
					}
					proccount=0;
				}
				assstruct[proccount].dest=&dest->aprioriasslist[dest->num];
				assstruct[proccount].left=left;
				assstruct[proccount].right=right;
				pthread_create(&thread[proccount], NULL, getassociationrulefunc, (void*)&assstruct[proccount]);
			
				threadrun[proccount]=1;
				dest->num++;
				proccount++;
			}
		}
	}
	for(i=0;i<proccount;i++){
		pthread_join(thread[i], NULL);
	}
}

int apriori(){
	aprioristruct data;
	aprioristruct candidate;
	aprioristruct result;
	FILE* datainput=fopen("adata", "rb");
	FILE* cinput=fopen("c1", "rb");
	FILE* output=fopen("l1", "wb");
	result.num=0;
	readapriorinnb(&data, datainput);
	readapriorinnb(&candidate, cinput);
	genL(&result, &candidate, &data, MIN);
	saveapriorinnb(&result, output);
	fclose(output);
	fclose(datainput);
	fclose(cinput);
	return 0;
}

int main(){
	apriori();
	return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define TRAN 10000
#define ITEM 20
#define LENGTH 10
#define MIN 300

#define GEM5_NUMPROCS 4

typedef struct aprioriset{
	int length;
	int support;
	char value[LENGTH];
}aprioriset;

typedef struct aprioristruct{
	int num;
	aprioriset valuelist[10000];
}aprioristruct;

typedef struct gencreturnstruct{
	int num;
	aprioriset valuelist[ITEM];
	int proper[ITEM];
}gencreturnstruct;

typedef struct gencstruct{
	aprioristruct* l;
	aprioriset* s;
	gencreturnstruct ret;
	int num;
	int length;
}gencstruct;

typedef struct genlstruct{
	aprioriset* c;
	aprioristruct* data;
}genlstruct;

typedef struct aprioriassvalue{
	char left[LENGTH];
	char right[LENGTH];
	float support;
	float confidence;
}aprioriassvalue;

typedef struct aprioriassstruct{
	int num;
	aprioriassvalue aprioriasslist[10000];
}aprioriassstruct;

typedef struct associationstruct{
	aprioriassvalue* dest;
	aprioriset* left;
	aprioriset* right;
}associationstruct;

double readtime, makec1time, makectime, makeltime, mergetime, asstime, writetime;

void readapriorib(aprioristruct* data, FILE* fp){
	data->num=TRAN;
	fread(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void saveapriorib(aprioristruct* data, FILE* fp){
	fwrite(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void readapriorinnb(aprioristruct* data, FILE* fp){
	fread(data, sizeof(aprioristruct), 1, fp);
}

void saveapriorinnb(aprioristruct* data, FILE* fp){
	fwrite(data, sizeof(aprioristruct), 1, fp);
}

void saveassstructb(aprioriassstruct* dest, FILE* fp){
	fwrite(dest->aprioriasslist, sizeof(aprioriassvalue), dest->num, fp);
}

void insertion(char* buf, char val, int length){
	int i, j;
	for(i=0;i<length;i++){
		if(buf[i]>val){
			break;
		}
	}
	for(j=length;j>i;j--){
		buf[j]=buf[j-1];
	}
	buf[i]=val;
}

int checkbuf(char* buf, char val, int length){
	int a;
	for(a=0;a<length;a++){
		if(buf[a]==val){
			return 1;
		}
	}
	return 0;
}

void initaprioriset(aprioriset** data, int length){
	(*data)=(aprioriset*)malloc(sizeof(aprioriset));
	(*data)->support=0;
	(*data)->length=length;
}

void deleteaprioriset(aprioriset** data){
//	free((*data)->value);
//	free(*data);
}

int compareset(aprioriset* a, aprioriset* b){
	if(a->length<b->length)
		return 1;
	else if(b->length<a->length)
		return 0;
	else{
		if(strncmp(a->value, b->value, a->length)<0)
			return 1;
		else return 0;
	}
}

int numofmatch(aprioriset* a, aprioriset* b){
	int ret=0;
	int acount=0, bcount=0;
	while(1){
		if(acount==a->length||bcount==b->length)
			break;
		if(a->value[acount]==b->value[bcount]){
			acount++;
			bcount++;
			ret++;
		}
		else if(a->value[acount]>b->value[bcount])
			bcount++;
		else
			acount++;
	}
	return ret;
}

int isequal(aprioriset* a, aprioriset* b){
	if(a->length==b->length)
		if(numofmatch(a, b)==a->length)
			return 1;
	return 0;
}

void add(aprioristruct* data, aprioriset* value, int equal){
	int i;
	if(equal){
		for(i=0;i<data->num;i++){
			if(isequal(value, &data->valuelist[i])){
				deleteaprioriset(&value);
				return;
			}
		}
	}
	data->valuelist[data->num]=*value;
	data->num++;
}

void mergeset(aprioriset* res, aprioriset* a, aprioriset* b){
	int i;
	char newval;
	for(i=0;i<a->length;i++){
		res->value[i]=a->value[i];
	}
	for(i=0;i<b->length;i++){
		if(checkbuf(res->value, b->value[i], a->length)==0){
			newval=b->value[i];
			break;
		}
	}
	insertion(res->value, newval, b->length);
}

int issubset(aprioriset* large, aprioriset* small){
	int largecount=0;
	int smallcount=0;
	char cl, cs;
	if(small->length>large->length)
		return 0;
	while(smallcount<small->length){
		if(largecount>=large->length)
			return 0;
		cl=large->value[largecount];
		cs=small->value[smallcount];
		if(cl==cs){
			largecount++;
			smallcount++;
		}
		else if(cl<cs)
			largecount++;
		else
			return 0;
	}
	return 1;
}

int isproper(aprioriset* set, aprioristruct* str){
	aprioriset* tset;
	int strcount;
	int setcount=set->length-1;
	initaprioriset(&tset, setcount);
	int i;
	for(i=0;i<set->length-1;i++){
		if(i==setcount)
			continue;
		else if(i<setcount){
			tset->value[i]=set->value[i];
		}
		else{
			tset->value[i-1]=set->value[i];
		}
	}
	
	for(strcount=0;strcount<str->num;strcount++){
		if(isequal(tset, &str->valuelist[strcount])){
			if(setcount==0)
				return 1;
			else{
				setcount--;
				for(i=0;i<set->length;i++){
					if(i==setcount)
						continue;
					else if(i<setcount){
						tset->value[i]=set->value[i];
					}
					else{
						tset->value[i-1]=set->value[i];
					}
				}
			}
		}
	}
	return 0;
}

void loadapriorifromfile(aprioristruct* data, FILE* fp){
	aprioriset* nowdata;
	int len, i, j;
	for(j=0;j<TRAN;j++){
		fscanf(fp, "%*s %*s %*s %d %*s", &len);
		initaprioriset(&nowdata, len);
		for(i=0;i<len;i++){
			fscanf(fp, "%*c %c", &(nowdata->value[i]));
		}
		add(data, nowdata, 0);
	}
}

void loadapriorifromfileb(aprioristruct* dest, FILE* fp){
	fread(dest, sizeof(aprioristruct), 1, fp);
}

void saveaprioritofile(aprioristruct* data, FILE* fp){
	int length;
	int i;
	int j;
	for(i=0;i<data->num;i++){
		length=data->valuelist[i].length;
		fprintf(fp, "LEN %02d SUP %03d :", length, data->valuelist[i].support);
		for(j=0;j<length;j++){
			fprintf(fp, " %c", data->valuelist[i].value[j]);
		}
		fprintf(fp, "\n");
	}
}

void saveaprioritofileb(aprioristruct* dest, FILE* fp){
	fwrite(dest, sizeof(aprioristruct), 1, fp);
}

void makec1(aprioristruct* target, aprioristruct* data){
	char itemlist[ITEM];
	int i=0, j, k;
	aprioriset* nowdata;
	for(k=0;k<data->num;k++){
		if(i==ITEM) break;
		nowdata=&data->valuelist[k];
		for(j=0;j<nowdata->length;j++){
			if(checkbuf(itemlist, nowdata->value[j], ITEM)==0){
				if(i==0)
					itemlist[0]=nowdata->value[j];
				else{
					insertion(itemlist, nowdata->value[j], i);
				}
				i++;
			}
		}
	}
	for(j=0;j<i;j++){
		initaprioriset(&nowdata, 1);
		nowdata->value[0]=itemlist[j];
		add(target, nowdata, 1);
	}
}

void* genlthreadfunc(void* thearg){
	genlstruct* arg=(genlstruct*)thearg;
	int datacount;
arg->c->support=0;
	for(datacount=0;datacount<arg->data->num;datacount++){
		if(issubset(&arg->data->valuelist[datacount], arg->c)){
			arg->c->support++;
		}
	}
	return NULL;
}

void genL(aprioristruct* l, aprioristruct* c, aprioristruct* data, int minnum){
	int ccount=0, cdatacount=0, datacount, i, j;
	aprioriset* nowdata;
	pthread_t thread[GEM5_NUMPROCS];
	genlstruct* genlstructs=(genlstruct*)malloc(sizeof(genlstruct)*c->num);
	if(c->num<GEM5_NUMPROCS){
		for(ccount=0;ccount<c->num;ccount++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
		}
		for(ccount=0;ccount<c->num;ccount++){
			pthread_join(thread[ccount], NULL);
		}
		for(ccount=0;ccount<c->num;ccount++){
			nowdata=&c->valuelist[ccount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
			ccount++;
		}
		while(ccount<c->num){
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				pthread_join(thread[i], NULL);	
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				nowdata=&c->valuelist[cdatacount];
				if(nowdata->support<minnum){
					deleteaprioriset(&nowdata);
				}
				else{
					add(l, nowdata, 1);
				}
				cdatacount++;
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				genlstructs[ccount].c=&c->valuelist[ccount];
				genlstructs[ccount].data=data;
				pthread_create(&thread[i], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
				ccount++;
				if(ccount==c->num)
					break;
			}
		}
		i++;
		for(j=0;j<i;j++){
			pthread_join(thread[j], NULL);
		}
		for(j=0;j<i;j++){
			nowdata=&c->valuelist[cdatacount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
			cdatacount++;
			if(cdatacount==c->num)
				break;
		}
	}
	c->num=0;
	free(genlstructs);
}



void* gencthreadfunc(void* thearg){
	gencstruct* arg=(gencstruct*)thearg;
	gencreturnstruct* ret=&arg->ret;
	aprioriset tset;
	aprioriset* pset=&tset;

	int i, j=0, k, flag, ccount=0;

	ret->num=0;
	for(i=arg->num;i<arg->l->num;i++){
		flag=0;
		if(numofmatch(&arg->l->valuelist[i], arg->s)==arg->length-1){
			mergeset(pset, &arg->l->valuelist[i], arg->s);
				pset->length=arg->length+1;
			for(k=0;k<j;k++){
				if(isequal(pset, &ret->valuelist[k])){
					flag=1;
					break;
				}
			}
			if(flag){
				deleteaprioriset(&pset);
			}
			else{
				ret->valuelist[j]=*pset;
				ret->proper[j]=isproper(&ret->valuelist[j], arg->l);
				ret->num++;
				j++;
			}
		}
	}
}

void genC(aprioristruct* c, aprioristruct* l){
	pthread_t thread[GEM5_NUMPROCS];
	gencstruct strarg[GEM5_NUMPROCS];
	gencstruct strarg2[GEM5_NUMPROCS];
	gencstruct* parg;
	int length=l->valuelist[0].length;
	int threadrun[GEM5_NUMPROCS]={0,};
	int threadrun2[GEM5_NUMPROCS]={0,};
	int* pthreadrun;
	int i, j, count=0, toggle=0, ccount=0;

	if(l->num<GEM5_NUMPROCS){
		for(i=0;i<l->num;i++){
			strarg[i].l=l;
			strarg[i].s=&l->valuelist[i];
			strarg[i].length=length;
			strarg[i].num=i+1;
			pthread_create(&thread[i], NULL, gencthreadfunc, (void*)&strarg[i]);
		}
		for(i=0;i<l->num;i++){
			pthread_join(thread[i], NULL);
		}
		for(i=0;i<l->num;i++){
			for(j=0;j<strarg[i].ret.num;j++){
				if(strarg[i].ret.proper[j]){
					add(c, &strarg[i].ret.valuelist[j], 1);
				}
			}
		}
	}
	else{
		for(count=0;count<GEM5_NUMPROCS-1;count++){
			strarg[count].l=l;
			strarg[count].s=&l->valuelist[count];
			strarg[count].length=length;
			strarg[count].num=count+1;
			pthread_create(&thread[count], NULL, gencthreadfunc, (void*)&strarg[count]);
			ccount++;
		}
		toggle=1;
		while(count<l->num){
			if(ccount==GEM5_NUMPROCS-1){
				for(i=0;i<GEM5_NUMPROCS-1;i++){
					pthread_join(thread[i], NULL);
					
				}
				ccount=0;
			}
			parg=toggle?strarg2:strarg;
			pthreadrun=toggle?threadrun2:threadrun;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				(parg+i)->l=l;
				(parg+i)->s=&l->valuelist[count];
				(parg+i)->length=length;
				(parg+i)->num=count+1;
				pthread_create(&thread[i], NULL, gencthreadfunc, (void*)(parg+i));
				count++;
				ccount++;
				*(pthreadrun+i)=1;
				if(count==l->num)
					break;
			}
			parg=toggle?strarg:strarg2;
			pthreadrun=toggle?threadrun:threadrun2;
			toggle=!toggle;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				*(pthreadrun+i)=0;
				for(j=0;j<(parg+i)->ret.num;j++){
					if((parg+i)->ret.proper[j]){
						add(c, &(parg+i)->ret.valuelist[j], 1);
					}
				}
			}
		}
		pthreadrun=toggle?threadrun:threadrun2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			pthread_join(thread[i], NULL);
		}
		parg=toggle?strarg:strarg2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			for(j=0;j<(parg+i)->ret.num;j++){
				if((parg+i)->ret.proper[j]){
					add(c, &(parg+i)->ret.valuelist[j], 1);
				}
			}
		}
	}
}
void mergestruct(aprioristruct* target, aprioristruct* l){
	int i;
	for(i=0;i<l->num;i++){
		add(target, &l->valuelist[i], 1);
	}
}

void* getassociationrulefunc(void* thearg){
	associationstruct* assstruct=(associationstruct*)thearg;
	aprioriset* left=assstruct->left, *right=assstruct->right;
	int k=0, l=0, m=0;
	for(k=0;k<right->length;k++){
		if(checkbuf(left->value, right->value[k], left->length)){
			assstruct->dest->left[l]=right->value[k];
			l++;
		}
		else{
			assstruct->dest->right[m]=right->value[k];
			m++;
		}
	}
	assstruct->dest->support=((float)right->support)/((float)TRAN);
	assstruct->dest->confidence=((float)right->support)/((float)left->support);
}

void getassociationrule(aprioriassstruct* dest, aprioristruct* list){
	aprioriset* right, *left;
	int i, j, k;
	pthread_t thread[GEM5_NUMPROCS];
	associationstruct assstruct[GEM5_NUMPROCS];
	int proccount=0;
	int threadrun[GEM5_NUMPROCS]={0,};
	for(i=0;i<list->num;i++){
		right=&list->valuelist[i];
		if(right->length<2)
			continue;
		for(j=0;j<list->num;j++){
			left=&list->valuelist[j];
			if(right->length==left->length)
				break;
			if(issubset(right, left)){

				if(proccount!=GEM5_NUMPROCS-1){
					for(k=0;k<proccount;k++){
						pthread_join(thread[k], NULL);
						threadrun[k]=0;
					}
					proccount=0;
				}
				assstruct[proccount].dest=&dest->aprioriasslist[dest->num];
				assstruct[proccount].left=left;
				assstruct[proccount].right=right;
				pthread_create(&thread[proccount], NULL, getassociationrulefunc, (void*)&assstruct[proccount]);
			
				threadrun[proccount]=1;
				dest->num++;
				proccount++;
			}
		}
	}
	for(i=0;i<proccount;i++){
		pthread_join(thread[i], NULL);
	}
}

int apriori(){
	aprioristruct data;
	aprioristruct candidate;
	aprioristruct result;
	FILE* datainput=fopen("adata", "rb");
	FILE* cinput=fopen("c2", "rb");
	FILE* output=fopen("l2", "wb");
	result.num=0;
	readapriorinnb(&data, datainput);
	readapriorinnb(&candidate, cinput);
	genL(&result, &candidate, &data, MIN);
	saveapriorinnb(&result, output);
	fclose(output);
	fclose(datainput);
	fclose(cinput);
	return 0;
}

int main(){
	apriori();
	return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define TRAN 10000
#define ITEM 20
#define LENGTH 10
#define MIN 300

#define GEM5_NUMPROCS 4

typedef struct aprioriset{
	int length;
	int support;
	char value[LENGTH];
}aprioriset;

typedef struct aprioristruct{
	int num;
	aprioriset valuelist[10000];
}aprioristruct;

typedef struct gencreturnstruct{
	int num;
	aprioriset valuelist[ITEM];
	int proper[ITEM];
}gencreturnstruct;

typedef struct gencstruct{
	aprioristruct* l;
	aprioriset* s;
	gencreturnstruct ret;
	int num;
	int length;
}gencstruct;

typedef struct genlstruct{
	aprioriset* c;
	aprioristruct* data;
}genlstruct;

typedef struct aprioriassvalue{
	char left[LENGTH];
	char right[LENGTH];
	float support;
	float confidence;
}aprioriassvalue;

typedef struct aprioriassstruct{
	int num;
	aprioriassvalue aprioriasslist[10000];
}aprioriassstruct;

typedef struct associationstruct{
	aprioriassvalue* dest;
	aprioriset* left;
	aprioriset* right;
}associationstruct;

double readtime, makec1time, makectime, makeltime, mergetime, asstime, writetime;

void readapriorib(aprioristruct* data, FILE* fp){
	data->num=TRAN;
	fread(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void saveapriorib(aprioristruct* data, FILE* fp){
	fwrite(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void readapriorinnb(aprioristruct* data, FILE* fp){
	fread(data, sizeof(aprioristruct), 1, fp);
}

void saveapriorinnb(aprioristruct* data, FILE* fp){
	fwrite(data, sizeof(aprioristruct), 1, fp);
}

void saveassstructb(aprioriassstruct* dest, FILE* fp){
	fwrite(dest->aprioriasslist, sizeof(aprioriassvalue), dest->num, fp);
}

void insertion(char* buf, char val, int length){
	int i, j;
	for(i=0;i<length;i++){
		if(buf[i]>val){
			break;
		}
	}
	for(j=length;j>i;j--){
		buf[j]=buf[j-1];
	}
	buf[i]=val;
}

int checkbuf(char* buf, char val, int length){
	int a;
	for(a=0;a<length;a++){
		if(buf[a]==val){
			return 1;
		}
	}
	return 0;
}

void initaprioriset(aprioriset** data, int length){
	(*data)=(aprioriset*)malloc(sizeof(aprioriset));
	(*data)->support=0;
	(*data)->length=length;
}

void deleteaprioriset(aprioriset** data){
//	free((*data)->value);
//	free(*data);
}

int compareset(aprioriset* a, aprioriset* b){
	if(a->length<b->length)
		return 1;
	else if(b->length<a->length)
		return 0;
	else{
		if(strncmp(a->value, b->value, a->length)<0)
			return 1;
		else return 0;
	}
}

int numofmatch(aprioriset* a, aprioriset* b){
	int ret=0;
	int acount=0, bcount=0;
	while(1){
		if(acount==a->length||bcount==b->length)
			break;
		if(a->value[acount]==b->value[bcount]){
			acount++;
			bcount++;
			ret++;
		}
		else if(a->value[acount]>b->value[bcount])
			bcount++;
		else
			acount++;
	}
	return ret;
}

int isequal(aprioriset* a, aprioriset* b){
	if(a->length==b->length)
		if(numofmatch(a, b)==a->length)
			return 1;
	return 0;
}

void add(aprioristruct* data, aprioriset* value, int equal){
	int i;
	if(equal){
		for(i=0;i<data->num;i++){
			if(isequal(value, &data->valuelist[i])){
				deleteaprioriset(&value);
				return;
			}
		}
	}
	data->valuelist[data->num]=*value;
	data->num++;
}

void mergeset(aprioriset* res, aprioriset* a, aprioriset* b){
	int i;
	char newval;
	for(i=0;i<a->length;i++){
		res->value[i]=a->value[i];
	}
	for(i=0;i<b->length;i++){
		if(checkbuf(res->value, b->value[i], a->length)==0){
			newval=b->value[i];
			break;
		}
	}
	insertion(res->value, newval, b->length);
}

int issubset(aprioriset* large, aprioriset* small){
	int largecount=0;
	int smallcount=0;
	char cl, cs;
	if(small->length>large->length)
		return 0;
	while(smallcount<small->length){
		if(largecount>=large->length)
			return 0;
		cl=large->value[largecount];
		cs=small->value[smallcount];
		if(cl==cs){
			largecount++;
			smallcount++;
		}
		else if(cl<cs)
			largecount++;
		else
			return 0;
	}
	return 1;
}

int isproper(aprioriset* set, aprioristruct* str){
	aprioriset* tset;
	int strcount;
	int setcount=set->length-1;
	initaprioriset(&tset, setcount);
	int i;
	for(i=0;i<set->length-1;i++){
		if(i==setcount)
			continue;
		else if(i<setcount){
			tset->value[i]=set->value[i];
		}
		else{
			tset->value[i-1]=set->value[i];
		}
	}
	
	for(strcount=0;strcount<str->num;strcount++){
		if(isequal(tset, &str->valuelist[strcount])){
			if(setcount==0)
				return 1;
			else{
				setcount--;
				for(i=0;i<set->length;i++){
					if(i==setcount)
						continue;
					else if(i<setcount){
						tset->value[i]=set->value[i];
					}
					else{
						tset->value[i-1]=set->value[i];
					}
				}
			}
		}
	}
	return 0;
}

void loadapriorifromfile(aprioristruct* data, FILE* fp){
	aprioriset* nowdata;
	int len, i, j;
	for(j=0;j<TRAN;j++){
		fscanf(fp, "%*s %*s %*s %d %*s", &len);
		initaprioriset(&nowdata, len);
		for(i=0;i<len;i++){
			fscanf(fp, "%*c %c", &(nowdata->value[i]));
		}
		add(data, nowdata, 0);
	}
}

void loadapriorifromfileb(aprioristruct* dest, FILE* fp){
	fread(dest, sizeof(aprioristruct), 1, fp);
}

void saveaprioritofile(aprioristruct* data, FILE* fp){
	int length;
	int i;
	int j;
	for(i=0;i<data->num;i++){
		length=data->valuelist[i].length;
		fprintf(fp, "LEN %02d SUP %03d :", length, data->valuelist[i].support);
		for(j=0;j<length;j++){
			fprintf(fp, " %c", data->valuelist[i].value[j]);
		}
		fprintf(fp, "\n");
	}
}

void saveaprioritofileb(aprioristruct* dest, FILE* fp){
	fwrite(dest, sizeof(aprioristruct), 1, fp);
}

void makec1(aprioristruct* target, aprioristruct* data){
	char itemlist[ITEM];
	int i=0, j, k;
	aprioriset* nowdata;
	for(k=0;k<data->num;k++){
		if(i==ITEM) break;
		nowdata=&data->valuelist[k];
		for(j=0;j<nowdata->length;j++){
			if(checkbuf(itemlist, nowdata->value[j], ITEM)==0){
				if(i==0)
					itemlist[0]=nowdata->value[j];
				else{
					insertion(itemlist, nowdata->value[j], i);
				}
				i++;
			}
		}
	}
	for(j=0;j<i;j++){
		initaprioriset(&nowdata, 1);
		nowdata->value[0]=itemlist[j];
		add(target, nowdata, 1);
	}
}

void* genlthreadfunc(void* thearg){
	genlstruct* arg=(genlstruct*)thearg;
	int datacount;
arg->c->support=0;
	for(datacount=0;datacount<arg->data->num;datacount++){
		if(issubset(&arg->data->valuelist[datacount], arg->c)){
			arg->c->support++;
		}
	}
	return NULL;
}

void genL(aprioristruct* l, aprioristruct* c, aprioristruct* data, int minnum){
	int ccount=0, cdatacount=0, datacount, i, j;
	aprioriset* nowdata;
	pthread_t thread[GEM5_NUMPROCS];
	genlstruct* genlstructs=(genlstruct*)malloc(sizeof(genlstruct)*c->num);
	if(c->num<GEM5_NUMPROCS){
		for(ccount=0;ccount<c->num;ccount++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
		}
		for(ccount=0;ccount<c->num;ccount++){
			pthread_join(thread[ccount], NULL);
		}
		for(ccount=0;ccount<c->num;ccount++){
			nowdata=&c->valuelist[ccount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
			ccount++;
		}
		while(ccount<c->num){
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				pthread_join(thread[i], NULL);	
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				nowdata=&c->valuelist[cdatacount];
				if(nowdata->support<minnum){
					deleteaprioriset(&nowdata);
				}
				else{
					add(l, nowdata, 1);
				}
				cdatacount++;
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				genlstructs[ccount].c=&c->valuelist[ccount];
				genlstructs[ccount].data=data;
				pthread_create(&thread[i], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
				ccount++;
				if(ccount==c->num)
					break;
			}
		}
		i++;
		for(j=0;j<i;j++){
			pthread_join(thread[j], NULL);
		}
		for(j=0;j<i;j++){
			nowdata=&c->valuelist[cdatacount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
			cdatacount++;
			if(cdatacount==c->num)
				break;
		}
	}
	c->num=0;
	free(genlstructs);
}



void* gencthreadfunc(void* thearg){
	gencstruct* arg=(gencstruct*)thearg;
	gencreturnstruct* ret=&arg->ret;
	aprioriset tset;
	aprioriset* pset=&tset;

	int i, j=0, k, flag, ccount=0;

	ret->num=0;
	for(i=arg->num;i<arg->l->num;i++){
		flag=0;
		if(numofmatch(&arg->l->valuelist[i], arg->s)==arg->length-1){
			mergeset(pset, &arg->l->valuelist[i], arg->s);
				pset->length=arg->length+1;
			for(k=0;k<j;k++){
				if(isequal(pset, &ret->valuelist[k])){
					flag=1;
					break;
				}
			}
			if(flag){
				deleteaprioriset(&pset);
			}
			else{
				ret->valuelist[j]=*pset;
				ret->proper[j]=isproper(&ret->valuelist[j], arg->l);
				ret->num++;
				j++;
			}
		}
	}
}

void genC(aprioristruct* c, aprioristruct* l){
	pthread_t thread[GEM5_NUMPROCS];
	gencstruct strarg[GEM5_NUMPROCS];
	gencstruct strarg2[GEM5_NUMPROCS];
	gencstruct* parg;
	int length=l->valuelist[0].length;
	int threadrun[GEM5_NUMPROCS]={0,};
	int threadrun2[GEM5_NUMPROCS]={0,};
	int* pthreadrun;
	int i, j, count=0, toggle=0, ccount=0;

	if(l->num<GEM5_NUMPROCS){
		for(i=0;i<l->num;i++){
			strarg[i].l=l;
			strarg[i].s=&l->valuelist[i];
			strarg[i].length=length;
			strarg[i].num=i+1;
			pthread_create(&thread[i], NULL, gencthreadfunc, (void*)&strarg[i]);
		}
		for(i=0;i<l->num;i++){
			pthread_join(thread[i], NULL);
		}
		for(i=0;i<l->num;i++){
			for(j=0;j<strarg[i].ret.num;j++){
				if(strarg[i].ret.proper[j]){
					add(c, &strarg[i].ret.valuelist[j], 1);
				}
			}
		}
	}
	else{
		for(count=0;count<GEM5_NUMPROCS-1;count++){
			strarg[count].l=l;
			strarg[count].s=&l->valuelist[count];
			strarg[count].length=length;
			strarg[count].num=count+1;
			pthread_create(&thread[count], NULL, gencthreadfunc, (void*)&strarg[count]);
			ccount++;
		}
		toggle=1;
		while(count<l->num){
			if(ccount==GEM5_NUMPROCS-1){
				for(i=0;i<GEM5_NUMPROCS-1;i++){
					pthread_join(thread[i], NULL);
					
				}
				ccount=0;
			}
			parg=toggle?strarg2:strarg;
			pthreadrun=toggle?threadrun2:threadrun;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				(parg+i)->l=l;
				(parg+i)->s=&l->valuelist[count];
				(parg+i)->length=length;
				(parg+i)->num=count+1;
				pthread_create(&thread[i], NULL, gencthreadfunc, (void*)(parg+i));
				count++;
				ccount++;
				*(pthreadrun+i)=1;
				if(count==l->num)
					break;
			}
			parg=toggle?strarg:strarg2;
			pthreadrun=toggle?threadrun:threadrun2;
			toggle=!toggle;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				*(pthreadrun+i)=0;
				for(j=0;j<(parg+i)->ret.num;j++){
					if((parg+i)->ret.proper[j]){
						add(c, &(parg+i)->ret.valuelist[j], 1);
					}
				}
			}
		}
		pthreadrun=toggle?threadrun:threadrun2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			pthread_join(thread[i], NULL);
		}
		parg=toggle?strarg:strarg2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			for(j=0;j<(parg+i)->ret.num;j++){
				if((parg+i)->ret.proper[j]){
					add(c, &(parg+i)->ret.valuelist[j], 1);
				}
			}
		}
	}
}
void mergestruct(aprioristruct* target, aprioristruct* l){
	int i;
	for(i=0;i<l->num;i++){
		add(target, &l->valuelist[i], 1);
	}
}

void* getassociationrulefunc(void* thearg){
	associationstruct* assstruct=(associationstruct*)thearg;
	aprioriset* left=assstruct->left, *right=assstruct->right;
	int k=0, l=0, m=0;
	for(k=0;k<right->length;k++){
		if(checkbuf(left->value, right->value[k], left->length)){
			assstruct->dest->left[l]=right->value[k];
			l++;
		}
		else{
			assstruct->dest->right[m]=right->value[k];
			m++;
		}
	}
	assstruct->dest->support=((float)right->support)/((float)TRAN);
	assstruct->dest->confidence=((float)right->support)/((float)left->support);
}

void getassociationrule(aprioriassstruct* dest, aprioristruct* list){
	aprioriset* right, *left;
	int i, j, k;
	pthread_t thread[GEM5_NUMPROCS];
	associationstruct assstruct[GEM5_NUMPROCS];
	int proccount=0;
	int threadrun[GEM5_NUMPROCS]={0,};
	for(i=0;i<list->num;i++){
		right=&list->valuelist[i];
		if(right->length<2)
			continue;
		for(j=0;j<list->num;j++){
			left=&list->valuelist[j];
			if(right->length==left->length)
				break;
			if(issubset(right, left)){

				if(proccount!=GEM5_NUMPROCS-1){
					for(k=0;k<proccount;k++){
						pthread_join(thread[k], NULL);
						threadrun[k]=0;
					}
					proccount=0;
				}
				assstruct[proccount].dest=&dest->aprioriasslist[dest->num];
				assstruct[proccount].left=left;
				assstruct[proccount].right=right;
				pthread_create(&thread[proccount], NULL, getassociationrulefunc, (void*)&assstruct[proccount]);
			
				threadrun[proccount]=1;
				dest->num++;
				proccount++;
			}
		}
	}
	for(i=0;i<proccount;i++){
		pthread_join(thread[i], NULL);
	}
}

int apriori(){
	aprioristruct data;
	aprioristruct candidate;
	aprioristruct result;
	FILE* datainput=fopen("adata", "rb");
	FILE* cinput=fopen("c3", "rb");
	FILE* output=fopen("l3", "wb");
	result.num=0;
	readapriorinnb(&data, datainput);
	readapriorinnb(&candidate, cinput);
	genL(&result, &candidate, &data, MIN);
	saveapriorinnb(&result, output);
	fclose(output);
	fclose(datainput);
	fclose(cinput);
	return 0;
}

int main(){
	apriori();
	return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define TRAN 10000
#define ITEM 20
#define LENGTH 10
#define MIN 300

#define GEM5_NUMPROCS 4

typedef struct aprioriset{
	int length;
	int support;
	char value[LENGTH];
}aprioriset;

typedef struct aprioristruct{
	int num;
	aprioriset valuelist[10000];
}aprioristruct;

typedef struct gencreturnstruct{
	int num;
	aprioriset valuelist[ITEM];
	int proper[ITEM];
}gencreturnstruct;

typedef struct gencstruct{
	aprioristruct* l;
	aprioriset* s;
	gencreturnstruct ret;
	int num;
	int length;
}gencstruct;

typedef struct genlstruct{
	aprioriset* c;
	aprioristruct* data;
}genlstruct;

typedef struct aprioriassvalue{
	char left[LENGTH];
	char right[LENGTH];
	float support;
	float confidence;
}aprioriassvalue;

typedef struct aprioriassstruct{
	int num;
	aprioriassvalue aprioriasslist[10000];
}aprioriassstruct;

typedef struct associationstruct{
	aprioriassvalue* dest;
	aprioriset* left;
	aprioriset* right;
}associationstruct;

double readtime, makec1time, makectime, makeltime, mergetime, asstime, writetime;

void readapriorib(aprioristruct* data, FILE* fp){
	data->num=TRAN;
	fread(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void saveapriorib(aprioristruct* data, FILE* fp){
	fwrite(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void readapriorinnb(aprioristruct* data, FILE* fp){
	fread(data, sizeof(aprioristruct), 1, fp);
}

void saveapriorinnb(aprioristruct* data, FILE* fp){
	fwrite(data, sizeof(aprioristruct), 1, fp);
}

void saveassstructb(aprioriassstruct* dest, FILE* fp){
	fwrite(dest->aprioriasslist, sizeof(aprioriassvalue), dest->num, fp);
}

void insertion(char* buf, char val, int length){
	int i, j;
	for(i=0;i<length;i++){
		if(buf[i]>val){
			break;
		}
	}
	for(j=length;j>i;j--){
		buf[j]=buf[j-1];
	}
	buf[i]=val;
}

int checkbuf(char* buf, char val, int length){
	int a;
	for(a=0;a<length;a++){
		if(buf[a]==val){
			return 1;
		}
	}
	return 0;
}

void initaprioriset(aprioriset** data, int length){
	(*data)=(aprioriset*)malloc(sizeof(aprioriset));
	(*data)->support=0;
	(*data)->length=length;
}

void deleteaprioriset(aprioriset** data){
//	free((*data)->value);
//	free(*data);
}

int compareset(aprioriset* a, aprioriset* b){
	if(a->length<b->length)
		return 1;
	else if(b->length<a->length)
		return 0;
	else{
		if(strncmp(a->value, b->value, a->length)<0)
			return 1;
		else return 0;
	}
}

int numofmatch(aprioriset* a, aprioriset* b){
	int ret=0;
	int acount=0, bcount=0;
	while(1){
		if(acount==a->length||bcount==b->length)
			break;
		if(a->value[acount]==b->value[bcount]){
			acount++;
			bcount++;
			ret++;
		}
		else if(a->value[acount]>b->value[bcount])
			bcount++;
		else
			acount++;
	}
	return ret;
}

int isequal(aprioriset* a, aprioriset* b){
	if(a->length==b->length)
		if(numofmatch(a, b)==a->length)
			return 1;
	return 0;
}

void add(aprioristruct* data, aprioriset* value, int equal){
	int i;
	if(equal){
		for(i=0;i<data->num;i++){
			if(isequal(value, &data->valuelist[i])){
				deleteaprioriset(&value);
				return;
			}
		}
	}
	data->valuelist[data->num]=*value;
	data->num++;
}

void mergeset(aprioriset* res, aprioriset* a, aprioriset* b){
	int i;
	char newval;
	for(i=0;i<a->length;i++){
		res->value[i]=a->value[i];
	}
	for(i=0;i<b->length;i++){
		if(checkbuf(res->value, b->value[i], a->length)==0){
			newval=b->value[i];
			break;
		}
	}
	insertion(res->value, newval, b->length);
}

int issubset(aprioriset* large, aprioriset* small){
	int largecount=0;
	int smallcount=0;
	char cl, cs;
	if(small->length>large->length)
		return 0;
	while(smallcount<small->length){
		if(largecount>=large->length)
			return 0;
		cl=large->value[largecount];
		cs=small->value[smallcount];
		if(cl==cs){
			largecount++;
			smallcount++;
		}
		else if(cl<cs)
			largecount++;
		else
			return 0;
	}
	return 1;
}

int isproper(aprioriset* set, aprioristruct* str){
	aprioriset* tset;
	int strcount;
	int setcount=set->length-1;
	initaprioriset(&tset, setcount);
	int i;
	for(i=0;i<set->length-1;i++){
		if(i==setcount)
			continue;
		else if(i<setcount){
			tset->value[i]=set->value[i];
		}
		else{
			tset->value[i-1]=set->value[i];
		}
	}
	
	for(strcount=0;strcount<str->num;strcount++){
		if(isequal(tset, &str->valuelist[strcount])){
			if(setcount==0)
				return 1;
			else{
				setcount--;
				for(i=0;i<set->length;i++){
					if(i==setcount)
						continue;
					else if(i<setcount){
						tset->value[i]=set->value[i];
					}
					else{
						tset->value[i-1]=set->value[i];
					}
				}
			}
		}
	}
	return 0;
}

void loadapriorifromfile(aprioristruct* data, FILE* fp){
	aprioriset* nowdata;
	int len, i, j;
	for(j=0;j<TRAN;j++){
		fscanf(fp, "%*s %*s %*s %d %*s", &len);
		initaprioriset(&nowdata, len);
		for(i=0;i<len;i++){
			fscanf(fp, "%*c %c", &(nowdata->value[i]));
		}
		add(data, nowdata, 0);
	}
}

void loadapriorifromfileb(aprioristruct* dest, FILE* fp){
	fread(dest, sizeof(aprioristruct), 1, fp);
}

void saveaprioritofile(aprioristruct* data, FILE* fp){
	int length;
	int i;
	int j;
	for(i=0;i<data->num;i++){
		length=data->valuelist[i].length;
		fprintf(fp, "LEN %02d SUP %03d :", length, data->valuelist[i].support);
		for(j=0;j<length;j++){
			fprintf(fp, " %c", data->valuelist[i].value[j]);
		}
		fprintf(fp, "\n");
	}
}

void saveaprioritofileb(aprioristruct* dest, FILE* fp){
	fwrite(dest, sizeof(aprioristruct), 1, fp);
}

void makec1(aprioristruct* target, aprioristruct* data){
	char itemlist[ITEM];
	int i=0, j, k;
	aprioriset* nowdata;
	for(k=0;k<data->num;k++){
		if(i==ITEM) break;
		nowdata=&data->valuelist[k];
		for(j=0;j<nowdata->length;j++){
			if(checkbuf(itemlist, nowdata->value[j], ITEM)==0){
				if(i==0)
					itemlist[0]=nowdata->value[j];
				else{
					insertion(itemlist, nowdata->value[j], i);
				}
				i++;
			}
		}
	}
	for(j=0;j<i;j++){
		initaprioriset(&nowdata, 1);
		nowdata->value[0]=itemlist[j];
		add(target, nowdata, 1);
	}
}

void* genlthreadfunc(void* thearg){
	genlstruct* arg=(genlstruct*)thearg;
	int datacount;
arg->c->support=0;
	for(datacount=0;datacount<arg->data->num;datacount++){
		if(issubset(&arg->data->valuelist[datacount], arg->c)){
			arg->c->support++;
		}
	}
	return NULL;
}

void genL(aprioristruct* l, aprioristruct* c, aprioristruct* data, int minnum){
	int ccount=0, cdatacount=0, datacount, i, j;
	aprioriset* nowdata;
	pthread_t thread[GEM5_NUMPROCS];
	genlstruct* genlstructs=(genlstruct*)malloc(sizeof(genlstruct)*c->num);
	if(c->num<GEM5_NUMPROCS){
		for(ccount=0;ccount<c->num;ccount++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
		}
		for(ccount=0;ccount<c->num;ccount++){
			pthread_join(thread[ccount], NULL);
		}
		for(ccount=0;ccount<c->num;ccount++){
			nowdata=&c->valuelist[ccount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
			ccount++;
		}
		while(ccount<c->num){
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				pthread_join(thread[i], NULL);	
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				nowdata=&c->valuelist[cdatacount];
				if(nowdata->support<minnum){
					deleteaprioriset(&nowdata);
				}
				else{
					add(l, nowdata, 1);
				}
				cdatacount++;
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				genlstructs[ccount].c=&c->valuelist[ccount];
				genlstructs[ccount].data=data;
				pthread_create(&thread[i], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
				ccount++;
				if(ccount==c->num)
					break;
			}
		}
		i++;
		for(j=0;j<i;j++){
			pthread_join(thread[j], NULL);
		}
		for(j=0;j<i;j++){
			nowdata=&c->valuelist[cdatacount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
			cdatacount++;
			if(cdatacount==c->num)
				break;
		}
	}
	c->num=0;
	free(genlstructs);
}



void* gencthreadfunc(void* thearg){
	gencstruct* arg=(gencstruct*)thearg;
	gencreturnstruct* ret=&arg->ret;
	aprioriset tset;
	aprioriset* pset=&tset;

	int i, j=0, k, flag, ccount=0;

	ret->num=0;
	for(i=arg->num;i<arg->l->num;i++){
		flag=0;
		if(numofmatch(&arg->l->valuelist[i], arg->s)==arg->length-1){
			mergeset(pset, &arg->l->valuelist[i], arg->s);
				pset->length=arg->length+1;
			for(k=0;k<j;k++){
				if(isequal(pset, &ret->valuelist[k])){
					flag=1;
					break;
				}
			}
			if(flag){
				deleteaprioriset(&pset);
			}
			else{
				ret->valuelist[j]=*pset;
				ret->proper[j]=isproper(&ret->valuelist[j], arg->l);
				ret->num++;
				j++;
			}
		}
	}
}

void genC(aprioristruct* c, aprioristruct* l){
	pthread_t thread[GEM5_NUMPROCS];
	gencstruct strarg[GEM5_NUMPROCS];
	gencstruct strarg2[GEM5_NUMPROCS];
	gencstruct* parg;
	int length=l->valuelist[0].length;
	int threadrun[GEM5_NUMPROCS]={0,};
	int threadrun2[GEM5_NUMPROCS]={0,};
	int* pthreadrun;
	int i, j, count=0, toggle=0, ccount=0;

	if(l->num<GEM5_NUMPROCS){
		for(i=0;i<l->num;i++){
			strarg[i].l=l;
			strarg[i].s=&l->valuelist[i];
			strarg[i].length=length;
			strarg[i].num=i+1;
			pthread_create(&thread[i], NULL, gencthreadfunc, (void*)&strarg[i]);
		}
		for(i=0;i<l->num;i++){
			pthread_join(thread[i], NULL);
		}
		for(i=0;i<l->num;i++){
			for(j=0;j<strarg[i].ret.num;j++){
				if(strarg[i].ret.proper[j]){
					add(c, &strarg[i].ret.valuelist[j], 1);
				}
			}
		}
	}
	else{
		for(count=0;count<GEM5_NUMPROCS-1;count++){
			strarg[count].l=l;
			strarg[count].s=&l->valuelist[count];
			strarg[count].length=length;
			strarg[count].num=count+1;
			pthread_create(&thread[count], NULL, gencthreadfunc, (void*)&strarg[count]);
			ccount++;
		}
		toggle=1;
		while(count<l->num){
			if(ccount==GEM5_NUMPROCS-1){
				for(i=0;i<GEM5_NUMPROCS-1;i++){
					pthread_join(thread[i], NULL);
					
				}
				ccount=0;
			}
			parg=toggle?strarg2:strarg;
			pthreadrun=toggle?threadrun2:threadrun;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				(parg+i)->l=l;
				(parg+i)->s=&l->valuelist[count];
				(parg+i)->length=length;
				(parg+i)->num=count+1;
				pthread_create(&thread[i], NULL, gencthreadfunc, (void*)(parg+i));
				count++;
				ccount++;
				*(pthreadrun+i)=1;
				if(count==l->num)
					break;
			}
			parg=toggle?strarg:strarg2;
			pthreadrun=toggle?threadrun:threadrun2;
			toggle=!toggle;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				*(pthreadrun+i)=0;
				for(j=0;j<(parg+i)->ret.num;j++){
					if((parg+i)->ret.proper[j]){
						add(c, &(parg+i)->ret.valuelist[j], 1);
					}
				}
			}
		}
		pthreadrun=toggle?threadrun:threadrun2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			pthread_join(thread[i], NULL);
		}
		parg=toggle?strarg:strarg2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			for(j=0;j<(parg+i)->ret.num;j++){
				if((parg+i)->ret.proper[j]){
					add(c, &(parg+i)->ret.valuelist[j], 1);
				}
			}
		}
	}
}
void mergestruct(aprioristruct* target, aprioristruct* l){
	int i;
	for(i=0;i<l->num;i++){
		add(target, &l->valuelist[i], 1);
	}
}

void* getassociationrulefunc(void* thearg){
	associationstruct* assstruct=(associationstruct*)thearg;
	aprioriset* left=assstruct->left, *right=assstruct->right;
	int k=0, l=0, m=0;
	for(k=0;k<right->length;k++){
		if(checkbuf(left->value, right->value[k], left->length)){
			assstruct->dest->left[l]=right->value[k];
			l++;
		}
		else{
			assstruct->dest->right[m]=right->value[k];
			m++;
		}
	}
	assstruct->dest->support=((float)right->support)/((float)TRAN);
	assstruct->dest->confidence=((float)right->support)/((float)left->support);
}

void getassociationrule(aprioriassstruct* dest, aprioristruct* list){
	aprioriset* right, *left;
	int i, j, k;
	pthread_t thread[GEM5_NUMPROCS];
	associationstruct assstruct[GEM5_NUMPROCS];
	int proccount=0;
	int threadrun[GEM5_NUMPROCS]={0,};
	for(i=0;i<list->num;i++){
		right=&list->valuelist[i];
		if(right->length<2)
			continue;
		for(j=0;j<list->num;j++){
			left=&list->valuelist[j];
			if(right->length==left->length)
				break;
			if(issubset(right, left)){

				if(proccount!=GEM5_NUMPROCS-1){
					for(k=0;k<proccount;k++){
						pthread_join(thread[k], NULL);
						threadrun[k]=0;
					}
					proccount=0;
				}
				assstruct[proccount].dest=&dest->aprioriasslist[dest->num];
				assstruct[proccount].left=left;
				assstruct[proccount].right=right;
				pthread_create(&thread[proccount], NULL, getassociationrulefunc, (void*)&assstruct[proccount]);
			
				threadrun[proccount]=1;
				dest->num++;
				proccount++;
			}
		}
	}
	for(i=0;i<proccount;i++){
		pthread_join(thread[i], NULL);
	}
}

int apriori(){
	aprioristruct data;
	aprioristruct candidate;
	aprioristruct result;
	FILE* datainput=fopen("adata", "rb");
	FILE* cinput=fopen("c4", "rb");
	FILE* output=fopen("l4", "wb");
	result.num=0;
	readapriorinnb(&data, datainput);
	readapriorinnb(&candidate, cinput);
	genL(&result, &candidate, &data, MIN);
	saveapriorinnb(&result, output);
	fclose(output);
	fclose(datainput);
	fclose(cinput);
	return 0;
}

int main(){
	apriori();
	return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define TRAN 10000
#define ITEM 20
#define LENGTH 10
#define MIN 300

#define GEM5_NUMPROCS 4

typedef struct aprioriset{
	int length;
	int support;
	char value[LENGTH];
}aprioriset;

typedef struct aprioristruct{
	int num;
	aprioriset valuelist[10000];
}aprioristruct;

typedef struct gencreturnstruct{
	int num;
	aprioriset valuelist[ITEM];
	int proper[ITEM];
}gencreturnstruct;

typedef struct gencstruct{
	aprioristruct* l;
	aprioriset* s;
	gencreturnstruct ret;
	int num;
	int length;
}gencstruct;

typedef struct genlstruct{
	aprioriset* c;
	aprioristruct* data;
}genlstruct;

typedef struct aprioriassvalue{
	char left[LENGTH];
	char right[LENGTH];
	float support;
	float confidence;
}aprioriassvalue;

typedef struct aprioriassstruct{
	int num;
	aprioriassvalue aprioriasslist[10000];
}aprioriassstruct;

typedef struct associationstruct{
	aprioriassvalue* dest;
	aprioriset* left;
	aprioriset* right;
}associationstruct;

double readtime, makec1time, makectime, makeltime, mergetime, asstime, writetime;

void readapriorib(aprioristruct* data, FILE* fp){
	data->num=TRAN;
	fread(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void saveapriorib(aprioristruct* data, FILE* fp){
	fwrite(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void readapriorinnb(aprioristruct* data, FILE* fp){
	fread(data, sizeof(aprioristruct), 1, fp);
}

void saveapriorinnb(aprioristruct* data, FILE* fp){
	fwrite(data, sizeof(aprioristruct), 1, fp);
}

void saveassstructb(aprioriassstruct* dest, FILE* fp){
	fwrite(dest->aprioriasslist, sizeof(aprioriassvalue), dest->num, fp);
}

void insertion(char* buf, char val, int length){
	int i, j;
	for(i=0;i<length;i++){
		if(buf[i]>val){
			break;
		}
	}
	for(j=length;j>i;j--){
		buf[j]=buf[j-1];
	}
	buf[i]=val;
}

int checkbuf(char* buf, char val, int length){
	int a;
	for(a=0;a<length;a++){
		if(buf[a]==val){
			return 1;
		}
	}
	return 0;
}

void initaprioriset(aprioriset** data, int length){
	(*data)=(aprioriset*)malloc(sizeof(aprioriset));
	(*data)->support=0;
	(*data)->length=length;
}

void deleteaprioriset(aprioriset** data){
//	free((*data)->value);
//	free(*data);
}

int compareset(aprioriset* a, aprioriset* b){
	if(a->length<b->length)
		return 1;
	else if(b->length<a->length)
		return 0;
	else{
		if(strncmp(a->value, b->value, a->length)<0)
			return 1;
		else return 0;
	}
}

int numofmatch(aprioriset* a, aprioriset* b){
	int ret=0;
	int acount=0, bcount=0;
	while(1){
		if(acount==a->length||bcount==b->length)
			break;
		if(a->value[acount]==b->value[bcount]){
			acount++;
			bcount++;
			ret++;
		}
		else if(a->value[acount]>b->value[bcount])
			bcount++;
		else
			acount++;
	}
	return ret;
}

int isequal(aprioriset* a, aprioriset* b){
	if(a->length==b->length)
		if(numofmatch(a, b)==a->length)
			return 1;
	return 0;
}

void add(aprioristruct* data, aprioriset* value, int equal){
	int i;
	if(equal){
		for(i=0;i<data->num;i++){
			if(isequal(value, &data->valuelist[i])){
				deleteaprioriset(&value);
				return;
			}
		}
	}
	data->valuelist[data->num]=*value;
	data->num++;
}

void mergeset(aprioriset* res, aprioriset* a, aprioriset* b){
	int i;
	char newval;
	for(i=0;i<a->length;i++){
		res->value[i]=a->value[i];
	}
	for(i=0;i<b->length;i++){
		if(checkbuf(res->value, b->value[i], a->length)==0){
			newval=b->value[i];
			break;
		}
	}
	insertion(res->value, newval, b->length);
}

int issubset(aprioriset* large, aprioriset* small){
	int largecount=0;
	int smallcount=0;
	char cl, cs;
	if(small->length>large->length)
		return 0;
	while(smallcount<small->length){
		if(largecount>=large->length)
			return 0;
		cl=large->value[largecount];
		cs=small->value[smallcount];
		if(cl==cs){
			largecount++;
			smallcount++;
		}
		else if(cl<cs)
			largecount++;
		else
			return 0;
	}
	return 1;
}

int isproper(aprioriset* set, aprioristruct* str){
	aprioriset* tset;
	int strcount;
	int setcount=set->length-1;
	initaprioriset(&tset, setcount);
	int i;
	for(i=0;i<set->length-1;i++){
		if(i==setcount)
			continue;
		else if(i<setcount){
			tset->value[i]=set->value[i];
		}
		else{
			tset->value[i-1]=set->value[i];
		}
	}
	
	for(strcount=0;strcount<str->num;strcount++){
		if(isequal(tset, &str->valuelist[strcount])){
			if(setcount==0)
				return 1;
			else{
				setcount--;
				for(i=0;i<set->length;i++){
					if(i==setcount)
						continue;
					else if(i<setcount){
						tset->value[i]=set->value[i];
					}
					else{
						tset->value[i-1]=set->value[i];
					}
				}
			}
		}
	}
	return 0;
}

void loadapriorifromfile(aprioristruct* data, FILE* fp){
	aprioriset* nowdata;
	int len, i, j;
	for(j=0;j<TRAN;j++){
		fscanf(fp, "%*s %*s %*s %d %*s", &len);
		initaprioriset(&nowdata, len);
		for(i=0;i<len;i++){
			fscanf(fp, "%*c %c", &(nowdata->value[i]));
		}
		add(data, nowdata, 0);
	}
}

void loadapriorifromfileb(aprioristruct* dest, FILE* fp){
	fread(dest, sizeof(aprioristruct), 1, fp);
}

void saveaprioritofile(aprioristruct* data, FILE* fp){
	int length;
	int i;
	int j;
	for(i=0;i<data->num;i++){
		length=data->valuelist[i].length;
		fprintf(fp, "LEN %02d SUP %03d :", length, data->valuelist[i].support);
		for(j=0;j<length;j++){
			fprintf(fp, " %c", data->valuelist[i].value[j]);
		}
		fprintf(fp, "\n");
	}
}

void saveaprioritofileb(aprioristruct* dest, FILE* fp){
	fwrite(dest, sizeof(aprioristruct), 1, fp);
}

void makec1(aprioristruct* target, aprioristruct* data){
	char itemlist[ITEM];
	int i=0, j, k;
	aprioriset* nowdata;
	for(k=0;k<data->num;k++){
		if(i==ITEM) break;
		nowdata=&data->valuelist[k];
		for(j=0;j<nowdata->length;j++){
			if(checkbuf(itemlist, nowdata->value[j], ITEM)==0){
				if(i==0)
					itemlist[0]=nowdata->value[j];
				else{
					insertion(itemlist, nowdata->value[j], i);
				}
				i++;
			}
		}
	}
	for(j=0;j<i;j++){
		initaprioriset(&nowdata, 1);
		nowdata->value[0]=itemlist[j];
		add(target, nowdata, 1);
	}
}

void* genlthreadfunc(void* thearg){
	genlstruct* arg=(genlstruct*)thearg;
	int datacount;
arg->c->support=0;
	for(datacount=0;datacount<arg->data->num;datacount++){
		if(issubset(&arg->data->valuelist[datacount], arg->c)){
			arg->c->support++;
		}
	}
	return NULL;
}

void genL(aprioristruct* l, aprioristruct* c, aprioristruct* data, int minnum){
	int ccount=0, cdatacount=0, datacount, i, j;
	aprioriset* nowdata;
	pthread_t thread[GEM5_NUMPROCS];
	genlstruct* genlstructs=(genlstruct*)malloc(sizeof(genlstruct)*c->num);
	if(c->num<GEM5_NUMPROCS){
		for(ccount=0;ccount<c->num;ccount++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
		}
		for(ccount=0;ccount<c->num;ccount++){
			pthread_join(thread[ccount], NULL);
		}
		for(ccount=0;ccount<c->num;ccount++){
			nowdata=&c->valuelist[ccount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
			ccount++;
		}
		while(ccount<c->num){
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				pthread_join(thread[i], NULL);	
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				nowdata=&c->valuelist[cdatacount];
				if(nowdata->support<minnum){
					deleteaprioriset(&nowdata);
				}
				else{
					add(l, nowdata, 1);
				}
				cdatacount++;
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				genlstructs[ccount].c=&c->valuelist[ccount];
				genlstructs[ccount].data=data;
				pthread_create(&thread[i], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
				ccount++;
				if(ccount==c->num)
					break;
			}
		}
		i++;
		for(j=0;j<i;j++){
			pthread_join(thread[j], NULL);
		}
		for(j=0;j<i;j++){
			nowdata=&c->valuelist[cdatacount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
			cdatacount++;
			if(cdatacount==c->num)
				break;
		}
	}
	c->num=0;
	free(genlstructs);
}



void* gencthreadfunc(void* thearg){
	gencstruct* arg=(gencstruct*)thearg;
	gencreturnstruct* ret=&arg->ret;
	aprioriset tset;
	aprioriset* pset=&tset;

	int i, j=0, k, flag, ccount=0;

	ret->num=0;
	for(i=arg->num;i<arg->l->num;i++){
		flag=0;
		if(numofmatch(&arg->l->valuelist[i], arg->s)==arg->length-1){
			mergeset(pset, &arg->l->valuelist[i], arg->s);
				pset->length=arg->length+1;
			for(k=0;k<j;k++){
				if(isequal(pset, &ret->valuelist[k])){
					flag=1;
					break;
				}
			}
			if(flag){
				deleteaprioriset(&pset);
			}
			else{
				ret->valuelist[j]=*pset;
				ret->proper[j]=isproper(&ret->valuelist[j], arg->l);
				ret->num++;
				j++;
			}
		}
	}
}

void genC(aprioristruct* c, aprioristruct* l){
	pthread_t thread[GEM5_NUMPROCS];
	gencstruct strarg[GEM5_NUMPROCS];
	gencstruct strarg2[GEM5_NUMPROCS];
	gencstruct* parg;
	int length=l->valuelist[0].length;
	int threadrun[GEM5_NUMPROCS]={0,};
	int threadrun2[GEM5_NUMPROCS]={0,};
	int* pthreadrun;
	int i, j, count=0, toggle=0, ccount=0;

	if(l->num<GEM5_NUMPROCS){
		for(i=0;i<l->num;i++){
			strarg[i].l=l;
			strarg[i].s=&l->valuelist[i];
			strarg[i].length=length;
			strarg[i].num=i+1;
			pthread_create(&thread[i], NULL, gencthreadfunc, (void*)&strarg[i]);
		}
		for(i=0;i<l->num;i++){
			pthread_join(thread[i], NULL);
		}
		for(i=0;i<l->num;i++){
			for(j=0;j<strarg[i].ret.num;j++){
				if(strarg[i].ret.proper[j]){
					add(c, &strarg[i].ret.valuelist[j], 1);
				}
			}
		}
	}
	else{
		for(count=0;count<GEM5_NUMPROCS-1;count++){
			strarg[count].l=l;
			strarg[count].s=&l->valuelist[count];
			strarg[count].length=length;
			strarg[count].num=count+1;
			pthread_create(&thread[count], NULL, gencthreadfunc, (void*)&strarg[count]);
			ccount++;
		}
		toggle=1;
		while(count<l->num){
			if(ccount==GEM5_NUMPROCS-1){
				for(i=0;i<GEM5_NUMPROCS-1;i++){
					pthread_join(thread[i], NULL);
					
				}
				ccount=0;
			}
			parg=toggle?strarg2:strarg;
			pthreadrun=toggle?threadrun2:threadrun;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				(parg+i)->l=l;
				(parg+i)->s=&l->valuelist[count];
				(parg+i)->length=length;
				(parg+i)->num=count+1;
				pthread_create(&thread[i], NULL, gencthreadfunc, (void*)(parg+i));
				count++;
				ccount++;
				*(pthreadrun+i)=1;
				if(count==l->num)
					break;
			}
			parg=toggle?strarg:strarg2;
			pthreadrun=toggle?threadrun:threadrun2;
			toggle=!toggle;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				*(pthreadrun+i)=0;
				for(j=0;j<(parg+i)->ret.num;j++){
					if((parg+i)->ret.proper[j]){
						add(c, &(parg+i)->ret.valuelist[j], 1);
					}
				}
			}
		}
		pthreadrun=toggle?threadrun:threadrun2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			pthread_join(thread[i], NULL);
		}
		parg=toggle?strarg:strarg2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			for(j=0;j<(parg+i)->ret.num;j++){
				if((parg+i)->ret.proper[j]){
					add(c, &(parg+i)->ret.valuelist[j], 1);
				}
			}
		}
	}
}
void mergestruct(aprioristruct* target, aprioristruct* l){
	int i;
	for(i=0;i<l->num;i++){
		add(target, &l->valuelist[i], 1);
	}
}

void* getassociationrulefunc(void* thearg){
	associationstruct* assstruct=(associationstruct*)thearg;
	aprioriset* left=assstruct->left, *right=assstruct->right;
	int k=0, l=0, m=0;
	for(k=0;k<right->length;k++){
		if(checkbuf(left->value, right->value[k], left->length)){
			assstruct->dest->left[l]=right->value[k];
			l++;
		}
		else{
			assstruct->dest->right[m]=right->value[k];
			m++;
		}
	}
	assstruct->dest->support=((float)right->support)/((float)TRAN);
	assstruct->dest->confidence=((float)right->support)/((float)left->support);
}

void getassociationrule(aprioriassstruct* dest, aprioristruct* list){
	aprioriset* right, *left;
	int i, j, k;
	pthread_t thread[GEM5_NUMPROCS];
	associationstruct assstruct[GEM5_NUMPROCS];
	int proccount=0;
	int threadrun[GEM5_NUMPROCS]={0,};
	for(i=0;i<list->num;i++){
		right=&list->valuelist[i];
		if(right->length<2)
			continue;
		for(j=0;j<list->num;j++){
			left=&list->valuelist[j];
			if(right->length==left->length)
				break;
			if(issubset(right, left)){

				if(proccount!=GEM5_NUMPROCS-1){
					for(k=0;k<proccount;k++){
						pthread_join(thread[k], NULL);
						threadrun[k]=0;
					}
					proccount=0;
				}
				assstruct[proccount].dest=&dest->aprioriasslist[dest->num];
				assstruct[proccount].left=left;
				assstruct[proccount].right=right;
				pthread_create(&thread[proccount], NULL, getassociationrulefunc, (void*)&assstruct[proccount]);
			
				threadrun[proccount]=1;
				dest->num++;
				proccount++;
			}
		}
	}
	for(i=0;i<proccount;i++){
		pthread_join(thread[i], NULL);
	}
}

int apriori(){
	aprioristruct alists[4];
	aprioristruct data;
	aprioristruct candidate;
	aprioristruct result;
aprioriassstruct ass={0,};
	FILE* input1=fopen("l1", "rb");
	FILE* input2=fopen("l2", "rb");
	FILE* input3=fopen("l3", "rb");
	FILE* input4=fopen("l4", "rb");
FILE* output=fopen("merged", "wb");
	result.num=0;
	readapriorinnb(&alists[0], input1);
	readapriorinnb(&alists[1], input2);
	readapriorinnb(&alists[2], input3);
	readapriorinnb(&alists[3], input4);
	mergestruct(&result, &alists[0]);
	mergestruct(&result, &alists[1]);
	mergestruct(&result, &alists[2]);
	mergestruct(&result, &alists[3]);
	saveapriorinnb(&result, output);
	fclose(output);
	fclose(input1);
	fclose(input2);
	fclose(input3);
	fclose(input4);
	return 0;
}

int main(){
	apriori();
	return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define TRAN 10000
#define ITEM 20
#define LENGTH 10
#define MIN 300

#define GEM5_NUMPROCS 4

typedef struct aprioriset{
	int length;
	int support;
	char value[LENGTH];
}aprioriset;

typedef struct aprioristruct{
	int num;
	aprioriset valuelist[10000];
}aprioristruct;

typedef struct gencreturnstruct{
	int num;
	aprioriset valuelist[ITEM];
	int proper[ITEM];
}gencreturnstruct;

typedef struct gencstruct{
	aprioristruct* l;
	aprioriset* s;
	gencreturnstruct ret;
	int num;
	int length;
}gencstruct;

typedef struct genlstruct{
	aprioriset* c;
	aprioristruct* data;
}genlstruct;

typedef struct aprioriassvalue{
	char left[LENGTH];
	char right[LENGTH];
	float support;
	float confidence;
}aprioriassvalue;

typedef struct aprioriassstruct{
	int num;
	aprioriassvalue aprioriasslist[10000];
}aprioriassstruct;

typedef struct associationstruct{
	aprioriassvalue* dest;
	aprioriset* left;
	aprioriset* right;
}associationstruct;

double readtime, makec1time, makectime, makeltime, mergetime, asstime, writetime;

void readapriorib(aprioristruct* data, FILE* fp){
	data->num=TRAN;
	fread(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void saveapriorib(aprioristruct* data, FILE* fp){
	fwrite(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void readapriorinnb(aprioristruct* data, FILE* fp){
	fread(data, sizeof(aprioristruct), 1, fp);
}

void saveapriorinnb(aprioristruct* data, FILE* fp){
	fwrite(data, sizeof(aprioristruct), 1, fp);
}

void saveassstructb(aprioriassstruct* dest, FILE* fp){
	fwrite(dest->aprioriasslist, sizeof(aprioriassvalue), dest->num, fp);
}

void insertion(char* buf, char val, int length){
	int i, j;
	for(i=0;i<length;i++){
		if(buf[i]>val){
			break;
		}
	}
	for(j=length;j>i;j--){
		buf[j]=buf[j-1];
	}
	buf[i]=val;
}

int checkbuf(char* buf, char val, int length){
	int a;
	for(a=0;a<length;a++){
		if(buf[a]==val){
			return 1;
		}
	}
	return 0;
}

void initaprioriset(aprioriset** data, int length){
	(*data)=(aprioriset*)malloc(sizeof(aprioriset));
	(*data)->support=0;
	(*data)->length=length;
}

void deleteaprioriset(aprioriset** data){
//	free((*data)->value);
//	free(*data);
}

int compareset(aprioriset* a, aprioriset* b){
	if(a->length<b->length)
		return 1;
	else if(b->length<a->length)
		return 0;
	else{
		if(strncmp(a->value, b->value, a->length)<0)
			return 1;
		else return 0;
	}
}

int numofmatch(aprioriset* a, aprioriset* b){
	int ret=0;
	int acount=0, bcount=0;
	while(1){
		if(acount==a->length||bcount==b->length)
			break;
		if(a->value[acount]==b->value[bcount]){
			acount++;
			bcount++;
			ret++;
		}
		else if(a->value[acount]>b->value[bcount])
			bcount++;
		else
			acount++;
	}
	return ret;
}

int isequal(aprioriset* a, aprioriset* b){
	if(a->length==b->length)
		if(numofmatch(a, b)==a->length)
			return 1;
	return 0;
}

void add(aprioristruct* data, aprioriset* value, int equal){
	int i;
	if(equal){
		for(i=0;i<data->num;i++){
			if(isequal(value, &data->valuelist[i])){
				deleteaprioriset(&value);
				return;
			}
		}
	}
	data->valuelist[data->num]=*value;
	data->num++;
}

void mergeset(aprioriset* res, aprioriset* a, aprioriset* b){
	int i;
	char newval;
	for(i=0;i<a->length;i++){
		res->value[i]=a->value[i];
	}
	for(i=0;i<b->length;i++){
		if(checkbuf(res->value, b->value[i], a->length)==0){
			newval=b->value[i];
			break;
		}
	}
	insertion(res->value, newval, b->length);
}

int issubset(aprioriset* large, aprioriset* small){
	int largecount=0;
	int smallcount=0;
	char cl, cs;
	if(small->length>large->length)
		return 0;
	while(smallcount<small->length){
		if(largecount>=large->length)
			return 0;
		cl=large->value[largecount];
		cs=small->value[smallcount];
		if(cl==cs){
			largecount++;
			smallcount++;
		}
		else if(cl<cs)
			largecount++;
		else
			return 0;
	}
	return 1;
}

int isproper(aprioriset* set, aprioristruct* str){
	aprioriset* tset;
	int strcount;
	int setcount=set->length-1;
	initaprioriset(&tset, setcount);
	int i;
	for(i=0;i<set->length-1;i++){
		if(i==setcount)
			continue;
		else if(i<setcount){
			tset->value[i]=set->value[i];
		}
		else{
			tset->value[i-1]=set->value[i];
		}
	}
	
	for(strcount=0;strcount<str->num;strcount++){
		if(isequal(tset, &str->valuelist[strcount])){
			if(setcount==0)
				return 1;
			else{
				setcount--;
				for(i=0;i<set->length;i++){
					if(i==setcount)
						continue;
					else if(i<setcount){
						tset->value[i]=set->value[i];
					}
					else{
						tset->value[i-1]=set->value[i];
					}
				}
			}
		}
	}
	return 0;
}

void loadapriorifromfile(aprioristruct* data, FILE* fp){
	aprioriset* nowdata;
	int len, i, j;
	for(j=0;j<TRAN;j++){
		fscanf(fp, "%*s %*s %*s %d %*s", &len);
		initaprioriset(&nowdata, len);
		for(i=0;i<len;i++){
			fscanf(fp, "%*c %c", &(nowdata->value[i]));
		}
		add(data, nowdata, 0);
	}
}

void loadapriorifromfileb(aprioristruct* dest, FILE* fp){
	fread(dest, sizeof(aprioristruct), 1, fp);
}

void saveaprioritofile(aprioristruct* data, FILE* fp){
	int length;
	int i;
	int j;
	for(i=0;i<data->num;i++){
		length=data->valuelist[i].length;
		fprintf(fp, "LEN %02d SUP %03d :", length, data->valuelist[i].support);
		for(j=0;j<length;j++){
			fprintf(fp, " %c", data->valuelist[i].value[j]);
		}
		fprintf(fp, "\n");
	}
}

void saveaprioritofileb(aprioristruct* dest, FILE* fp){
	fwrite(dest, sizeof(aprioristruct), 1, fp);
}

void makec1(aprioristruct* target, aprioristruct* data){
	char itemlist[ITEM];
	int i=0, j, k;
	aprioriset* nowdata;
	for(k=0;k<data->num;k++){
		if(i==ITEM) break;
		nowdata=&data->valuelist[k];
		for(j=0;j<nowdata->length;j++){
			if(checkbuf(itemlist, nowdata->value[j], ITEM)==0){
				if(i==0)
					itemlist[0]=nowdata->value[j];
				else{
					insertion(itemlist, nowdata->value[j], i);
				}
				i++;
			}
		}
	}
	for(j=0;j<i;j++){
		initaprioriset(&nowdata, 1);
		nowdata->value[0]=itemlist[j];
		add(target, nowdata, 1);
	}
}

void* genlthreadfunc(void* thearg){
	genlstruct* arg=(genlstruct*)thearg;
	int datacount;
arg->c->support=0;
	for(datacount=0;datacount<arg->data->num;datacount++){
		if(issubset(&arg->data->valuelist[datacount], arg->c)){
			arg->c->support++;
		}
	}
	return NULL;
}

void genL(aprioristruct* l, aprioristruct* c, aprioristruct* data, int minnum){
	int ccount=0, cdatacount=0, datacount, i, j;
	aprioriset* nowdata;
	pthread_t thread[GEM5_NUMPROCS];
	genlstruct* genlstructs=(genlstruct*)malloc(sizeof(genlstruct)*c->num);
	if(c->num<GEM5_NUMPROCS){
		for(ccount=0;ccount<c->num;ccount++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
		}
		for(ccount=0;ccount<c->num;ccount++){
			pthread_join(thread[ccount], NULL);
		}
		for(ccount=0;ccount<c->num;ccount++){
			nowdata=&c->valuelist[ccount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
			ccount++;
		}
		while(ccount<c->num){
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				pthread_join(thread[i], NULL);	
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				nowdata=&c->valuelist[cdatacount];
				if(nowdata->support<minnum){
					deleteaprioriset(&nowdata);
				}
				else{
					add(l, nowdata, 1);
				}
				cdatacount++;
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				genlstructs[ccount].c=&c->valuelist[ccount];
				genlstructs[ccount].data=data;
				pthread_create(&thread[i], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
				ccount++;
				if(ccount==c->num)
					break;
			}
		}
		i++;
		for(j=0;j<i;j++){
			pthread_join(thread[j], NULL);
		}
		for(j=0;j<i;j++){
			nowdata=&c->valuelist[cdatacount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
			cdatacount++;
			if(cdatacount==c->num)
				break;
		}
	}
	c->num=0;
	free(genlstructs);
}



void* gencthreadfunc(void* thearg){
	gencstruct* arg=(gencstruct*)thearg;
	gencreturnstruct* ret=&arg->ret;
	aprioriset tset;
	aprioriset* pset=&tset;

	int i, j=0, k, flag, ccount=0;

	ret->num=0;
	for(i=arg->num;i<arg->l->num;i++){
		flag=0;
		if(numofmatch(&arg->l->valuelist[i], arg->s)==arg->length-1){
			mergeset(pset, &arg->l->valuelist[i], arg->s);
				pset->length=arg->length+1;
			for(k=0;k<j;k++){
				if(isequal(pset, &ret->valuelist[k])){
					flag=1;
					break;
				}
			}
			if(flag){
				deleteaprioriset(&pset);
			}
			else{
				ret->valuelist[j]=*pset;
				ret->proper[j]=isproper(&ret->valuelist[j], arg->l);
				ret->num++;
				j++;
			}
		}
	}
}

void genC(aprioristruct* c, aprioristruct* l){
	pthread_t thread[GEM5_NUMPROCS];
	gencstruct strarg[GEM5_NUMPROCS];
	gencstruct strarg2[GEM5_NUMPROCS];
	gencstruct* parg;
	int length=l->valuelist[0].length;
	int threadrun[GEM5_NUMPROCS]={0,};
	int threadrun2[GEM5_NUMPROCS]={0,};
	int* pthreadrun;
	int i, j, count=0, toggle=0, ccount=0;

	if(l->num<GEM5_NUMPROCS){
		for(i=0;i<l->num;i++){
			strarg[i].l=l;
			strarg[i].s=&l->valuelist[i];
			strarg[i].length=length;
			strarg[i].num=i+1;
			pthread_create(&thread[i], NULL, gencthreadfunc, (void*)&strarg[i]);
		}
		for(i=0;i<l->num;i++){
			pthread_join(thread[i], NULL);
		}
		for(i=0;i<l->num;i++){
			for(j=0;j<strarg[i].ret.num;j++){
				if(strarg[i].ret.proper[j]){
					add(c, &strarg[i].ret.valuelist[j], 1);
				}
			}
		}
	}
	else{
		for(count=0;count<GEM5_NUMPROCS-1;count++){
			strarg[count].l=l;
			strarg[count].s=&l->valuelist[count];
			strarg[count].length=length;
			strarg[count].num=count+1;
			pthread_create(&thread[count], NULL, gencthreadfunc, (void*)&strarg[count]);
			ccount++;
		}
		toggle=1;
		while(count<l->num){
			if(ccount==GEM5_NUMPROCS-1){
				for(i=0;i<GEM5_NUMPROCS-1;i++){
					pthread_join(thread[i], NULL);
					
				}
				ccount=0;
			}
			parg=toggle?strarg2:strarg;
			pthreadrun=toggle?threadrun2:threadrun;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				(parg+i)->l=l;
				(parg+i)->s=&l->valuelist[count];
				(parg+i)->length=length;
				(parg+i)->num=count+1;
				pthread_create(&thread[i], NULL, gencthreadfunc, (void*)(parg+i));
				count++;
				ccount++;
				*(pthreadrun+i)=1;
				if(count==l->num)
					break;
			}
			parg=toggle?strarg:strarg2;
			pthreadrun=toggle?threadrun:threadrun2;
			toggle=!toggle;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				*(pthreadrun+i)=0;
				for(j=0;j<(parg+i)->ret.num;j++){
					if((parg+i)->ret.proper[j]){
						add(c, &(parg+i)->ret.valuelist[j], 1);
					}
				}
			}
		}
		pthreadrun=toggle?threadrun:threadrun2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			pthread_join(thread[i], NULL);
		}
		parg=toggle?strarg:strarg2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			for(j=0;j<(parg+i)->ret.num;j++){
				if((parg+i)->ret.proper[j]){
					add(c, &(parg+i)->ret.valuelist[j], 1);
				}
			}
		}
	}
}
void mergestruct(aprioristruct* target, aprioristruct* l){
	int i;
	for(i=0;i<l->num;i++){
		add(target, &l->valuelist[i], 1);
	}
}

void* getassociationrulefunc(void* thearg){
	associationstruct* assstruct=(associationstruct*)thearg;
	aprioriset* left=assstruct->left, *right=assstruct->right;
	int k=0, l=0, m=0;
	for(k=0;k<right->length;k++){
		if(checkbuf(left->value, right->value[k], left->length)){
			assstruct->dest->left[l]=right->value[k];
			l++;
		}
		else{
			assstruct->dest->right[m]=right->value[k];
			m++;
		}
	}
	assstruct->dest->support=((float)right->support)/((float)TRAN);
	assstruct->dest->confidence=((float)right->support)/((float)left->support);
}

void getassociationrule(aprioriassstruct* dest, aprioristruct* list){
	aprioriset* right, *left;
	int i, j, k;
	pthread_t thread[GEM5_NUMPROCS];
	associationstruct assstruct[GEM5_NUMPROCS];
	int proccount=0;
	int threadrun[GEM5_NUMPROCS]={0,};
	for(i=0;i<list->num;i++){
		right=&list->valuelist[i];
		if(right->length<2)
			continue;
		for(j=0;j<list->num;j++){
			left=&list->valuelist[j];
			if(right->length==left->length)
				break;
			if(issubset(right, left)){

				if(proccount!=GEM5_NUMPROCS-1){
					for(k=0;k<proccount;k++){
						pthread_join(thread[k], NULL);
						threadrun[k]=0;
					}
					proccount=0;
				}
				assstruct[proccount].dest=&dest->aprioriasslist[dest->num];
				assstruct[proccount].left=left;
				assstruct[proccount].right=right;
				pthread_create(&thread[proccount], NULL, getassociationrulefunc, (void*)&assstruct[proccount]);
			
				threadrun[proccount]=1;
				dest->num++;
				proccount++;
			}
		}
	}
	for(i=0;i<proccount;i++){
		pthread_join(thread[i], NULL);
	}
}

int apriori(){
	aprioristruct data;
	FILE* input=fopen("apriori10000", "rb");
	FILE* output=fopen("adata", "wb");
	readapriorib(&data, input);
	saveapriorinnb(&data, output);
	fclose(output);
	fclose(input);
	return 0;
}

int main(){
	apriori();
	return 0;
}
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define TRAN 10000
#define ITEM 20
#define LENGTH 10
#define MIN 300

#define GEM5_NUMPROCS 4

typedef struct aprioriset{
	int length;
	int support;
	char value[LENGTH];
}aprioriset;

typedef struct aprioristruct{
	int num;
	aprioriset valuelist[10000];
}aprioristruct;

typedef struct gencreturnstruct{
	int num;
	aprioriset valuelist[ITEM];
	int proper[ITEM];
}gencreturnstruct;

typedef struct gencstruct{
	aprioristruct* l;
	aprioriset* s;
	gencreturnstruct ret;
	int num;
	int length;
}gencstruct;

typedef struct genlstruct{
	aprioriset* c;
	aprioristruct* data;
}genlstruct;

typedef struct aprioriassvalue{
	char left[LENGTH];
	char right[LENGTH];
	float support;
	float confidence;
}aprioriassvalue;

typedef struct aprioriassstruct{
	int num;
	aprioriassvalue aprioriasslist[10000];
}aprioriassstruct;

typedef struct associationstruct{
	aprioriassvalue* dest;
	aprioriset* left;
	aprioriset* right;
}associationstruct;

double readtime, makec1time, makectime, makeltime, mergetime, asstime, writetime;

void readapriorib(aprioristruct* data, FILE* fp){
	data->num=TRAN;
	fread(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void saveapriorib(aprioristruct* data, FILE* fp){
	fwrite(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void readapriorinnb(aprioristruct* data, FILE* fp){
	fread(data, sizeof(aprioristruct), 1, fp);
}

void saveapriorinnb(aprioristruct* data, FILE* fp){
	fwrite(data, sizeof(aprioristruct), 1, fp);
}

void saveassstructb(aprioriassstruct* dest, FILE* fp){
	fwrite(dest->aprioriasslist, sizeof(aprioriassvalue), dest->num, fp);
}
void readassstructnnb(aprioriassstruct* dest, FILE* fp){
	fread(dest, sizeof(aprioriassstruct), 1, fp);
}

void insertion(char* buf, char val, int length){
	int i, j;
	for(i=0;i<length;i++){
		if(buf[i]>val){
			break;
		}
	}
	for(j=length;j>i;j--){
		buf[j]=buf[j-1];
	}
	buf[i]=val;
}

int checkbuf(char* buf, char val, int length){
	int a;
	for(a=0;a<length;a++){
		if(buf[a]==val){
			return 1;
		}
	}
	return 0;
}

void initaprioriset(aprioriset** data, int length){
	(*data)=(aprioriset*)malloc(sizeof(aprioriset));
	(*data)->support=0;
	(*data)->length=length;
}

void deleteaprioriset(aprioriset** data){
//	free((*data)->value);
//	free(*data);
}

int compareset(aprioriset* a, aprioriset* b){
	if(a->length<b->length)
		return 1;
	else if(b->length<a->length)
		return 0;
	else{
		if(strncmp(a->value, b->value, a->length)<0)
			return 1;
		else return 0;
	}
}

int numofmatch(aprioriset* a, aprioriset* b){
	int ret=0;
	int acount=0, bcount=0;
	while(1){
		if(acount==a->length||bcount==b->length)
			break;
		if(a->value[acount]==b->value[bcount]){
			acount++;
			bcount++;
			ret++;
		}
		else if(a->value[acount]>b->value[bcount])
			bcount++;
		else
			acount++;
	}
	return ret;
}

int isequal(aprioriset* a, aprioriset* b){
	if(a->length==b->length)
		if(numofmatch(a, b)==a->length)
			return 1;
	return 0;
}

void add(aprioristruct* data, aprioriset* value, int equal){
	int i;
	if(equal){
		for(i=0;i<data->num;i++){
			if(isequal(value, &data->valuelist[i])){
				deleteaprioriset(&value);
				return;
			}
		}
	}
	data->valuelist[data->num]=*value;
	data->num++;
}

void mergeset(aprioriset* res, aprioriset* a, aprioriset* b){
	int i;
	char newval;
	for(i=0;i<a->length;i++){
		res->value[i]=a->value[i];
	}
	for(i=0;i<b->length;i++){
		if(checkbuf(res->value, b->value[i], a->length)==0){
			newval=b->value[i];
			break;
		}
	}
	insertion(res->value, newval, b->length);
}

int issubset(aprioriset* large, aprioriset* small){
	int largecount=0;
	int smallcount=0;
	char cl, cs;
	if(small->length>large->length)
		return 0;
	while(smallcount<small->length){
		if(largecount>=large->length)
			return 0;
		cl=large->value[largecount];
		cs=small->value[smallcount];
		if(cl==cs){
			largecount++;
			smallcount++;
		}
		else if(cl<cs)
			largecount++;
		else
			return 0;
	}
	return 1;
}

int isproper(aprioriset* set, aprioristruct* str){
	aprioriset* tset;
	int strcount;
	int setcount=set->length-1;
	initaprioriset(&tset, setcount);
	int i;
	for(i=0;i<set->length-1;i++){
		if(i==setcount)
			continue;
		else if(i<setcount){
			tset->value[i]=set->value[i];
		}
		else{
			tset->value[i-1]=set->value[i];
		}
	}
	
	for(strcount=0;strcount<str->num;strcount++){
		if(isequal(tset, &str->valuelist[strcount])){
			if(setcount==0)
				return 1;
			else{
				setcount--;
				for(i=0;i<set->length;i++){
					if(i==setcount)
						continue;
					else if(i<setcount){
						tset->value[i]=set->value[i];
					}
					else{
						tset->value[i-1]=set->value[i];
					}
				}
			}
		}
	}
	return 0;
}

void loadapriorifromfile(aprioristruct* data, FILE* fp){
	aprioriset* nowdata;
	int len, i, j;
	for(j=0;j<TRAN;j++){
		fscanf(fp, "%*s %*s %*s %d %*s", &len);
		initaprioriset(&nowdata, len);
		for(i=0;i<len;i++){
			fscanf(fp, "%*c %c", &(nowdata->value[i]));
		}
		add(data, nowdata, 0);
	}
}

void loadapriorifromfileb(aprioristruct* dest, FILE* fp){
	fread(dest, sizeof(aprioristruct), 1, fp);
}

void saveaprioritofile(aprioristruct* data, FILE* fp){
	int length;
	int i;
	int j;
	for(i=0;i<data->num;i++){
		length=data->valuelist[i].length;
		fprintf(fp, "LEN %02d SUP %03d :", length, data->valuelist[i].support);
		for(j=0;j<length;j++){
			fprintf(fp, " %c", data->valuelist[i].value[j]);
		}
		fprintf(fp, "\n");
	}
}

void saveaprioritofileb(aprioristruct* dest, FILE* fp){
	fwrite(dest, sizeof(aprioristruct), 1, fp);
}

void makec1(aprioristruct* target, aprioristruct* data){
	char itemlist[ITEM];
	int i=0, j, k;
	aprioriset* nowdata;
	for(k=0;k<data->num;k++){
		if(i==ITEM) break;
		nowdata=&data->valuelist[k];
		for(j=0;j<nowdata->length;j++){
			if(checkbuf(itemlist, nowdata->value[j], ITEM)==0){
				if(i==0)
					itemlist[0]=nowdata->value[j];
				else{
					insertion(itemlist, nowdata->value[j], i);
				}
				i++;
			}
		}
	}
	for(j=0;j<i;j++){
		initaprioriset(&nowdata, 1);
		nowdata->value[0]=itemlist[j];
		add(target, nowdata, 1);
	}
}

void* genlthreadfunc(void* thearg){
	genlstruct* arg=(genlstruct*)thearg;
	int datacount;
arg->c->support=0;
	for(datacount=0;datacount<arg->data->num;datacount++){
		if(issubset(&arg->data->valuelist[datacount], arg->c)){
			arg->c->support++;
		}
	}
	return NULL;
}

void genL(aprioristruct* l, aprioristruct* c, aprioristruct* data, int minnum){
	int ccount=0, cdatacount=0, datacount, i, j;
	aprioriset* nowdata;
	pthread_t thread[GEM5_NUMPROCS];
	genlstruct* genlstructs=(genlstruct*)malloc(sizeof(genlstruct)*c->num);
	if(c->num<GEM5_NUMPROCS){
		for(ccount=0;ccount<c->num;ccount++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
		}
		for(ccount=0;ccount<c->num;ccount++){
			pthread_join(thread[ccount], NULL);
		}
		for(ccount=0;ccount<c->num;ccount++){
			nowdata=&c->valuelist[ccount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
			ccount++;
		}
		while(ccount<c->num){
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				pthread_join(thread[i], NULL);	
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				nowdata=&c->valuelist[cdatacount];
				if(nowdata->support<minnum){
					deleteaprioriset(&nowdata);
				}
				else{
					add(l, nowdata, 1);
				}
				cdatacount++;
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				genlstructs[ccount].c=&c->valuelist[ccount];
				genlstructs[ccount].data=data;
				pthread_create(&thread[i], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
				ccount++;
				if(ccount==c->num)
					break;
			}
		}
		i++;
		for(j=0;j<i;j++){
			pthread_join(thread[j], NULL);
		}
		for(j=0;j<i;j++){
			nowdata=&c->valuelist[cdatacount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
			cdatacount++;
			if(cdatacount==c->num)
				break;
		}
	}
	c->num=0;
	free(genlstructs);
}



void* gencthreadfunc(void* thearg){
	gencstruct* arg=(gencstruct*)thearg;
	gencreturnstruct* ret=&arg->ret;
	aprioriset tset;
	aprioriset* pset=&tset;

	int i, j=0, k, flag, ccount=0;

	ret->num=0;
	for(i=arg->num;i<arg->l->num;i++){
		flag=0;
		if(numofmatch(&arg->l->valuelist[i], arg->s)==arg->length-1){
			mergeset(pset, &arg->l->valuelist[i], arg->s);
				pset->length=arg->length+1;
			for(k=0;k<j;k++){
				if(isequal(pset, &ret->valuelist[k])){
					flag=1;
					break;
				}
			}
			if(flag){
				deleteaprioriset(&pset);
			}
			else{
				ret->valuelist[j]=*pset;
				ret->proper[j]=isproper(&ret->valuelist[j], arg->l);
				ret->num++;
				j++;
			}
		}
	}
}

void genC(aprioristruct* c, aprioristruct* l){
	pthread_t thread[GEM5_NUMPROCS];
	gencstruct strarg[GEM5_NUMPROCS];
	gencstruct strarg2[GEM5_NUMPROCS];
	gencstruct* parg;
	int length=l->valuelist[0].length;
	int threadrun[GEM5_NUMPROCS]={0,};
	int threadrun2[GEM5_NUMPROCS]={0,};
	int* pthreadrun;
	int i, j, count=0, toggle=0, ccount=0;

	if(l->num<GEM5_NUMPROCS){
		for(i=0;i<l->num;i++){
			strarg[i].l=l;
			strarg[i].s=&l->valuelist[i];
			strarg[i].length=length;
			strarg[i].num=i+1;
			pthread_create(&thread[i], NULL, gencthreadfunc, (void*)&strarg[i]);
		}
		for(i=0;i<l->num;i++){
			pthread_join(thread[i], NULL);
		}
		for(i=0;i<l->num;i++){
			for(j=0;j<strarg[i].ret.num;j++){
				if(strarg[i].ret.proper[j]){
					add(c, &strarg[i].ret.valuelist[j], 1);
				}
			}
		}
	}
	else{
		for(count=0;count<GEM5_NUMPROCS-1;count++){
			strarg[count].l=l;
			strarg[count].s=&l->valuelist[count];
			strarg[count].length=length;
			strarg[count].num=count+1;
			pthread_create(&thread[count], NULL, gencthreadfunc, (void*)&strarg[count]);
			ccount++;
		}
		toggle=1;
		while(count<l->num){
			if(ccount==GEM5_NUMPROCS-1){
				for(i=0;i<GEM5_NUMPROCS-1;i++){
					pthread_join(thread[i], NULL);
					
				}
				ccount=0;
			}
			parg=toggle?strarg2:strarg;
			pthreadrun=toggle?threadrun2:threadrun;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				(parg+i)->l=l;
				(parg+i)->s=&l->valuelist[count];
				(parg+i)->length=length;
				(parg+i)->num=count+1;
				pthread_create(&thread[i], NULL, gencthreadfunc, (void*)(parg+i));
				count++;
				ccount++;
				*(pthreadrun+i)=1;
				if(count==l->num)
					break;
			}
			parg=toggle?strarg:strarg2;
			pthreadrun=toggle?threadrun:threadrun2;
			toggle=!toggle;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				*(pthreadrun+i)=0;
				for(j=0;j<(parg+i)->ret.num;j++){
					if((parg+i)->ret.proper[j]){
						add(c, &(parg+i)->ret.valuelist[j], 1);
					}
				}
			}
		}
		pthreadrun=toggle?threadrun:threadrun2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			pthread_join(thread[i], NULL);
		}
		parg=toggle?strarg:strarg2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			for(j=0;j<(parg+i)->ret.num;j++){
				if((parg+i)->ret.proper[j]){
					add(c, &(parg+i)->ret.valuelist[j], 1);
				}
			}
		}
	}
}
void mergestruct(aprioristruct* target, aprioristruct* l){
	int i;
	for(i=0;i<l->num;i++){
		add(target, &l->valuelist[i], 1);
	}
}

void* getassociationrulefunc(void* thearg){
	associationstruct* assstruct=(associationstruct*)thearg;
	aprioriset* left=assstruct->left, *right=assstruct->right;
	int k=0, l=0, m=0;
	for(k=0;k<right->length;k++){
		if(checkbuf(left->value, right->value[k], left->length)){
			assstruct->dest->left[l]=right->value[k];
			l++;
		}
		else{
			assstruct->dest->right[m]=right->value[k];
			m++;
		}
	}
	assstruct->dest->support=((float)right->support)/((float)TRAN);
	assstruct->dest->confidence=((float)right->support)/((float)left->support);
}

void getassociationrule(aprioriassstruct* dest, aprioristruct* list){
	aprioriset* right, *left;
	int i, j, k;
	pthread_t thread[GEM5_NUMPROCS];
	associationstruct assstruct[GEM5_NUMPROCS];
	int proccount=0;
	int threadrun[GEM5_NUMPROCS]={0,};
	for(i=0;i<list->num;i++){
		right=&list->valuelist[i];
		if(right->length<2)
			continue;
		for(j=0;j<list->num;j++){
			left=&list->valuelist[j];
			if(right->length==left->length)
				break;
			if(issubset(right, left)){

				if(proccount!=GEM5_NUMPROCS-1){
					for(k=0;k<proccount;k++){
						pthread_join(thread[k], NULL);
						threadrun[k]=0;
					}
					proccount=0;
				}
				assstruct[proccount].dest=&dest->aprioriasslist[dest->num];
				assstruct[proccount].left=left;
				assstruct[proccount].right=right;
				pthread_create(&thread[proccount], NULL, getassociationrulefunc, (void*)&assstruct[proccount]);
			
				threadrun[proccount]=1;
				dest->num++;
				proccount++;
			}
		}
	}
	for(i=0;i<proccount;i++){
		pthread_join(thread[i], NULL);
	}
}

int apriori(){
aprioriassstruct ass;
	FILE* input=fopen("ass", "rb");
FILE* output=fopen("aprioriout", "wb");
readassstructnnb(&ass, input);

	saveassstructb(&ass, output);
fclose(output);
	fclose(input);
	return 0;
}

int main(){
	apriori();
	return 0;
}
#
S4SIM_HOME = ../..
CFLAGS = -g

INCLUDE=-I${S4SIM_HOME}/include
PTHREAD = ${S4SIM_HOME}/external/m5threads
CC = gcc
CPP = g++

ARMCC = arm-linux-gnueabi-gcc
ARMFLAGS = -march=armv7-a -marm

all : run_apriori apriori_isp_makec1 apriori_isp_makec2 apriori_isp_makec3 apriori_isp_makec4 apriori_isp_makel1 apriori_isp_makel2 apriori_isp_makel3 apriori_isp_makel4 apriori_isp_merge apriori_isp_read apriori_isp_write apriori_isp_genass

run_apriori : run_apriori.c ${S4SIM_HOME}/src/isp_socket.c
	$(CC) $(CFLAGS) -o $@ $^ -lpthread $(INCLUDE)

apriori_isp_makec1 : apriori_isp_makec1.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_makec2 : apriori_isp_makec2.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_makec3 : apriori_isp_makec3.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_makec4 : apriori_isp_makec4.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_makel1 : apriori_isp_makel1.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_makel2 : apriori_isp_makel2.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_makel3 : apriori_isp_makel3.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_makel4 : apriori_isp_makel4.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_merge : apriori_isp_merge.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_read : apriori_isp_read.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_write : apriori_isp_write.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_genass : apriori_isp_genass.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)
#include <stdio.h>
#include <stdlib.h>
#include "isp.h"

#define issd_clock 400
#define issd_numcpu 4

int main(int argc, const char* argv[])
{
        isp_device_id device;
        FILE* ifp;
	int n;
	char buffer[1024];
	int cycle;
	int i;
	char cpuhz[16];
	char cmd[64];
	char pname[64];
	char funcname[64];
	int numcpu=issd_numcpu;
	int clock=issd_clock;
	sprintf(cpuhz, "%dMHz", clock);
	sprintf(funcname, "read");
	sprintf(pname, "./apriori_isp_%s", funcname);
	sprintf(cmd, "cp ./m5out/stats.txt ./m5out/apriori_%s_%d_%s.txt", funcname, numcpu, cpuhz);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
	system(cmd);
	
	sprintf(funcname, "makec1");
	sprintf(pname, "./apriori_isp_%s", funcname);
	sprintf(cmd, "cp ./m5out/stats.txt ./m5out/apriori_%s_%d_%s.txt", funcname, numcpu, cpuhz);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
	system(cmd);
	
	sprintf(funcname, "makel1");
	sprintf(pname, "./apriori_isp_%s", funcname);
	sprintf(cmd, "cp ./m5out/stats.txt ./m5out/apriori_%s_%d_%s.txt", funcname, numcpu, cpuhz);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
	system(cmd);
	
	sprintf(funcname, "makec2");
	sprintf(pname, "./apriori_isp_%s", funcname);
	sprintf(cmd, "cp ./m5out/stats.txt ./m5out/apriori_%s_%d_%s.txt", funcname, numcpu, cpuhz);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
	system(cmd);
	
	sprintf(funcname, "makel2");
	sprintf(pname, "./apriori_isp_%s", funcname);
	sprintf(cmd, "cp ./m5out/stats.txt ./m5out/apriori_%s_%d_%s.txt", funcname, numcpu, cpuhz);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
	system(cmd);
	
	sprintf(funcname, "makec3");
	sprintf(pname, "./apriori_isp_%s", funcname);
	sprintf(cmd, "cp ./m5out/stats.txt ./m5out/apriori_%s_%d_%s.txt", funcname, numcpu, cpuhz);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
	system(cmd);
	
	sprintf(funcname, "makel3");
	sprintf(pname, "./apriori_isp_%s", funcname);
	sprintf(cmd, "cp ./m5out/stats.txt ./m5out/apriori_%s_%d_%s.txt", funcname, numcpu, cpuhz);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
	system(cmd);
	
	sprintf(funcname, "makec4");
	sprintf(pname, "./apriori_isp_%s", funcname);
	sprintf(cmd, "cp ./m5out/stats.txt ./m5out/apriori_%s_%d_%s.txt", funcname, numcpu, cpuhz);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
	system(cmd);
	
	sprintf(funcname, "makel4");
	sprintf(pname, "./apriori_isp_%s", funcname);
	sprintf(cmd, "cp ./m5out/stats.txt ./m5out/apriori_%s_%d_%s.txt", funcname, numcpu, cpuhz);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
	system(cmd);
	
	sprintf(funcname, "merge");
	sprintf(pname, "./apriori_isp_%s", funcname);
	sprintf(cmd, "cp ./m5out/stats.txt ./m5out/apriori_%s_%d_%s.txt", funcname, numcpu, cpuhz);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
	system(cmd);
	
	sprintf(funcname, "genass");
	sprintf(pname, "./apriori_isp_%s", funcname);
	sprintf(cmd, "cp ./m5out/stats.txt ./m5out/apriori_%s_%d_%s.txt", funcname, numcpu, cpuhz);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
	system(cmd);
	
	sprintf(funcname, "write");
	sprintf(pname, "./apriori_isp_%s", funcname);
	sprintf(cmd, "cp ./m5out/stats.txt ./m5out/apriori_%s_%d_%s.txt", funcname, numcpu, cpuhz);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
	system(cmd);
	
	printf("ISP cycle = %d\n", cycle);
return 0;
}
#include <stdio.h>
#include <string.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 150
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4


typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;
typedef struct treenode{
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;

void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}





void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}















int main(){
	value val[TRAIN_N];
	value tval[TEST_N];
	FILE* valinput=fopen("data.txt", "r");
	FILE* valoutput=fopen("val", "wb");
	FILE* tvalinput=fopen("test.txt", "r");
	FILE* tvaloutput=fopen("testval", "wb");

FILE* tvalout2=fopen("testval2.txt", "w");
	
	read(val, valinput);
	readtest(tval, tvalinput);
	
	savevalb(val, valoutput, TRAIN_N);
	savevalb(tval, tvaloutput, TEST_N);
fprinttest(tval, tvalout2);
fclose(tvalout2);
	fclose(valinput);
	fclose(valoutput);
	fclose(tvalinput);
	fclose(tvaloutput);
	return 0;
}
#include <stdio.h>
#include <string.h>
#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 150
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4

typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;
typedef struct treenode{
	float info;
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;


void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}

void fprinttrain(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}

void savetreet(dicisiontree* tree, FILE* fp){
	int i, j;
	treenode* node;
	fprintf(fp, "maxnum : %d\n", tree->maxnum);
	for(i=0;i<MAX_TREE_NUM;i++){
		node=&tree->node[i];
		fprintf(fp, "node %d\ntreeval %d\nstartnum %d\nnum %d\nattnum %d\nlistcount : ", i, node->treeval, node->startnum, node->num, node->attnum);
		for(j=0;j<MAX_ATTR_VAL;j++){
			fprintf(fp, "%d ", node->listcount[j]);
		}
		fprintf(fp, "\n%d\n", node->subnum);
		for(j=0;j<node->subnum;j++){
			fprintf(fp, "%d ", node->subptr[j]);
		}
		fprintf(fp, "\n\n");
	}
}

void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}






int main(){
	value val[TRAIN_N];
	value tval[TEST_N];
	dicisiontree tree;
	FILE* treeinput=fopen("tree", "rb");
	FILE* treeoutput=fopen("treeout.txt", "w");
	FILE* tvalinput=fopen("test2.txt", "w");
	FILE* tvaloutput=fopen("testvalo", "rb");
	FILE* valinput=fopen("val22.txt", "w");
	FILE* valoutput=fopen("val", "rb");
	
	readvalb(tval, tvaloutput, TEST_N);
	fprinttest(tval, tvalinput);
	fclose(tvalinput);
	fclose(tvaloutput);
	readtree(&tree, treeinput);
	savetreet(&tree, treeoutput);
	fclose(treeinput);
	fclose(treeoutput);
	readvalb(val, valoutput, TRAIN_N);
	fprinttrain(val, valinput);
	fclose(valinput);
	fclose(valoutput);
	return 0;
}
8	7	0	16	2	2	3	2	4	4	0	1	4	0	1	1	0	0	0	5
8	11	13	2	2	4	3	2	4	5	2	16	4	3	0	0	0	0	0	9
1	3	2	10	2	1	1	3	2	2	0	1	4	0	1	1	0	0	1	1
8	1	0	21	1	3	1	1	1	5	0	15	1	1	1	1	1	1	1	0
1	3	3	2	1	1	3	1	5	2	1	0	0	0	1	0	0	0	0	2
1	0	0	8	2	1	0	2	5	0	0	1	0	1	1	0	0	0	0	3
8	0	0	10	2	3	6	2	5	0	0	8	0	0	1	1	0	0	1	0
1	0	0	8	2	8	6	2	5	0	2	16	4	3	0	0	0	0	0	9
1	7	0	12	2	3	3	2	0	4	2	16	4	3	0	0	0	0	0	9
8	5	0	19	2	2	3	2	5	3	0	15	0	0	1	1	0	0	1	1
1	6	0	11	2	2	2	2	5	1	2	0	1	1	0	0	0	0	0	3
8	7	0	20	1	3	3	1	4	4	0	5	4	0	1	1	0	1	1	0
1	3	10	10	2	3	2	2	0	2	0	11	1	0	1	1	1	0	1	3
8	6	12	9	2	2	0	2	4	1	0	4	4	0	0	0	1	0	0	3
8	7	0	8	2	2	2	2	1	5	0	6	1	0	1	1	0	1	1	1
8	3	5	13	1	2	3	3	4	2	0	4	1	1	1	1	1	0	0	3
8	7	5	12	1	3	6	2	1	4	0	12	1	0	1	1	1	1	1	4
8	5	0	11	2	4	3	2	4	3	0	8	1	0	1	1	1	1	1	4
8	3	0	16	2	1	3	2	4	2	0	3	1	1	1	1	0	1	0	3
1	5	9	14	2	3	3	2	2	3	0	9	1	1	1	1	0	0	0	0
8	0	14	1	1	2	6	1	5	5	2	16	4	3	0	0	0	0	0	9
8	0	2	21	1	2	3	1	5	5	0	3	4	1	0	0	0	0	0	7
1	5	0	8	2	2	3	2	5	3	0	3	0	1	1	1	1	0	0	6
8	3	0	15	2	2	3	2	4	2	0	1	1	1	1	1	0	0	1	3
1	3	0	15	2	6	3	2	0	2	0	15	1	0	1	1	1	1	1	4
8	6	0	15	2	2	3	2	4	1	0	3	1	0	1	1	1	1	1	2
1	7	14	29	1	2	1	2	2	5	0	12	1	0	1	1	1	0	1	7
1	3	0	19	2	3	3	2	5	2	0	3	1	0	1	1	0	0	1	5
1	6	6	8	1	3	3	2	5	1	2	16	4	3	0	0	0	0	0	9
8	3	12	15	2	4	3	2	5	2	0	1	0	0	1	1	0	0	1	1
8	3	4	6	2	4	7	2	5	2	0	9	1	0	1	1	1	1	1	3
1	7	2	6	2	1	1	2	2	4	2	2	3	1	0	1	0	0	0	7
8	3	0	15	2	3	3	2	4	2	0	15	0	0	1	0	0	0	0	7
8	7	0	8	2	1	2	2	4	4	0	3	4	0	1	0	0	1	0	3
8	7	0	19	2	6	3	2	4	4	0	4	1	0	1	1	1	0	0	3
8	3	0	20	1	5	3	1	5	2	2	0	1	0	0	0	0	0	0	3
1	0	12	5	2	1	6	3	5	0	0	1	3	0	0	1	0	0	0	8
8	3	2	8	2	3	2	3	1	2	0	7	0	1	0	0	0	0	0	3
1	11	8	21	1	1	3	2	5	5	2	16	4	3	0	0	0	0	0	9
1	7	14	5	2	2	2	3	2	4	0	1	0	1	1	1	0	0	1	3
1	6	6	8	1	2	3	2	2	1	0	4	4	1	1	1	0	1	1	1
1	3	0	8	2	4	2	2	5	2	0	6	1	0	0	1	0	0	1	4
8	3	0	8	2	2	3	2	4	2	0	6	0	1	1	1	1	0	1	1
1	5	0	18	2	4	3	2	2	3	2	16	4	3	0	0	0	0	0	9
8	9	2	6	1	1	5	1	1	5	2	16	4	3	0	0	0	0	0	9
1	5	0	12	2	4	3	2	0	5	0	10	1	0	1	1	1	0	1	3
1	7	11	7	2	2	3	3	2	4	0	4	1	0	1	1	0	0	1	1
8	3	0	14	2	3	3	2	1	2	0	3	1	0	1	0	0	0	0	3
1	0	0	8	2	2	6	2	5	0	2	1	3	0	0	0	0	0	0	3
1	5	6	9	2	4	3	3	2	3	0	11	0	0	1	1	0	0	1	4
1	5	0	16	2	2	3	2	2	3	0	5	4	0	1	1	0	1	1	0
1	5	0	25	1	2	3	1	2	5	2	1	0	1	0	0	0	0	0	0
1	5	11	20	2	2	3	2	0	3	0	3	4	0	1	1	0	0	1	0
1	7	6	24	1	2	3	3	2	4	0	3	2	0	1	1	0	0	1	4
1	6	0	12	2	1	3	2	5	1	0	7	0	0	1	1	1	0	1	5
8	5	2	11	2	2	1	3	1	3	0	13	3	0	0	0	0	0	0	8
1	9	7	3	2	1	3	2	5	5	0	3	2	0	1	1	1	0	0	1
1	0	0	8	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0	9
1	3	0	15	2	3	3	2	5	2	2	5	2	0	1	1	0	0	0	3
8	0	0	8	2	2	0	2	3	0	2	16	0	0	1	0	0	0	0	1
8	3	0	14	2	2	3	2	4	2	0	3	1	0	1	1	0	0	1	3
8	3	0	20	1	5	2	1	5	2	0	5	2	0	1	1	0	0	1	2
1	3	0	13	2	3	3	2	0	2	0	5	0	1	1	0	1	0	0	5
8	7	0	20	2	2	3	2	4	4	2	16	4	3	0	0	0	0	0	9
8	3	0	12	2	1	3	2	4	2	1	2	0	1	1	1	0	0	0	2
1	6	0	21	1	3	3	1	0	1	0	6	1	0	1	1	0	0	1	4
1	6	3	6	2	3	3	2	5	1	0	9	1	0	1	1	1	0	1	5
8	3	5	7	2	2	3	2	1	2	0	3	1	0	0	0	0	0	0	5
8	9	2	6	2	3	1	2	1	5	0	5	0	0	1	1	0	0	0	7
1	11	10	28	1	3	1	3	5	5	0	10	4	0	0	0	0	0	0	3
1	0	14	6	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0	9
1	7	2	3	2	2	1	2	2	4	2	16	4	3	0	0	0	0	0	9
1	9	2	14	1	3	3	2	2	5	0	6	1	0	1	1	0	0	0	4
8	3	9	1	2	5	6	2	4	2	0	15	1	1	1	1	0	0	1	3
1	7	0	19	2	2	3	2	5	5	2	16	4	3	0	0	0	0	0	9
1	3	3	3	2	3	3	2	0	2	0	10	4	0	1	1	0	0	0	1
8	5	0	15	2	3	3	2	1	3	0	5	0	0	1	1	0	0	0	2
8	0	9	8	2	9	6	3	3	0	2	16	4	3	0	0	0	0	0	9
8	5	13	4	2	3	2	3	5	3	0	5	2	0	1	1	0	0	0	4
8	5	2	3	2	2	1	2	5	3	2	16	4	3	0	0	0	0	0	9
8	7	0	15	2	2	3	2	1	5	2	3	2	0	1	0	0	0	0	2
8	5	3	8	2	1	1	2	1	3	0	9	0	0	1	1	0	0	0	7
8	0	0	9	2	3	6	2	5	0	0	6	1	0	0	0	0	0	0	1
8	5	11	1	2	5	2	2	5	3	2	16	4	3	0	0	0	0	0	9
1	7	0	3	3	2	3	3	2	4	0	4	2	0	1	1	0	0	0	6
1	3	0	29	1	5	3	1	0	2	0	15	1	0	1	1	1	1	1	3
1	3	0	17	2	2	3	2	0	2	0	5	2	0	1	1	0	0	1	5
1	7	0	11	2	2	2	2	0	4	0	2	3	1	1	1	0	0	1	2
8	3	0	9	2	3	3	2	4	2	0	15	1	0	1	1	1	1	1	2
8	0	0	9	2	9	6	2	3	0	0	2	4	0	1	1	0	0	1	2
1	3	0	8	2	4	2	2	5	2	0	8	1	0	1	1	0	1	1	1
1	3	0	19	2	3	4	2	5	2	0	2	1	1	1	1	0	0	1	4
8	3	0	12	2	1	2	2	1	2	2	16	4	1	0	0	0	0	0	9
1	2	5	7	2	2	0	2	2	5	2	16	4	3	0	0	0	0	0	9
8	7	0	31	1	2	2	1	1	5	0	7	1	0	1	0	1	0	0	3
1	5	0	20	1	3	3	1	5	3	0	6	1	0	1	1	0	0	1	2
1	7	0	8	2	1	2	2	5	5	2	3	3	0	1	1	0	0	1	7
8	6	0	15	2	3	3	2	1	1	0	4	1	0	1	1	1	0	1	6
1	9	0	16	2	3	3	2	5	5	2	16	4	3	0	0	0	0	0	9
1	3	0	26	1	5	3	1	5	5	0	15	1	0	1	1	1	1	1	1
0	3	7	8	2	1	2	3	2	2	2	16	4	3	0	0	0	0	0	9
8	6	3	8	2	2	3	2	5	1	2	16	4	3	0	0	0	0	0	9
1	6	3	2	2	1	1	2	0	1	2	16	4	3	0	0	0	0	0	9
1	11	2	12	2	1	0	3	2	5	0	3	0	1	1	1	0	0	0	6
8	5	3	10	1	5	3	2	1	3	0	15	0	0	1	1	0	0	1	2
8	5	2	6	1	1	3	2	5	3	0	11	1	0	1	1	0	0	1	0
8	7	11	13	2	1	2	3	4	4	0	7	2	0	1	1	0	0	1	6
8	0	0	9	2	9	6	2	3	0	0	2	4	0	1	0	0	0	0	9
1	7	2	8	1	4	1	1	5	5	0	9	0	0	1	1	1	0	1	4
8	3	0	14	2	2	1	2	4	2	0	6	0	0	1	1	1	1	0	2
8	3	11	3	1	1	1	2	4	2	0	3	1	1	0	1	0	0	1	1
8	7	5	15	2	3	1	2	5	4	2	16	4	3	0	0	0	0	0	9
1	3	3	4	2	2	3	3	5	2	0	2	0	0	1	0	0	0	0	0
8	7	0	17	2	6	1	2	4	4	0	11	3	0	1	0	1	0	0	3
8	3	0	29	1	4	3	1	1	2	0	15	2	0	1	1	0	0	0	3
1	7	0	14	2	2	3	2	5	4	0	1	1	0	1	0	0	0	0	6
3	6	5	11	2	2	1	2	1	1	0	4	4	0	1	1	1	0	0	3
1	6	0	17	2	2	3	2	0	1	0	3	1	0	1	1	1	1	1	3
8	3	0	13	2	3	3	2	4	2	0	10	4	0	0	0	0	0	0	5
8	0	9	8	2	2	6	3	3	0	2	16	4	3	0	0	0	0	0	9
1	7	2	11	1	3	3	1	2	5	0	6	0	0	1	1	1	0	1	0
1	5	6	2	2	1	2	2	5	3	0	2	1	0	1	1	1	0	0	2
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0	9
8	3	0	8	2	3	3	2	4	2	0	2	3	0	0	0	0	0	0	3
8	3	11	16	2	4	3	2	4	2	0	14	1	0	1	1	1	0	1	3
1	6	3	10	1	3	3	2	5	1	2	16	4	3	0	0	0	0	0	9
1	0	9	9	2	3	6	3	5	0	2	16	4	3	0	0	0	0	0	9
8	3	6	2	2	4	3	2	4	2	0	5	4	0	1	1	1	1	1	3
1	3	0	19	2	2	3	2	0	5	2	0	4	0	0	0	0	0	0	6
8	3	0	12	2	3	3	2	5	2	0	7	1	0	1	1	0	0	1	3
1	3	2	9	1	3	1	2	5	2	0	5	1	1	0	0	0	0	0	3
8	5	0	13	2	1	3	2	4	3	2	16	0	0	1	1	0	0	1	0
1	6	5	21	1	3	3	3	2	1	0	3	1	0	1	1	0	1	0	3
8	5	2	9	1	4	1	2	5	3	0	8	0	0	1	1	0	0	1	1
1	3	0	20	1	4	3	1	5	2	2	16	4	3	0	0	0	0	0	9
8	0	0	10	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0	9
1	3	14	32	0	2	3	2	2	2	0	6	4	0	1	1	0	1	0	4
8	11	4	10	1	2	6	2	1	5	0	5	0	0	1	1	0	0	1	7
1	5	9	6	1	1	6	2	5	3	2	2	0	0	0	0	0	0	0	1
8	9	0	10	2	1	2	3	4	5	2	16	0	0	1	0	0	1	0	2
1	3	0	11	2	4	3	2	5	2	0	8	0	0	1	1	1	0	1	4
1	3	3	5	2	2	3	2	2	2	0	10	4	2	1	1	0	0	1	4
1	9	7	15	1	3	2	2	2	5	0	5	2	0	1	1	0	0	1	4
8	3	9	8	1	2	0	2	4	2	0	4	1	0	1	1	0	0	1	3
8	3	5	1	1	2	0	2	4	2	2	16	4	3	0	0	0	0	0	9
8	11	2	4	2	2	3	2	4	5	2	16	4	3	0	0	0	0	0	9
1	5	12	1	2	3	3	2	5	3	0	7	0	0	1	1	0	0	1	2
8	3	5	17	1	3	1	1	4	2	0	1	3	2	0	0	0	0	0	2
1	11	0	13	2	2	3	2	0	5	2	2	3	0	0	0	0	0	0	3
8	7	2	5	1	4	3	1	1	5	2	4	0	0	1	1	0	0	0	2
8	5	3	15	2	1	6	3	1	3	0	3	4	1	0	1	1	0	0	3
8	3	0	19	1	3	3	1	4	2	0	6	1	0	1	1	1	0	1	4
8	3	5	5	2	1	3	2	5	2	2	16	4	3	0	0	0	0	0	9
8	0	0	8	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0	9
1	7	2	4	2	1	1	2	5	4	0	1	0	1	1	1	0	0	1	0
1	11	0	30	1	3	2	1	0	5	2	16	4	3	0	0	0	0	0	9
1	9	11	3	1	2	3	2	2	5	0	15	1	0	1	1	1	0	1	1
5	0	14	2	2	0	6	3	3	0	2	16	4	3	0	0	0	0	0	9
8	3	0	15	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0	9
8	3	11	8	1	1	1	2	5	2	2	16	4	3	0	0	0	0	0	9
8	3	0	15	2	2	3	2	5	2	0	2	0	0	1	1	0	0	1	3
8	2	5	7	2	2	0	2	4	5	0	6	1	0	1	1	0	0	1	1
8	1	0	24	1	2	3	1	5	5	0	14	4	0	1	1	0	0	1	5
8	7	0	15	2	5	3	2	1	4	0	10	1	0	1	1	1	1	1	4
8	5	4	5	1	1	3	1	5	5	0	7	4	0	1	1	0	1	0	3
8	5	0	14	2	3	3	2	1	3	0	7	1	0	1	1	1	0	0	0
1	0	9	10	2	1	6	3	5	0	2	16	4	3	0	0	0	0	0	9
8	7	0	15	2	2	3	2	5	4	2	16	4	3	0	0	0	0	0	9
8	5	9	7	2	3	0	2	4	3	2	16	4	3	0	0	0	0	0	9
1	7	2	7	2	3	3	3	2	4	0	7	0	2	1	1	1	0	1	4
8	9	10	31	0	3	3	1	1	5	0	5	0	0	1	1	1	1	0	3
1	3	1	3	1	2	3	2	5	2	2	16	4	3	0	0	0	0	0	9
8	0	9	8	2	1	6	3	3	0	1	0	0	0	1	1	0	0	0	7
8	5	0	19	1	3	5	1	1	5	0	8	1	0	1	1	0	0	0	6
8	5	7	10	1	2	3	2	5	3	0	4	1	0	1	1	0	0	0	0
1	5	0	12	2	1	0	2	5	3	0	14	0	0	1	1	1	0	1	4
1	5	0	17	2	2	3	2	0	3	2	16	4	3	0	0	0	0	0	9
1	6	6	2	2	4	1	2	2	1	0	11	1	0	1	1	1	0	0	4
1	8	2	4	1	3	1	1	2	5	0	4	2	0	1	1	0	0	1	2
1	0	0	10	2	1	0	2	5	0	0	3	0	1	1	0	0	0	0	3
8	0	0	9	2	1	6	2	3	0	2	16	4	0	0	0	0	0	0	9
8	3	2	6	2	1	1	2	5	2	2	2	3	2	0	0	0	0	0	7
8	11	0	16	2	3	3	2	4	5	0	5	1	0	1	1	1	0	1	5
8	3	6	1	2	6	3	2	4	2	0	9	1	0	1	1	0	0	1	3
8	0	0	8	2	2	6	2	4	5	2	16	4	3	0	0	0	0	0	9
8	7	12	5	2	1	1	2	5	4	2	16	4	3	0	0	0	0	0	9
8	1	5	7	1	2	3	2	1	5	2	2	0	0	1	0	1	0	0	6
8	7	0	10	2	1	1	3	4	5	2	6	0	0	1	0	1	0	0	7
1	11	2	5	2	2	1	2	2	5	2	16	4	3	0	0	0	0	0	9
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0	9
8	3	0	8	2	2	0	2	1	2	2	0	1	1	0	0	0	0	0	0
1	5	0	16	2	3	3	2	5	3	0	10	1	0	1	1	0	0	1	3
1	5	0	9	2	4	2	2	2	3	0	6	0	0	1	1	1	0	1	3
8	7	0	8	2	3	2	2	4	4	2	16	4	3	0	0	0	0	0	9
1	0	0	9	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0	9
8	3	0	16	2	6	3	2	5	2	0	6	0	0	1	1	0	0	1	8
8	0	9	8	2	9	6	3	3	0	2	16	4	3	0	0	0	0	0	9
8	3	0	17	2	3	3	2	4	2	0	5	1	0	1	1	1	0	1	4
1	6	14	11	2	3	3	3	5	1	0	8	0	0	1	1	0	0	0	5
1	7	0	10	2	3	2	2	2	5	2	16	4	3	0	0	0	0	0	9
1	3	0	20	1	5	3	1	5	2	0	10	1	0	1	1	1	0	1	3
8	1	0	23	1	6	3	1	1	5	0	2	0	0	1	1	1	0	0	1
8	7	0	17	2	4	5	2	4	4	0	3	2	0	1	1	0	0	1	3
1	9	2	5	1	1	1	2	2	5	2	16	4	3	0	0	0	0	0	9
8	5	0	15	2	2	3	2	4	3	2	0	4	0	0	0	0	0	0	3
8	3	6	15	2	4	3	2	4	2	0	14	0	0	1	1	0	0	1	2
1	5	2	12	1	4	3	2	5	5	0	6	4	0	1	1	1	1	1	3
8	5	14	20	1	3	2	2	5	5	0	13	0	0	1	1	1	0	0	3
8	3	9	4	2	2	0	2	1	2	2	1	4	1	0	0	0	0	0	1
1	11	0	31	1	3	3	1	2	5	0	5	1	0	1	1	0	0	1	4
2	3	5	6	1	3	6	2	5	2	0	4	4	1	1	0	0	0	0	3
8	2	12	9	2	3	0	2	4	5	2	16	4	3	0	0	0	0	0	9
8	11	0	12	2	2	1	2	4	5	2	16	4	3	0	0	0	0	0	9
1	6	11	7	1	1	1	2	0	1	0	6	0	0	1	1	0	1	1	7
1	7	0	13	2	1	1	2	0	4	2	16	4	3	0	0	0	0	0	9
8	2	5	9	2	2	0	3	4	5	0	5	4	0	1	1	1	0	1	5
1	7	2	18	0	2	2	2	0	5	0	13	0	0	1	0	0	0	0	7
1	11	0	21	1	3	3	1	5	5	0	6	1	0	1	1	0	0	1	4
1	6	9	4	2	4	3	2	5	1	0	8	4	0	1	1	1	1	1	2
1	5	2	8	1	1	2	1	5	5	2	16	4	3	0	0	0	0	0	9
8	0	0	8	2	3	0	2	3	0	2	16	4	3	0	0	0	0	0	9
1	0	0	9	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0	9
1	7	6	6	0	4	3	1	5	5	0	5	1	0	1	1	1	0	1	5
8	3	0	20	1	4	3	1	5	2	0	6	0	0	1	1	0	0	1	3
8	6	0	13	2	3	3	2	4	1	0	3	0	0	1	1	1	0	1	2
8	5	0	20	1	2	1	1	5	3	0	3	4	0	1	1	1	1	1	3
1	7	2	10	1	2	6	1	5	5	0	3	4	0	1	1	1	1	1	3
1	3	0	17	2	4	3	2	5	2	0	14	1	0	1	1	1	0	0	0
1	7	0	19	2	4	3	2	0	4	0	15	3	0	1	0	0	0	0	4
1	1	11	4	1	1	3	1	5	5	0	4	0	0	1	1	1	0	0	3
8	6	0	16	2	2	3	2	4	1	0	3	4	0	1	1	0	1	1	3
1	7	0	31	1	3	2	1	5	5	0	6	1	0	1	1	0	0	1	3
1	0	0	8	2	4	6	2	5	0	0	2	0	0	1	0	1	0	0	3
8	3	0	19	1	4	3	1	4	2	0	15	1	0	1	1	0	0	1	3
1	5	9	11	1	3	3	2	0	3	0	7	4	1	1	1	0	0	1	5
1	3	2	15	2	5	1	3	4	2	2	16	4	3	0	0	0	0	0	9
1	5	0	18	2	3	3	2	0	3	0	7	0	1	1	1	1	0	1	1
8	3	0	12	2	4	3	2	4	2	0	4	1	0	1	1	0	0	1	4
8	3	0	20	1	3	3	1	1	2	0	5	1	0	1	1	1	0	0	2
1	3	0	17	2	3	3	2	0	2	0	5	1	1	1	1	0	0	1	2
1	3	0	20	1	4	3	1	2	2	0	11	0	0	1	1	0	0	0	4
8	3	10	15	2	2	3	3	5	2	0	4	1	0	1	1	1	0	0	2
3	7	12	20	1	2	3	3	2	4	0	5	0	0	1	1	1	0	1	3
8	7	0	11	2	3	3	2	4	4	0	5	2	0	1	1	0	0	1	2
8	5	0	21	1	4	3	1	5	3	0	13	0	0	1	1	1	0	0	0
1	3	0	10	2	4	2	2	2	2	0	13	1	0	1	0	0	0	0	3
8	11	2	7	2	3	0	2	5	5	2	3	4	0	0	0	0	0	0	3
1	7	6	3	1	3	3	1	5	5	0	9	1	0	1	1	1	0	0	3
6	3	0	17	2	4	3	2	5	2	0	15	1	0	1	0	1	0	0	4
8	1	14	23	1	4	3	2	5	5	2	16	4	3	0	0	0	0	0	9
1	3	7	10	2	3	3	3	5	2	0	4	4	0	1	1	0	0	0	3
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0	9
8	3	1	6	2	3	3	2	5	2	0	3	4	0	1	0	0	0	0	4
8	6	2	6	1	4	1	2	4	1	0	11	4	0	1	1	1	0	1	3
8	3	14	2	2	3	3	2	4	2	0	4	0	0	1	1	0	0	1	4
8	6	0	12	2	5	3	2	4	1	0	15	4	0	1	1	0	1	0	3
8	0	0	9	2	1	6	2	3	0	0	16	0	0	0	0	0	0	0	0
1	6	0	19	2	2	3	2	0	1	0	3	2	0	1	1	1	1	1	5
1	5	0	10	2	2	3	2	5	3	0	13	1	0	1	1	0	0	0	5
1	3	0	25	1	2	3	1	2	2	0	2	1	0	1	1	0	1	1	3
8	3	0	17	2	2	1	2	1	2	0	5	1	0	1	1	0	0	1	2
3	6	2	8	2	1	3	3	2	1	2	16	4	3	0	0	0	0	0	9
1	5	0	26	1	3	3	3	5	5	0	7	1	0	1	1	0	0	0	2
1	5	0	12	2	2	3	2	2	3	0	5	1	0	1	1	1	0	1	4
8	3	0	9	2	5	3	2	4	2	0	15	4	0	1	1	1	1	1	8
8	0	0	6	3	1	0	3	3	0	2	16	4	3	0	0	0	0	0	9
1	6	13	8	2	2	3	3	2	1	0	5	1	0	1	1	1	0	1	5
8	11	6	4	2	4	6	2	1	5	2	16	4	3	0	0	0	0	0	9
7	3	5	3	2	1	3	2	1	2	0	16	0	1	0	0	0	0	0	8
8	6	9	11	2	3	1	2	4	1	0	3	3	0	1	1	1	0	1	3
8	3	1	2	1	3	3	2	4	2	2	16	4	3	0	0	0	0	0	9
8	6	0	14	2	1	1	2	4	1	1	1	2	0	1	1	1	0	1	0
1	6	2	7	2	3	3	2	5	1	0	5	4	2	1	1	0	0	1	7
8	6	1	5	1	2	1	2	1	1	0	8	4	0	1	1	1	0	1	3
1	3	2	4	2	1	0	3	5	2	0	10	1	0	1	1	1	0	0	3
1	3	0	19	1	3	3	1	0	5	0	6	1	0	1	1	1	0	1	5
1	3	0	12	2	4	3	2	0	2	0	9	4	0	1	1	0	1	1	0
8	9	14	20	1	2	3	3	4	5	0	4	3	0	1	1	0	0	1	3
1	5	0	23	1	2	3	1	0	5	0	5	0	1	1	1	0	0	1	4
3	7	7	8	2	2	4	2	1	4	0	9	0	0	1	1	0	0	0	1
1	11	0	16	2	4	3	2	5	5	0	7	1	0	1	1	1	1	1	6
1	7	0	31	1	2	5	1	5	5	0	4	1	0	1	1	0	0	1	4
1	3	2	6	1	1	3	2	5	5	1	4	0	0	1	1	0	0	1	7
8	0	9	8	2	1	6	3	3	0	2	16	4	3	0	0	0	0	0	9
1	5	2	11	1	3	6	2	5	5	0	6	1	0	1	1	0	0	0	2
1	7	0	11	2	2	3	2	0	4	0	1	1	0	1	0	1	0	0	3
8	7	0	13	2	1	3	2	4	4	0	9	4	1	1	1	0	0	1	0
8	6	0	13	2	7	3	2	4	1	0	15	1	0	1	1	0	0	1	2
1	0	0	9	2	1	6	2	5	0	2	6	4	0	1	1	0	0	0	2
1	7	0	9	2	1	0	2	0	5	2	16	4	3	0	0	0	0	0	9
1	5	0	14	2	2	3	2	4	3	2	16	4	3	0	0	0	0	0	9
8	6	0	16	2	2	3	3	5	1	0	15	1	0	1	1	0	0	1	3
8	3	9	2	2	3	3	2	4	2	2	16	4	3	0	0	0	0	0	9
8	7	2	9	1	5	1	1	1	5	2	3	0	0	0	0	0	0	0	1
8	3	0	10	2	2	2	2	5	2	0	4	4	0	1	1	1	0	1	0
8	3	7	13	2	1	6	3	1	2	0	4	1	0	1	1	1	0	1	6
1	3	0	13	2	1	3	2	4	2	0	16	1	1	1	0	0	0	0	8
1	7	0	13	2	3	3	3	0	4	0	15	2	0	1	1	0	0	1	1
8	6	6	1	1	4	3	1	1	1	0	12	1	0	1	1	0	0	1	3
1	7	5	16	2	1	1	3	0	4	2	16	4	3	0	0	0	0	0	9
8	3	12	19	1	2	0	2	4	2	0	4	1	0	1	1	0	1	1	6
1	0	0	9	2	3	0	2	5	0	0	4	3	1	1	1	0	0	1	3
1	3	0	19	2	3	1	2	5	2	0	5	1	1	1	1	0	0	1	2
8	11	14	3	2	3	3	2	4	5	2	16	4	3	0	0	0	0	0	9
8	5	0	17	2	2	1	2	5	3	0	11	0	0	1	1	1	0	0	7
8	3	0	10	2	2	2	2	4	2	0	2	1	0	1	1	0	0	1	4
8	6	6	1	2	4	3	2	4	1	0	2	3	0	1	0	0	0	0	3
1	3	0	12	2	4	3	2	2	2	0	8	0	1	1	1	0	0	0	4
1	7	2	14	1	2	3	2	5	5	0	5	0	0	1	1	0	0	0	2
8	0	0	10	2	1	6	2	5	0	2	6	0	1	1	1	1	0	0	3
8	3	9	14	2	1	1	2	1	2	0	7	0	0	1	1	0	0	1	2
8	9	10	19	1	2	3	2	4	5	0	2	1	0	0	0	0	0	0	3
1	7	0	16	2	2	3	2	0	4	0	2	1	0	1	1	1	0	1	5
8	5	0	15	2	2	1	2	4	3	0	1	1	0	1	1	0	0	1	3
1	11	10	31	1	2	0	2	2	5	2	16	4	3	0	0	0	0	0	9
1	8	10	25	1	3	3	2	2	5	0	13	1	0	1	1	1	1	1	2
1	5	0	13	2	3	3	2	2	3	0	6	1	0	1	1	0	0	0	2
1	1	0	23	1	1	3	1	5	5	2	3	4	1	1	1	0	0	1	1
8	5	2	12	2	2	1	3	1	3	0	9	4	1	0	0	0	0	0	2
1	5	0	18	2	2	3	2	0	3	0	4	0	0	1	0	0	0	0	1
8	5	0	18	2	4	3	2	1	3	0	6	4	0	1	1	1	0	0	1
1	6	6	2	2	6	3	2	5	1	2	16	4	3	0	0	0	0	0	9
8	3	0	14	2	4	3	2	4	5	0	5	1	0	1	0	0	0	0	5
1	7	0	31	0	4	5	0	2	5	0	7	3	0	1	1	0	0	0	1
8	0	0	9	2	1	0	2	3	0	2	16	4	3	0	0	0	0	0	9
8	11	10	16	2	3	5	2	1	5	0	7	4	0	1	1	1	0	0	2
8	6	0	10	2	1	3	2	4	1	0	4	0	0	1	1	0	0	0	8
8	3	0	11	2	5	2	2	1	2	0	15	0	1	1	1	1	0	1	8
8	11	10	29	1	4	1	2	1	5	2	16	4	3	0	0	0	0	0	9
8	5	0	14	2	4	3	2	4	3	0	2	0	1	1	1	1	0	1	0
8	3	5	9	2	3	3	2	1	2	0	6	2	0	1	1	1	0	0	0
1	3	1	8	2	2	1	2	5	2	2	16	4	3	0	0	0	0	0	9
8	5	0	15	2	7	3	2	4	3	0	15	1	0	1	1	1	0	1	2
1	5	0	16	2	2	3	2	5	3	0	1	2	0	1	1	0	0	1	6
8	5	0	12	2	6	1	2	5	3	0	8	0	1	1	1	1	0	1	0
8	3	0	13	2	4	3	2	1	2	2	1	1	0	1	1	0	0	0	4
8	3	0	12	2	3	3	2	5	2	0	7	1	0	1	1	1	1	1	4
1	3	0	24	1	2	3	1	0	2	0	2	1	0	1	1	1	1	1	6
7	7	2	10	1	3	2	1	5	5	2	5	2	0	0	0	0	0	0	0
3	3	5	8	2	1	3	3	4	2	0	8	1	0	1	1	1	0	1	1
8	5	11	5	1	4	3	2	4	3	0	15	1	0	1	1	1	0	1	3
8	3	6	3	2	1	3	2	4	5	0	5	1	0	1	1	0	0	1	8
8	5	0	9	2	2	2	2	1	3	0	2	1	0	1	1	0	1	1	1
1	7	0	17	2	4	3	2	5	4	0	10	0	0	1	1	0	0	0	3
1	0	0	10	2	4	6	2	5	0	0	15	1	0	1	1	0	0	1	4
1	3	0	20	1	3	3	1	2	2	0	7	3	0	1	1	0	0	1	3
8	3	0	9	2	1	3	2	5	2	2	1	4	0	1	1	0	0	1	7
1	7	2	5	2	4	3	2	5	4	2	16	4	3	0	0	0	0	0	9
8	7	0	11	2	4	2	2	1	4	0	9	0	0	1	1	0	0	1	6
1	5	0	11	2	1	3	2	5	3	2	0	4	0	0	0	0	0	0	0
1	7	0	31	1	3	3	1	0	5	0	13	4	0	1	0	0	0	0	6
1	3	9	2	2	1	6	2	0	2	0	4	1	0	1	1	0	0	1	1
8	3	6	2	2	4	3	2	5	2	0	10	1	0	1	1	0	1	1	6
1	7	12	17	1	4	3	2	5	4	0	10	1	0	1	1	0	0	0	3
1	3	0	10	2	3	2	2	0	2	2	16	4	3	0	0	0	0	0	9
8	5	11	27	1	3	3	3	1	5	0	1	4	1	1	1	0	1	1	3
1	7	0	16	2	4	3	2	5	5	0	11	2	0	1	1	0	0	0	3
1	6	14	21	1	3	3	1	2	1	0	5	4	0	1	1	0	0	0	1
1	11	0	19	2	2	1	2	5	5	0	2	2	0	1	1	1	0	1	5
1	6	2	7	2	1	3	2	5	1	2	16	4	3	0	0	0	0	0	9
8	3	0	20	1	2	3	1	1	2	0	5	1	0	1	0	0	0	0	0
8	0	0	8	2	4	6	2	3	0	2	16	4	0	1	0	1	0	0	9
8	0	0	8	2	1	6	2	3	0	2	16	0	0	1	1	0	0	1	1
8	3	10	16	2	2	3	2	0	2	0	1	0	1	1	1	0	0	1	6
1	0	14	6	2	2	2	3	5	0	0	4	2	0	1	1	0	0	1	0
1	7	6	1	1	7	2	1	0	5	2	16	4	3	0	0	0	0	0	9
8	5	0	17	2	1	2	2	4	3	0	5	0	1	1	1	0	0	1	3
8	3	3	2	1	1	1	1	5	2	2	16	4	3	0	0	0	0	0	9
8	5	0	22	1	1	3	1	5	5	2	2	3	0	0	0	0	0	0	3
1	9	5	6	2	1	1	2	5	5	2	16	4	3	0	0	0	0	0	9
1	11	2	12	2	3	2	2	5	5	0	4	1	0	0	1	0	0	0	0
8	7	0	16	2	4	3	2	4	4	0	12	1	0	1	1	1	0	1	3
1	3	2	6	2	2	1	2	2	2	2	16	4	3	0	0	0	0	0	9
1	3	0	15	2	1	0	2	5	2	0	3	4	0	1	1	1	0	1	7
1	11	2	4	2	1	1	2	2	5	0	15	1	0	1	1	0	0	0	4
1	7	0	14	2	4	3	2	5	5	0	12	1	0	1	1	1	1	1	3
8	7	10	22	1	2	2	3	1	5	0	15	0	0	0	0	0	0	0	4
1	3	14	8	2	1	1	3	5	2	2	16	4	3	0	0	0	0	0	9
1	5	5	12	2	6	1	3	5	3	2	16	4	3	0	0	0	0	0	9
8	6	3	1	2	1	2	2	4	1	2	16	4	3	0	0	0	0	0	9
4	3	2	7	2	2	6	2	2	2	2	8	3	0	1	0	0	0	0	3
1	11	0	21	1	1	1	1	5	5	2	16	4	3	0	0	0	0	0	9
8	6	0	9	2	5	2	2	4	1	0	3	4	0	1	1	1	0	0	1
8	6	2	18	2	3	3	2	5	1	0	6	0	0	1	1	1	1	1	6
1	3	0	17	2	1	1	2	5	2	0	15	2	0	1	1	1	0	0	8
8	3	0	19	1	4	3	1	5	2	0	13	1	0	1	1	1	1	1	5
1	6	9	7	2	2	3	2	2	1	2	16	4	3	0	0	0	0	0	9
1	3	0	12	2	2	2	2	5	2	0	3	0	0	0	0	0	0	0	0
8	3	0	19	1	3	5	1	1	2	0	15	1	1	1	1	0	0	1	3
1	3	6	3	2	1	6	2	5	2	2	16	4	3	0	0	0	0	0	9
3	3	6	9	2	2	3	3	0	2	0	5	4	0	1	1	1	0	1	3
1	7	5	7	2	3	2	3	5	4	2	0	1	0	0	0	0	0	0	7
8	5	0	20	1	3	3	1	5	3	2	16	4	3	0	0	0	0	0	9
1	7	2	4	2	1	1	2	0	4	0	1	0	0	1	1	0	0	1	0
8	3	0	20	1	5	3	1	4	2	0	13	1	0	1	1	0	1	1	6
8	7	0	25	1	4	6	1	1	5	0	3	2	0	1	1	0	0	1	4
8	5	3	13	2	5	1	3	5	3	0	16	1	0	1	0	0	0	0	1
1	5	9	4	2	4	6	2	5	3	0	7	2	0	1	1	1	1	1	4
8	11	0	9	2	2	2	2	4	5	0	5	1	0	1	1	1	1	1	5
1	6	13	20	1	2	3	3	5	1	2	16	4	3	0	0	0	0	0	9
8	0	9	8	2	1	6	3	3	0	0	5	3	1	1	1	0	0	0	1
8	9	0	21	1	2	2	1	1	5	2	16	4	3	0	0	0	0	0	9
1	3	14	14	2	3	3	3	0	2	0	9	1	1	0	0	0	0	0	3
1	0	12	3	2	1	0	3	5	0	0	10	1	0	1	0	1	0	0	3
1	7	0	17	2	2	3	2	5	4	0	0	1	1	1	1	0	0	0	3
8	3	0	9	2	2	5	2	4	2	0	2	0	0	1	1	0	0	1	2
8	5	8	10	1	3	3	2	1	3	0	13	2	0	1	1	0	0	1	4
8	5	0	15	2	4	1	2	4	3	0	6	1	0	1	1	0	0	0	3
8	1	2	23	1	3	3	3	1	5	0	6	1	0	1	0	0	0	0	0
1	3	0	12	2	2	3	2	5	2	0	5	1	0	1	0	1	1	0	2
1	3	0	1	3	1	2	3	5	2	2	3	4	0	0	0	0	0	0	3
8	0	14	5	2	1	6	2	1	5	2	0	4	0	0	0	0	0	0	4
1	3	10	19	1	5	3	3	5	2	0	3	0	0	1	1	1	0	1	1
1	3	10	19	1	3	3	3	5	5	2	16	4	3	0	0	0	0	0	9
1	3	0	10	2	2	2	2	5	2	0	4	1	0	1	1	1	0	0	3
8	3	0	20	2	4	3	2	4	5	0	15	4	1	1	1	0	1	1	2
1	3	2	5	2	1	2	2	0	2	0	2	0	1	1	1	1	0	1	3
1	5	0	14	2	2	3	2	0	5	0	10	2	0	1	1	0	1	1	6
8	3	0	9	2	3	2	2	5	2	2	0	1	1	0	0	0	0	0	8
8	2	2	6	2	2	0	2	4	5	2	16	4	3	0	0	0	0	0	9
1	7	0	23	1	2	3	1	5	5	0	6	1	0	1	1	1	0	0	3
1	6	2	7	2	1	0	3	2	1	2	1	4	0	1	0	0	0	0	1
8	7	2	14	1	3	1	2	5	5	2	16	4	3	0	0	0	0	0	9
1	5	0	20	1	4	2	1	5	3	0	2	1	0	1	0	1	0	0	5
8	3	12	3	2	2	3	2	4	2	0	7	1	0	1	1	1	0	1	1
1	5	0	26	1	4	3	1	0	5	0	4	1	0	1	1	1	0	1	3
1	7	0	32	0	1	3	1	0	5	0	8	0	0	1	1	1	1	0	4
8	7	11	7	2	4	2	3	1	4	0	10	1	0	1	1	1	0	1	3
8	3	0	12	2	2	3	2	4	2	0	3	0	0	1	1	0	0	1	3
1	3	0	20	1	3	2	2	0	2	0	7	1	0	1	1	1	1	1	4
1	6	9	4	2	5	2	3	2	1	0	13	1	0	1	0	1	0	0	7
1	0	9	9	2	1	6	3	5	0	2	16	4	3	0	0	0	0	0	9
8	6	3	4	1	1	6	2	1	1	2	4	1	0	1	1	0	0	1	3
8	6	5	9	2	3	3	2	1	1	2	16	4	3	0	0	0	0	0	9
8	3	0	18	2	3	3	2	4	2	0	2	3	0	1	0	0	1	0	8
8	11	2	8	2	2	0	2	1	5	2	16	0	0	1	1	0	0	0	3
8	6	13	1	2	4	1	2	4	1	0	2	0	0	1	1	0	0	1	3
1	5	1	18	1	2	3	2	2	5	0	10	4	0	1	1	1	0	1	0
1	11	0	19	2	3	1	2	2	5	0	5	1	0	1	1	0	0	1	6
8	3	2	19	1	3	3	2	1	2	2	16	4	3	0	0	0	0	0	9
1	3	0	10	2	2	2	2	2	2	0	2	2	0	1	1	1	0	1	2
1	6	0	21	1	2	3	1	0	5	0	1	0	0	0	0	1	0	0	1
1	11	3	6	2	2	0	3	5	5	0	11	1	0	1	1	0	1	1	1
8	3	0	22	1	3	3	1	5	5	0	5	1	0	1	1	1	0	1	3
8	0	0	8	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0	9
8	7	0	14	2	2	2	2	1	5	0	2	0	1	1	1	0	0	1	8
8	11	14	23	1	3	7	1	5	5	2	16	4	3	0	0	0	0	0	9
8	3	6	2	2	3	6	2	1	2	0	5	1	0	1	1	1	0	0	3
1	7	2	7	0	3	3	1	2	5	2	16	4	3	0	0	0	0	0	9
8	3	0	9	2	2	2	2	4	2	0	3	1	0	1	1	1	0	1	6
8	7	2	6	1	3	1	2	1	4	2	6	0	0	0	0	0	0	0	7
1	7	1	15	1	3	1	2	0	5	0	14	1	0	0	1	1	0	0	6
8	3	0	15	2	5	3	2	5	2	0	1	1	0	1	1	0	0	0	3
1	6	14	20	1	1	1	3	2	1	2	16	0	2	1	1	0	0	1	1
8	3	0	9	2	5	2	2	4	2	0	9	1	0	1	1	0	0	1	8
8	2	2	6	2	3	6	2	1	5	2	16	4	3	0	0	0	0	0	9
1	7	0	24	1	3	3	2	0	5	0	6	4	0	1	0	0	0	0	3
1	7	2	4	2	1	1	2	2	4	0	6	0	1	1	1	0	0	1	0
8	7	4	12	2	3	3	3	4	4	0	7	2	0	0	1	0	0	0	2
1	0	0	9	2	2	0	2	5	0	2	3	0	1	0	0	0	0	0	7
8	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0	9
8	3	0	17	2	4	3	2	1	2	0	15	1	0	1	1	1	0	1	4
1	9	0	13	2	2	3	2	2	5	0	5	1	0	1	1	0	0	1	3
8	2	14	2	2	2	6	2	1	5	2	16	4	3	0	0	0	0	0	9
8	7	3	28	0	2	3	3	1	5	0	1	0	0	1	1	0	0	0	5
1	6	6	1	2	2	3	2	2	1	0	5	1	0	1	1	0	0	1	2
1	7	10	17	2	0	4	2	2	4	0	11	4	0	1	1	0	0	1	3
8	5	14	21	1	3	6	2	1	3	2	16	4	3	0	0	0	0	0	9
8	5	14	1	2	4	4	2	4	3	2	16	4	3	0	0	0	0	0	9
1	6	0	12	2	1	3	2	0	5	0	11	1	0	1	1	1	1	1	4
1	11	0	9	2	3	2	2	5	5	2	16	4	3	0	0	0	0	0	9
1	11	2	4	2	1	1	2	2	5	2	16	4	3	0	0	0	0	0	9
8	3	0	18	2	3	3	2	4	5	2	16	4	3	0	0	0	0	0	9
8	6	2	12	1	2	1	2	1	1	0	1	0	2	1	1	0	0	1	8
1	3	13	18	1	2	3	3	0	2	0	4	2	0	1	1	1	0	1	3
1	7	2	12	1	1	1	1	5	5	2	16	0	1	0	0	0	0	0	1
8	3	0	11	2	4	3	2	4	2	2	16	3	0	1	1	0	0	1	4
1	3	0	20	1	3	6	1	5	2	0	5	0	1	1	1	0	0	1	1
0	7	0	8	2	4	3	2	5	4	0	7	1	0	1	1	1	1	1	2
8	6	0	19	2	1	1	2	0	1	2	16	4	3	0	0	0	0	0	9
1	3	0	14	2	4	3	2	0	2	2	16	4	3	0	0	0	0	0	9
8	3	14	6	2	2	2	3	4	2	2	16	4	1	1	1	0	0	1	2
1	3	0	9	2	1	2	2	5	2	0	8	1	0	1	0	0	0	0	7
1	0	9	8	2	1	6	3	5	0	2	16	4	3	0	0	0	0	0	9
1	11	0	18	2	5	3	3	5	5	0	9	2	0	1	1	0	0	1	0
1	11	10	29	1	3	3	2	2	5	0	4	1	0	1	1	0	0	1	2
8	7	11	12	2	2	1	3	5	4	2	16	4	3	0	0	0	0	0	9
8	3	0	13	2	2	3	2	1	2	2	15	4	0	1	0	0	0	0	2
8	6	0	12	2	1	3	2	4	1	0	15	4	1	1	1	0	1	1	6
1	3	3	5	2	3	6	2	5	5	0	4	1	0	1	1	1	0	0	3
8	3	0	9	2	2	2	2	4	2	0	1	1	0	1	1	0	1	1	3
1	5	0	8	2	2	2	2	5	3	0	4	1	0	1	1	0	0	1	1
1	5	0	25	1	5	3	1	2	5	2	16	4	3	0	0	0	0	0	9
1	7	10	31	1	2	5	2	0	5	0	2	0	0	0	0	0	0	0	8
8	0	9	8	2	9	6	3	3	0	2	16	4	1	1	0	1	0	0	9
8	3	0	18	2	3	3	2	4	2	0	3	2	0	1	1	0	0	0	3
8	3	5	5	2	2	6	2	5	2	2	1	4	1	1	1	0	0	0	3
8	5	6	2	1	3	6	1	1	3	2	16	4	3	0	0	0	0	0	9
1	7	0	14	2	2	3	2	2	4	0	3	2	0	1	1	0	0	0	1
8	0	9	10	2	1	6	3	3	0	0	12	4	0	1	1	0	0	1	3
1	5	0	13	2	2	0	2	2	3	0	4	0	0	1	1	1	0	1	4
8	6	13	11	2	3	3	3	1	1	0	15	1	0	1	1	0	0	0	1
1	3	0	13	2	1	1	2	5	2	0	12	0	0	1	1	1	0	0	2
1	5	2	13	1	2	1	2	2	5	0	11	1	0	1	1	0	0	0	4
1	9	0	11	2	1	3	2	0	5	2	16	4	3	0	0	0	0	0	9
8	3	0	16	2	2	0	2	1	2	0	5	0	0	1	1	0	0	1	4
1	5	5	23	1	1	3	2	2	5	2	5	0	1	1	1	0	0	1	7
1	3	0	20	1	3	3	1	5	2	0	7	1	0	1	1	0	0	1	6
1	3	0	18	1	2	3	1	2	2	2	5	4	1	1	1	1	0	0	0
1	11	2	2	2	1	1	2	2	5	2	16	4	3	0	0	0	0	0	9
1	2	2	5	2	2	0	2	5	5	2	16	4	3	0	0	0	0	0	9
8	6	0	20	1	3	1	1	1	1	0	1	1	0	1	0	0	0	0	4
8	9	2	6	2	1	1	3	1	5	2	3	0	0	1	1	0	0	1	1
8	6	14	1	2	4	3	2	4	1	0	4	1	1	1	1	0	0	0	3
8	3	0	9	2	3	2	2	4	2	0	11	4	0	1	1	1	0	1	3
8	5	13	17	1	3	0	3	5	5	2	16	4	3	0	0	0	0	0	9
8	6	0	13	2	2	3	2	4	1	0	2	0	0	1	0	0	1	0	3
8	0	0	10	2	2	6	2	5	0	0	3	1	0	1	1	1	0	1	2
8	6	3	12	2	2	3	3	4	1	0	2	1	0	0	1	0	0	1	7
1	3	13	17	1	2	2	3	0	2	0	8	1	0	1	1	1	0	1	6
8	6	0	11	2	2	3	2	4	1	0	2	1	0	1	1	0	0	1	4
8	3	0	15	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0	9
1	0	0	8	2	2	6	2	5	0	0	3	4	0	0	0	0	0	0	0
8	5	11	19	1	2	3	2	5	3	0	2	4	2	1	0	0	0	0	4
8	9	0	12	2	1	3	3	4	5	0	2	0	0	1	1	0	0	1	3
8	11	2	5	2	2	1	2	1	5	2	16	4	3	0	0	0	0	0	9
1	5	0	14	2	1	3	2	5	3	2	2	4	1	1	1	0	0	1	4
8	3	0	20	1	4	3	2	4	2	0	4	2	0	0	0	0	0	0	3
1	5	0	8	2	1	1	2	5	3	0	7	1	0	0	0	0	0	0	3
1	7	3	2	1	3	6	2	0	4	0	9	1	0	1	1	1	0	1	4
8	6	0	14	2	3	3	2	1	1	0	2	3	0	0	0	0	0	0	6
8	3	2	14	2	3	1	3	5	2	0	6	1	0	1	1	1	0	0	1
1	6	9	2	2	1	3	2	5	1	1	2	0	0	1	1	0	0	1	1
8	5	2	20	1	4	1	3	1	3	0	15	1	0	1	1	1	0	0	1
1	5	2	6	1	1	3	1	0	5	0	11	4	1	1	1	0	0	1	2
1	5	0	24	1	3	3	1	2	5	0	4	1	0	1	1	0	0	1	4
1	7	0	31	1	2	0	2	0	5	2	1	3	0	0	0	0	0	0	7
1	9	0	11	2	3	3	2	2	5	0	15	0	0	1	1	0	1	1	6
8	2	1	1	2	2	0	2	5	5	1	2	1	0	1	0	0	0	0	2
1	6	2	17	2	1	0	3	2	1	0	4	0	0	1	1	0	0	0	7
1	7	5	19	1	3	3	2	2	5	0	4	0	0	1	1	1	0	1	5
8	0	0	9	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0	9
8	3	0	16	2	1	3	2	4	2	2	16	0	0	1	0	0	0	0	2
8	0	0	9	2	2	6	2	5	0	2	3	4	0	1	0	1	0	0	0
1	6	10	26	1	3	3	2	0	1	0	9	1	0	1	1	0	1	1	4
8	3	0	20	1	3	3	1	5	2	0	0	0	0	1	1	1	0	0	3
1	3	0	20	1	3	3	1	2	2	0	14	3	0	0	0	0	0	0	3
8	7	0	20	1	2	5	1	1	5	0	5	2	0	1	1	0	0	1	3
8	7	8	2	2	1	1	2	5	4	2	16	4	3	0	0	0	0	0	9
1	1	12	14	1	1	2	2	2	5	2	16	4	3	0	0	0	0	0	9
1	0	9	9	2	1	6	3	5	0	2	16	4	3	0	0	0	0	0	9
1	3	13	6	2	3	2	3	5	2	0	6	1	0	1	0	1	0	0	4
8	6	5	8	1	1	1	2	5	1	0	2	0	0	1	1	0	0	0	3
1	5	0	27	1	2	3	1	0	5	0	1	4	0	1	1	0	1	1	3
1	5	0	13	2	5	3	2	2	3	0	10	1	0	1	1	1	0	1	5
1	1	0	24	1	3	3	1	0	5	0	15	1	0	1	1	1	1	0	1
8	3	11	8	2	6	3	2	4	2	0	5	1	0	1	1	1	1	1	4
8	7	4	2	2	2	1	2	4	4	0	2	4	0	1	0	0	0	0	0
1	7	11	30	0	4	3	3	2	5	0	12	1	0	1	1	0	1	1	5
1	3	0	8	2	4	2	2	0	2	0	15	1	0	1	1	1	0	1	2
8	6	2	9	2	2	1	2	4	1	2	16	0	0	1	1	0	0	1	1
1	5	0	22	1	1	2	1	2	5	2	16	4	3	0	0	0	0	0	9
8	6	0	22	1	2	3	1	5	1	0	2	4	0	1	1	0	0	1	5
8	3	0	14	2	2	3	2	4	2	0	1	3	0	1	1	0	0	1	1
1	5	0	9	2	1	2	2	0	3	1	1	0	1	1	0	0	0	0	7
1	3	0	11	2	5	3	2	2	2	2	0	1	1	0	0	0	0	0	4
8	3	0	20	1	3	3	3	4	2	2	16	4	3	0	0	0	0	0	9
1	7	0	31	1	3	3	1	5	5	0	5	4	0	1	0	0	0	0	3
1	3	0	11	2	5	2	2	5	2	0	1	2	0	1	1	0	0	0	5
8	5	0	24	1	2	3	1	1	5	2	16	4	3	0	0	0	0	0	9
1	3	14	10	2	2	2	2	1	2	2	16	4	3	0	0	0	0	0	9
1	1	0	24	1	1	2	2	0	5	2	16	4	3	0	0	0	0	0	9
1	3	0	20	1	4	2	1	0	2	0	10	4	0	1	1	0	0	1	3
8	3	9	8	2	1	3	2	5	2	2	16	4	0	0	0	0	0	0	9
1	5	2	16	1	2	1	2	2	3	0	3	0	0	1	1	1	1	1	2
1	6	0	19	2	5	3	2	2	1	0	5	1	0	1	1	1	1	0	4
1	7	4	10	2	2	2	3	0	4	0	5	2	0	1	1	0	0	1	1
1	7	2	4	2	2	2	3	5	4	0	4	1	0	1	1	1	0	1	3
8	5	0	11	2	3	3	2	4	3	0	13	1	0	1	1	1	1	1	4
8	0	0	10	2	1	5	2	3	0	1	0	4	1	0	0	1	0	0	4
1	3	0	12	2	1	7	2	5	2	2	5	2	0	0	0	0	0	0	4
1	3	0	21	1	3	2	1	5	2	0	6	1	0	1	1	1	0	1	3
8	7	9	1	2	3	3	2	4	4	0	3	1	0	1	1	1	1	1	5
8	3	13	4	2	2	2	3	5	2	2	1	1	0	0	0	0	0	0	0
1	11	0	20	2	4	3	2	2	5	2	16	4	3	0	0	0	0	0	9
8	7	0	9	2	3	2	2	1	5	0	12	1	0	1	1	0	0	0	2
8	3	0	11	2	2	2	2	4	2	2	16	4	3	0	0	0	0	0	9
1	5	0	24	1	3	3	1	0	5	0	7	0	0	1	1	0	0	1	0
1	6	2	7	2	2	1	2	2	1	2	1	0	1	0	1	0	0	0	7
8	3	11	8	2	2	2	3	5	2	2	0	4	0	0	0	0	0	0	5
1	7	14	18	1	2	6	2	2	5	0	16	0	1	1	0	1	0	0	4
8	3	9	7	2	1	1	3	5	2	0	13	1	0	1	1	1	0	1	4
3	11	7	10	2	1	1	3	5	5	2	16	4	3	0	0	0	0	0	9
8	7	0	16	2	3	1	2	4	4	0	8	0	0	1	0	0	0	0	3
1	0	0	8	2	3	0	2	5	0	2	1	4	1	0	0	0	0	0	3
8	0	9	8	2	1	6	3	3	0	0	8	0	0	1	0	1	0	0	2
8	7	2	9	1	4	1	1	5	5	2	16	4	3	0	0	0	0	0	9
1	5	2	22	1	2	1	2	5	3	2	2	1	0	1	1	0	0	1	0
1	7	2	10	2	3	1	3	0	4	2	16	4	3	0	0	0	0	0	9
8	3	10	21	1	4	3	2	5	2	0	8	4	0	1	1	0	0	0	4
1	7	10	31	0	2	0	3	0	5	2	16	4	3	0	0	0	0	0	9
8	3	9	2	2	4	3	2	5	2	0	15	4	0	1	1	0	0	0	1
8	3	3	8	2	2	2	3	4	2	2	16	4	3	0	0	0	0	0	9
8	6	11	6	2	1	1	2	1	1	2	16	4	2	0	0	0	0	0	9
1	5	0	13	2	4	3	2	5	3	0	9	1	0	1	1	1	1	1	6
1	3	0	11	2	2	3	2	0	2	0	10	1	0	1	1	0	0	1	3
1	7	9	22	1	2	1	1	0	5	0	4	1	0	1	0	1	0	0	2
8	7	0	19	2	2	3	2	4	4	0	10	2	0	1	1	1	1	1	3
8	9	2	5	1	1	1	2	1	5	0	15	0	1	1	1	0	0	0	7
8	7	3	5	1	1	3	2	5	4	0	12	0	1	1	1	0	0	1	2
1	6	2	4	2	1	6	2	2	1	2	1	0	1	0	0	0	0	1	1
1	3	11	21	1	5	1	2	0	2	2	2	0	0	1	0	0	0	0	6
8	7	11	11	2	2	1	3	4	4	2	16	4	3	0	0	0	0	0	9
1	0	5	2	2	3	6	2	3	0	0	5	3	0	0	1	0	0	0	7
8	7	0	8	2	2	2	2	4	5	0	3	1	0	1	1	1	1	1	3
8	3	0	12	2	1	3	2	4	2	0	3	0	1	1	1	0	0	1	4
8	11	1	16	0	2	1	2	5	5	2	2	0	0	0	1	0	0	0	3
1	7	0	17	2	4	3	2	5	4	0	10	1	0	1	1	1	0	0	4
1	11	14	4	2	3	2	3	5	5	2	16	4	3	0	0	0	0	0	9
8	7	0	19	1	1	3	1	4	5	0	4	4	0	1	1	0	0	1	1
1	7	2	12	1	2	2	2	2	5	0	5	0	0	1	1	0	0	1	2
8	10	0	12	2	2	3	2	4	5	0	2	1	1	1	1	0	1	1	3
1	6	7	21	1	3	1	3	0	1	2	16	4	3	0	0	0	0	0	9
1	6	0	18	2	1	3	2	0	1	0	11	0	1	1	1	0	0	1	7
1	6	3	12	2	1	1	3	5	1	0	9	1	0	1	1	0	0	0	3
8	6	13	16	1	4	4	3	4	1	0	9	1	0	1	1	1	0	1	3
1	7	0	8	2	3	2	2	5	4	0	5	2	0	1	1	1	0	1	2
8	3	13	9	2	1	2	3	4	2	0	5	0	0	1	1	0	1	0	3
1	7	10	15	2	2	3	3	5	4	0	1	1	0	1	1	0	0	0	1
8	3	0	12	2	3	3	2	4	2	0	8	1	0	1	1	1	1	1	2
8	0	0	9	2	2	6	2	5	0	2	16	4	3	0	0	0	0	0	9
1	3	0	12	2	1	2	2	5	2	2	16	4	3	0	0	0	0	0	9
1	0	9	8	2	2	6	3	5	0	0	3	2	0	1	1	1	0	1	8
8	7	6	7	2	5	6	2	1	4	2	16	4	3	0	0	0	0	0	9
1	6	13	13	2	2	3	3	5	1	0	4	1	0	1	1	0	0	1	5
1	3	3	14	1	1	3	2	5	5	0	5	1	0	1	1	0	0	1	3
1	3	11	8	1	2	3	2	5	2	0	2	4	0	1	1	0	0	0	5
8	11	2	6	2	2	1	2	4	5	2	16	4	3	0	0	0	0	0	9
1	11	14	3	2	5	1	2	2	5	2	16	4	3	0	0	0	0	0	9
1	6	0	10	2	4	2	2	2	1	0	10	4	2	1	1	0	0	0	7
1	9	2	9	1	5	1	1	5	5	0	8	1	0	1	0	0	0	0	3
1	5	0	17	2	2	3	2	0	3	2	16	4	3	0	0	0	0	0	9
8	6	11	7	1	1	6	2	1	1	0	15	1	0	1	1	1	0	1	2
8	5	2	16	1	2	3	3	1	5	0	15	0	1	1	1	1	0	1	0
8	11	9	2	2	2	1	2	5	5	2	16	4	3	0	0	0	0	0	9
1	5	10	25	1	2	3	3	2	5	2	16	0	1	1	1	0	0	1	0
1	7	0	12	2	3	0	2	5	4	0	5	0	2	1	0	0	0	0	2
8	3	5	7	2	1	6	2	5	2	2	2	3	1	0	0	0	0	0	3
1	3	0	7	2	3	2	2	0	5	0	6	0	0	1	1	0	0	1	5
1	9	0	11	2	3	2	2	2	5	0	1	1	0	1	1	0	0	0	0
8	5	0	8	2	1	2	2	4	3	0	1	1	0	1	1	0	1	1	2
1	7	2	6	1	3	3	2	2	4	2	5	4	1	0	0	0	0	0	7
1	7	13	6	2	2	0	3	2	4	0	7	2	0	1	1	1	1	1	5
8	6	5	19	1	2	3	2	4	1	0	13	0	0	1	1	1	0	1	3
1	11	9	6	2	1	1	2	2	5	0	9	4	0	1	1	0	1	0	4
8	6	2	8	2	1	3	2	1	1	2	16	4	3	0	0	0	0	0	9
1	3	2	4	2	5	6	2	5	2	2	3	0	1	1	0	0	0	0	6
1	7	10	31	1	2	3	2	5	5	2	16	4	3	0	0	0	0	0	9
8	6	5	13	2	3	3	3	5	1	0	5	0	1	1	0	0	1	0	1
1	5	2	8	1	2	6	2	2	5	0	0	4	1	1	1	0	0	0	3
1	7	2	10	1	8	6	1	5	5	2	16	4	3	0	0	0	0	0	9
8	5	0	16	2	3	3	2	5	3	0	7	2	0	1	1	0	0	0	2
8	7	14	1	2	2	6	2	4	4	0	5	0	0	1	0	0	0	0	3
8	0	0	8	2	2	0	2	3	0	2	16	4	3	0	0	0	0	0	9
1	5	11	26	1	4	3	3	2	5	0	15	1	1	1	1	0	0	1	3
1	5	5	7	1	2	3	2	5	5	0	4	0	0	1	1	0	0	0	2
1	3	0	13	2	3	3	2	5	2	0	5	1	0	1	1	1	0	1	3
8	0	9	8	2	1	6	3	3	0	0	6	0	2	1	0	0	0	0	0
1	3	0	12	2	3	3	2	2	2	0	12	1	1	1	1	0	0	1	3
8	6	0	10	2	3	3	2	4	1	0	15	0	0	1	1	1	1	1	5
1	6	1	6	1	2	3	1	5	5	2	4	2	0	1	1	0	0	1	3
8	5	0	25	1	3	3	1	1	5	0	4	4	1	1	1	1	0	1	3
8	7	6	3	1	2	3	1	1	5	0	4	1	0	1	1	0	0	1	6
8	3	0	20	1	2	3	1	1	2	2	1	2	0	1	1	0	0	0	0
8	7	2	12	1	3	3	2	1	5	0	12	1	0	1	1	1	0	0	3
1	3	0	17	2	2	3	2	0	2	0	4	1	0	1	1	1	0	0	3
8	6	13	2	2	2	3	2	5	1	0	2	4	1	1	1	1	0	0	0
8	3	0	20	1	4	3	1	4	2	0	12	1	0	1	1	1	0	1	4
8	0	0	8	2	4	6	2	5	0	2	2	1	0	0	0	0	0	0	0
1	5	0	8	2	4	2	2	2	3	0	15	1	0	1	1	0	0	0	4
1	0	9	9	2	9	6	3	3	0	0	6	2	0	1	1	0	0	0	1
8	5	2	7	2	4	1	3	5	3	2	16	4	3	0	0	0	0	0	9
8	0	0	8	2	9	6	2	3	0	0	3	4	0	1	1	1	0	1	1
1	0	9	8	2	1	5	3	3	0	0	5	0	0	1	1	0	0	0	3
1	7	9	11	1	2	3	1	5	5	0	10	4	0	1	1	1	1	1	4
8	1	2	21	1	2	0	3	5	5	0	2	4	2	1	1	0	1	1	2
8	5	6	2	2	3	3	2	5	3	0	12	1	0	1	1	1	0	0	4
8	3	0	9	2	3	3	2	5	5	0	6	1	0	1	1	1	1	1	2
1	11	0	31	0	2	3	0	5	5	0	5	3	0	1	1	0	0	1	7
8	0	9	8	2	1	6	3	5	0	0	12	1	0	1	1	0	0	1	2
1	7	0	16	2	4	4	2	2	5	2	16	4	3	0	0	0	0	0	9
8	0	0	8	2	9	6	2	3	0	0	1	0	0	1	0	0	0	0	2
8	3	14	1	2	2	2	2	4	2	2	16	4	3	0	0	0	0	0	9
8	6	0	19	2	3	3	2	5	1	0	6	4	1	1	1	0	0	1	4
8	8	0	17	2	2	3	2	4	5	0	4	4	0	1	1	1	1	1	4
8	3	0	17	2	2	4	2	4	2	2	16	4	3	0	0	0	0	0	9
8	7	0	15	2	2	3	2	4	4	0	4	1	0	1	1	1	0	0	3
1	6	0	21	1	1	3	1	5	1	2	0	4	0	0	0	0	0	0	1
1	5	10	24	1	3	3	2	5	5	0	11	1	0	0	0	0	0	0	6
8	3	0	20	1	3	3	1	4	2	0	3	1	0	1	1	1	0	0	4#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 8500
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4


typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;

typedef struct treenode{
	float info;
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;

typedef struct calcinfostruct{
	float* info;
	value* val;
	treenode* node;
	int snum;
	int nnum;
	float (*subptr)[MAX_ATTR_VAL];
}calcinfostruct;

typedef struct checkleafnodestruct{
	int* res;
	value* val;
	int firstnum;
	int startnum;
	int num;
}checkleafnodestruct;

typedef struct teststruct{
	value* val;
	int startnum;
	int num;
	dicisiontree* tree;
}teststruct;



int subnum[MAX_ATTR_NUM]=ATTR_MAX;
void readsubinfo(float dest[][MAX_ATTR_VAL], FILE* fp){
	fread(dest, MAX_ATTR_VAL*MAX_ATTR_NUM, sizeof(float), fp);
}

void savesubinfo(float dest[][MAX_ATTR_VAL], FILE* fp){
	fwrite(dest, MAX_ATTR_VAL*MAX_ATTR_NUM, sizeof(float), fp);
}

void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}





void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}
















void* checkleafnodefunc(void* thearg){
	checkleafnodestruct* arg=(checkleafnodestruct*) thearg;
	int* res=arg->res;
	value* val=arg->val;
	int firstnum=arg->firstnum;
	int startnum=arg->startnum;
	int num=arg->num;
	int rres=0;
	int i;
	for(i=startnum;i<startnum+num;i++){
		if(rres==0){
			if(val[firstnum].res!=val[i].res){
				rres=1;
				break;
			}
		}
	}

	*res=rres;
}

int checkleafnode(value* val, dicisiontree* tree){
	treenode* node=&tree->node[++tree->num];
	int i;

	int res[GEM5_NUMPROCS];
	int rres=0;

	int rest=node->num%(GEM5_NUMPROCS-1);
	int snum=node->startnum;
	pthread_t thread[GEM5_NUMPROCS];
	checkleafnodestruct structs[GEM5_NUMPROCS];
	
	if(node->num==0)
		return 0;
	else{
		if(node->num<GEM5_NUMPROCS){
			for(i=0;i<node->num;i++){
				structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				structs[i].num=1;
				snum++;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<node->num;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<node->num;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		else{
			for(i=0;i<GEM5_NUMPROCS;i++){structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				if(rest==0){
					structs[i].num=node->num/(GEM5_NUMPROCS-1);
				}
				else{
					structs[i].num=node->num/(GEM5_NUMPROCS-1)+1;
					rest--;
				}
				snum+=structs[i].num;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		if(rres==0)
			node->treeval=val[node->startnum].res;
	}
	return rres;
}

void* calcinfofunc(void* thearg){
	calcinfostruct* arg=(calcinfostruct*)thearg;
	float* dest=arg->info;
	value* val=arg->val;
	treenode* node=arg->node;
	int snum=arg->snum;
	int num=arg->nnum;
	int i, j, k;
	int n;
	int attnum[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	int attnumval[MAX_ATTR_NUM][MAX_ATTR_VAL][MAX_INFO_VAL]={0,};
	float sum, psum, p, fnum, anum, tp;
	value* pval;
	float (*subptr)[MAX_ATTR_VAL]=arg->subptr;

	for(i=snum;i<snum+num;i++){

		for(j=node->startnum;j<node->startnum+node->num;j++){
		pval=&val[j];
			attnumval[i][pval->attr[i]][pval->res]++;
			attnum[i][pval->attr[i]]++;
		}

		sum=0.0f;
		fnum=(float)node->num;
		for(j=0;j<MAX_ATTR_VAL;j++){
			psum=0.0f;
			anum=(float)attnum[i][j];
			for(k=0;k<MAX_INFO_VAL;k++){
				n=attnumval[i][j][k];
				if(n!=0&&n!=attnum[i][j]){
					p=(float)n/anum;
					tp=(log(p)/log(2.0f));
					p=p*tp;
					psum-=p;
					subptr[i][j]+=tp;
				}
			}
			sum+=psum*anum/fnum;
		}
		dest[i]=sum;
	}
}

void calcinfo(float subinfo[][MAX_ATTR_VAL], value* val, dicisiontree* tree, float* info){
	int i;
	int snum=0;
	int nnum;
	treenode* node=&tree->node[tree->num];

	int rest=MAX_ATTR_NUM%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcinfostruct structs[GEM5_NUMPROCS];

	
	if(MAX_ATTR_NUM<GEM5_NUMPROCS){
			for(i=0;i<MAX_ATTR_NUM;i++){
				structs[i].info=info;
				structs[i].val=val;
				structs[i].node=node;
				structs[i].snum=snum;
				structs[i].nnum=1;
				structs[i].subptr=subinfo;
				pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
				snum++;
			}
			for(i=0;i<MAX_ATTR_NUM;i++){
				pthread_join(thread[i], NULL);
			}
		}
		else{
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				structs[i].info=info;
				structs[i].val=val;
				structs[i].node=node;
				structs[i].snum=snum;
				structs[i].subptr=subinfo;
				if(rest==0){
					structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1);
				}
				else{
					structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1)+1;
					rest--;
				}
				pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
				snum+=structs[i].nnum;
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				pthread_join(thread[i], NULL);
			}
		}
}

void compareinfo(value* val, dicisiontree* tree, float* info){
	treenode* node=&tree->node[tree->num];
	int i;
	int sel;
	float self=0xFFFFFFFF;

	for(i=0;i<MAX_ATTR_NUM;i++){
		if(node->flag[i]==0){
			if(self>info[i]){
				sel=i;
				self=info[i];
			}
		}
	}
	node->attnum=sel;
	node->flag[sel]=1;
}

void dividesection(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	value vallist[TRAIN_N][MAX_ATTR_VAL];
	int i, j, k;

	for(i=node->startnum;i<node->startnum+node->num;i++){
		j=val[i].attr[node->attnum];
		vallist[node->listcount[j]][j]=val[i];
		node->listcount[j]++;
	}
	i=node->startnum;
	while(i<node->startnum+node->num){
		for(j=0;j<MAX_ATTR_VAL;j++){
			for(k=0;k<node->listcount[j];k++){
				val[i]=vallist[k][j];
				i++;
			}
		}
	}
}

void makesubtree(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	treenode* nextnode;
	int i;
	int num=0;
	node->subnum=subnum[node->attnum];
	for(i=0;i<node->subnum;i++){
		nextnode=&tree->node[tree->maxnum];
		memcpy(nextnode, node, sizeof(treenode));
		nextnode->startnum=node->startnum+num;
		nextnode->num=node->listcount[i];
		memset(nextnode->listcount, 0, 4*MAX_ATTR_VAL);
		num+=nextnode->num;
		node->subptr[i]=tree->maxnum;
		tree->maxnum++;
	}
}







void* testfunc(void* thearg){
	teststruct* arg=(teststruct*)thearg;
	int i;
	int res;
	value* vval;
	treenode* node;
	value* val=arg->val;
	int startnum=arg->startnum;
	int num=arg->num;
	dicisiontree* tree=arg->tree;

	for(i=startnum;i<startnum+num;i++){
		vval=&val[i];
		node=&tree->node[0];
		while(node->treeval<0){
			if(node->num==0)
				break;
			node=&tree->node[node->subptr[vval->attr[node->attnum]]];
		}
		vval->res=node->treeval;
	}
}

void test(value* val, dicisiontree* tree){
	int i;

	int rest=TEST_N%(GEM5_NUMPROCS-1);
	int count=0;
	pthread_t thread[GEM5_NUMPROCS];
	teststruct structs[GEM5_NUMPROCS];
	if(TEST_N<GEM5_NUMPROCS){
		for(i=0;i<TEST_N;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			structs[i].num=1;
			structs[i].tree=tree;
			count++;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<TEST_N;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			if(rest==0){
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			structs[i].tree=tree;
			count+=structs[i].num;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}









int main(){
	value val[TRAIN_N];
	FILE* ftree=fopen("tree", "rb");
	FILE* fval=fopen("val", "rb");
	FILE* finfo=fopen("info", "wb");
	FILE* fsubinfo=fopen("fsubinfo", "wb");
	dicisiontree tree;
	float info[MAX_ATTR_NUM]={0,};
	float subinfo[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	
	readtree(&tree, ftree);
	readvalb(val, fval, TRAIN_N);
	calcinfo(subinfo, val, &tree, info);
	
	saveinfob(info, finfo, MAX_ATTR_NUM);
	
	savesubinfo(subinfo, fsubinfo);
	
	fclose(fsubinfo);
	fclose(ftree);
	fclose(fval);
	fclose(finfo);
	return 0;
}
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 8500
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4


typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;

typedef struct treenode{
	float info;
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;

typedef struct calcinfostruct{
	float* info;
	value* val;
	treenode* node;
	int snum;
	int nnum;
}calcinfostruct;


typedef struct teststruct{
	value* val;
	int startnum;
	int num;
	dicisiontree* tree;
}teststruct;



int subnum[MAX_ATTR_NUM]=ATTR_MAX;


void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}





void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}















int checkleafnode(value* val, dicisiontree* tree){
	treenode* node=&tree->node[++tree->num];

	if(node->num==0)
		return 0;
	else if(node->info==0.0f){
		node->treeval=val[node->startnum].res;
		return 0;
	}
	else return 1;
}

void* calcinfofunc(void* thearg){
	calcinfostruct* arg=(calcinfostruct*)thearg;
	float* dest=arg->info;
	value* val=arg->val;
	treenode* node=arg->node;
	int snum=arg->snum;
	int num=arg->nnum;
	int i, j, k;
	int n;
	int attnum[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	int attnumval[MAX_ATTR_NUM][MAX_ATTR_VAL][MAX_INFO_VAL]={0,};
	float sum, psum, p, fnum, anum;
	value* pval;

	for(i=snum;i<snum+num;i++){

		for(j=node->startnum;j<node->startnum+node->num;j++){
		pval=&val[j];
			attnumval[i][pval->attr[i]][pval->res]++;
			attnum[i][pval->attr[i]]++;
		}

		sum=0.0f;
		fnum=(float)node->num;
		for(j=0;j<MAX_ATTR_VAL;j++){
			psum=0.0f;
			anum=(float)attnum[i][j];
			for(k=0;k<MAX_INFO_VAL;k++){
				n=attnumval[i][j][k];
				if(n!=0&&n!=attnum[i][j]){
					p=(float)n/anum;
					p=p*(log(p)/log(2.0f));
					psum-=p;
				}
			}
			sum+=psum*anum/fnum;
		}
		dest[i]=sum;
	}
}

void calcinfo(value* val, dicisiontree* tree, float* info){
	int i;
	int snum=0;
	int nnum;
	treenode* node=&tree->node[tree->num];

	int rest=MAX_ATTR_NUM%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcinfostruct structs[GEM5_NUMPROCS];

	if(MAX_ATTR_NUM<GEM5_NUMPROCS){
		for(i=0;i<MAX_ATTR_NUM;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			structs[i].nnum=1;
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum++;
		}
		for(i=0;i<MAX_ATTR_NUM;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			if(rest==0){
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum+=structs[i].nnum;
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}

void compareinfo(value* val, dicisiontree* tree, float* info){
	treenode* node=&tree->node[tree->num];
	int i;
	int sel;
	float self=0xFFFFFFFF;

	for(i=0;i<MAX_ATTR_NUM;i++){
		if(node->flag[i]==0){
			if(self>info[i]){
				sel=i;
				self=info[i];
			}
		}
	}
	node->attnum=sel;
	node->flag[sel]=1;
}

void dividesection(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	value vallist[TRAIN_N][MAX_ATTR_VAL];
	int i, j, k;

	for(i=node->startnum;i<node->startnum+node->num;i++){
		j=val[i].attr[node->attnum];
		vallist[node->listcount[j]][j]=val[i];
		node->listcount[j]++;
	}
	i=node->startnum;
	while(i<node->startnum+node->num){
		for(j=0;j<MAX_ATTR_VAL;j++){
			for(k=0;k<node->listcount[j];k++){
				val[i]=vallist[k][j];
				i++;
			}
		}
	}
}

void makesubtree(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	treenode* nextnode;
	int i;
	int num=0;
	node->subnum=subnum[node->attnum];
	for(i=0;i<node->subnum;i++){
		nextnode=&tree->node[tree->maxnum];
		memcpy(nextnode, node, sizeof(treenode));
		nextnode->startnum=node->startnum+num;
		nextnode->num=node->listcount[i];
		memset(nextnode->listcount, 0, 4*MAX_ATTR_VAL);
		num+=nextnode->num;
		node->subptr[i]=tree->maxnum;
		tree->maxnum++;
	}
}







void* testfunc(void* thearg){
	teststruct* arg=(teststruct*)thearg;
	int i;
	int res;
	value* vval;
	treenode* node;
	value* val=arg->val;
	int startnum=arg->startnum;
	int num=arg->num;
	dicisiontree* tree=arg->tree;

	for(i=startnum;i<startnum+num;i++){
		vval=&val[i];
		node=&tree->node[0];
		while(node->treeval<0){
			if(node->num==0)
				break;
			node=&tree->node[node->subptr[vval->attr[node->attnum]]];
		}
		vval->res=node->treeval;
	}
}

void test(value* val, dicisiontree* tree){
	int i;

	int rest=TEST_N%(GEM5_NUMPROCS-1);
	int count=0;
	pthread_t thread[GEM5_NUMPROCS];
	teststruct structs[GEM5_NUMPROCS];
	if(TEST_N<GEM5_NUMPROCS){
		for(i=0;i<TEST_N;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			structs[i].num=1;
			structs[i].tree=tree;
			count++;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<TEST_N;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			if(rest==0){
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			structs[i].tree=tree;
			count+=structs[i].num;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}









int main(){
	value val[TRAIN_N];
	FILE* ftree=fopen("tree", "rb");
	FILE* fval=fopen("val", "rb");
	dicisiontree tree;
	
	readtree(&tree, ftree);
	readvalb(val, fval, TRAIN_N);

	checkleafnode(val, &tree);
	
	fclose(ftree);
	fclose(fval);
	ftree=fopen("tree", "wb");
	savetree(&tree, ftree);
	fclose(ftree);
	return 0;
}
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 8500
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4


typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;

typedef struct treenode{
	float info;
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;

typedef struct calcinfostruct{
	float* info;
	value* val;
	treenode* node;
	int snum;
	int nnum;
}calcinfostruct;

typedef struct checkleafnodestruct{
	int* res;
	value* val;
	int firstnum;
	int startnum;
	int num;
}checkleafnodestruct;

typedef struct teststruct{
	value* val;
	int startnum;
	int num;
	dicisiontree* tree;
}teststruct;



int subnum[MAX_ATTR_NUM]=ATTR_MAX;


void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}





void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}
















void* checkleafnodefunc(void* thearg){
	checkleafnodestruct* arg=(checkleafnodestruct*) thearg;
	int* res=arg->res;
	value* val=arg->val;
	int firstnum=arg->firstnum;
	int startnum=arg->startnum;
	int num=arg->num;
	int rres=0;
	int i;
	for(i=startnum;i<startnum+num;i++){
		if(rres==0){
			if(val[firstnum].res!=val[i].res){
				rres=1;
				break;
			}
		}
	}

	*res=rres;
}

int checkleafnode(value* val, dicisiontree* tree){
	treenode* node=&tree->node[++tree->num];
	int i;

	int res[GEM5_NUMPROCS];
	int rres=0;

	int rest=node->num%(GEM5_NUMPROCS-1);
	int snum=node->startnum;
	pthread_t thread[GEM5_NUMPROCS];
	checkleafnodestruct structs[GEM5_NUMPROCS];
	
	if(node->num==0)
		return 0;
	else{
		if(node->num<GEM5_NUMPROCS){
			for(i=0;i<node->num;i++){
				structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				structs[i].num=1;
				snum++;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<node->num;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<node->num;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		else{
			for(i=0;i<GEM5_NUMPROCS;i++){structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				if(rest==0){
					structs[i].num=node->num/(GEM5_NUMPROCS-1);
				}
				else{
					structs[i].num=node->num/(GEM5_NUMPROCS-1)+1;
					rest--;
				}
				snum+=structs[i].num;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		if(rres==0)
			node->treeval=val[node->startnum].res;
	}
	return rres;
}

void* calcinfofunc(void* thearg){
	calcinfostruct* arg=(calcinfostruct*)thearg;
	float* dest=arg->info;
	value* val=arg->val;
	treenode* node=arg->node;
	int snum=arg->snum;
	int num=arg->nnum;
	int i, j, k;
	int n;
	int attnum[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	int attnumval[MAX_ATTR_NUM][MAX_ATTR_VAL][MAX_INFO_VAL]={0,};
	float sum, psum, p, fnum, anum;
	value* pval;

	for(i=snum;i<snum+num;i++){

		for(j=node->startnum;j<node->startnum+node->num;j++){
		pval=&val[j];
			attnumval[i][pval->attr[i]][pval->res]++;
			attnum[i][pval->attr[i]]++;
		}

		sum=0.0f;
		fnum=(float)node->num;
		for(j=0;j<MAX_ATTR_VAL;j++){
			psum=0.0f;
			anum=(float)attnum[i][j];
			for(k=0;k<MAX_INFO_VAL;k++){
				n=attnumval[i][j][k];
				if(n!=0&&n!=attnum[i][j]){
					p=(float)n/anum;
					p=p*(log(p)/log(2.0f));
					psum-=p;
				}
			}
			sum+=psum*anum/fnum;
		}
		dest[i]=sum;
	}
}

void calcinfo(value* val, dicisiontree* tree, float* info){
	int i;
	int snum=0;
	int nnum;
	treenode* node=&tree->node[tree->num];

	int rest=MAX_ATTR_NUM%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcinfostruct structs[GEM5_NUMPROCS];

	if(MAX_ATTR_NUM<GEM5_NUMPROCS){
		for(i=0;i<MAX_ATTR_NUM;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			structs[i].nnum=1;
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum++;
		}
		for(i=0;i<MAX_ATTR_NUM;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			if(rest==0){
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum+=structs[i].nnum;
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}

void compareinfo(value* val, dicisiontree* tree, float* info){
	treenode* node=&tree->node[tree->num];
	int i;
	int sel;
	float self=0xFFFFFFFF;

	for(i=0;i<MAX_ATTR_NUM;i++){
		if(node->flag[i]==0){
			if(self>info[i]){
				sel=i;
				self=info[i];
			}
		}
	}
	node->attnum=sel;
	node->flag[sel]=1;
}

void dividesection(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	value vallist[TRAIN_N][MAX_ATTR_VAL];
	int i, j, k;

	for(i=node->startnum;i<node->startnum+node->num;i++){
		j=val[i].attr[node->attnum];
		vallist[node->listcount[j]][j]=val[i];
		node->listcount[j]++;
	}
	i=node->startnum;
	while(i<node->startnum+node->num){
		for(j=0;j<MAX_ATTR_VAL;j++){
			for(k=0;k<node->listcount[j];k++){
				val[i]=vallist[k][j];
				i++;
			}
		}
	}
}

void makesubtree(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	treenode* nextnode;
	int i;
	int num=0;
	node->subnum=subnum[node->attnum];
	for(i=0;i<node->subnum;i++){
		nextnode=&tree->node[tree->maxnum];
		memcpy(nextnode, node, sizeof(treenode));
		nextnode->startnum=node->startnum+num;
		nextnode->num=node->listcount[i];
		memset(nextnode->listcount, 0, 4*MAX_ATTR_VAL);
		num+=nextnode->num;
		node->subptr[i]=tree->maxnum;
		tree->maxnum++;
	}
}







void* testfunc(void* thearg){
	teststruct* arg=(teststruct*)thearg;
	int i;
	int res;
	value* vval;
	treenode* node;
	value* val=arg->val;
	int startnum=arg->startnum;
	int num=arg->num;
	dicisiontree* tree=arg->tree;

	for(i=startnum;i<startnum+num;i++){
		vval=&val[i];
		node=&tree->node[0];
		while(node->treeval<0){
			if(node->num==0)
				break;
			node=&tree->node[node->subptr[vval->attr[node->attnum]]];
		}
		vval->res=node->treeval;
	}
}

void test(value* val, dicisiontree* tree){
	int i;

	int rest=TEST_N%(GEM5_NUMPROCS-1);
	int count=0;
	pthread_t thread[GEM5_NUMPROCS];
	teststruct structs[GEM5_NUMPROCS];
	if(TEST_N<GEM5_NUMPROCS){
		for(i=0;i<TEST_N;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			structs[i].num=1;
			structs[i].tree=tree;
			count++;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<TEST_N;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			if(rest==0){
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			structs[i].tree=tree;
			count+=structs[i].num;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}









int main(){
	value val[TRAIN_N];
	FILE* ftree=fopen("tree", "rb");
	FILE* fval=fopen("val", "rb");
	FILE* finfo=fopen("info", "rb");
	dicisiontree tree;
	float info[MAX_ATTR_NUM]={0,};
	
	readtree(&tree, ftree);
	readvalb(val, fval, TRAIN_N);
	readinfob(info, finfo, MAX_ATTR_NUM);
	
	compareinfo(val, &tree, info);
	
	fclose(ftree);
	fclose(fval);
	fclose(finfo);
	ftree=fopen("tree", "wb");
	savetree(&tree, ftree);
	fclose(ftree);
	return 0;
}
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 8500
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4


typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;

typedef struct treenode{
	float info;
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;

typedef struct calcinfostruct{
	float* info;
	value* val;
	treenode* node;
	int snum;
	int nnum;
}calcinfostruct;

typedef struct checkleafnodestruct{
	int* res;
	value* val;
	int firstnum;
	int startnum;
	int num;
}checkleafnodestruct;

typedef struct teststruct{
	value* val;
	int startnum;
	int num;
	dicisiontree* tree;
}teststruct;



int subnum[MAX_ATTR_NUM]=ATTR_MAX;


void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}





void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}
















void* checkleafnodefunc(void* thearg){
	checkleafnodestruct* arg=(checkleafnodestruct*) thearg;
	int* res=arg->res;
	value* val=arg->val;
	int firstnum=arg->firstnum;
	int startnum=arg->startnum;
	int num=arg->num;
	int rres=0;
	int i;
	for(i=startnum;i<startnum+num;i++){
		if(rres==0){
			if(val[firstnum].res!=val[i].res){
				rres=1;
				break;
			}
		}
	}

	*res=rres;
}

int checkleafnode(value* val, dicisiontree* tree){
	treenode* node=&tree->node[++tree->num];
	int i;

	int res[GEM5_NUMPROCS];
	int rres=0;

	int rest=node->num%(GEM5_NUMPROCS-1);
	int snum=node->startnum;
	pthread_t thread[GEM5_NUMPROCS];
	checkleafnodestruct structs[GEM5_NUMPROCS];
	
	if(node->num==0)
		return 0;
	else{
		if(node->num<GEM5_NUMPROCS){
			for(i=0;i<node->num;i++){
				structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				structs[i].num=1;
				snum++;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<node->num;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<node->num;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		else{
			for(i=0;i<GEM5_NUMPROCS;i++){structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				if(rest==0){
					structs[i].num=node->num/(GEM5_NUMPROCS-1);
				}
				else{
					structs[i].num=node->num/(GEM5_NUMPROCS-1)+1;
					rest--;
				}
				snum+=structs[i].num;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		if(rres==0)
			node->treeval=val[node->startnum].res;
	}
	return rres;
}

void* calcinfofunc(void* thearg){
	calcinfostruct* arg=(calcinfostruct*)thearg;
	float* dest=arg->info;
	value* val=arg->val;
	treenode* node=arg->node;
	int snum=arg->snum;
	int num=arg->nnum;
	int i, j, k;
	int n;
	int attnum[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	int attnumval[MAX_ATTR_NUM][MAX_ATTR_VAL][MAX_INFO_VAL]={0,};
	float sum, psum, p, fnum, anum;
	value* pval;

	for(i=snum;i<snum+num;i++){

		for(j=node->startnum;j<node->startnum+node->num;j++){
		pval=&val[j];
			attnumval[i][pval->attr[i]][pval->res]++;
			attnum[i][pval->attr[i]]++;
		}

		sum=0.0f;
		fnum=(float)node->num;
		for(j=0;j<MAX_ATTR_VAL;j++){
			psum=0.0f;
			anum=(float)attnum[i][j];
			for(k=0;k<MAX_INFO_VAL;k++){
				n=attnumval[i][j][k];
				if(n!=0&&n!=attnum[i][j]){
					p=(float)n/anum;
					p=p*(log(p)/log(2.0f));
					psum-=p;
				}
			}
			sum+=psum*anum/fnum;
		}
		dest[i]=sum;
	}
}

void calcinfo(value* val, dicisiontree* tree, float* info){
	int i;
	int snum=0;
	int nnum;
	treenode* node=&tree->node[tree->num];

	int rest=MAX_ATTR_NUM%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcinfostruct structs[GEM5_NUMPROCS];

	if(MAX_ATTR_NUM<GEM5_NUMPROCS){
		for(i=0;i<MAX_ATTR_NUM;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			structs[i].nnum=1;
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum++;
		}
		for(i=0;i<MAX_ATTR_NUM;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			if(rest==0){
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum+=structs[i].nnum;
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}

void compareinfo(value* val, dicisiontree* tree, float* info){
	treenode* node=&tree->node[tree->num];
	int i;
	int sel;
	float self=0xFFFFFFFF;

	for(i=0;i<MAX_ATTR_NUM;i++){
		if(node->flag[i]==0){
			if(self>info[i]){
				sel=i;
				self=info[i];
			}
		}
	}
	node->attnum=sel;
	node->flag[sel]=1;
}

void dividesection(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	value vallist[TRAIN_N][MAX_ATTR_VAL];
	int i, j, k;

	for(i=node->startnum;i<node->startnum+node->num;i++){
		j=val[i].attr[node->attnum];
		vallist[node->listcount[j]][j]=val[i];
		node->listcount[j]++;
	}
	i=node->startnum;
	while(i<node->startnum+node->num){
		for(j=0;j<MAX_ATTR_VAL;j++){
			for(k=0;k<node->listcount[j];k++){
				val[i]=vallist[k][j];
				i++;
			}
		}
	}
}

void makesubtree(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	treenode* nextnode;
	int i;
	int num=0;
	node->subnum=subnum[node->attnum];
	for(i=0;i<node->subnum;i++){
		nextnode=&tree->node[tree->maxnum];
		memcpy(nextnode, node, sizeof(treenode));
		nextnode->startnum=node->startnum+num;
		nextnode->num=node->listcount[i];
		memset(nextnode->listcount, 0, 4*MAX_ATTR_VAL);
		num+=nextnode->num;
		node->subptr[i]=tree->maxnum;
		tree->maxnum++;
	}
}







void* testfunc(void* thearg){
	teststruct* arg=(teststruct*)thearg;
	int i;
	int res;
	value* vval;
	treenode* node;
	value* val=arg->val;
	int startnum=arg->startnum;
	int num=arg->num;
	dicisiontree* tree=arg->tree;

	for(i=startnum;i<startnum+num;i++){
		vval=&val[i];
		node=&tree->node[0];
		while(node->treeval<0){
			if(node->num==0)
				break;
			node=&tree->node[node->subptr[vval->attr[node->attnum]]];
		}
		vval->res=node->treeval;
	}
}

void test(value* val, dicisiontree* tree){
	int i;

	int rest=TEST_N%(GEM5_NUMPROCS-1);
	int count=0;
	pthread_t thread[GEM5_NUMPROCS];
	teststruct structs[GEM5_NUMPROCS];
	if(TEST_N<GEM5_NUMPROCS){
		for(i=0;i<TEST_N;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			structs[i].num=1;
			structs[i].tree=tree;
			count++;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<TEST_N;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			if(rest==0){
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			structs[i].tree=tree;
			count+=structs[i].num;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}









int main(){
	value val[TRAIN_N];
	FILE* ftree=fopen("tree", "rb");
	FILE* fval=fopen("val", "rb");
	dicisiontree tree;
	
	readtree(&tree, ftree);
	readvalb(val, fval, TRAIN_N);
	dividesection(val, &tree);
	
	fclose(ftree);
	fclose(fval);
	fval=fopen("val", "wb");
	savevalb(val, fval, TRAIN_N);
	fclose(fval);
	ftree=fopen("tree", "wb");
	savetree(&tree, ftree);
	fclose(ftree);
	return 0;
}
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 8500
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4


typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;

typedef struct treenode{
	float info;
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;

typedef struct calcinfostruct{
	float* info;
	value* val;
	treenode* node;
	int snum;
	int nnum;
	float (*subptr)[MAX_ATTR_VAL];
}calcinfostruct;

typedef struct checkleafnodestruct{
	int* res;
	value* val;
	int firstnum;
	int startnum;
	int num;
}checkleafnodestruct;

typedef struct teststruct{
	value* val;
	int startnum;
	int num;
	dicisiontree* tree;
}teststruct;



int subnum[MAX_ATTR_NUM]=ATTR_MAX;
void readsubinfo(float dest[][MAX_ATTR_VAL], FILE* fp){
	fread(dest, MAX_ATTR_VAL*MAX_ATTR_NUM, sizeof(float), fp);
}

void savesubinfo(float dest[][MAX_ATTR_VAL], FILE* fp){
	fwrite(dest, MAX_ATTR_VAL*MAX_ATTR_NUM, sizeof(float), fp);
}

void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}





void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}
















void* checkleafnodefunc(void* thearg){
	checkleafnodestruct* arg=(checkleafnodestruct*) thearg;
	int* res=arg->res;
	value* val=arg->val;
	int firstnum=arg->firstnum;
	int startnum=arg->startnum;
	int num=arg->num;
	int rres=0;
	int i;
	for(i=startnum;i<startnum+num;i++){
		if(rres==0){
			if(val[firstnum].res!=val[i].res){
				rres=1;
				break;
			}
		}
	}

	*res=rres;
}

int checkleafnode(value* val, dicisiontree* tree){
	treenode* node=&tree->node[++tree->num];
	int i;

	int res[GEM5_NUMPROCS];
	int rres=0;

	int rest=node->num%(GEM5_NUMPROCS-1);
	int snum=node->startnum;
	pthread_t thread[GEM5_NUMPROCS];
	checkleafnodestruct structs[GEM5_NUMPROCS];
	
	if(node->num==0)
		return 0;
	else{
		if(node->num<GEM5_NUMPROCS){
			for(i=0;i<node->num;i++){
				structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				structs[i].num=1;
				snum++;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<node->num;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<node->num;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		else{
			for(i=0;i<GEM5_NUMPROCS;i++){structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				if(rest==0){
					structs[i].num=node->num/(GEM5_NUMPROCS-1);
				}
				else{
					structs[i].num=node->num/(GEM5_NUMPROCS-1)+1;
					rest--;
				}
				snum+=structs[i].num;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		if(rres==0)
			node->treeval=val[node->startnum].res;
	}
	return rres;
}

void* calcinfofunc(void* thearg){
	calcinfostruct* arg=(calcinfostruct*)thearg;
	float* dest=arg->info;
	value* val=arg->val;
	treenode* node=arg->node;
	int snum=arg->snum;
	int num=arg->nnum;
	int i, j, k;
	int n;
	int attnum[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	int attnumval[MAX_ATTR_NUM][MAX_ATTR_VAL][MAX_INFO_VAL]={0,};
	float sum, psum, p, fnum, anum, tp;
	value* pval;
	float (*subptr)[MAX_ATTR_VAL]=arg->subptr;

	for(i=snum;i<snum+num;i++){

		for(j=node->startnum;j<node->startnum+node->num;j++){
		pval=&val[j];
			attnumval[i][pval->attr[i]][pval->res]++;
			attnum[i][pval->attr[i]]++;
		}

		sum=0.0f;
		fnum=(float)node->num;
		for(j=0;j<MAX_ATTR_VAL;j++){
			psum=0.0f;
			anum=(float)attnum[i][j];
			for(k=0;k<MAX_INFO_VAL;k++){
				n=attnumval[i][j][k];
				if(n!=0&&n!=attnum[i][j]){
					p=(float)n/anum;
					tp=(log(p)/log(2.0f));
					p=p*tp;
					psum-=p;
					subptr[i][j]+=tp;
				}
			}
			sum+=psum*anum/fnum;
		}
		dest[i]=sum;
	}
}

void calcinfo(float subinfo[][MAX_ATTR_VAL], value* val, dicisiontree* tree, float* info){
	int i;
	int snum=0;
	int nnum;
	treenode* node=&tree->node[tree->num];

	int rest=MAX_ATTR_NUM%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcinfostruct structs[GEM5_NUMPROCS];

	
	if(MAX_ATTR_NUM<GEM5_NUMPROCS){
			for(i=0;i<MAX_ATTR_NUM;i++){
				structs[i].info=info;
				structs[i].val=val;
				structs[i].node=node;
				structs[i].snum=snum;
				structs[i].nnum=1;
				structs[i].subptr=subinfo;
				pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
				snum++;
			}
			for(i=0;i<MAX_ATTR_NUM;i++){
				pthread_join(thread[i], NULL);
			}
		}
		else{
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				structs[i].info=info;
				structs[i].val=val;
				structs[i].node=node;
				structs[i].snum=snum;
				structs[i].subptr=subinfo;
				if(rest==0){
					structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1);
				}
				else{
					structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1)+1;
					rest--;
				}
				pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
				snum+=structs[i].nnum;
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				pthread_join(thread[i], NULL);
			}
		}
}

void compareinfo(value* val, dicisiontree* tree, float* info){
	treenode* node=&tree->node[tree->num];
	int i;
	int sel;
	float self=0xFFFFFFFF;

	for(i=0;i<MAX_ATTR_NUM;i++){
		if(node->flag[i]==0){
			if(self>info[i]){
				sel=i;
				self=info[i];
			}
		}
	}
	node->attnum=sel;
	node->flag[sel]=1;
}

void dividesection(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	value vallist[TRAIN_N][MAX_ATTR_VAL];
	int i, j, k;

	for(i=node->startnum;i<node->startnum+node->num;i++){
		j=val[i].attr[node->attnum];
		vallist[node->listcount[j]][j]=val[i];
		node->listcount[j]++;
	}
	i=node->startnum;
	while(i<node->startnum+node->num){
		for(j=0;j<MAX_ATTR_VAL;j++){
			for(k=0;k<node->listcount[j];k++){
				val[i]=vallist[k][j];
				i++;
			}
		}
	}
}

void makesubtree(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	treenode* nextnode;
	int i;
	int num=0;
	node->subnum=subnum[node->attnum];
	for(i=0;i<node->subnum;i++){
		nextnode=&tree->node[tree->maxnum];
		memcpy(nextnode, node, sizeof(treenode));
		nextnode->startnum=node->startnum+num;
		nextnode->num=node->listcount[i];
		memset(nextnode->listcount, 0, 4*MAX_ATTR_VAL);
		num+=nextnode->num;
		node->subptr[i]=tree->maxnum;
		tree->maxnum++;
	}
}







void* testfunc(void* thearg){
	teststruct* arg=(teststruct*)thearg;
	int i;
	int res;
	value* vval;
	treenode* node;
	value* val=arg->val;
	int startnum=arg->startnum;
	int num=arg->num;
	dicisiontree* tree=arg->tree;

	for(i=startnum;i<startnum+num;i++){
		vval=&val[i];
		node=&tree->node[0];
		while(node->treeval<0){
			if(node->num==0)
				break;
			node=&tree->node[node->subptr[vval->attr[node->attnum]]];
		}
		vval->res=node->treeval;
	}
}

void test(value* val, dicisiontree* tree){
	int i;

	int rest=TEST_N%(GEM5_NUMPROCS-1);
	int count=0;
	pthread_t thread[GEM5_NUMPROCS];
	teststruct structs[GEM5_NUMPROCS];
	if(TEST_N<GEM5_NUMPROCS){
		for(i=0;i<TEST_N;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			structs[i].num=1;
			structs[i].tree=tree;
			count++;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<TEST_N;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			if(rest==0){
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			structs[i].tree=tree;
			count+=structs[i].num;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}









int main(){
	value val[TRAIN_N];
	FILE* ftree=fopen("tree", "rb");
	FILE* fval=fopen("val", "rb");
	FILE* finfo=fopen("info", "wb");
	FILE* fsubinfo=fopen("fsubinfo", "wb");
	dicisiontree tree;
	float info[MAX_ATTR_NUM]={0,};
	float subinfo[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	
	readtree(&tree, ftree);
	readvalb(val, fval, TRAIN_N);
	calcinfo(subinfo, val, &tree, info);
	
	saveinfob(info, finfo, MAX_ATTR_NUM);
	
	savesubinfo(subinfo, fsubinfo);
	
	fclose(fsubinfo);
	fclose(ftree);
	fclose(fval);
	fclose(finfo);
	return 0;
}
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 8500
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4


typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;

typedef struct treenode{
	float info;
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;

typedef struct calcinfostruct{
	float* info;
	value* val;
	treenode* node;
	int snum;
	int nnum;
}calcinfostruct;


typedef struct teststruct{
	value* val;
	int startnum;
	int num;
	dicisiontree* tree;
}teststruct;



int subnum[MAX_ATTR_NUM]=ATTR_MAX;


void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}





void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}















int checkleafnode(value* val, dicisiontree* tree){
	treenode* node=&tree->node[++tree->num];

	if(node->num==0)
		return 0;
	else if(node->info==0.0f){
		node->treeval=val[node->startnum].res;
		return 0;
	}
	else return 1;
}

void* calcinfofunc(void* thearg){
	calcinfostruct* arg=(calcinfostruct*)thearg;
	float* dest=arg->info;
	value* val=arg->val;
	treenode* node=arg->node;
	int snum=arg->snum;
	int num=arg->nnum;
	int i, j, k;
	int n;
	int attnum[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	int attnumval[MAX_ATTR_NUM][MAX_ATTR_VAL][MAX_INFO_VAL]={0,};
	float sum, psum, p, fnum, anum;
	value* pval;

	for(i=snum;i<snum+num;i++){

		for(j=node->startnum;j<node->startnum+node->num;j++){
		pval=&val[j];
			attnumval[i][pval->attr[i]][pval->res]++;
			attnum[i][pval->attr[i]]++;
		}

		sum=0.0f;
		fnum=(float)node->num;
		for(j=0;j<MAX_ATTR_VAL;j++){
			psum=0.0f;
			anum=(float)attnum[i][j];
			for(k=0;k<MAX_INFO_VAL;k++){
				n=attnumval[i][j][k];
				if(n!=0&&n!=attnum[i][j]){
					p=(float)n/anum;
					p=p*(log(p)/log(2.0f));
					psum-=p;
				}
			}
			sum+=psum*anum/fnum;
		}
		dest[i]=sum;
	}
}

void calcinfo(value* val, dicisiontree* tree, float* info){
	int i;
	int snum=0;
	int nnum;
	treenode* node=&tree->node[tree->num];

	int rest=MAX_ATTR_NUM%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcinfostruct structs[GEM5_NUMPROCS];

	if(MAX_ATTR_NUM<GEM5_NUMPROCS){
		for(i=0;i<MAX_ATTR_NUM;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			structs[i].nnum=1;
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum++;
		}
		for(i=0;i<MAX_ATTR_NUM;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			if(rest==0){
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum+=structs[i].nnum;
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}

void compareinfo(value* val, dicisiontree* tree, float* info){
	treenode* node=&tree->node[tree->num];
	int i;
	int sel;
	float self=0xFFFFFFFF;

	for(i=0;i<MAX_ATTR_NUM;i++){
		if(node->flag[i]==0){
			if(self>info[i]){
				sel=i;
				self=info[i];
			}
		}
	}
	node->attnum=sel;
	node->flag[sel]=1;
}

void dividesection(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	value vallist[TRAIN_N][MAX_ATTR_VAL];
	int i, j, k;

	for(i=node->startnum;i<node->startnum+node->num;i++){
		j=val[i].attr[node->attnum];
		vallist[node->listcount[j]][j]=val[i];
		node->listcount[j]++;
	}
	i=node->startnum;
	while(i<node->startnum+node->num){
		for(j=0;j<MAX_ATTR_VAL;j++){
			for(k=0;k<node->listcount[j];k++){
				val[i]=vallist[k][j];
				i++;
			}
		}
	}
}

void makesubtree(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	treenode* nextnode;
	int i;
	int num=0;
	node->subnum=subnum[node->attnum];
	for(i=0;i<node->subnum;i++){
		nextnode=&tree->node[tree->maxnum];
		memcpy(nextnode, node, sizeof(treenode));
		nextnode->startnum=node->startnum+num;
		nextnode->num=node->listcount[i];
		memset(nextnode->listcount, 0, 4*MAX_ATTR_VAL);
		num+=nextnode->num;
		node->subptr[i]=tree->maxnum;
		tree->maxnum++;
	}
}







void* testfunc(void* thearg){
	teststruct* arg=(teststruct*)thearg;
	int i;
	int res;
	value* vval;
	treenode* node;
	value* val=arg->val;
	int startnum=arg->startnum;
	int num=arg->num;
	dicisiontree* tree=arg->tree;

	for(i=startnum;i<startnum+num;i++){
		vval=&val[i];
		node=&tree->node[0];
		while(node->treeval<0){
			if(node->num==0)
				break;
			node=&tree->node[node->subptr[vval->attr[node->attnum]]];
		}
		vval->res=node->treeval;
	}
}

void test(value* val, dicisiontree* tree){
	int i;

	int rest=TEST_N%(GEM5_NUMPROCS-1);
	int count=0;
	pthread_t thread[GEM5_NUMPROCS];
	teststruct structs[GEM5_NUMPROCS];
	if(TEST_N<GEM5_NUMPROCS){
		for(i=0;i<TEST_N;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			structs[i].num=1;
			structs[i].tree=tree;
			count++;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<TEST_N;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			if(rest==0){
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			structs[i].tree=tree;
			count+=structs[i].num;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}









int main(){
	value val[TRAIN_N];
	FILE* ftree=fopen("tree", "rb");
	FILE* fval=fopen("val", "rb");
	dicisiontree tree;
	
	readtree(&tree, ftree);
	readvalb(val, fval, TRAIN_N);

	checkleafnode(val, &tree);
	
	fclose(ftree);
	fclose(fval);
	ftree=fopen("tree", "wb");
	savetree(&tree, ftree);
	fclose(ftree);
	return 0;
}
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 8500
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4


typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;

typedef struct treenode{
	float info;
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;

typedef struct calcinfostruct{
	float* info;
	value* val;
	treenode* node;
	int snum;
	int nnum;
}calcinfostruct;

typedef struct checkleafnodestruct{
	int* res;
	value* val;
	int firstnum;
	int startnum;
	int num;
}checkleafnodestruct;

typedef struct teststruct{
	value* val;
	int startnum;
	int num;
	dicisiontree* tree;
}teststruct;



int subnum[MAX_ATTR_NUM]=ATTR_MAX;


void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}





void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}
















void* checkleafnodefunc(void* thearg){
	checkleafnodestruct* arg=(checkleafnodestruct*) thearg;
	int* res=arg->res;
	value* val=arg->val;
	int firstnum=arg->firstnum;
	int startnum=arg->startnum;
	int num=arg->num;
	int rres=0;
	int i;
	for(i=startnum;i<startnum+num;i++){
		if(rres==0){
			if(val[firstnum].res!=val[i].res){
				rres=1;
				break;
			}
		}
	}

	*res=rres;
}

int checkleafnode(value* val, dicisiontree* tree){
	treenode* node=&tree->node[++tree->num];
	int i;

	int res[GEM5_NUMPROCS];
	int rres=0;

	int rest=node->num%(GEM5_NUMPROCS-1);
	int snum=node->startnum;
	pthread_t thread[GEM5_NUMPROCS];
	checkleafnodestruct structs[GEM5_NUMPROCS];
	
	if(node->num==0)
		return 0;
	else{
		if(node->num<GEM5_NUMPROCS){
			for(i=0;i<node->num;i++){
				structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				structs[i].num=1;
				snum++;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<node->num;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<node->num;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		else{
			for(i=0;i<GEM5_NUMPROCS;i++){structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				if(rest==0){
					structs[i].num=node->num/(GEM5_NUMPROCS-1);
				}
				else{
					structs[i].num=node->num/(GEM5_NUMPROCS-1)+1;
					rest--;
				}
				snum+=structs[i].num;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		if(rres==0)
			node->treeval=val[node->startnum].res;
	}
	return rres;
}

void* calcinfofunc(void* thearg){
	calcinfostruct* arg=(calcinfostruct*)thearg;
	float* dest=arg->info;
	value* val=arg->val;
	treenode* node=arg->node;
	int snum=arg->snum;
	int num=arg->nnum;
	int i, j, k;
	int n;
	int attnum[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	int attnumval[MAX_ATTR_NUM][MAX_ATTR_VAL][MAX_INFO_VAL]={0,};
	float sum, psum, p, fnum, anum;
	value* pval;

	for(i=snum;i<snum+num;i++){

		for(j=node->startnum;j<node->startnum+node->num;j++){
		pval=&val[j];
			attnumval[i][pval->attr[i]][pval->res]++;
			attnum[i][pval->attr[i]]++;
		}

		sum=0.0f;
		fnum=(float)node->num;
		for(j=0;j<MAX_ATTR_VAL;j++){
			psum=0.0f;
			anum=(float)attnum[i][j];
			for(k=0;k<MAX_INFO_VAL;k++){
				n=attnumval[i][j][k];
				if(n!=0&&n!=attnum[i][j]){
					p=(float)n/anum;
					p=p*(log(p)/log(2.0f));
					psum-=p;
				}
			}
			sum+=psum*anum/fnum;
		}
		dest[i]=sum;
	}
}

void calcinfo(value* val, dicisiontree* tree, float* info){
	int i;
	int snum=0;
	int nnum;
	treenode* node=&tree->node[tree->num];

	int rest=MAX_ATTR_NUM%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcinfostruct structs[GEM5_NUMPROCS];

	if(MAX_ATTR_NUM<GEM5_NUMPROCS){
		for(i=0;i<MAX_ATTR_NUM;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			structs[i].nnum=1;
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum++;
		}
		for(i=0;i<MAX_ATTR_NUM;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			if(rest==0){
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum+=structs[i].nnum;
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}

void compareinfo(value* val, dicisiontree* tree, float* info){
	treenode* node=&tree->node[tree->num];
	int i;
	int sel;
	float self=0xFFFFFFFF;

	for(i=0;i<MAX_ATTR_NUM;i++){
		if(node->flag[i]==0){
			if(self>info[i]){
				sel=i;
				self=info[i];
			}
		}
	}
	node->attnum=sel;
	node->flag[sel]=1;
}

void dividesection(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	value vallist[TRAIN_N][MAX_ATTR_VAL];
	int i, j, k;

	for(i=node->startnum;i<node->startnum+node->num;i++){
		j=val[i].attr[node->attnum];
		vallist[node->listcount[j]][j]=val[i];
		node->listcount[j]++;
	}
	i=node->startnum;
	while(i<node->startnum+node->num){
		for(j=0;j<MAX_ATTR_VAL;j++){
			for(k=0;k<node->listcount[j];k++){
				val[i]=vallist[k][j];
				i++;
			}
		}
	}
}

void makesubtree(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	treenode* nextnode;
	int i;
	int num=0;
	node->subnum=subnum[node->attnum];
	for(i=0;i<node->subnum;i++){
		nextnode=&tree->node[tree->maxnum];
		memcpy(nextnode, node, sizeof(treenode));
		nextnode->startnum=node->startnum+num;
		nextnode->num=node->listcount[i];
		memset(nextnode->listcount, 0, 4*MAX_ATTR_VAL);
		num+=nextnode->num;
		node->subptr[i]=tree->maxnum;
		tree->maxnum++;
	}
}







void* testfunc(void* thearg){
	teststruct* arg=(teststruct*)thearg;
	int i;
	int res;
	value* vval;
	treenode* node;
	value* val=arg->val;
	int startnum=arg->startnum;
	int num=arg->num;
	dicisiontree* tree=arg->tree;

	for(i=startnum;i<startnum+num;i++){
		vval=&val[i];
		node=&tree->node[0];
		while(node->treeval<0){
			if(node->num==0)
				break;
			node=&tree->node[node->subptr[vval->attr[node->attnum]]];
		}
		vval->res=node->treeval;
	}
}

void test(value* val, dicisiontree* tree){
	int i;

	int rest=TEST_N%(GEM5_NUMPROCS-1);
	int count=0;
	pthread_t thread[GEM5_NUMPROCS];
	teststruct structs[GEM5_NUMPROCS];
	if(TEST_N<GEM5_NUMPROCS){
		for(i=0;i<TEST_N;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			structs[i].num=1;
			structs[i].tree=tree;
			count++;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<TEST_N;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			if(rest==0){
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			structs[i].tree=tree;
			count+=structs[i].num;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}









int main(){
	value val[TRAIN_N];
	FILE* ftree=fopen("tree", "rb");
	FILE* fval=fopen("val", "rb");
	FILE* finfo=fopen("info", "rb");
	dicisiontree tree;
	float info[MAX_ATTR_NUM]={0,};
	
	readtree(&tree, ftree);
	readvalb(val, fval, TRAIN_N);
	readinfob(info, finfo, MAX_ATTR_NUM);
	
	compareinfo(val, &tree, info);
	
	fclose(ftree);
	fclose(fval);
	fclose(finfo);
	ftree=fopen("tree", "wb");
	savetree(&tree, ftree);
	fclose(ftree);
	return 0;
}
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 8500
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4


typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;

typedef struct treenode{
	float info;
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;

typedef struct calcinfostruct{
	float* info;
	value* val;
	treenode* node;
	int snum;
	int nnum;
}calcinfostruct;

typedef struct checkleafnodestruct{
	int* res;
	value* val;
	int firstnum;
	int startnum;
	int num;
}checkleafnodestruct;

typedef struct teststruct{
	value* val;
	int startnum;
	int num;
	dicisiontree* tree;
}teststruct;



int subnum[MAX_ATTR_NUM]=ATTR_MAX;


void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}





void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}
















void* checkleafnodefunc(void* thearg){
	checkleafnodestruct* arg=(checkleafnodestruct*) thearg;
	int* res=arg->res;
	value* val=arg->val;
	int firstnum=arg->firstnum;
	int startnum=arg->startnum;
	int num=arg->num;
	int rres=0;
	int i;
	for(i=startnum;i<startnum+num;i++){
		if(rres==0){
			if(val[firstnum].res!=val[i].res){
				rres=1;
				break;
			}
		}
	}

	*res=rres;
}

int checkleafnode(value* val, dicisiontree* tree){
	treenode* node=&tree->node[++tree->num];
	int i;

	int res[GEM5_NUMPROCS];
	int rres=0;

	int rest=node->num%(GEM5_NUMPROCS-1);
	int snum=node->startnum;
	pthread_t thread[GEM5_NUMPROCS];
	checkleafnodestruct structs[GEM5_NUMPROCS];
	
	if(node->num==0)
		return 0;
	else{
		if(node->num<GEM5_NUMPROCS){
			for(i=0;i<node->num;i++){
				structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				structs[i].num=1;
				snum++;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<node->num;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<node->num;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		else{
			for(i=0;i<GEM5_NUMPROCS;i++){structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				if(rest==0){
					structs[i].num=node->num/(GEM5_NUMPROCS-1);
				}
				else{
					structs[i].num=node->num/(GEM5_NUMPROCS-1)+1;
					rest--;
				}
				snum+=structs[i].num;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		if(rres==0)
			node->treeval=val[node->startnum].res;
	}
	return rres;
}

void* calcinfofunc(void* thearg){
	calcinfostruct* arg=(calcinfostruct*)thearg;
	float* dest=arg->info;
	value* val=arg->val;
	treenode* node=arg->node;
	int snum=arg->snum;
	int num=arg->nnum;
	int i, j, k;
	int n;
	int attnum[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	int attnumval[MAX_ATTR_NUM][MAX_ATTR_VAL][MAX_INFO_VAL]={0,};
	float sum, psum, p, fnum, anum;
	value* pval;

	for(i=snum;i<snum+num;i++){

		for(j=node->startnum;j<node->startnum+node->num;j++){
		pval=&val[j];
			attnumval[i][pval->attr[i]][pval->res]++;
			attnum[i][pval->attr[i]]++;
		}

		sum=0.0f;
		fnum=(float)node->num;
		for(j=0;j<MAX_ATTR_VAL;j++){
			psum=0.0f;
			anum=(float)attnum[i][j];
			for(k=0;k<MAX_INFO_VAL;k++){
				n=attnumval[i][j][k];
				if(n!=0&&n!=attnum[i][j]){
					p=(float)n/anum;
					p=p*(log(p)/log(2.0f));
					psum-=p;
				}
			}
			sum+=psum*anum/fnum;
		}
		dest[i]=sum;
	}
}

void calcinfo(value* val, dicisiontree* tree, float* info){
	int i;
	int snum=0;
	int nnum;
	treenode* node=&tree->node[tree->num];

	int rest=MAX_ATTR_NUM%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcinfostruct structs[GEM5_NUMPROCS];

	if(MAX_ATTR_NUM<GEM5_NUMPROCS){
		for(i=0;i<MAX_ATTR_NUM;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			structs[i].nnum=1;
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum++;
		}
		for(i=0;i<MAX_ATTR_NUM;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			if(rest==0){
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum+=structs[i].nnum;
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}

void compareinfo(value* val, dicisiontree* tree, float* info){
	treenode* node=&tree->node[tree->num];
	int i;
	int sel;
	float self=0xFFFFFFFF;

	for(i=0;i<MAX_ATTR_NUM;i++){
		if(node->flag[i]==0){
			if(self>info[i]){
				sel=i;
				self=info[i];
			}
		}
	}
	node->attnum=sel;
	node->flag[sel]=1;
}

void dividesection(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	value vallist[TRAIN_N][MAX_ATTR_VAL];
	int i, j, k;

	for(i=node->startnum;i<node->startnum+node->num;i++){
		j=val[i].attr[node->attnum];
		vallist[node->listcount[j]][j]=val[i];
		node->listcount[j]++;
	}
	i=node->startnum;
	while(i<node->startnum+node->num){
		for(j=0;j<MAX_ATTR_VAL;j++){
			for(k=0;k<node->listcount[j];k++){
				val[i]=vallist[k][j];
				i++;
			}
		}
	}
}

void makesubtree(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	treenode* nextnode;
	int i;
	int num=0;
	node->subnum=subnum[node->attnum];
	for(i=0;i<node->subnum;i++){
		nextnode=&tree->node[tree->maxnum];
		memcpy(nextnode, node, sizeof(treenode));
		nextnode->startnum=node->startnum+num;
		nextnode->num=node->listcount[i];
		memset(nextnode->listcount, 0, 4*MAX_ATTR_VAL);
		num+=nextnode->num;
		node->subptr[i]=tree->maxnum;
		tree->maxnum++;
	}
}







void* testfunc(void* thearg){
	teststruct* arg=(teststruct*)thearg;
	int i;
	int res;
	value* vval;
	treenode* node;
	value* val=arg->val;
	int startnum=arg->startnum;
	int num=arg->num;
	dicisiontree* tree=arg->tree;

	for(i=startnum;i<startnum+num;i++){
		vval=&val[i];
		node=&tree->node[0];
		while(node->treeval<0){
			if(node->num==0)
				break;
			node=&tree->node[node->subptr[vval->attr[node->attnum]]];
		}
		vval->res=node->treeval;
	}
}

void test(value* val, dicisiontree* tree){
	int i;

	int rest=TEST_N%(GEM5_NUMPROCS-1);
	int count=0;
	pthread_t thread[GEM5_NUMPROCS];
	teststruct structs[GEM5_NUMPROCS];
	if(TEST_N<GEM5_NUMPROCS){
		for(i=0;i<TEST_N;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			structs[i].num=1;
			structs[i].tree=tree;
			count++;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<TEST_N;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			if(rest==0){
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			structs[i].tree=tree;
			count+=structs[i].num;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}









int main(){
	value val[TRAIN_N];
	FILE* ftree=fopen("tree", "rb");
	FILE* fval=fopen("val", "rb");
	dicisiontree tree;
	
	readtree(&tree, ftree);
	readvalb(val, fval, TRAIN_N);
	dividesection(val, &tree);
	
	fclose(ftree);
	fclose(fval);
	fval=fopen("val", "wb");
	savevalb(val, fval, TRAIN_N);
	fclose(fval);
	ftree=fopen("tree", "wb");
	savetree(&tree, ftree);
	fclose(ftree);
	return 0;
}
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 8500
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4


typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;

typedef struct treenode{
	float info;
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;

typedef struct calcinfostruct{
	float* info;
	value* val;
	treenode* node;
	int snum;
	int nnum;
}calcinfostruct;

typedef struct checkleafnodestruct{
	int* res;
	value* val;
	int firstnum;
	int startnum;
	int num;
}checkleafnodestruct;

typedef struct teststruct{
	value* val;
	int startnum;
	int num;
	dicisiontree* tree;
}teststruct;



int subnum[MAX_ATTR_NUM]=ATTR_MAX;
void readsubinfo(float dest[][MAX_ATTR_VAL], FILE* fp){
	fread(dest, MAX_ATTR_VAL*MAX_ATTR_NUM, sizeof(float), fp);
}

void savesubinfo(float dest[][MAX_ATTR_VAL], FILE* fp){
	fwrite(dest, MAX_ATTR_VAL*MAX_ATTR_NUM, sizeof(float), fp);
}

void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}





void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}
















void* checkleafnodefunc(void* thearg){
	checkleafnodestruct* arg=(checkleafnodestruct*) thearg;
	int* res=arg->res;
	value* val=arg->val;
	int firstnum=arg->firstnum;
	int startnum=arg->startnum;
	int num=arg->num;
	int rres=0;
	int i;
	for(i=startnum;i<startnum+num;i++){
		if(rres==0){
			if(val[firstnum].res!=val[i].res){
				rres=1;
				break;
			}
		}
	}

	*res=rres;
}

int checkleafnode(value* val, dicisiontree* tree){
	treenode* node=&tree->node[++tree->num];
	int i;

	int res[GEM5_NUMPROCS];
	int rres=0;

	int rest=node->num%(GEM5_NUMPROCS-1);
	int snum=node->startnum;
	pthread_t thread[GEM5_NUMPROCS];
	checkleafnodestruct structs[GEM5_NUMPROCS];
	
	if(node->num==0)
		return 0;
	else{
		if(node->num<GEM5_NUMPROCS){
			for(i=0;i<node->num;i++){
				structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				structs[i].num=1;
				snum++;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<node->num;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<node->num;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		else{
			for(i=0;i<GEM5_NUMPROCS;i++){structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				if(rest==0){
					structs[i].num=node->num/(GEM5_NUMPROCS-1);
				}
				else{
					structs[i].num=node->num/(GEM5_NUMPROCS-1)+1;
					rest--;
				}
				snum+=structs[i].num;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		if(rres==0)
			node->treeval=val[node->startnum].res;
	}
	return rres;
}

void* calcinfofunc(void* thearg){
	calcinfostruct* arg=(calcinfostruct*)thearg;
	float* dest=arg->info;
	value* val=arg->val;
	treenode* node=arg->node;
	int snum=arg->snum;
	int num=arg->nnum;
	int i, j, k;
	int n;
	int attnum[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	int attnumval[MAX_ATTR_NUM][MAX_ATTR_VAL][MAX_INFO_VAL]={0,};
	float sum, psum, p, fnum, anum;
	value* pval;

	for(i=snum;i<snum+num;i++){

		for(j=node->startnum;j<node->startnum+node->num;j++){
		pval=&val[j];
			attnumval[i][pval->attr[i]][pval->res]++;
			attnum[i][pval->attr[i]]++;
		}

		sum=0.0f;
		fnum=(float)node->num;
		for(j=0;j<MAX_ATTR_VAL;j++){
			psum=0.0f;
			anum=(float)attnum[i][j];
			for(k=0;k<MAX_INFO_VAL;k++){
				n=attnumval[i][j][k];
				if(n!=0&&n!=attnum[i][j]){
					p=(float)n/anum;
					p=p*(log(p)/log(2.0f));
					psum-=p;
				}
			}
			sum+=psum*anum/fnum;
		}
		dest[i]=sum;
	}
}

void calcinfo(value* val, dicisiontree* tree, float* info){
	int i;
	int snum=0;
	int nnum;
	treenode* node=&tree->node[tree->num];

	int rest=MAX_ATTR_NUM%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcinfostruct structs[GEM5_NUMPROCS];

	if(MAX_ATTR_NUM<GEM5_NUMPROCS){
		for(i=0;i<MAX_ATTR_NUM;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			structs[i].nnum=1;
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum++;
		}
		for(i=0;i<MAX_ATTR_NUM;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			if(rest==0){
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum+=structs[i].nnum;
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}

void compareinfo(value* val, dicisiontree* tree, float* info){
	treenode* node=&tree->node[tree->num];
	int i;
	int sel;
	float self=0xFFFFFFFF;

	for(i=0;i<MAX_ATTR_NUM;i++){
		if(node->flag[i]==0){
			if(self>info[i]){
				sel=i;
				self=info[i];
			}
		}
	}
	node->attnum=sel;
	node->flag[sel]=1;
}

void dividesection(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	value vallist[TRAIN_N][MAX_ATTR_VAL];
	int i, j, k;

	for(i=node->startnum;i<node->startnum+node->num;i++){
		j=val[i].attr[node->attnum];
		vallist[node->listcount[j]][j]=val[i];
		node->listcount[j]++;
	}
	i=node->startnum;
	while(i<node->startnum+node->num){
		for(j=0;j<MAX_ATTR_VAL;j++){
			for(k=0;k<node->listcount[j];k++){
				val[i]=vallist[k][j];
				i++;
			}
		}
	}
}

void makesubtree(float subinfo[][MAX_ATTR_VAL], value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	treenode* nextnode;
	int i;
	int num=0;
	node->subnum=subnum[node->attnum];
	for(i=0;i<node->subnum;i++){
		nextnode=&tree->node[tree->maxnum];
		memcpy(nextnode, node, sizeof(treenode));
		nextnode->startnum=node->startnum+num;
		nextnode->num=node->listcount[i];
		memset(nextnode->listcount, 0, 4*MAX_ATTR_VAL);
		num+=nextnode->num;
		node->subptr[i]=tree->maxnum;
		tree->maxnum++;
		nextnode->info=subinfo[node->attnum][i];
	}
}







void* testfunc(void* thearg){
	teststruct* arg=(teststruct*)thearg;
	int i;
	int res;
	value* vval;
	treenode* node;
	value* val=arg->val;
	int startnum=arg->startnum;
	int num=arg->num;
	dicisiontree* tree=arg->tree;

	for(i=startnum;i<startnum+num;i++){
		vval=&val[i];
		node=&tree->node[0];
		while(node->treeval<0){
			if(node->num==0)
				break;
			node=&tree->node[node->subptr[vval->attr[node->attnum]]];
		}
		vval->res=node->treeval;
	}
}

void test(value* val, dicisiontree* tree){
	int i;

	int rest=TEST_N%(GEM5_NUMPROCS-1);
	int count=0;
	pthread_t thread[GEM5_NUMPROCS];
	teststruct structs[GEM5_NUMPROCS];
	if(TEST_N<GEM5_NUMPROCS){
		for(i=0;i<TEST_N;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			structs[i].num=1;
			structs[i].tree=tree;
			count++;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<TEST_N;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			if(rest==0){
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			structs[i].tree=tree;
			count+=structs[i].num;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}









int main(){
	value val[TRAIN_N];
	FILE* ftree=fopen("tree", "rb");
	FILE* fval=fopen("val", "rb");
	dicisiontree tree;
	FILE* finfo=fopen("fsubinfo", "rb");
	float subinfo[MAX_ATTR_NUM][MAX_ATTR_VAL];
	
	readsubinfo(subinfo, finfo);
	fclose(finfo);
	
	readtree(&tree, ftree);
	readvalb(val, fval, TRAIN_N);

	makesubtree(subinfo, val, &tree);
	
	fclose(ftree);
	fclose(fval);
	ftree=fopen("tree", "wb");
	savetree(&tree, ftree);
	fclose(ftree);

	return 0;
}
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 8500
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4


typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;

typedef struct treenode{
	float info;
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;

typedef struct calcinfostruct{
	float* info;
	value* val;
	treenode* node;
	int snum;
	int nnum;
}calcinfostruct;

typedef struct checkleafnodestruct{
	int* res;
	value* val;
	int firstnum;
	int startnum;
	int num;
}checkleafnodestruct;

typedef struct teststruct{
	value* val;
	int startnum;
	int num;
	dicisiontree* tree;
}teststruct;



int subnum[MAX_ATTR_NUM]=ATTR_MAX;


void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}





void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}
















void* checkleafnodefunc(void* thearg){
	checkleafnodestruct* arg=(checkleafnodestruct*) thearg;
	int* res=arg->res;
	value* val=arg->val;
	int firstnum=arg->firstnum;
	int startnum=arg->startnum;
	int num=arg->num;
	int rres=0;
	int i;
	for(i=startnum;i<startnum+num;i++){
		if(rres==0){
			if(val[firstnum].res!=val[i].res){
				rres=1;
				break;
			}
		}
	}

	*res=rres;
}

int checkleafnode(value* val, dicisiontree* tree){
	treenode* node=&tree->node[++tree->num];
	int i;

	int res[GEM5_NUMPROCS];
	int rres=0;

	int rest=node->num%(GEM5_NUMPROCS-1);
	int snum=node->startnum;
	pthread_t thread[GEM5_NUMPROCS];
	checkleafnodestruct structs[GEM5_NUMPROCS];
	
	if(node->num==0)
		return 0;
	else{
		if(node->num<GEM5_NUMPROCS){
			for(i=0;i<node->num;i++){
				structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				structs[i].num=1;
				snum++;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<node->num;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<node->num;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		else{
			for(i=0;i<GEM5_NUMPROCS;i++){structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				if(rest==0){
					structs[i].num=node->num/(GEM5_NUMPROCS-1);
				}
				else{
					structs[i].num=node->num/(GEM5_NUMPROCS-1)+1;
					rest--;
				}
				snum+=structs[i].num;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		if(rres==0)
			node->treeval=val[node->startnum].res;
	}
	return rres;
}

void* calcinfofunc(void* thearg){
	calcinfostruct* arg=(calcinfostruct*)thearg;
	float* dest=arg->info;
	value* val=arg->val;
	treenode* node=arg->node;
	int snum=arg->snum;
	int num=arg->nnum;
	int i, j, k;
	int n;
	int attnum[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	int attnumval[MAX_ATTR_NUM][MAX_ATTR_VAL][MAX_INFO_VAL]={0,};
	float sum, psum, p, fnum, anum;
	value* pval;

	for(i=snum;i<snum+num;i++){

		for(j=node->startnum;j<node->startnum+node->num;j++){
		pval=&val[j];
			attnumval[i][pval->attr[i]][pval->res]++;
			attnum[i][pval->attr[i]]++;
		}

		sum=0.0f;
		fnum=(float)node->num;
		for(j=0;j<MAX_ATTR_VAL;j++){
			psum=0.0f;
			anum=(float)attnum[i][j];
			for(k=0;k<MAX_INFO_VAL;k++){
				n=attnumval[i][j][k];
				if(n!=0&&n!=attnum[i][j]){
					p=(float)n/anum;
					p=p*(log(p)/log(2.0f));
					psum-=p;
				}
			}
			sum+=psum*anum/fnum;
		}
		dest[i]=sum;
	}
}

void calcinfo(value* val, dicisiontree* tree, float* info){
	int i;
	int snum=0;
	int nnum;
	treenode* node=&tree->node[tree->num];

	int rest=MAX_ATTR_NUM%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcinfostruct structs[GEM5_NUMPROCS];

	if(MAX_ATTR_NUM<GEM5_NUMPROCS){
		for(i=0;i<MAX_ATTR_NUM;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			structs[i].nnum=1;
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum++;
		}
		for(i=0;i<MAX_ATTR_NUM;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			if(rest==0){
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum+=structs[i].nnum;
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}

void compareinfo(value* val, dicisiontree* tree, float* info){
	treenode* node=&tree->node[tree->num];
	int i;
	int sel;
	float self=0xFFFFFFFF;

	for(i=0;i<MAX_ATTR_NUM;i++){
		if(node->flag[i]==0){
			if(self>info[i]){
				sel=i;
				self=info[i];
			}
		}
	}
	node->attnum=sel;
	node->flag[sel]=1;
}

void dividesection(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	value vallist[TRAIN_N][MAX_ATTR_VAL];
	int i, j, k;

	for(i=node->startnum;i<node->startnum+node->num;i++){
		j=val[i].attr[node->attnum];
		vallist[node->listcount[j]][j]=val[i];
		node->listcount[j]++;
	}
	i=node->startnum;
	while(i<node->startnum+node->num){
		for(j=0;j<MAX_ATTR_VAL;j++){
			for(k=0;k<node->listcount[j];k++){
				val[i]=vallist[k][j];
				i++;
			}
		}
	}
}

void makesubtree(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	treenode* nextnode;
	int i;
	int num=0;
	node->subnum=subnum[node->attnum];
	for(i=0;i<node->subnum;i++){
		nextnode=&tree->node[tree->maxnum];
		memcpy(nextnode, node, sizeof(treenode));
		nextnode->startnum=node->startnum+num;
		nextnode->num=node->listcount[i];
		memset(nextnode->listcount, 0, 4*MAX_ATTR_VAL);
		num+=nextnode->num;
		node->subptr[i]=tree->maxnum;
		tree->maxnum++;
	}
}







void* testfunc(void* thearg){
	teststruct* arg=(teststruct*)thearg;
	int i;
	int res;
	value* vval;
	treenode* node;
	value* val=arg->val;
	int startnum=arg->startnum;
	int num=arg->num;
	dicisiontree* tree=arg->tree;

	for(i=startnum;i<startnum+num;i++){
		vval=&val[i];
		node=&tree->node[0];
		while(node->treeval<0){
			if(node->num==0)
				break;
			node=&tree->node[node->subptr[vval->attr[node->attnum]]];
		}
		vval->res=node->treeval;
	}
}

void test(value* val, dicisiontree* tree){
	int i;

	int rest=TEST_N%(GEM5_NUMPROCS-1);
	int count=0;
	pthread_t thread[GEM5_NUMPROCS];
	teststruct structs[GEM5_NUMPROCS];
	if(TEST_N<GEM5_NUMPROCS){
		for(i=0;i<TEST_N;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			structs[i].num=1;
			structs[i].tree=tree;
			count++;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<TEST_N;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			if(rest==0){
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			structs[i].tree=tree;
			count+=structs[i].num;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}









int main(){
	FILE* ftree=fopen("tree", "wb");
	dicisiontree tree={-1,1,{1.0f,-1,0,TRAIN_N,},};

	savetree(&tree, ftree);
	fclose(ftree);

	return 0;
}
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 150
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4


typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;

typedef struct treenode{
	float info;
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;

typedef struct calcinfostruct{
	float* info;
	value* val;
	treenode* node;
	int snum;
	int nnum;
}calcinfostruct;

typedef struct checkleafnodestruct{
	int* res;
	value* val;
	int firstnum;
	int startnum;
	int num;
}checkleafnodestruct;

typedef struct teststruct{
	value* val;
	int startnum;
	int num;
	dicisiontree* tree;
}teststruct;



int subnum[MAX_ATTR_NUM]=ATTR_MAX;


void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}





void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}
















void* checkleafnodefunc(void* thearg){
	checkleafnodestruct* arg=(checkleafnodestruct*) thearg;
	int* res=arg->res;
	value* val=arg->val;
	int firstnum=arg->firstnum;
	int startnum=arg->startnum;
	int num=arg->num;
	int rres=0;
	int i;
	for(i=startnum;i<startnum+num;i++){
		if(rres==0){
			if(val[firstnum].res!=val[i].res){
				rres=1;
				break;
			}
		}
	}

	*res=rres;
}

int checkleafnode(value* val, dicisiontree* tree){
	treenode* node=&tree->node[++tree->num];
	int i;

	int res[GEM5_NUMPROCS];
	int rres=0;

	int rest=node->num%(GEM5_NUMPROCS-1);
	int snum=node->startnum;
	pthread_t thread[GEM5_NUMPROCS];
	checkleafnodestruct structs[GEM5_NUMPROCS];
	
	if(node->num==0)
		return 0;
	else{
		if(node->num<GEM5_NUMPROCS){
			for(i=0;i<node->num;i++){
				structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				structs[i].num=1;
				snum++;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<node->num;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<node->num;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		else{
			for(i=0;i<GEM5_NUMPROCS;i++){structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				if(rest==0){
					structs[i].num=node->num/(GEM5_NUMPROCS-1);
				}
				else{
					structs[i].num=node->num/(GEM5_NUMPROCS-1)+1;
					rest--;
				}
				snum+=structs[i].num;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		if(rres==0)
			node->treeval=val[node->startnum].res;
	}
	return rres;
}

void* calcinfofunc(void* thearg){
	calcinfostruct* arg=(calcinfostruct*)thearg;
	float* dest=arg->info;
	value* val=arg->val;
	treenode* node=arg->node;
	int snum=arg->snum;
	int num=arg->nnum;
	int i, j, k;
	int n;
	int attnum[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	int attnumval[MAX_ATTR_NUM][MAX_ATTR_VAL][MAX_INFO_VAL]={0,};
	float sum, psum, p, fnum, anum;
	value* pval;

	for(i=snum;i<snum+num;i++){

		for(j=node->startnum;j<node->startnum+node->num;j++){
		pval=&val[j];
			attnumval[i][pval->attr[i]][pval->res]++;
			attnum[i][pval->attr[i]]++;
		}

		sum=0.0f;
		fnum=(float)node->num;
		for(j=0;j<MAX_ATTR_VAL;j++){
			psum=0.0f;
			anum=(float)attnum[i][j];
			for(k=0;k<MAX_INFO_VAL;k++){
				n=attnumval[i][j][k];
				if(n!=0&&n!=attnum[i][j]){
					p=(float)n/anum;
					p=p*(log(p)/log(2.0f));
					psum-=p;
				}
			}
			sum+=psum*anum/fnum;
		}
		dest[i]=sum;
	}
}

void calcinfo(value* val, dicisiontree* tree, float* info){
	int i;
	int snum=0;
	int nnum;
	treenode* node=&tree->node[tree->num];

	int rest=MAX_ATTR_NUM%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcinfostruct structs[GEM5_NUMPROCS];

	if(MAX_ATTR_NUM<GEM5_NUMPROCS){
		for(i=0;i<MAX_ATTR_NUM;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			structs[i].nnum=1;
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum++;
		}
		for(i=0;i<MAX_ATTR_NUM;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			if(rest==0){
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum+=structs[i].nnum;
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}

void compareinfo(value* val, dicisiontree* tree, float* info){
	treenode* node=&tree->node[tree->num];
	int i;
	int sel;
	float self=0xFFFFFFFF;

	for(i=0;i<MAX_ATTR_NUM;i++){
		if(node->flag[i]==0){
			if(self>info[i]){
				sel=i;
				self=info[i];
			}
		}
	}
	node->attnum=sel;
	node->flag[sel]=1;
}

void dividesection(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	value vallist[TRAIN_N][MAX_ATTR_VAL];
	int i, j, k;

	for(i=node->startnum;i<node->startnum+node->num;i++){
		j=val[i].attr[node->attnum];
		vallist[node->listcount[j]][j]=val[i];
		node->listcount[j]++;
	}
	i=node->startnum;
	while(i<node->startnum+node->num){
		for(j=0;j<MAX_ATTR_VAL;j++){
			for(k=0;k<node->listcount[j];k++){
				val[i]=vallist[k][j];
				i++;
			}
		}
	}
}

void makesubtree(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	treenode* nextnode;
	int i;
	int num=0;
	node->subnum=subnum[node->attnum];
	for(i=0;i<node->subnum;i++){
		nextnode=&tree->node[tree->maxnum];
		memcpy(nextnode, node, sizeof(treenode));
		nextnode->startnum=node->startnum+num;
		nextnode->num=node->listcount[i];
		memset(nextnode->listcount, 0, 4*MAX_ATTR_VAL);
		num+=nextnode->num;
		node->subptr[i]=tree->maxnum;
		tree->maxnum++;
	}
}







void testfunc(value* val, int startnum, int num, dicisiontree* tree){
	int i;
	int res;
	value* vval;
	treenode* node;
	for(i=startnum;i<startnum+num;i++){
		vval=&val[i];
		node=&tree->node[0];
		while(node->treeval<0){
			if(node->num==0)
				break;
			node=&tree->node[node->subptr[vval->attr[node->attnum]]];
		}
		vval->res=node->treeval;
	}
}

void test(value* val, dicisiontree* tree){
	int i;

	for(i=0;i<1;i++){
		testfunc(val, 0, TEST_N, tree);
	}
}








int main(){
	value val[TEST_N];
	FILE* ftree=fopen("tree", "rb");
	FILE* fval=fopen("testval", "rb");
	FILE* fvalo=fopen("testvalo", "wb");
	dicisiontree tree;
	
	readtree(&tree, ftree);
	readvalb(val, fval, TEST_N);
	
	test(val, &tree);
	
	savevalb(val, fvalo, TEST_N);
	fclose(ftree);
	fclose(fval);
	fclose(fvalo);

	return 0;
}
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 8500
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4


typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;

typedef struct treenode{
	float info;
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;

typedef struct calcinfostruct{
	float* info;
	value* val;
	treenode* node;
	int snum;
	int nnum;
}calcinfostruct;

typedef struct checkleafnodestruct{
	int* res;
	value* val;
	int firstnum;
	int startnum;
	int num;
}checkleafnodestruct;

typedef struct teststruct{
	value* val;
	int startnum;
	int num;
	dicisiontree* tree;
}teststruct;



int subnum[MAX_ATTR_NUM]=ATTR_MAX;
void readsubinfo(float dest[][MAX_ATTR_VAL], FILE* fp){
	fread(dest, MAX_ATTR_VAL*MAX_ATTR_NUM, sizeof(float), fp);
}

void savesubinfo(float dest[][MAX_ATTR_VAL], FILE* fp){
	fwrite(dest, MAX_ATTR_VAL*MAX_ATTR_NUM, sizeof(float), fp);
}

void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}





void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}
















void* checkleafnodefunc(void* thearg){
	checkleafnodestruct* arg=(checkleafnodestruct*) thearg;
	int* res=arg->res;
	value* val=arg->val;
	int firstnum=arg->firstnum;
	int startnum=arg->startnum;
	int num=arg->num;
	int rres=0;
	int i;
	for(i=startnum;i<startnum+num;i++){
		if(rres==0){
			if(val[firstnum].res!=val[i].res){
				rres=1;
				break;
			}
		}
	}

	*res=rres;
}

int checkleafnode(value* val, dicisiontree* tree){
	treenode* node=&tree->node[++tree->num];
	int i;

	int res[GEM5_NUMPROCS];
	int rres=0;

	int rest=node->num%(GEM5_NUMPROCS-1);
	int snum=node->startnum;
	pthread_t thread[GEM5_NUMPROCS];
	checkleafnodestruct structs[GEM5_NUMPROCS];
	
	if(node->num==0)
		return 0;
	else{
		if(node->num<GEM5_NUMPROCS){
			for(i=0;i<node->num;i++){
				structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				structs[i].num=1;
				snum++;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<node->num;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<node->num;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		else{
			for(i=0;i<GEM5_NUMPROCS;i++){structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				if(rest==0){
					structs[i].num=node->num/(GEM5_NUMPROCS-1);
				}
				else{
					structs[i].num=node->num/(GEM5_NUMPROCS-1)+1;
					rest--;
				}
				snum+=structs[i].num;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		if(rres==0)
			node->treeval=val[node->startnum].res;
	}
	return rres;
}

void* calcinfofunc(void* thearg){
	calcinfostruct* arg=(calcinfostruct*)thearg;
	float* dest=arg->info;
	value* val=arg->val;
	treenode* node=arg->node;
	int snum=arg->snum;
	int num=arg->nnum;
	int i, j, k;
	int n;
	int attnum[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	int attnumval[MAX_ATTR_NUM][MAX_ATTR_VAL][MAX_INFO_VAL]={0,};
	float sum, psum, p, fnum, anum;
	value* pval;

	for(i=snum;i<snum+num;i++){

		for(j=node->startnum;j<node->startnum+node->num;j++){
		pval=&val[j];
			attnumval[i][pval->attr[i]][pval->res]++;
			attnum[i][pval->attr[i]]++;
		}

		sum=0.0f;
		fnum=(float)node->num;
		for(j=0;j<MAX_ATTR_VAL;j++){
			psum=0.0f;
			anum=(float)attnum[i][j];
			for(k=0;k<MAX_INFO_VAL;k++){
				n=attnumval[i][j][k];
				if(n!=0&&n!=attnum[i][j]){
					p=(float)n/anum;
					p=p*(log(p)/log(2.0f));
					psum-=p;
				}
			}
			sum+=psum*anum/fnum;
		}
		dest[i]=sum;
	}
}

void calcinfo(value* val, dicisiontree* tree, float* info){
	int i;
	int snum=0;
	int nnum;
	treenode* node=&tree->node[tree->num];

	int rest=MAX_ATTR_NUM%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcinfostruct structs[GEM5_NUMPROCS];

	if(MAX_ATTR_NUM<GEM5_NUMPROCS){
		for(i=0;i<MAX_ATTR_NUM;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			structs[i].nnum=1;
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum++;
		}
		for(i=0;i<MAX_ATTR_NUM;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			if(rest==0){
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum+=structs[i].nnum;
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}

void compareinfo(value* val, dicisiontree* tree, float* info){
	treenode* node=&tree->node[tree->num];
	int i;
	int sel;
	float self=0xFFFFFFFF;

	for(i=0;i<MAX_ATTR_NUM;i++){
		if(node->flag[i]==0){
			if(self>info[i]){
				sel=i;
				self=info[i];
			}
		}
	}
	node->attnum=sel;
	node->flag[sel]=1;
}

void dividesection(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	value vallist[TRAIN_N][MAX_ATTR_VAL];
	int i, j, k;

	for(i=node->startnum;i<node->startnum+node->num;i++){
		j=val[i].attr[node->attnum];
		vallist[node->listcount[j]][j]=val[i];
		node->listcount[j]++;
	}
	i=node->startnum;
	while(i<node->startnum+node->num){
		for(j=0;j<MAX_ATTR_VAL;j++){
			for(k=0;k<node->listcount[j];k++){
				val[i]=vallist[k][j];
				i++;
			}
		}
	}
}

void makesubtree(float subinfo[][MAX_ATTR_VAL], value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	treenode* nextnode;
	int i;
	int num=0;
	node->subnum=subnum[node->attnum];
	for(i=0;i<node->subnum;i++){
		nextnode=&tree->node[tree->maxnum];
		memcpy(nextnode, node, sizeof(treenode));
		nextnode->startnum=node->startnum+num;
		nextnode->num=node->listcount[i];
		memset(nextnode->listcount, 0, 4*MAX_ATTR_VAL);
		num+=nextnode->num;
		node->subptr[i]=tree->maxnum;
		tree->maxnum++;
		nextnode->info=subinfo[node->attnum][i];
	}
}







void* testfunc(void* thearg){
	teststruct* arg=(teststruct*)thearg;
	int i;
	int res;
	value* vval;
	treenode* node;
	value* val=arg->val;
	int startnum=arg->startnum;
	int num=arg->num;
	dicisiontree* tree=arg->tree;

	for(i=startnum;i<startnum+num;i++){
		vval=&val[i];
		node=&tree->node[0];
		while(node->treeval<0){
			if(node->num==0)
				break;
			node=&tree->node[node->subptr[vval->attr[node->attnum]]];
		}
		vval->res=node->treeval;
	}
}

void test(value* val, dicisiontree* tree){
	int i;

	int rest=TEST_N%(GEM5_NUMPROCS-1);
	int count=0;
	pthread_t thread[GEM5_NUMPROCS];
	teststruct structs[GEM5_NUMPROCS];
	if(TEST_N<GEM5_NUMPROCS){
		for(i=0;i<TEST_N;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			structs[i].num=1;
			structs[i].tree=tree;
			count++;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<TEST_N;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			if(rest==0){
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			structs[i].tree=tree;
			count+=structs[i].num;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}









int main(){
	value val[TRAIN_N];
	FILE* ftree=fopen("tree", "rb");
	FILE* fval=fopen("val", "rb");
	dicisiontree tree;
	FILE* finfo=fopen("fsubinfo", "rb");
	float subinfo[MAX_ATTR_NUM][MAX_ATTR_VAL];
	
	readsubinfo(subinfo, finfo);
	fclose(finfo);
	
	readtree(&tree, ftree);
	readvalb(val, fval, TRAIN_N);

	makesubtree(subinfo, val, &tree);
	
	fclose(ftree);
	fclose(fval);
	ftree=fopen("tree", "wb");
	savetree(&tree, ftree);
	fclose(ftree);

	return 0;
}
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 8500
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4


typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;

typedef struct treenode{
	float info;
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;

typedef struct calcinfostruct{
	float* info;
	value* val;
	treenode* node;
	int snum;
	int nnum;
}calcinfostruct;

typedef struct checkleafnodestruct{
	int* res;
	value* val;
	int firstnum;
	int startnum;
	int num;
}checkleafnodestruct;

typedef struct teststruct{
	value* val;
	int startnum;
	int num;
	dicisiontree* tree;
}teststruct;



int subnum[MAX_ATTR_NUM]=ATTR_MAX;


void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}





void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}
















void* checkleafnodefunc(void* thearg){
	checkleafnodestruct* arg=(checkleafnodestruct*) thearg;
	int* res=arg->res;
	value* val=arg->val;
	int firstnum=arg->firstnum;
	int startnum=arg->startnum;
	int num=arg->num;
	int rres=0;
	int i;
	for(i=startnum;i<startnum+num;i++){
		if(rres==0){
			if(val[firstnum].res!=val[i].res){
				rres=1;
				break;
			}
		}
	}

	*res=rres;
}

int checkleafnode(value* val, dicisiontree* tree){
	treenode* node=&tree->node[++tree->num];
	int i;

	int res[GEM5_NUMPROCS];
	int rres=0;

	int rest=node->num%(GEM5_NUMPROCS-1);
	int snum=node->startnum;
	pthread_t thread[GEM5_NUMPROCS];
	checkleafnodestruct structs[GEM5_NUMPROCS];
	
	if(node->num==0)
		return 0;
	else{
		if(node->num<GEM5_NUMPROCS){
			for(i=0;i<node->num;i++){
				structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				structs[i].num=1;
				snum++;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<node->num;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<node->num;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		else{
			for(i=0;i<GEM5_NUMPROCS;i++){structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				if(rest==0){
					structs[i].num=node->num/(GEM5_NUMPROCS-1);
				}
				else{
					structs[i].num=node->num/(GEM5_NUMPROCS-1)+1;
					rest--;
				}
				snum+=structs[i].num;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		if(rres==0)
			node->treeval=val[node->startnum].res;
	}
	return rres;
}

void* calcinfofunc(void* thearg){
	calcinfostruct* arg=(calcinfostruct*)thearg;
	float* dest=arg->info;
	value* val=arg->val;
	treenode* node=arg->node;
	int snum=arg->snum;
	int num=arg->nnum;
	int i, j, k;
	int n;
	int attnum[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	int attnumval[MAX_ATTR_NUM][MAX_ATTR_VAL][MAX_INFO_VAL]={0,};
	float sum, psum, p, fnum, anum;
	value* pval;

	for(i=snum;i<snum+num;i++){

		for(j=node->startnum;j<node->startnum+node->num;j++){
		pval=&val[j];
			attnumval[i][pval->attr[i]][pval->res]++;
			attnum[i][pval->attr[i]]++;
		}

		sum=0.0f;
		fnum=(float)node->num;
		for(j=0;j<MAX_ATTR_VAL;j++){
			psum=0.0f;
			anum=(float)attnum[i][j];
			for(k=0;k<MAX_INFO_VAL;k++){
				n=attnumval[i][j][k];
				if(n!=0&&n!=attnum[i][j]){
					p=(float)n/anum;
					p=p*(log(p)/log(2.0f));
					psum-=p;
				}
			}
			sum+=psum*anum/fnum;
		}
		dest[i]=sum;
	}
}

void calcinfo(value* val, dicisiontree* tree, float* info){
	int i;
	int snum=0;
	int nnum;
	treenode* node=&tree->node[tree->num];

	int rest=MAX_ATTR_NUM%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcinfostruct structs[GEM5_NUMPROCS];

	if(MAX_ATTR_NUM<GEM5_NUMPROCS){
		for(i=0;i<MAX_ATTR_NUM;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			structs[i].nnum=1;
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum++;
		}
		for(i=0;i<MAX_ATTR_NUM;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			if(rest==0){
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum+=structs[i].nnum;
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}

void compareinfo(value* val, dicisiontree* tree, float* info){
	treenode* node=&tree->node[tree->num];
	int i;
	int sel;
	float self=0xFFFFFFFF;

	for(i=0;i<MAX_ATTR_NUM;i++){
		if(node->flag[i]==0){
			if(self>info[i]){
				sel=i;
				self=info[i];
			}
		}
	}
	node->attnum=sel;
	node->flag[sel]=1;
}

void dividesection(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	value vallist[TRAIN_N][MAX_ATTR_VAL];
	int i, j, k;

	for(i=node->startnum;i<node->startnum+node->num;i++){
		j=val[i].attr[node->attnum];
		vallist[node->listcount[j]][j]=val[i];
		node->listcount[j]++;
	}
	i=node->startnum;
	while(i<node->startnum+node->num){
		for(j=0;j<MAX_ATTR_VAL;j++){
			for(k=0;k<node->listcount[j];k++){
				val[i]=vallist[k][j];
				i++;
			}
		}
	}
}

void makesubtree(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	treenode* nextnode;
	int i;
	int num=0;
	node->subnum=subnum[node->attnum];
	for(i=0;i<node->subnum;i++){
		nextnode=&tree->node[tree->maxnum];
		memcpy(nextnode, node, sizeof(treenode));
		nextnode->startnum=node->startnum+num;
		nextnode->num=node->listcount[i];
		memset(nextnode->listcount, 0, 4*MAX_ATTR_VAL);
		num+=nextnode->num;
		node->subptr[i]=tree->maxnum;
		tree->maxnum++;
	}
}







void* testfunc(void* thearg){
	teststruct* arg=(teststruct*)thearg;
	int i;
	int res;
	value* vval;
	treenode* node;
	value* val=arg->val;
	int startnum=arg->startnum;
	int num=arg->num;
	dicisiontree* tree=arg->tree;

	for(i=startnum;i<startnum+num;i++){
		vval=&val[i];
		node=&tree->node[0];
		while(node->treeval<0){
			if(node->num==0)
				break;
			node=&tree->node[node->subptr[vval->attr[node->attnum]]];
		}
		vval->res=node->treeval;
	}
}

void test(value* val, dicisiontree* tree){
	int i;

	int rest=TEST_N%(GEM5_NUMPROCS-1);
	int count=0;
	pthread_t thread[GEM5_NUMPROCS];
	teststruct structs[GEM5_NUMPROCS];
	if(TEST_N<GEM5_NUMPROCS){
		for(i=0;i<TEST_N;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			structs[i].num=1;
			structs[i].tree=tree;
			count++;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<TEST_N;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			if(rest==0){
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			structs[i].tree=tree;
			count+=structs[i].num;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}









int main(){
	FILE* ftree=fopen("tree", "wb");
	dicisiontree tree={-1,1,{1.0f,-1,0,TRAIN_N,},};

	savetree(&tree, ftree);
	fclose(ftree);

	return 0;
}
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 150
#define MAX_TREE_NUM 2227//500

#define ATTR_MAX {9,	16,	15,	33,	4,	10,	8,	4,	6,	6,	3,	17,	5,	4,	2,	2,	2,	2,	2}

#define GEM5_NUMPROCS 4


typedef struct value{
	int attr[MAX_ATTR_NUM];
	int res;
}value;

typedef struct treenode{
	float info;
	int treeval;//
	int startnum;//
	int num;//
	int subnum;
	int subptr[MAX_ATTR_VAL];//
	int listcount[MAX_ATTR_VAL];
	int attnum;//
	int flag[MAX_ATTR_NUM];
}treenode;

typedef struct dicisiontree{
	int num;
	int maxnum;
	struct treenode node[MAX_TREE_NUM];
}dicisiontree;

typedef struct calcinfostruct{
	float* info;
	value* val;
	treenode* node;
	int snum;
	int nnum;
}calcinfostruct;

typedef struct checkleafnodestruct{
	int* res;
	value* val;
	int firstnum;
	int startnum;
	int num;
}checkleafnodestruct;

typedef struct teststruct{
	value* val;
	int startnum;
	int num;
	dicisiontree* tree;
}teststruct;



int subnum[MAX_ATTR_NUM]=ATTR_MAX;


void read(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TRAIN_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
		fscanf(fp, "%d", &dest[i].res);
	}
}

void readtest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fscanf(fp, "%d", &dest[i].attr[j]);
		}
	}
}

void printtest(value* dest){
	int i;
	for(i=0;i<TEST_N;i++){
		printf("%d\n", dest[i].res);
	}
}
void fprinttest(value* dest, FILE* fp){
	int i, j;
	for(i=0;i<TEST_N;i++){
		for(j=0;j<MAX_ATTR_NUM;j++){
			fprintf(fp, "%d\t", dest[i].attr[j]);
		}
		fprintf(fp, "%d\n", dest[i].res);
	}
}





void readvalb(value* dest, FILE* fp, int num){
	fread(dest, num, sizeof(value), fp);
}

void readinfob(float* info, FILE* fp, int num){
	fread(info, num, sizeof(float), fp);
}

void readtree(dicisiontree* dest, FILE* fp){
	fread(dest, 1, sizeof(dicisiontree), fp);
}
void savevalb(value* dest, FILE* fp, int num){
	fwrite(dest, num, sizeof(value), fp);
}

void saveinfob(float* info, FILE* fp, int num){
	fwrite(info, num, sizeof(float), fp);
}

void savetree(dicisiontree* dest, FILE* fp){
	fwrite(dest, 1, sizeof(dicisiontree), fp);
}
















void* checkleafnodefunc(void* thearg){
	checkleafnodestruct* arg=(checkleafnodestruct*) thearg;
	int* res=arg->res;
	value* val=arg->val;
	int firstnum=arg->firstnum;
	int startnum=arg->startnum;
	int num=arg->num;
	int rres=0;
	int i;
	for(i=startnum;i<startnum+num;i++){
		if(rres==0){
			if(val[firstnum].res!=val[i].res){
				rres=1;
				break;
			}
		}
	}

	*res=rres;
}

int checkleafnode(value* val, dicisiontree* tree){
	treenode* node=&tree->node[++tree->num];
	int i;

	int res[GEM5_NUMPROCS];
	int rres=0;

	int rest=node->num%(GEM5_NUMPROCS-1);
	int snum=node->startnum;
	pthread_t thread[GEM5_NUMPROCS];
	checkleafnodestruct structs[GEM5_NUMPROCS];
	
	if(node->num==0)
		return 0;
	else{
		if(node->num<GEM5_NUMPROCS){
			for(i=0;i<node->num;i++){
				structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				structs[i].num=1;
				snum++;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<node->num;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<node->num;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		else{
			for(i=0;i<GEM5_NUMPROCS;i++){structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				if(rest==0){
					structs[i].num=node->num/(GEM5_NUMPROCS-1);
				}
				else{
					structs[i].num=node->num/(GEM5_NUMPROCS-1)+1;
					rest--;
				}
				snum+=structs[i].num;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		if(rres==0)
			node->treeval=val[node->startnum].res;
	}
	return rres;
}

void* calcinfofunc(void* thearg){
	calcinfostruct* arg=(calcinfostruct*)thearg;
	float* dest=arg->info;
	value* val=arg->val;
	treenode* node=arg->node;
	int snum=arg->snum;
	int num=arg->nnum;
	int i, j, k;
	int n;
	int attnum[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	int attnumval[MAX_ATTR_NUM][MAX_ATTR_VAL][MAX_INFO_VAL]={0,};
	float sum, psum, p, fnum, anum;
	value* pval;

	for(i=snum;i<snum+num;i++){

		for(j=node->startnum;j<node->startnum+node->num;j++){
		pval=&val[j];
			attnumval[i][pval->attr[i]][pval->res]++;
			attnum[i][pval->attr[i]]++;
		}

		sum=0.0f;
		fnum=(float)node->num;
		for(j=0;j<MAX_ATTR_VAL;j++){
			psum=0.0f;
			anum=(float)attnum[i][j];
			for(k=0;k<MAX_INFO_VAL;k++){
				n=attnumval[i][j][k];
				if(n!=0&&n!=attnum[i][j]){
					p=(float)n/anum;
					p=p*(log(p)/log(2.0f));
					psum-=p;
				}
			}
			sum+=psum*anum/fnum;
		}
		dest[i]=sum;
	}
}

void calcinfo(value* val, dicisiontree* tree, float* info){
	int i;
	int snum=0;
	int nnum;
	treenode* node=&tree->node[tree->num];

	int rest=MAX_ATTR_NUM%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcinfostruct structs[GEM5_NUMPROCS];

	if(MAX_ATTR_NUM<GEM5_NUMPROCS){
		for(i=0;i<MAX_ATTR_NUM;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			structs[i].nnum=1;
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum++;
		}
		for(i=0;i<MAX_ATTR_NUM;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			if(rest==0){
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum+=structs[i].nnum;
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}

void compareinfo(value* val, dicisiontree* tree, float* info){
	treenode* node=&tree->node[tree->num];
	int i;
	int sel;
	float self=0xFFFFFFFF;

	for(i=0;i<MAX_ATTR_NUM;i++){
		if(node->flag[i]==0){
			if(self>info[i]){
				sel=i;
				self=info[i];
			}
		}
	}
	node->attnum=sel;
	node->flag[sel]=1;
}

void dividesection(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	value vallist[TRAIN_N][MAX_ATTR_VAL];
	int i, j, k;

	for(i=node->startnum;i<node->startnum+node->num;i++){
		j=val[i].attr[node->attnum];
		vallist[node->listcount[j]][j]=val[i];
		node->listcount[j]++;
	}
	i=node->startnum;
	while(i<node->startnum+node->num){
		for(j=0;j<MAX_ATTR_VAL;j++){
			for(k=0;k<node->listcount[j];k++){
				val[i]=vallist[k][j];
				i++;
			}
		}
	}
}

void makesubtree(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	treenode* nextnode;
	int i;
	int num=0;
	node->subnum=subnum[node->attnum];
	for(i=0;i<node->subnum;i++){
		nextnode=&tree->node[tree->maxnum];
		memcpy(nextnode, node, sizeof(treenode));
		nextnode->startnum=node->startnum+num;
		nextnode->num=node->listcount[i];
		memset(nextnode->listcount, 0, 4*MAX_ATTR_VAL);
		num+=nextnode->num;
		node->subptr[i]=tree->maxnum;
		tree->maxnum++;
	}
}







void testfunc(value* val, int startnum, int num, dicisiontree* tree){
	int i;
	int res;
	value* vval;
	treenode* node;
	for(i=startnum;i<startnum+num;i++){
		vval=&val[i];
		node=&tree->node[0];
		while(node->treeval<0){
			if(node->num==0)
				break;
			node=&tree->node[node->subptr[vval->attr[node->attnum]]];
		}
		vval->res=node->treeval;
	}
}

void test(value* val, dicisiontree* tree){
	int i;

	for(i=0;i<1;i++){
		testfunc(val, 0, TEST_N, tree);
	}
}








int main(){
	value val[TEST_N];
	FILE* ftree=fopen("tree", "rb");
	FILE* fval=fopen("testval", "rb");
	FILE* fvalo=fopen("testvalo", "wb");
	dicisiontree tree;
	
	readtree(&tree, ftree);
	readvalb(val, fval, TEST_N);
	
	test(val, &tree);
	
	savevalb(val, fvalo, TEST_N);
	fclose(ftree);
	fclose(fval);
	fclose(fvalo);

	return 0;
}
#
S4SIM_HOME = ../..
CFLAGS = -g

INCLUDE=-I${S4SIM_HOME}/include
PTHREAD = ${S4SIM_HOME}/external/m5threads
CC = gcc
CPP = g++

ARMCC = arm-linux-gnueabi-gcc
ARMFLAGS = -march=armv7-a -marm

all : convert convertrev run_decisiontree decisiontree_isp_calc decisiontree_isp_check decisiontree_isp_compare decisiontree_isp_divide decisiontree_isp_makesub decisiontree_isp_test decisiontree_isp_read

convert : convert.c
	$(CC) $(CFLAGS) -o $@ $^ $(INCLUDE)
convertrev : convertrev.c
	$(CC) $(CFLAGS) -o $@ $^ $(INCLUDE)
	
run_decisiontree : run_decisiontree.c ${S4SIM_HOME}/src/isp_socket.c
	$(CC) $(CFLAGS) -o $@ $^ -lpthread $(INCLUDE)

decisiontree_isp_calc : decisiontree_isp_calc.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static -lm $(INCLUDE)
	
decisiontree_isp_check : decisiontree_isp_check.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static -lm $(INCLUDE)
	
decisiontree_isp_compare : decisiontree_isp_compare.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static -lm $(INCLUDE)
	
decisiontree_isp_divide : decisiontree_isp_divide.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static -lm $(INCLUDE)
	
decisiontree_isp_makesub : decisiontree_isp_makesub.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static -lm $(INCLUDE)
	
decisiontree_isp_test : decisiontree_isp_test.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static -lm $(INCLUDE)
	
decisiontree_isp_read : decisiontree_isp_read.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static -lm $(INCLUDE)
	
#include <stdio.h>
#include <stdlib.h>
#include "isp.h"

#define issd_clock 400
#define issd_numcpu 4


void doone(int cpuhz, int numcpu, int num, isp_device_id device, FILE* ifp){
	char cmd[128];
	char pname[64];
	char funcname[64];
	char clock[32];
int cycle;
	sprintf(clock, "%dMHz", cpuhz);
	sprintf(funcname, "check");
	sprintf(pname, "./decisiontree_isp_%s", funcname);
	sprintf(cmd, "mv ./m5out/stats.txt ./m5out/decisiontree_%s_%d_%dMHz_%d.txt", funcname, numcpu, cpuhz, num);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, clock);
	system(cmd);
}

void doall(int cpuhz, int numcpu, int num, isp_device_id device, FILE* ifp){
	char cmd[128];
	char pname[64];
	char funcname[64];
	char clock[32];
int cycle;
	sprintf(clock, "%dMHz", cpuhz);

sprintf(funcname, "check");
	sprintf(pname, "./decisiontree_isp_%s", funcname);
	sprintf(cmd, "mv ./m5out/stats.txt ./m5out/decisiontree_%s_%d_%dMHz_%d.txt", funcname, numcpu, cpuhz, num);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, clock);
	system(cmd);
	
	sprintf(funcname, "calc");
	sprintf(pname, "./decisiontree_isp_%s", funcname);
	sprintf(cmd, "mv ./m5out/stats.txt ./m5out/decisiontree_%s_%d_%dMHz_%d.txt", funcname, numcpu, cpuhz, num);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, clock);
	system(cmd);
	
	sprintf(funcname, "compare");
	sprintf(pname, "./decisiontree_isp_%s", funcname);
	sprintf(cmd, "mv ./m5out/stats.txt ./m5out/decisiontree_%s_%d_%dMHz_%d.txt", funcname, numcpu, cpuhz, num);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, clock);
	system(cmd);
	
	sprintf(funcname, "divide");
	sprintf(pname, "./decisiontree_isp_%s", funcname);
	sprintf(cmd, "mv ./m5out/stats.txt ./m5out/decisiontree_%s_%d_%dMHz_%d.txt", funcname, numcpu, cpuhz, num);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, clock);
	system(cmd);
	
	sprintf(funcname, "makesub");
	sprintf(pname, "./decisiontree_isp_%s", funcname);
	sprintf(cmd, "mv ./m5out/stats.txt ./m5out/decisiontree_%s_%d_%dMHz_%d.txt", funcname, numcpu, cpuhz, num);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, clock);
	system(cmd);
}

void test(int cpuhz, int numcpu, isp_device_id device, FILE* ifp){
	char cmd[128];
	char pname[64];
	char funcname[64];
	char clock[32];
	int cycle;
	sprintf(clock, "%dMHz", cpuhz);
	sprintf(funcname, "test");
	sprintf(pname, "./decisiontree_isp_%s", funcname);
	sprintf(cmd, "mv ./m5out/stats.txt ./m5out/decisiontree_%s_%d_%dMHz.txt", funcname, numcpu, cpuhz);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, clock);
	system(cmd);
}

int main(int argc, const char* argv[])
{
	isp_device_id device;
	FILE* ifp;
	int n;
	char buffer[1024];
	int cycle;
	int i;
	char cpuhz[16];
	char cmd[64];
	char pname[64];
	char funcname[64];
	int numcpu=issd_numcpu;
	int clock=issd_clock;
	int a, b;
	FILE* treeinfof;
	sprintf(cpuhz, "%dMHz", clock);
	system("./convert");
	sprintf(funcname, "read");
	sprintf(pname, "./decisiontree_isp_%s", funcname);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
	treeinfof=fopen("treeinfo.txt", "r");
	while(1){
		fscanf(treeinfof, "%d %d", &a, &b);
		if(a==-1)
			break;
		if(b==1)
			doone(clock, numcpu, a, device, ifp);
		else
			doall(clock, numcpu, a, device, ifp);
	}
	fclose(treeinfof);
	test(clock, numcpu, device, ifp);

	system("./convertrev");
	sprintf(pname, "cp test2.txt test_%d_%s.txt", numcpu, cpuhz);
	system(pname);
	sprintf(pname, "cp treeout.txt tree_%d_%s.txt", numcpu, cpuhz);
	system(pname);

	printf("ISP cycle = %d\n", cycle);
	return 0;
}
8	3	0	23	1	4	3	1	5	5	0	9	1	0	1	1	1	0	0
8	7	0	13	2	2	3	2	4	4	0	4	4	0	1	1	0	0	1
8	7	0	15	2	3	3	2	1	4	2	16	4	3	0	0	0	0	0
1	2	0	9	2	3	6	2	2	5	2	16	4	3	0	0	0	0	0
1	9	0	15	2	2	3	2	0	5	2	0	4	0	1	1	0	0	0
8	3	0	8	2	3	3	2	4	2	0	5	2	0	1	1	0	1	1
8	5	2	13	2	1	1	3	1	3	0	3	1	0	1	0	0	0	0
1	11	11	13	2	3	3	2	2	5	0	7	2	0	1	1	1	1	1
8	9	11	9	2	3	3	2	4	5	0	5	0	1	1	1	0	0	1
1	11	0	11	2	2	0	2	0	5	2	5	4	0	1	0	1	0	0
8	0	5	2	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
4	9	5	13	2	2	1	3	0	5	2	16	4	3	0	0	0	0	0
6	3	5	8	2	2	3	2	0	2	2	16	4	3	0	0	0	0	0
1	6	2	11	1	4	1	2	5	1	2	4	4	0	1	0	0	0	0
8	6	2	23	1	2	2	3	1	1	2	2	4	2	1	1	0	0	0
8	5	0	17	2	3	3	2	4	3	0	12	1	0	1	1	0	0	1
1	1	14	13	1	3	3	2	5	5	2	16	4	3	0	0	0	0	0
1	2	5	7	1	2	0	2	2	5	0	6	1	0	1	1	0	0	1
1	0	0	9	2	1	6	2	5	0	0	2	0	1	1	1	1	0	1
1	3	8	21	1	2	6	3	5	2	0	5	2	0	1	1	0	0	0
8	7	5	5	2	3	3	2	4	4	0	10	4	0	1	0	1	0	0
8	3	9	1	2	3	3	2	4	2	2	16	4	3	0	0	0	0	0
1	1	0	23	1	2	3	1	0	5	2	3	0	0	1	1	0	0	1
1	6	0	16	2	3	3	2	2	1	0	11	1	0	1	1	0	0	1
8	5	4	11	2	1	3	3	4	3	0	2	3	0	1	1	0	0	1
8	5	14	20	1	3	3	3	5	3	0	7	1	0	1	1	1	0	0
8	6	0	17	2	2	3	2	4	1	0	4	1	1	1	1	0	0	0
8	5	0	11	2	3	0	2	4	3	0	3	0	0	1	1	0	1	1
8	5	0	19	1	2	3	1	4	3	0	5	1	0	1	1	0	1	1
1	3	13	1	2	3	3	2	5	2	0	6	1	0	1	0	1	1	0
8	3	0	26	1	8	4	1	1	5	2	3	4	0	0	0	0	0	0
8	5	2	13	1	1	1	2	1	3	2	16	4	3	0	0	0	0	0
1	5	0	24	1	3	3	1	5	5	0	7	1	0	1	1	0	0	1
8	5	2	6	1	1	1	1	1	5	1	4	0	1	1	1	0	0	0
8	3	5	14	1	3	3	2	5	2	2	16	4	3	0	0	0	0	0
1	5	2	8	1	3	3	2	5	3	0	2	4	1	1	1	1	0	0
8	0	9	8	2	9	6	3	3	0	0	6	4	0	1	1	1	0	1
1	3	0	18	2	4	1	2	5	2	0	8	1	0	1	0	0	0	0
1	7	6	7	1	2	3	2	5	4	0	2	4	0	1	1	0	0	1
1	5	0	10	2	1	2	2	2	3	2	16	4	3	0	0	0	0	0
8	3	0	12	2	2	3	2	4	2	0	3	1	0	1	1	0	0	1
8	5	3	5	2	2	1	2	4	3	0	1	1	1	1	1	0	0	1
1	3	0	13	2	3	3	2	0	2	0	5	1	0	1	1	0	0	0
1	3	10	14	2	3	3	2	5	2	0	10	1	0	1	1	0	0	1
8	3	6	16	2	2	3	3	5	2	0	2	3	0	1	1	1	0	0
8	5	2	5	1	4	6	1	1	5	0	16	0	1	0	0	0	0	0
8	5	9	8	2	3	3	2	5	3	0	4	2	0	1	1	0	0	1
1	3	0	20	1	3	3	1	5	2	0	8	1	0	1	1	0	0	1
1	3	10	13	2	1	3	3	5	2	0	11	0	2	1	1	0	1	1
1	0	0	9	2	3	0	2	5	0	2	3	4	0	0	0	0	0	0
1	6	3	7	2	4	1	2	2	1	0	7	3	0	1	0	0	0	0
1	3	0	14	2	2	3	2	0	2	0	5	3	2	1	1	1	0	0
8	5	0	20	1	2	3	1	5	3	2	1	1	1	1	1	0	0	0
8	3	0	12	2	3	3	2	4	2	0	6	1	0	1	1	0	1	1
8	3	0	12	2	3	3	2	5	2	0	4	2	0	1	1	0	0	1
1	6	2	9	2	2	3	3	2	1	2	16	4	3	0	0	0	0	0
8	0	0	9	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
1	7	0	16	2	2	3	2	5	4	0	0	0	0	1	0	0	0	0
8	3	12	9	1	2	3	2	5	2	2	16	4	3	0	0	0	0	0
1	3	0	20	1	3	2	1	5	2	0	3	1	0	1	1	1	0	1
1	7	0	31	1	3	3	1	5	5	0	6	1	0	1	1	0	1	1
1	8	14	13	2	3	3	2	5	5	2	16	4	3	0	0	0	0	0
1	7	0	20	2	3	3	2	2	5	2	16	4	3	0	0	0	0	0
8	5	6	3	1	2	3	1	1	5	2	16	4	3	0	0	0	0	0
1	5	2	9	1	4	3	2	5	3	0	3	4	1	1	1	0	1	0
8	6	2	10	2	2	1	3	4	1	2	16	4	3	0	0	0	0	0
1	5	0	19	2	2	1	2	5	3	0	6	0	0	1	0	1	0	0
1	3	0	17	2	3	3	2	5	2	2	0	4	0	0	0	0	0	0
8	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
8	9	0	17	2	2	3	2	4	5	0	9	1	0	1	1	1	0	0
1	7	8	18	1	2	2	3	2	4	0	1	0	0	0	1	0	0	1
1	7	0	13	2	3	3	2	0	5	0	5	1	0	1	1	1	0	1
1	8	4	10	1	4	7	2	5	5	2	16	4	3	0	0	0	0	0
8	7	0	12	2	2	5	2	1	4	0	5	0	0	1	0	0	0	0
1	0	0	8	2	2	6	2	5	0	2	16	4	3	0	0	0	0	0
8	6	5	2	2	2	1	2	1	1	0	3	0	0	0	1	0	0	1
1	5	0	17	2	4	3	2	0	3	0	10	4	0	1	1	0	0	1
1	3	0	18	2	1	2	2	2	2	0	5	1	0	1	1	0	0	0
8	5	0	16	2	2	3	2	4	3	0	2	1	1	0	0	0	0	0
1	7	3	18	0	3	3	2	5	5	0	13	4	0	1	1	1	0	1
1	5	0	18	2	2	6	2	2	3	0	2	1	1	0	1	0	0	0
1	7	14	1	1	8	6	1	5	5	2	16	4	3	0	0	0	0	0
8	3	0	8	2	3	3	2	5	2	0	5	4	0	1	1	0	1	1
1	8	9	7	1	2	3	2	2	5	0	11	2	0	1	0	1	1	0
1	11	2	7	1	3	1	2	2	5	0	5	1	0	1	0	0	0	0
8	3	1	12	1	6	3	2	1	2	0	7	1	0	1	1	1	0	0
1	7	2	10	1	5	6	1	2	5	2	16	4	3	0	0	0	0	0
8	7	0	23	1	4	3	1	1	4	0	15	1	0	1	1	1	1	1
8	5	2	8	2	1	1	3	5	3	2	16	4	3	0	0	0	0	0
1	5	2	9	2	4	3	2	2	3	0	11	1	0	1	1	1	1	1
1	3	0	12	2	1	1	2	2	2	0	6	0	2	1	1	0	0	0
8	6	0	13	2	2	3	2	4	1	2	16	4	3	0	0	0	0	0
8	3	0	18	2	1	3	2	5	5	0	3	0	1	1	1	1	1	0
1	3	0	12	2	1	1	2	5	2	2	16	4	3	0	0	0	0	0
1	3	14	21	1	3	3	1	0	2	0	7	1	0	1	1	1	0	1
1	8	0	15	2	4	6	2	2	5	0	7	1	0	1	1	0	0	1
1	5	10	20	1	2	3	3	5	3	0	3	0	0	1	1	1	0	0
8	7	0	9	2	2	5	2	4	4	2	16	4	3	0	0	0	0	0
0	3	0	8	2	2	1	2	5	2	0	7	4	0	1	1	0	0	1
1	0	14	8	2	4	6	3	5	0	0	9	1	0	1	0	1	0	0
8	3	0	7	2	1	0	2	4	5	0	15	1	0	1	1	1	0	1
8	9	2	8	2	3	2	3	4	5	0	6	0	0	1	1	1	0	1
1	3	0	11	2	5	3	2	0	2	0	11	4	0	1	1	0	1	1
8	7	2	13	1	5	3	2	1	5	2	16	4	3	0	0	0	0	0
1	3	0	20	1	2	2	1	0	2	0	10	1	0	1	1	1	0	1
1	3	13	6	2	2	2	3	5	2	2	16	4	3	0	0	0	0	0
8	3	0	20	1	6	3	1	5	2	0	15	1	0	1	1	0	0	0
8	0	5	1	2	3	6	2	3	0	1	0	3	1	0	1	0	0	0
8	7	2	13	1	2	3	3	4	4	2	9	3	0	0	0	0	0	0
8	5	0	17	2	1	3	2	4	3	2	16	4	3	0	0	0	0	0
3	7	2	3	2	1	1	2	5	4	2	16	4	3	0	0	0	0	0
1	5	2	11	1	1	3	2	2	5	0	5	0	0	1	1	0	0	1
8	5	14	25	1	3	3	3	1	5	0	7	1	0	1	1	0	0	0
1	7	7	18	2	3	1	3	2	4	2	16	4	3	0	0	0	0	0
8	0	11	7	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	0	15	2	4	3	2	5	2	0	7	1	0	1	1	0	1	1
8	6	6	2	2	3	1	2	4	1	2	16	4	3	0	0	0	0	0
8	3	0	8	2	3	3	2	4	2	0	8	4	0	0	0	0	0	0
1	7	14	14	2	3	3	3	0	5	2	1	1	0	0	0	1	0	0
1	7	7	14	2	3	4	3	2	4	2	16	4	3	0	0	0	0	0
8	3	0	16	2	4	3	2	4	2	0	6	1	0	1	1	1	1	1
8	5	2	20	1	3	3	3	1	5	2	16	4	3	0	0	0	0	0
1	6	0	10	2	3	3	2	2	1	0	3	1	0	1	1	0	1	1
1	7	2	4	2	2	2	3	2	4	0	3	1	0	1	0	1	0	0
8	5	0	15	2	2	3	2	4	3	0	4	4	0	1	1	1	1	1
1	3	0	13	2	1	1	2	5	2	2	16	4	3	0	0	0	0	0
8	6	2	4	2	1	1	2	4	1	2	16	4	3	0	0	0	0	0
1	7	14	2	1	6	3	1	2	5	0	11	1	0	1	1	1	0	1
1	5	0	11	2	4	2	2	5	3	2	16	4	3	0	0	0	0	0
1	6	9	2	1	1	3	1	5	1	0	4	0	0	1	1	0	0	1
1	2	2	10	1	2	0	2	2	5	0	15	2	0	0	1	0	0	0
8	0	0	8	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
1	11	0	8	2	2	3	2	5	5	0	1	1	0	1	0	1	0	0
1	0	0	13	2	1	1	2	0	5	2	16	4	3	0	0	0	0	0
8	6	2	7	2	3	1	2	5	1	2	2	1	1	0	0	0	0	0
8	6	6	13	2	4	1	3	4	1	0	15	1	0	1	1	1	1	1
1	0	0	9	2	1	6	2	5	0	2	16	0	0	0	1	0	0	1
1	3	0	17	2	3	3	2	0	2	0	6	1	0	1	1	1	1	0
8	6	2	7	2	3	1	2	1	1	0	15	4	1	1	1	0	0	1
1	3	10	21	1	3	1	2	2	2	0	8	1	0	0	0	0	0	0
1	3	0	11	2	5	1	2	2	2	0	3	1	0	1	0	1	0	0
1	9	0	13	2	2	3	2	2	5	0	5	1	0	1	1	0	0	1
1	11	3	24	0	3	3	2	2	5	0	7	1	0	1	1	0	0	0
8	0	9	9	2	1	0	3	3	0	2	16	4	3	0	0	0	0	0
3	3	5	7	1	1	3	2	2	2	1	2	1	0	1	1	0	1	1
1	3	0	8	2	4	2	2	5	2	0	10	1	0	1	1	0	1	1
1	5	0	15	2	2	3	2	5	3	0	5	4	0	1	1	1	0	1
1	6	0	20	2	2	3	2	0	1	0	8	1	0	1	1	1	0	1
1	1	11	15	1	1	1	2	5	5	0	16	0	0	1	0	0	0	0
1	7	2	8	1	2	5	1	2	5	2	1	4	2	0	0	0	0	0
8	5	12	7	2	8	5	2	4	3	2	16	4	3	0	0	0	0	0
1	3	0	24	1	2	3	1	2	2	0	1	2	0	1	1	0	0	0
1	5	0	13	2	3	3	2	2	3	0	5	1	0	1	1	0	0	1
8	3	5	12	1	2	3	2	5	2	2	16	4	3	0	0	0	0	0
8	5	0	8	2	2	2	2	5	3	0	5	4	2	1	1	0	0	1
1	3	9	19	1	1	3	3	2	2	2	6	1	0	1	0	1	0	0
1	0	9	8	2	2	6	3	5	0	0	5	1	0	1	0	0	0	0
8	0	0	9	2	2	6	2	5	0	0	6	2	0	1	1	0	0	0
1	6	0	9	2	3	2	2	5	1	0	2	0	0	1	0	1	0	0
3	0	9	1	2	9	6	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	18	2	2	3	2	5	2	0	3	1	0	1	1	0	0	1
8	3	0	19	1	1	3	1	1	2	0	1	4	1	1	1	0	0	1
8	3	0	8	2	2	2	2	4	2	2	1	1	0	0	0	0	0	0
1	0	0	9	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
8	6	0	12	2	1	1	2	4	1	0	8	0	0	1	1	1	1	0
1	7	0	21	1	2	3	3	2	4	0	15	0	0	1	1	0	0	1
1	7	0	18	2	1	3	2	2	4	0	15	1	0	1	1	0	0	1
1	7	0	30	1	3	3	1	2	5	0	5	1	0	1	1	1	1	1
1	5	0	23	1	5	3	1	2	5	0	14	0	0	1	1	0	0	1
8	3	2	2	2	5	3	2	1	2	0	9	0	0	1	1	1	0	0
1	3	2	8	2	3	1	2	2	2	0	1	0	0	0	0	0	0	0
8	0	0	8	2	2	6	2	3	0	0	1	3	0	1	0	0	0	0
8	8	0	12	2	4	3	2	5	5	0	9	0	0	1	1	0	0	0
8	3	2	20	2	3	3	3	1	2	0	11	1	1	1	1	1	0	1
1	3	0	20	1	3	3	1	2	2	0	14	1	0	1	1	1	0	0
8	3	0	20	1	3	3	1	5	2	0	15	1	0	1	1	1	0	1
8	7	2	9	2	5	3	3	5	4	0	15	1	0	1	1	1	0	0
8	11	11	4	2	4	7	2	4	5	0	10	0	0	1	1	0	1	1
8	3	0	8	2	3	2	2	4	2	0	3	1	0	1	1	1	0	1
8	1	0	31	1	4	3	1	5	5	0	2	0	0	1	0	1	0	0
1	5	7	11	1	2	3	2	2	3	0	5	1	0	1	1	1	0	1
1	0	9	9	2	2	0	3	5	0	2	16	4	3	0	0	0	0	0
1	11	9	3	2	1	3	2	5	5	2	16	4	3	0	0	0	0	0
8	3	0	9	2	5	3	2	1	2	0	3	1	0	1	1	1	0	0
1	1	0	23	1	4	3	1	5	5	0	8	1	0	1	1	0	0	1
8	3	6	11	2	1	3	3	4	2	2	16	4	3	0	0	0	0	0
1	5	0	26	1	2	3	1	5	5	0	5	0	0	1	1	1	0	0
8	6	0	17	2	3	3	2	4	1	0	15	4	0	1	1	1	0	0
1	5	0	22	1	3	3	1	2	5	0	4	1	0	1	1	1	1	1
1	3	11	20	1	3	3	2	0	2	0	5	1	0	1	0	0	0	0
8	5	2	7	1	2	2	1	1	5	2	2	4	0	0	0	0	0	0
1	8	2	6	1	2	3	1	2	5	2	16	4	3	0	0	0	0	0
8	0	2	9	2	3	6	3	4	5	2	16	4	3	0	0	0	0	0
1	6	2	4	2	1	1	2	2	1	2	16	0	2	0	0	0	0	0
8	5	2	5	1	2	1	2	5	3	2	16	4	3	0	0	0	0	0
1	7	0	21	1	4	1	1	5	5	2	16	4	3	0	0	0	0	0
1	5	0	12	2	3	3	2	5	3	0	7	1	0	1	0	1	1	0
8	5	5	7	2	2	2	2	4	3	0	5	1	0	1	1	0	0	1
8	1	0	23	1	4	3	1	1	5	2	16	4	3	0	0	0	0	0
8	3	0	20	1	2	3	1	1	2	0	1	1	0	0	0	0	0	0
1	3	0	16	2	4	3	2	2	2	1	3	1	0	1	0	0	0	0
1	1	0	24	1	5	3	1	2	5	0	10	1	0	1	1	1	1	1
8	7	1	19	2	2	2	3	1	4	1	2	1	0	1	1	0	0	0
8	3	0	13	2	3	3	2	4	2	0	5	3	0	1	1	0	0	1
8	5	2	12	2	2	1	3	4	3	0	2	3	0	1	0	1	0	0
1	7	0	15	2	1	2	2	2	4	0	7	4	0	1	1	0	0	1
8	7	2	6	2	3	1	2	5	4	0	6	1	0	1	1	1	0	1
8	7	0	10	2	3	2	2	4	4	0	2	1	0	1	0	0	0	0
8	3	6	1	1	4	2	1	1	2	0	15	1	0	1	1	1	0	0
8	5	4	8	2	2	2	3	4	3	2	16	4	3	0	0	0	0	0
1	7	0	18	2	4	2	2	5	4	0	13	1	1	1	1	0	0	1
1	0	0	8	2	2	6	2	5	0	2	16	0	0	0	0	0	0	0
8	3	0	8	2	2	2	2	4	2	0	5	0	0	1	1	0	0	1
8	7	2	8	2	3	3	3	5	4	2	16	4	3	0	0	0	0	0
1	7	12	13	1	1	2	2	2	5	0	15	0	1	1	1	0	0	1
8	3	2	16	2	1	3	2	4	2	2	16	4	3	0	0	0	0	0
8	3	0	16	2	4	3	2	1	2	0	7	1	0	1	0	0	0	0
8	7	2	9	1	2	3	1	1	5	0	9	1	0	1	1	1	0	0
1	3	0	11	2	2	3	2	5	2	2	1	4	2	0	0	0	0	0
8	6	11	15	1	1	2	2	1	1	2	16	4	3	0	0	0	0	0
8	5	0	10	2	3	3	2	4	3	0	1	1	0	1	1	0	0	1
1	5	0	7	2	4	2	2	0	5	0	6	0	0	1	1	1	0	1
8	5	14	3	2	3	2	3	5	3	0	7	1	0	1	1	0	0	0
1	0	0	10	2	2	6	2	5	0	2	16	4	3	0	0	0	0	0
8	3	0	8	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
8	3	14	9	2	4	2	2	4	2	0	3	1	0	1	1	1	0	1
1	3	14	9	2	3	6	3	2	2	0	2	1	0	1	1	1	0	1
8	11	2	5	2	1	1	2	5	5	2	16	4	3	0	0	0	0	0
1	5	0	26	1	3	0	1	5	5	0	1	1	0	0	1	0	0	0
8	6	0	9	2	2	2	2	5	1	2	16	4	2	0	0	0	0	0
1	5	0	14	2	2	1	2	5	3	0	2	0	0	1	1	0	0	1
1	3	0	9	2	5	2	2	0	2	0	6	1	0	1	1	1	0	1
1	7	0	23	1	4	2	1	5	4	0	9	1	0	1	1	0	0	1
8	0	0	8	2	2	6	2	3	0	2	2	3	0	0	0	0	0	0
1	3	0	10	2	3	1	2	0	2	0	1	0	1	1	1	0	0	1
8	7	0	13	2	2	6	2	1	4	0	4	1	0	1	1	1	0	1
8	7	2	11	1	3	1	1	1	5	0	13	3	0	0	0	0	0	0
1	9	0	13	2	4	3	3	0	5	0	3	0	0	1	1	0	0	0
1	3	0	12	2	6	3	2	0	2	0	15	1	0	1	1	0	0	1
1	0	0	10	2	2	6	2	2	5	0	15	2	0	1	1	1	0	1
8	5	0	13	2	1	2	2	4	3	0	14	1	0	1	1	1	0	1
8	0	0	9	2	1	0	2	3	0	0	9	4	0	1	1	0	0	0
3	6	12	2	2	4	2	2	2	1	0	15	4	0	1	1	1	0	0
1	6	2	5	2	5	1	2	2	1	0	11	4	0	0	0	0	0	0
1	7	0	17	2	2	1	2	5	4	0	7	4	0	0	0	0	0	0
8	6	5	20	1	4	3	3	4	1	0	8	4	0	1	1	0	0	1
1	8	2	4	1	2	3	1	5	5	2	16	4	3	0	0	0	0	0
8	3	0	10	2	4	3	2	5	2	2	0	1	0	1	0	0	0	0
1	3	0	20	1	5	3	1	2	2	0	15	1	0	1	1	1	0	1
8	3	0	20	1	4	3	1	5	2	0	9	0	1	1	1	0	1	1
8	3	0	11	2	3	1	2	4	2	1	1	1	0	1	1	0	0	1
8	8	2	9	1	3	3	2	1	5	0	11	1	0	1	1	1	0	0
8	5	2	6	2	1	1	2	4	3	0	11	4	1	1	1	0	1	1
8	5	9	8	1	1	3	1	1	5	0	3	0	2	1	1	0	0	1
1	9	2	4	1	3	6	1	5	5	0	5	1	1	0	1	0	0	1
1	5	0	20	1	3	5	1	5	3	0	6	1	0	0	0	0	0	0
8	3	0	12	2	6	3	2	4	2	0	7	1	0	1	1	1	1	1
1	3	0	12	2	4	3	2	0	2	0	0	1	0	0	0	0	0	0
8	0	0	10	2	3	6	2	5	5	2	16	4	3	0	0	0	0	0
1	0	11	2	2	1	6	3	5	0	0	2	0	1	0	1	0	0	1
1	9	2	7	1	1	1	1	0	5	0	3	3	0	0	0	0	0	0
1	3	0	12	2	2	3	2	0	2	0	3	1	0	1	1	1	0	0
8	0	9	9	2	3	6	3	3	0	0	10	1	0	1	0	0	0	0
8	3	0	19	2	2	2	2	4	2	0	2	4	0	1	1	0	1	1
1	5	0	27	1	4	2	1	5	3	0	5	1	0	1	1	1	0	1
8	6	2	5	2	1	1	2	5	1	2	16	4	3	0	0	0	0	0
1	7	0	16	2	2	3	2	0	4	0	6	0	0	1	1	0	0	1
1	3	3	14	1	2	3	3	2	2	0	3	2	0	1	1	1	0	0
8	3	0	13	2	6	3	2	4	2	2	16	4	3	0	0	0	0	0
8	3	2	8	2	2	1	3	4	2	2	16	4	3	0	0	0	0	0
1	5	13	12	1	3	3	3	5	5	0	4	1	0	1	1	0	1	1
1	7	0	20	1	2	7	1	2	4	2	16	4	3	0	0	0	0	0
8	5	0	24	1	4	3	1	1	5	0	2	1	0	0	0	0	0	0
1	5	2	8	2	4	1	2	2	3	0	7	1	0	1	1	1	0	1
1	0	0	9	2	4	6	2	5	0	0	1	4	0	1	1	1	0	1
8	0	0	9	2	3	0	2	3	0	0	9	4	0	1	1	0	0	1
8	7	0	18	2	3	3	2	4	4	0	2	0	1	1	1	0	1	1
1	1	10	18	1	5	3	3	5	5	0	15	1	0	1	1	1	0	1
1	3	0	11	2	4	0	2	5	2	0	7	1	0	1	1	1	0	1
1	5	2	8	1	1	6	2	0	3	0	4	0	0	1	1	0	0	0
1	9	14	14	1	4	3	2	0	5	0	15	4	1	1	1	1	0	0
1	7	0	9	2	3	2	2	5	4	0	5	4	1	0	0	0	0	0
8	11	14	2	2	5	3	2	4	5	2	16	4	3	0	0	0	0	0
1	3	9	9	2	4	2	3	2	2	0	3	3	1	1	0	1	0	0
8	3	0	13	2	3	3	2	4	2	0	4	1	0	1	1	0	0	1
8	8	0	21	1	2	3	1	5	5	2	16	4	3	0	0	0	0	0
8	5	0	20	1	3	1	1	5	3	0	15	1	0	1	1	0	0	0
8	3	0	9	2	3	2	2	4	2	0	2	1	0	1	1	0	1	1
1	0	0	14	2	2	6	2	0	5	2	1	4	0	1	0	0	0	0
1	0	9	9	2	1	0	3	5	0	2	16	4	3	0	0	0	0	0
8	3	0	8	2	4	2	2	5	2	0	10	0	0	1	1	1	0	1
8	5	0	10	2	1	1	2	4	3	2	16	4	3	0	0	0	0	0
1	9	0	13	2	2	3	2	2	5	2	1	4	0	0	0	0	0	0
1	11	0	14	2	3	3	2	0	5	0	2	4	0	0	0	0	0	0
1	0	12	3	2	2	0	3	5	0	2	3	3	1	1	0	0	0	0
1	3	0	14	2	3	3	2	5	2	0	4	1	0	1	1	0	0	0
8	7	0	13	2	2	3	2	4	5	0	2	0	0	1	1	0	1	1
8	3	0	16	2	4	3	2	4	2	0	9	1	0	1	1	0	0	1
8	3	5	7	1	1	3	1	5	2	2	16	4	3	0	0	0	0	0
1	7	8	15	0	3	3	1	5	5	0	9	1	0	1	1	0	0	0
8	5	2	5	2	1	1	2	5	3	2	16	4	3	0	0	0	0	0
8	1	0	23	1	4	3	1	1	5	0	4	1	0	1	1	1	0	1
1	1	0	24	1	3	3	1	5	5	0	5	1	0	1	1	1	1	1
1	8	2	4	1	1	1	1	2	5	2	16	4	3	0	0	0	0	0
8	6	13	3	2	1	1	2	5	1	2	16	0	0	1	0	0	0	0
8	2	2	3	1	1	0	2	1	5	0	15	2	0	1	1	1	0	1
1	3	2	10	1	2	0	1	0	5	2	16	4	3	0	0	0	0	0
1	6	2	16	1	1	3	3	2	1	0	3	0	0	1	1	0	0	1
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	6	5	11	2	2	1	3	4	1	0	15	1	0	1	1	1	0	1
8	6	2	9	1	1	1	2	5	1	2	16	4	3	0	0	0	0	0
8	7	0	8	2	2	3	2	4	4	2	0	0	1	0	0	0	0	0
8	6	2	17	2	2	1	2	4	1	0	4	0	0	1	1	1	0	1
1	5	0	20	1	2	3	1	0	3	0	4	3	0	1	0	1	0	0
8	3	0	20	1	4	3	1	1	2	0	15	1	0	1	1	1	1	1
1	5	0	12	2	2	3	2	0	3	0	2	1	0	1	1	0	0	1
1	9	2	9	2	2	1	3	5	5	0	13	0	1	1	1	0	1	1
8	3	9	13	2	4	3	3	5	2	0	11	1	0	1	1	0	0	1
8	3	6	6	2	3	3	2	5	2	0	12	0	0	1	1	1	0	0
1	7	11	9	2	1	4	3	2	4	2	16	4	3	0	0	0	0	0
1	6	0	14	2	2	3	2	0	1	0	2	1	0	1	1	0	0	1
1	5	1	9	1	3	3	2	0	5	0	6	0	0	1	0	1	0	0
8	5	7	16	2	4	3	2	5	3	0	10	1	0	1	1	0	0	1
8	11	2	15	1	3	5	2	1	5	2	16	4	3	0	0	0	0	0
8	0	14	8	2	1	6	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	15	2	6	3	2	5	2	0	9	1	1	1	1	0	0	0
1	3	9	14	2	1	3	3	5	2	2	16	4	3	0	0	0	0	0
8	3	11	11	1	2	3	2	1	2	0	2	1	1	1	1	0	0	0
8	7	0	18	2	3	1	2	4	4	2	16	4	3	0	0	0	0	0
1	3	0	21	1	6	3	1	2	5	0	4	1	0	1	1	1	0	1
1	0	0	9	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
1	5	7	1	2	3	2	2	4	3	0	7	1	0	1	0	0	0	0
8	5	6	2	2	4	3	2	1	3	0	9	1	0	1	1	0	0	0
1	5	0	18	2	4	3	2	0	3	0	6	1	0	1	1	1	0	1
8	7	2	10	2	1	3	3	4	4	2	16	4	3	0	0	0	0	0
5	5	5	9	2	3	3	2	4	3	0	7	1	0	1	1	0	0	0
8	3	0	20	1	3	2	1	4	2	0	15	1	0	1	1	1	0	1
1	3	10	18	2	6	3	3	2	2	0	4	1	0	1	1	1	0	1
1	7	0	15	2	4	3	2	2	4	0	15	1	0	1	1	1	0	0
8	2	12	5	2	4	0	2	5	5	0	7	1	0	1	1	0	0	1
1	7	0	14	2	3	0	2	0	5	2	16	4	3	0	0	0	0	0
1	6	10	10	2	3	2	3	0	1	0	5	2	0	1	1	1	0	1
1	7	9	5	1	1	1	2	2	4	0	1	0	0	1	1	0	0	1
8	0	1	1	2	2	6	2	4	5	0	2	0	0	1	1	0	0	1
8	3	8	2	2	5	3	2	1	2	0	1	0	1	1	1	1	1	1
1	0	0	9	2	2	0	2	5	0	2	4	2	0	1	1	1	0	0
8	3	0	10	2	3	2	2	4	2	0	5	4	0	1	1	0	1	1
1	3	0	9	2	1	2	2	0	2	0	3	1	0	1	0	0	0	0
8	11	14	1	2	2	3	2	4	5	0	9	1	1	1	1	0	0	1
8	5	0	20	1	3	3	1	4	3	2	16	4	3	0	0	0	0	0
1	0	0	9	2	1	6	2	5	0	0	4	1	0	1	0	1	0	0
1	7	0	9	2	2	2	2	5	4	2	1	4	0	0	0	0	0	0
8	9	9	3	2	2	3	2	1	5	2	16	4	3	0	0	0	0	0
8	7	0	8	2	1	0	2	4	4	0	3	3	0	1	1	0	0	1
8	5	2	7	1	1	1	2	5	3	0	15	0	0	1	1	1	0	1
3	0	9	7	2	3	0	3	3	0	2	16	3	1	0	0	0	0	0
1	3	8	11	1	4	3	2	5	2	0	6	2	0	1	1	0	1	1
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	0	16	2	1	3	2	2	2	0	8	4	0	1	1	0	0	1
1	0	1	1	2	4	0	2	5	5	0	10	0	0	1	1	1	0	0
1	0	0	8	2	2	0	2	5	0	0	2	2	0	1	0	0	0	0
8	9	0	9	2	2	2	2	5	5	0	4	4	0	1	0	1	0	0
8	0	12	1	2	2	6	2	5	0	2	16	4	3	0	0	0	0	0
1	8	5	24	1	4	7	2	2	5	0	14	0	0	1	0	0	1	0
8	6	0	14	2	3	3	2	4	1	0	1	1	1	1	1	0	0	1
1	6	2	3	1	6	3	2	2	1	2	16	4	3	0	0	0	0	0
8	7	2	4	2	4	1	2	4	4	0	1	4	1	1	1	0	0	1
8	3	0	17	2	4	3	2	4	2	0	9	1	0	1	1	0	1	1
8	0	0	8	2	9	6	2	3	0	2	1	0	2	1	1	0	0	0
8	7	0	19	1	4	3	2	4	4	0	1	1	0	1	1	0	1	1
1	5	0	10	2	1	6	2	2	3	2	6	3	0	1	1	0	0	1
8	0	0	8	2	3	6	2	3	0	0	3	4	0	1	1	0	0	0
8	9	0	19	1	2	2	1	1	5	0	0	1	0	1	1	0	0	0
8	6	0	13	2	3	3	2	1	1	0	6	0	0	1	1	0	0	1
8	7	2	10	2	1	1	3	4	4	2	16	4	3	0	0	0	0	0
8	5	14	8	2	1	3	3	4	3	2	16	4	3	0	0	0	0	0
1	7	5	2	2	3	3	2	2	4	0	2	4	0	0	0	0	0	0
8	7	0	19	1	2	3	1	5	4	0	6	4	0	1	1	0	1	1
8	3	1	2	1	1	3	2	5	5	2	2	3	0	0	0	0	0	0
8	3	2	16	2	3	0	3	1	2	0	2	1	1	1	1	0	0	0
8	6	0	21	1	4	3	1	1	1	0	15	1	0	1	1	1	0	1
1	3	6	18	1	3	5	3	2	2	0	8	1	0	1	1	0	0	1
1	7	0	12	2	1	3	2	0	4	0	15	1	0	1	1	1	0	1
1	3	0	19	2	4	3	2	5	2	0	9	4	0	1	1	0	0	0
8	9	2	6	2	1	1	2	1	5	0	6	0	1	1	1	0	0	1
7	3	6	9	2	2	3	3	0	2	0	15	1	0	1	1	0	0	1
8	11	2	4	2	3	2	3	4	5	2	16	4	3	0	0	0	0	0
1	11	0	31	1	3	3	1	2	5	0	10	1	0	1	0	0	0	0
8	1	9	17	1	4	3	3	1	5	0	4	1	0	1	1	1	1	1
8	6	0	15	2	2	2	2	4	1	0	4	0	1	1	1	0	0	1
8	7	0	16	2	3	3	2	4	5	0	4	4	0	1	0	0	0	0
8	11	2	7	2	5	1	3	4	5	0	13	4	1	0	0	1	0	0
8	7	0	16	2	1	3	2	5	4	0	4	0	0	1	1	0	0	1
1	3	0	11	2	4	3	2	5	2	0	15	1	0	1	1	0	0	0
1	8	1	6	1	1	2	2	0	5	2	2	1	1	0	0	0	0	0
1	0	9	8	2	2	0	3	5	0	2	16	4	3	0	0	0	0	0
1	6	0	13	2	2	3	2	2	1	2	0	4	1	0	0	0	0	0
8	3	0	20	1	3	3	1	5	2	2	16	4	3	0	0	0	0	0
1	3	0	20	1	5	3	1	2	2	0	3	1	0	1	1	0	0	1
1	11	9	19	1	1	5	2	5	5	0	3	1	0	1	1	0	0	1
1	1	0	20	1	5	3	1	5	5	0	4	1	0	1	1	1	0	0
8	7	5	7	2	1	1	2	5	5	2	16	4	3	0	0	0	0	0
1	7	2	11	2	2	1	2	2	4	0	8	1	0	1	1	1	1	0
1	3	0	21	1	1	3	1	5	2	2	16	4	3	0	0	0	0	0
1	1	0	24	1	3	3	1	5	5	2	6	4	0	0	1	0	0	0
8	7	0	22	1	1	2	1	5	4	0	1	1	0	1	1	0	0	1
1	6	9	7	2	3	3	2	0	1	0	7	4	1	1	1	0	0	1
8	7	5	9	1	2	3	1	5	5	2	16	4	3	0	0	0	0	0
1	7	0	31	1	4	3	1	0	5	0	15	1	0	1	1	0	1	1
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	8	0	13	2	2	3	2	4	5	0	9	4	2	1	1	0	0	1
1	7	3	11	1	2	3	2	5	4	2	16	4	3	0	0	0	0	0
1	11	0	20	1	2	3	1	0	5	0	6	1	0	1	1	1	0	1
1	3	7	2	2	4	3	2	5	2	0	6	1	0	1	1	1	0	1
1	6	10	16	2	1	3	2	0	1	0	14	1	0	1	1	1	0	1
1	3	0	17	2	1	3	2	0	2	0	2	4	0	0	1	0	0	0
8	7	2	7	1	1	1	2	5	4	2	16	3	1	0	0	0	0	0
8	6	2	5	2	1	1	2	4	1	0	14	1	0	1	1	0	0	0
1	5	6	1	1	1	1	1	2	5	0	15	1	0	1	1	1	0	1
5	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	5	7	11	1	3	3	2	5	5	2	16	4	3	0	0	0	0	0
1	3	0	14	2	3	3	2	5	2	2	16	4	3	0	0	0	0	0
3	0	2	8	2	2	0	2	2	5	2	16	4	3	0	0	0	0	0
1	0	14	8	2	2	2	3	5	5	0	10	0	0	1	1	0	0	1
1	3	6	8	2	5	6	2	0	2	0	9	1	0	1	1	0	1	1
8	2	5	12	2	1	0	3	5	5	0	15	1	0	1	1	1	0	0
6	6	3	7	1	2	1	2	5	1	2	16	4	3	0	0	0	0	0
8	5	13	1	2	1	3	2	1	3	2	16	0	1	1	1	0	0	1
1	1	14	22	1	4	3	3	5	5	0	3	0	1	1	1	0	0	1
8	7	5	5	1	5	6	2	5	4	0	8	1	0	1	1	0	0	0
8	0	0	8	2	1	6	2	3	0	0	11	1	0	0	0	0	0	0
1	3	0	9	2	1	1	2	5	2	2	16	4	1	0	0	0	0	0
8	3	0	20	1	6	3	1	1	2	0	15	1	0	1	1	1	0	1
8	6	9	2	1	2	3	2	5	1	2	5	4	1	1	1	0	0	1
1	7	0	31	1	3	2	1	2	5	2	16	4	3	0	0	0	0	0
8	6	5	9	2	2	0	2	3	1	2	3	0	0	1	0	0	0	0
8	3	9	7	1	1	3	2	5	5	0	5	1	0	1	1	0	0	1
8	0	0	8	2	2	6	2	3	0	0	2	2	0	1	0	0	0	0
1	3	0	13	2	1	3	2	5	2	2	2	1	1	1	1	0	0	1
1	7	0	14	2	1	3	2	0	4	2	1	3	0	1	0	0	0	0
1	3	0	13	2	1	1	2	5	2	0	2	3	0	0	1	0	0	0
8	6	0	13	2	1	1	2	4	1	2	16	4	3	0	0	0	0	0
1	0	0	8	2	1	0	2	5	0	0	10	2	0	1	0	1	1	0
1	6	2	10	1	2	3	1	0	5	2	16	4	3	0	0	0	0	0
8	7	5	5	2	1	3	2	4	4	2	16	4	1	1	1	0	0	1
1	9	4	2	2	1	3	2	2	5	2	16	4	3	0	0	0	0	0
1	3	7	1	2	4	3	2	5	2	0	3	2	0	1	1	0	1	1
8	0	5	1	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	5	9	7	1	2	6	2	5	3	0	4	1	0	1	1	0	0	0
8	3	0	12	2	5	3	2	4	2	0	15	0	1	1	1	1	0	1
8	5	9	10	2	2	1	3	1	3	2	16	4	3	0	0	0	0	0
8	3	0	15	2	2	3	2	4	2	0	2	1	1	1	0	0	0	0
8	9	6	11	2	3	3	3	4	5	0	5	1	0	1	1	0	0	1
8	9	0	12	2	3	1	2	5	5	0	5	1	0	1	0	0	0	0
8	8	2	2	1	3	1	2	1	5	2	16	4	3	0	0	0	0	0
1	3	0	14	2	4	2	2	5	2	0	8	1	0	1	1	1	0	1
8	6	0	13	2	3	6	2	5	1	0	9	1	0	1	1	1	0	1
1	3	14	15	1	2	3	3	5	2	2	16	4	3	0	0	0	0	0
1	9	0	31	1	3	3	1	0	5	2	16	4	3	0	0	0	0	0
8	0	0	9	2	9	0	2	3	0	2	9	4	0	1	1	0	0	1
8	5	0	20	1	2	3	1	4	3	2	16	4	3	0	0	0	0	0
8	3	0	18	1	2	1	2	5	2	0	4	0	0	0	1	0	0	1
1	7	7	15	2	4	2	3	2	4	0	8	2	0	1	1	1	0	1
1	11	0	31	1	2	3	1	0	5	0	2	1	0	1	1	1	0	1
1	6	2	9	2	4	1	3	2	1	0	15	1	0	1	1	0	0	0
8	3	9	17	2	1	1	2	5	2	2	16	4	3	0	0	0	0	0
8	6	11	6	2	1	1	2	5	1	0	8	1	0	1	1	0	0	1
1	13	10	26	1	3	3	2	5	5	0	5	4	0	1	1	1	1	1
8	3	0	8	2	5	3	2	4	2	0	11	1	0	1	1	1	0	0
8	3	5	10	1	2	5	2	5	2	2	16	4	3	0	0	0	0	0
8	3	0	9	2	2	2	2	4	2	0	3	1	0	1	1	1	0	0
1	7	6	2	1	4	3	1	0	5	0	15	1	0	1	1	1	0	1
8	5	13	24	1	3	5	3	5	5	0	9	1	0	1	1	1	1	1
1	1	0	22	1	2	3	1	5	5	2	16	3	0	1	0	0	0	0
1	3	0	11	2	4	3	2	5	2	0	4	1	0	1	1	1	0	1
8	5	11	5	2	1	1	2	5	3	2	16	4	0	1	0	0	0	0
8	3	0	12	2	1	3	2	4	2	2	16	4	3	0	0	0	0	0
1	9	2	9	1	2	6	1	0	5	0	2	4	3	0	0	0	0	0
8	3	9	9	2	2	3	3	4	2	2	16	4	3	0	0	0	0	0
1	5	0	17	2	2	3	2	2	3	2	16	4	3	0	0	0	0	0
1	3	6	14	2	1	2	2	2	2	2	1	0	1	1	1	0	0	1
8	7	2	10	1	1	2	2	1	4	2	9	4	0	1	1	1	0	0
1	7	6	11	0	3	3	1	5	5	0	16	3	1	0	0	0	0	0
1	9	2	18	1	1	2	2	2	5	2	2	0	0	1	1	0	0	0
1	11	6	18	1	4	3	2	2	5	0	6	0	0	1	0	0	0	0
8	2	9	7	2	2	0	2	1	5	0	10	0	0	1	1	0	1	1
1	7	2	11	1	2	3	1	0	5	0	3	1	0	1	1	0	0	1
8	11	7	18	1	2	3	3	4	5	0	1	1	0	1	1	0	1	1
8	1	14	18	2	3	1	2	5	5	0	6	1	0	0	1	0	0	1
1	7	0	18	2	4	3	2	5	4	0	8	0	0	1	1	1	0	0
1	5	0	12	2	1	3	2	5	3	0	6	1	0	1	1	1	0	1
1	5	3	21	1	2	1	1	5	5	2	16	0	1	0	0	0	0	0
1	0	0	10	2	1	6	2	3	0	2	1	3	2	0	0	0	0	0
1	6	6	1	2	1	2	2	2	1	0	13	1	0	1	1	0	0	1
1	5	5	3	2	2	6	2	5	3	2	1	0	0	0	0	0	0	0
8	8	0	18	2	5	3	3	4	5	0	9	1	0	1	0	1	0	0
8	5	0	12	2	3	3	2	4	3	0	9	1	0	1	1	0	0	0
8	6	2	9	2	2	3	3	5	1	0	1	3	0	1	0	0	0	0
8	0	0	9	2	2	6	2	3	0	0	2	2	0	1	1	0	0	1
1	0	0	9	2	8	6	2	5	0	2	16	3	0	1	0	0	0	0
1	3	0	18	2	4	3	2	0	2	0	8	4	0	1	1	0	0	1
1	11	2	5	1	1	6	2	0	5	0	15	1	0	1	1	1	0	1
1	6	2	3	2	3	1	2	5	1	0	3	0	0	1	1	0	0	1
1	2	2	3	2	1	0	2	2	5	2	16	4	1	1	1	0	0	1
8	3	0	9	2	3	2	2	4	2	0	6	1	0	1	1	1	0	1
8	7	0	10	2	3	3	2	4	4	0	8	4	0	1	1	1	1	1
8	0	2	10	1	1	3	1	5	5	0	3	0	0	1	0	0	0	0
8	0	14	8	2	3	0	3	4	0	2	16	4	3	0	0	0	0	0
8	6	5	2	2	2	3	2	1	1	0	15	4	0	1	1	1	0	0
1	9	2	7	2	1	1	3	2	5	0	9	1	0	1	1	0	0	0
1	6	0	16	2	3	3	2	0	1	0	7	1	0	1	1	0	0	0
1	6	2	5	2	1	1	2	2	1	0	2	4	0	0	0	0	0	0
1	5	0	14	2	3	3	2	2	3	0	14	1	0	1	1	0	0	0
8	7	4	3	2	2	2	2	4	4	0	3	0	0	1	0	0	0	0
1	0	0	6	3	1	0	3	5	0	0	10	3	0	0	0	0	0	0
1	3	2	9	1	3	3	2	2	2	0	1	1	0	1	1	0	0	1
1	3	5	7	2	2	3	2	5	2	0	3	3	2	1	1	0	0	1
1	0	0	14	2	3	6	2	2	5	0	6	1	0	1	1	0	1	1
1	6	8	1	1	2	3	1	0	5	0	2	1	1	0	1	0	0	1
1	6	3	5	2	2	3	2	2	1	0	3	0	0	1	1	1	0	0
8	3	13	12	1	2	3	2	5	2	0	5	1	0	1	1	1	0	1
1	0	12	2	2	3	6	3	5	5	2	1	1	0	0	0	0	0	0
1	5	0	20	1	5	2	1	5	5	0	15	1	0	1	1	0	0	1
1	6	2	11	2	3	2	3	2	1	0	15	1	0	1	1	0	0	0
1	3	0	10	2	4	2	2	0	2	0	5	1	1	0	1	0	0	0
1	1	0	26	1	4	3	1	0	5	0	9	1	1	1	1	1	1	0
8	3	5	7	2	1	3	2	4	2	0	4	4	0	1	1	0	0	1
8	6	2	14	2	1	1	2	4	1	1	2	0	1	1	1	0	0	0
8	0	9	9	2	2	6	3	3	0	0	5	4	1	1	0	0	0	0
1	11	9	14	1	5	3	2	2	5	0	15	0	0	1	1	1	0	1
8	9	10	20	1	2	3	2	5	5	2	16	4	3	0	0	0	0	0
1	6	2	7	1	1	1	2	0	1	2	16	4	3	0	0	0	0	0
8	3	6	2	1	1	1	1	1	5	2	16	4	3	0	0	0	0	0
8	5	0	12	2	1	1	2	4	3	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	0	2	3	0	0	6	1	0	1	1	0	0	1
1	5	0	22	1	4	2	1	0	5	2	2	4	1	0	0	1	0	0
8	7	0	20	1	5	3	1	1	4	0	15	1	0	1	1	1	0	1
8	6	2	4	2	4	1	2	5	1	0	5	1	0	1	1	0	0	1
1	7	0	30	1	6	0	2	2	5	0	15	1	1	1	1	1	0	1
1	6	11	3	2	2	1	2	2	1	2	16	4	3	0	0	0	0	0
1	7	2	4	2	1	1	2	5	4	0	8	1	0	1	1	0	0	1
1	7	2	14	2	1	2	3	5	4	0	3	1	0	1	0	0	0	0
1	3	0	22	1	2	3	1	2	2	0	10	1	0	1	1	0	0	1
1	3	6	9	2	4	3	3	0	2	2	16	4	3	0	0	0	0	0
1	3	0	8	2	7	3	2	0	2	0	8	0	0	1	1	1	1	1
1	3	0	20	1	3	3	1	2	2	2	16	4	3	0	0	0	0	0
8	0	0	9	2	2	6	2	5	0	2	1	1	0	1	1	0	0	1
8	5	5	11	2	1	3	3	4	3	2	16	4	3	0	0	0	0	0
1	0	14	1	1	4	0	1	2	5	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
8	11	0	19	2	2	3	2	4	5	2	16	4	3	0	0	0	0	0
8	7	0	20	2	2	5	2	1	4	0	4	3	1	1	1	0	0	1
1	7	0	30	1	4	3	1	0	4	0	6	1	0	1	1	0	1	1
1	7	2	6	1	2	1	2	2	4	0	12	3	1	1	1	0	0	1
1	7	0	22	1	5	3	1	2	4	0	1	1	0	1	1	0	0	0
8	3	1	13	2	4	3	3	1	2	0	3	1	0	1	0	0	0	0
8	7	10	11	2	3	3	2	4	4	0	4	1	0	1	1	1	0	0
1	7	10	10	2	4	2	3	0	4	0	14	1	0	1	1	1	0	1
8	5	0	16	2	1	2	2	4	3	0	13	1	0	1	1	0	0	0
1	3	0	14	2	3	3	2	2	2	0	7	1	0	1	1	0	0	0
1	11	0	27	1	2	2	1	5	5	2	1	4	0	0	0	0	0	0
1	5	9	7	2	1	0	3	2	3	0	9	4	0	0	1	0	0	1
1	3	0	22	1	4	3	1	2	2	0	7	4	1	1	1	1	0	1
8	2	5	8	2	2	0	2	4	5	0	3	3	2	1	1	1	0	0
8	11	0	19	2	1	3	2	4	5	0	2	4	2	0	0	0	0	0
1	2	2	9	2	1	0	3	2	5	2	1	4	0	0	0	0	0	0
1	11	10	22	1	9	4	2	5	5	2	16	4	3	0	0	0	0	0
1	7	0	25	1	4	3	1	2	5	0	6	1	0	1	1	0	0	1
8	5	14	1	2	2	3	2	1	3	0	11	0	1	1	0	0	0	0
8	5	14	18	1	2	3	2	1	5	0	4	4	0	1	1	0	0	1
8	6	13	7	2	1	3	3	4	1	2	16	4	3	0	0	0	0	0
1	6	6	1	1	1	3	1	0	5	2	16	4	3	0	0	0	0	0
8	5	0	15	2	2	3	2	5	5	2	9	0	0	0	0	0	0	0
1	1	6	21	1	3	3	3	2	5	0	6	3	0	1	1	0	0	1
8	0	0	9	2	2	6	2	3	0	0	1	1	0	0	0	0	0	0
8	0	0	9	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	2	17	1	4	3	2	5	5	0	8	0	0	1	1	0	1	1
8	5	0	25	1	2	1	2	1	5	0	4	1	0	1	1	1	0	1
1	0	5	8	1	2	0	2	2	5	2	4	2	0	0	0	0	0	0
1	3	0	12	2	4	3	2	0	2	0	15	0	1	1	1	0	0	1
1	6	6	2	1	3	6	1	5	5	2	16	4	3	0	0	0	0	0
1	3	0	11	2	3	3	2	2	2	0	4	4	1	1	1	0	0	1
8	7	2	9	2	1	1	2	1	4	2	16	4	3	0	0	0	0	0
1	8	2	5	1	3	3	1	2	5	0	4	1	1	1	1	1	0	1
1	3	0	12	2	2	1	2	5	2	0	1	1	0	1	1	1	1	1
8	11	3	5	2	1	6	2	5	5	2	16	4	3	0	0	0	0	0
8	7	0	18	2	3	3	2	4	4	0	5	3	0	1	1	0	1	1
1	5	8	10	1	3	1	2	0	5	0	13	0	1	1	1	1	0	0
1	2	2	5	2	3	0	2	0	5	0	15	3	1	0	1	0	0	1
8	3	2	8	1	2	3	1	1	5	2	6	0	0	1	1	0	0	0
1	0	11	3	2	1	0	2	5	0	2	1	4	3	0	0	0	0	0
1	3	14	15	1	2	3	3	2	2	0	4	1	0	1	1	1	0	1
1	5	0	26	1	4	3	1	5	5	0	8	1	0	1	1	0	0	1
8	9	0	11	2	2	2	2	5	5	2	16	4	3	0	0	0	0	0
1	9	0	30	1	2	1	1	0	5	2	16	4	3	0	0	0	0	0
8	3	5	2	1	3	3	2	2	2	0	3	1	0	1	1	0	0	0
1	6	3	8	2	4	2	2	0	1	0	3	3	1	0	0	0	0	0
1	0	9	8	2	1	0	3	5	0	0	3	0	0	0	1	1	0	0
1	1	2	7	1	1	1	2	0	5	0	15	1	0	1	1	1	0	1
8	6	2	3	1	2	6	2	1	1	0	15	0	0	1	1	0	0	1
8	7	0	17	2	3	1	2	1	4	2	16	4	3	0	0	0	0	0
1	3	0	12	2	4	3	2	5	2	0	14	1	0	1	1	1	0	0
8	0	14	7	2	2	0	3	3	0	2	16	4	3	0	0	0	0	0
8	3	0	11	2	3	1	2	4	2	0	9	1	1	1	0	0	0	0
1	11	0	10	2	9	4	2	5	5	2	16	4	3	0	0	0	0	0
1	5	8	11	1	3	1	2	2	5	0	2	1	0	1	1	0	0	1
8	7	7	6	2	6	3	3	4	4	0	15	1	0	1	1	1	0	1
8	5	0	14	2	2	3	2	4	3	2	16	4	3	0	0	0	0	0
8	3	0	10	2	1	2	2	4	2	0	9	2	0	1	1	0	0	0
8	3	6	8	2	4	3	3	1	2	2	16	4	3	0	0	0	0	0
8	6	0	12	2	4	1	2	1	1	0	8	2	0	1	1	0	0	1
8	3	10	19	1	1	3	3	4	2	2	16	4	3	0	0	0	0	0
8	7	0	26	1	2	5	1	1	4	2	4	3	0	0	0	0	0	0
1	6	2	8	1	1	1	1	0	5	2	3	3	1	0	0	0	0	0
8	0	9	9	2	3	0	3	3	0	0	4	1	0	1	0	1	0	0
1	3	9	11	2	4	3	3	2	2	0	5	1	0	1	1	1	0	1
8	6	0	31	1	3	3	1	1	1	0	13	1	0	1	1	1	1	1
8	7	0	20	1	2	3	1	5	5	2	16	4	3	0	0	0	0	0
1	3	0	15	2	2	3	2	5	2	0	5	1	0	1	1	1	0	1
1	3	0	10	2	4	2	2	2	2	0	15	1	0	1	1	0	0	1
8	6	6	8	2	3	3	3	1	1	0	3	1	0	1	0	1	1	0
8	5	0	9	2	4	3	2	4	3	0	1	1	0	1	1	0	0	0
1	8	5	1	1	5	3	1	2	5	2	16	4	3	0	0	0	0	0
8	7	0	16	2	2	3	2	4	5	0	7	1	0	1	1	0	0	1
1	3	0	9	2	6	2	2	5	2	0	15	1	0	1	1	0	0	0
1	1	0	23	1	5	3	1	0	5	2	16	4	3	0	0	0	0	0
8	3	0	12	2	4	3	2	4	2	2	16	4	3	0	0	0	0	0
1	5	1	7	1	5	6	2	5	3	0	9	0	0	1	0	0	0	0
1	5	0	11	2	2	2	2	0	3	0	11	0	0	1	1	1	0	1
8	6	2	8	2	3	1	3	5	1	0	6	4	0	1	1	0	1	1
1	5	0	20	1	2	3	1	2	3	0	9	1	0	1	1	0	0	0
1	5	2	9	1	2	2	2	2	5	0	4	0	0	1	1	1	0	0
8	5	0	9	2	2	3	2	4	3	0	2	3	1	1	1	0	0	0
8	5	5	3	2	1	3	2	5	3	2	16	4	0	1	1	0	0	1
8	3	9	12	2	2	3	3	4	2	0	2	1	0	1	0	0	0	0
1	7	14	3	1	2	3	2	2	4	0	15	0	0	1	1	0	0	1
8	3	0	18	2	4	3	2	5	2	0	12	4	1	1	1	1	0	1
1	7	0	9	2	4	3	2	0	4	0	15	1	0	1	1	1	1	1
8	3	0	19	1	2	3	1	4	2	0	9	2	0	1	1	1	1	1
8	3	0	19	1	2	3	1	4	5	0	1	0	0	1	1	1	0	1
8	11	0	14	2	6	3	2	4	5	2	16	4	3	0	0	0	0	0
1	3	12	1	2	4	3	2	0	2	0	15	0	0	1	1	1	0	1
8	3	3	8	1	1	6	2	5	2	2	16	4	3	0	0	0	0	0
1	6	13	14	2	1	1	3	5	1	0	7	1	0	1	1	1	1	1
1	3	0	19	1	3	3	1	5	2	0	3	4	0	1	1	0	1	1
1	5	0	9	2	3	3	2	5	5	0	6	1	0	1	1	1	0	1
1	3	0	21	1	1	3	1	2	2	0	3	1	0	1	1	1	0	0
1	5	10	23	1	2	3	2	5	5	2	16	4	3	0	0	0	0	0
8	5	1	5	2	1	1	2	1	3	0	4	4	1	0	1	0	0	1
8	3	9	5	2	4	3	3	1	2	0	15	1	0	1	1	1	0	1
1	5	12	26	1	4	3	3	2	5	0	8	1	1	1	1	0	0	1
8	3	0	14	2	2	3	2	5	2	0	6	0	0	1	1	0	0	1
1	5	0	11	2	2	3	2	2	5	0	5	0	0	1	1	0	0	0
1	9	0	17	2	4	3	2	2	5	0	4	1	0	1	1	1	0	0
1	3	0	17	2	1	2	2	2	2	0	15	1	0	1	1	0	0	1
8	3	0	8	2	2	3	2	4	2	2	3	4	1	1	1	1	0	0
1	7	2	13	1	5	1	2	2	5	2	16	0	0	1	1	0	0	1
1	3	0	21	1	4	2	1	0	2	0	0	1	0	0	1	0	0	0
1	0	0	9	2	1	6	2	5	0	0	15	1	0	1	1	0	0	0
8	2	12	5	2	3	0	3	4	5	0	10	1	0	1	1	0	0	1
1	3	5	13	2	2	3	3	0	2	2	1	3	1	1	1	0	0	1
8	3	4	17	2	3	3	3	4	2	2	16	4	3	0	0	0	0	0
1	7	7	18	2	4	3	3	0	4	0	11	1	0	1	1	1	1	1
1	5	0	11	2	1	2	2	5	3	2	2	0	0	1	1	0	0	1
8	11	2	8	2	1	1	2	1	5	0	10	4	1	1	1	0	0	0
8	6	0	21	1	1	2	1	4	1	2	16	4	3	0	0	0	0	0
1	3	0	22	1	3	3	1	5	2	2	2	3	0	0	0	0	0	0
1	5	12	11	2	1	3	3	5	3	0	15	1	0	1	1	0	0	1
1	11	0	11	2	2	2	2	2	5	0	5	1	0	1	1	1	0	0
1	11	10	20	2	3	1	2	0	5	0	7	0	0	1	1	1	0	1
8	6	6	6	1	4	3	2	4	1	0	3	4	1	1	1	1	1	1
8	5	0	9	2	2	3	2	4	5	2	16	4	3	0	0	0	0	0
8	7	2	9	1	2	3	1	1	5	0	15	4	0	0	0	1	0	0
1	5	3	3	1	1	2	1	2	5	0	11	0	1	1	1	0	0	1
1	7	2	5	2	3	3	2	2	4	2	1	4	2	1	1	0	0	1
8	3	0	15	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
8	6	0	13	2	3	3	2	4	1	0	5	4	0	1	1	0	0	1
8	7	0	15	2	2	3	2	4	4	2	16	4	3	0	0	0	0	0
1	0	0	14	2	2	6	2	5	5	0	2	4	0	1	1	1	0	0
1	11	10	25	1	1	1	2	2	5	2	16	4	3	0	0	0	0	0
1	6	12	24	1	2	3	3	5	1	2	16	4	3	0	0	0	0	0
8	5	6	13	2	4	3	3	2	3	2	16	4	3	0	0	0	0	0
1	7	0	10	2	2	2	2	5	4	2	5	3	1	0	1	0	0	0
8	3	6	2	1	4	3	2	5	2	0	13	1	0	1	1	1	0	1
8	9	2	4	1	2	1	2	5	5	2	3	4	1	1	1	0	0	0
1	7	2	7	2	3	1	2	2	4	2	16	4	3	0	0	0	0	0
8	3	0	12	2	2	3	2	4	2	0	6	1	0	1	1	1	0	1
8	6	2	5	2	1	1	2	5	1	2	16	4	3	0	0	0	0	0
1	7	0	9	2	2	2	2	5	4	0	3	0	1	1	1	0	1	1
1	3	0	9	2	3	2	2	0	2	0	15	4	2	1	1	0	0	0
8	7	9	5	2	3	3	2	5	4	0	4	4	1	1	0	0	0	0
8	3	11	15	2	4	3	3	4	2	0	15	1	0	1	1	1	1	1
1	1	0	25	1	3	3	1	0	5	2	16	4	3	0	0	0	0	0
8	7	2	8	1	4	3	2	1	4	0	15	4	1	1	0	1	0	0
8	3	6	1	2	1	3	2	1	2	0	4	1	0	1	1	0	0	0
8	5	2	15	2	3	0	3	1	3	0	9	4	1	1	0	0	0	0
1	1	0	23	1	4	2	1	0	5	0	7	3	0	1	1	0	0	0
1	9	5	3	2	5	3	2	0	5	2	16	4	3	0	0	0	0	0
8	3	9	15	1	5	3	3	1	2	0	15	1	0	1	1	0	0	1
1	11	0	11	2	4	1	3	0	5	2	16	4	3	0	0	0	0	0
1	5	2	2	2	1	1	2	0	3	2	1	4	1	0	0	0	0	0
1	11	0	19	2	2	4	2	5	5	0	7	2	0	1	0	0	0	0
1	7	0	20	1	3	3	1	5	4	0	6	0	0	1	1	1	0	1
1	5	0	20	1	4	3	1	0	3	0	8	0	0	1	1	1	0	1
8	0	0	9	2	1	6	2	3	0	2	16	0	2	0	0	0	0	0
1	0	9	14	2	1	0	3	0	5	2	1	4	0	0	0	0	0	0
1	6	0	15	2	2	3	2	2	5	0	4	4	0	1	0	0	0	0
8	6	0	9	2	3	2	2	5	1	0	7	0	0	1	1	0	0	0
1	6	11	13	1	4	1	2	0	1	2	5	0	0	0	0	0	0	0
1	6	0	11	2	2	3	2	2	1	0	3	0	1	1	1	0	0	0
1	3	0	10	2	3	3	2	2	2	0	15	1	0	1	1	1	0	1
1	7	0	12	2	3	3	2	5	4	0	7	2	0	1	1	0	0	0
1	3	0	17	2	4	3	2	0	2	0	6	4	0	1	0	0	0	0
1	0	0	8	2	9	6	2	3	0	0	1	2	0	0	0	0	0	0
8	7	13	10	2	1	1	3	5	4	2	16	4	3	0	0	0	0	0
8	3	2	4	2	3	2	3	4	2	0	2	1	0	1	1	0	0	0
1	9	2	7	2	1	1	2	0	5	0	15	4	1	1	1	1	0	1
8	3	0	18	2	2	3	2	4	2	2	16	0	1	1	0	0	0	0
1	1	0	22	1	5	3	1	5	5	0	11	1	0	1	1	0	0	1
1	7	0	21	1	5	3	1	5	4	0	4	1	0	1	1	0	0	1
8	6	11	6	2	2	1	2	1	1	2	2	2	0	1	1	0	0	1
8	3	14	1	2	2	3	2	1	2	0	7	2	0	1	1	1	0	1
1	3	0	8	2	2	6	2	5	5	0	2	0	0	1	0	1	0	0
1	3	0	31	1	1	3	1	5	5	2	16	4	3	0	0	0	0	0
8	3	0	16	2	3	3	2	4	2	2	16	4	3	0	0	0	0	0
1	1	0	24	1	1	2	1	2	5	0	11	0	2	1	1	0	0	1
8	7	0	9	2	1	2	2	4	4	0	10	1	0	1	1	1	0	1
8	7	2	9	2	4	3	3	4	4	0	15	4	0	1	1	0	0	1
8	3	0	12	2	4	3	2	4	2	0	15	1	0	1	1	1	1	1
1	3	5	7	2	2	3	2	0	2	2	1	0	0	1	1	1	0	1
8	0	0	10	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
1	3	0	13	2	1	3	2	5	5	0	1	0	0	1	1	0	0	1
1	1	0	24	1	4	3	3	2	5	0	10	1	0	1	1	1	1	0
8	3	9	1	2	2	3	2	4	2	0	2	1	0	1	1	1	1	1
8	5	0	15	2	2	3	2	4	3	0	5	1	0	1	1	0	0	1
8	6	2	5	2	3	1	2	4	1	2	16	4	3	0	0	0	0	0
8	3	3	13	2	1	3	3	1	2	0	4	1	0	1	1	1	0	1
1	6	3	3	2	4	3	2	2	1	0	8	0	1	1	1	1	0	1
8	6	5	5	2	1	1	3	1	1	0	15	1	0	1	1	1	0	1
8	5	0	13	2	1	1	2	4	3	2	16	4	3	0	0	0	0	0
8	1	0	23	1	2	3	1	5	5	0	2	0	0	1	1	1	0	1
8	3	0	19	1	1	3	1	4	2	0	7	1	0	1	1	1	1	1
1	5	5	7	1	4	3	2	2	3	2	16	4	3	0	0	0	0	0
1	11	0	11	2	2	3	2	2	5	0	5	1	1	1	1	0	0	1
8	3	11	8	2	3	3	3	4	2	0	4	1	0	1	1	1	0	1
1	3	2	29	1	4	3	3	0	2	0	15	1	0	1	1	1	1	1
8	0	0	10	2	1	6	2	5	0	0	10	1	0	1	1	1	0	1
8	3	8	7	2	4	3	2	4	2	0	11	1	0	1	1	0	1	1
8	3	5	4	2	6	3	3	4	2	2	16	4	3	0	0	0	0	0
1	14	6	6	1	3	3	1	5	5	0	13	4	0	1	1	1	0	0
1	3	0	13	2	3	3	2	2	5	2	2	4	1	1	1	0	0	0
1	5	2	3	1	2	0	2	0	5	2	16	4	3	0	0	0	0	0
8	3	2	12	1	2	3	2	5	2	0	2	0	0	1	1	1	0	1
8	5	9	8	1	4	5	2	1	5	0	15	1	0	1	1	1	0	1
1	0	9	9	2	1	6	3	5	0	2	16	4	3	0	0	0	0	0
8	3	0	9	2	5	3	2	4	2	0	2	1	0	1	1	0	0	1
8	5	2	6	1	1	2	2	1	5	0	15	1	0	1	1	1	0	0
1	6	2	6	2	3	2	3	4	1	2	1	2	0	1	1	0	0	0
8	3	0	11	2	3	3	2	4	2	0	5	1	1	1	1	0	1	1
1	11	2	3	1	2	1	2	2	5	0	14	0	0	1	1	0	0	1
8	5	0	17	2	2	3	2	4	3	1	2	0	0	1	1	1	1	1
1	3	0	12	2	3	0	2	0	2	2	16	4	3	0	0	0	0	0
1	0	0	9	2	4	2	2	5	0	0	15	1	1	0	1	0	0	0
1	7	6	20	1	4	2	2	2	5	2	9	4	1	1	0	0	0	0
1	13	10	17	2	2	1	3	2	5	2	4	1	1	0	1	0	0	0
1	6	0	13	2	3	3	2	0	1	0	7	1	0	1	1	1	0	1
8	7	0	14	2	1	3	2	4	4	0	3	3	0	1	1	0	1	1
1	6	2	17	1	2	3	2	2	1	0	4	1	0	1	1	1	0	1
8	0	0	8	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
8	6	0	12	2	2	3	2	4	1	2	16	4	3	0	0	0	0	0
1	6	2	7	2	1	1	3	0	1	0	3	0	1	1	1	0	0	0
8	3	0	12	2	1	3	2	4	2	0	15	2	0	1	1	1	0	1
8	7	9	22	1	3	3	2	5	5	0	6	1	0	1	1	0	0	0
8	0	11	8	2	1	6	3	3	0	0	6	1	1	1	0	0	0	0
8	2	4	11	2	2	0	2	3	5	2	16	4	3	0	0	0	0	0
1	0	14	7	2	3	6	3	5	5	0	10	2	0	1	1	1	0	0
1	3	0	11	2	2	0	2	5	2	0	7	0	0	1	1	1	0	1
1	12	13	16	1	2	6	2	5	5	2	16	4	3	0	0	0	0	0
1	3	0	12	2	6	2	2	0	2	0	15	1	0	1	1	1	0	0
8	7	0	30	1	2	3	1	1	5	0	6	4	1	1	1	0	1	0
8	6	9	1	2	4	3	2	5	1	2	16	4	3	0	0	0	0	0
8	3	0	12	2	2	1	2	5	5	0	3	0	0	1	0	0	0	0
8	5	0	10	2	2	2	2	4	3	0	3	1	0	1	1	1	0	1
8	11	2	10	2	1	1	3	1	5	2	16	4	3	0	0	0	0	0
8	3	0	22	1	4	3	1	1	2	0	9	1	0	1	1	1	0	1
1	7	0	29	1	3	3	1	2	5	0	9	1	0	1	1	1	0	1
8	6	0	15	2	3	3	2	5	1	0	8	1	0	1	1	0	0	0
1	7	14	16	1	2	7	2	2	5	2	16	4	3	0	0	0	0	0
8	7	6	2	2	2	6	2	5	4	2	16	4	3	0	0	0	0	0
8	3	5	9	2	2	2	3	4	2	0	7	1	1	1	1	0	0	1
1	5	2	5	1	1	5	1	5	5	2	16	4	3	0	0	0	0	0
1	3	0	10	2	5	3	2	5	2	0	12	0	1	1	1	1	0	1
1	7	0	13	2	2	3	2	5	5	0	15	0	0	1	1	1	0	0
8	5	0	15	2	3	3	2	4	3	0	4	1	0	1	1	1	0	1
8	9	13	10	2	2	3	3	4	5	0	9	0	0	1	1	1	1	1
8	5	0	23	1	4	0	1	1	5	0	8	0	0	1	1	0	0	0
1	3	0	13	2	3	3	2	0	5	2	0	1	0	1	0	0	0	0
1	7	10	23	1	3	1	3	2	4	0	7	2	0	1	1	0	0	1
8	3	0	17	2	1	3	2	1	2	0	1	1	0	1	1	0	0	1
8	2	2	8	2	2	6	2	4	5	2	1	4	1	0	0	0	0	0
8	3	0	15	2	4	3	2	4	2	0	10	1	0	1	1	1	1	1
8	6	0	16	2	3	7	2	4	1	2	16	4	3	0	0	0	0	0
8	6	1	18	2	3	3	2	4	1	0	4	1	0	0	0	0	0	0
1	7	0	17	2	5	3	2	5	5	0	12	4	1	1	1	1	0	1
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	9	19	1	3	3	3	5	2	0	6	1	0	1	1	1	0	1
8	7	9	3	2	3	1	2	1	4	2	16	4	3	0	0	0	0	0
8	5	0	10	2	2	2	2	4	3	2	16	3	0	1	1	0	1	1
1	6	2	7	1	2	3	2	2	1	0	7	2	0	1	1	0	0	1
8	5	0	16	2	5	3	2	4	5	0	11	1	0	1	1	1	0	1
8	6	2	12	2	3	3	3	4	1	2	1	1	0	1	1	0	0	1
8	5	2	14	2	1	3	3	4	3	0	2	1	1	1	1	0	0	1
1	5	0	15	2	2	3	2	0	3	0	3	4	0	0	0	0	0	0
8	5	2	16	1	2	3	2	5	3	2	16	4	3	0	0	0	0	0
1	5	1	6	1	1	6	2	5	3	0	4	1	0	1	1	0	0	0
1	5	6	1	1	4	3	1	2	5	0	7	1	1	0	0	0	0	0
8	3	0	14	2	2	3	2	4	2	0	5	0	0	1	1	0	0	1
1	5	2	13	2	1	3	3	5	3	0	3	0	1	1	1	0	0	0
1	0	9	8	2	8	6	3	5	0	2	16	4	3	0	0	0	0	0
1	7	2	11	1	3	6	1	2	5	2	16	4	3	0	0	0	0	0
1	3	0	13	2	4	3	2	5	2	0	5	1	0	1	1	0	0	1
1	3	2	16	2	5	1	3	2	2	0	15	1	0	1	1	0	0	0
8	6	0	17	2	5	7	2	4	1	2	16	4	3	0	0	0	0	0
8	6	0	13	2	1	3	2	4	1	2	16	4	3	0	0	0	0	0
8	3	0	14	2	3	3	2	4	2	0	5	0	0	1	1	1	1	1
8	3	0	20	1	2	3	2	1	2	0	6	0	0	1	1	1	0	1
1	3	7	3	2	5	3	2	5	2	0	12	1	0	1	1	1	1	1
1	11	0	11	2	4	0	2	0	5	2	16	4	3	0	0	0	0	0
8	3	0	10	2	1	2	2	4	2	2	2	3	0	0	0	0	0	0
1	1	0	24	1	1	3	1	2	5	0	6	1	1	1	1	0	0	1
1	10	10	13	2	2	3	2	5	5	2	16	4	3	0	0	0	0	0
1	0	0	9	2	3	0	2	5	0	0	8	4	1	0	0	0	0	0
1	3	0	25	1	3	1	1	2	2	0	4	1	0	1	0	0	0	0
8	6	14	21	1	4	3	2	5	1	0	15	0	0	1	1	1	0	0
8	7	0	15	2	2	3	2	4	4	0	1	0	0	1	1	0	1	1
8	3	12	15	1	2	1	3	1	5	2	16	4	3	0	0	0	0	0
1	7	0	15	2	2	3	2	5	4	0	6	1	0	1	1	0	0	1
1	5	9	12	1	2	3	2	2	5	0	1	2	0	1	1	0	0	1
8	6	14	1	2	1	1	2	5	1	0	2	0	0	1	1	0	0	1
8	5	8	3	1	2	3	1	1	5	2	16	4	3	0	0	0	0	0
1	3	0	12	2	4	3	2	5	2	0	9	1	0	1	1	1	0	1
1	1	9	14	1	2	3	2	2	5	2	16	4	3	0	0	0	0	0
1	3	6	4	1	2	3	2	2	2	0	9	1	0	1	1	1	0	0
1	11	0	30	0	5	1	0	2	5	0	9	4	0	1	1	1	0	1
8	6	10	16	2	1	1	2	5	1	2	16	4	3	0	0	0	0	0
1	3	0	13	2	3	3	2	5	2	0	6	1	0	1	1	0	0	1
1	3	0	9	2	3	2	2	0	2	0	7	0	1	1	1	1	0	1
1	3	0	9	2	2	2	2	0	2	0	7	1	0	1	0	1	0	0
8	5	0	20	1	4	3	1	4	3	0	7	3	0	1	1	0	1	1
1	3	0	11	2	4	3	2	2	2	0	11	1	0	1	1	1	0	1
8	7	0	12	2	3	3	2	4	4	0	6	1	0	1	1	0	0	1
8	0	0	8	2	9	6	2	3	0	0	8	0	1	1	1	0	0	0
1	7	0	10	2	4	2	2	0	4	0	4	1	0	1	1	1	0	0
1	6	14	15	1	1	1	2	0	1	0	3	1	0	1	1	0	0	1
8	6	2	9	2	3	3	2	1	1	0	1	1	0	1	0	0	0	0
8	3	0	8	2	3	2	2	4	2	0	4	2	0	1	1	1	0	1
8	5	0	16	2	2	1	2	4	3	2	5	0	0	1	0	0	0	0
8	3	0	12	2	1	1	2	4	2	0	12	1	0	1	1	1	1	1
8	7	2	6	2	1	5	2	4	4	2	16	4	3	0	0	0	0	0
1	3	0	21	1	5	3	1	0	2	0	7	1	0	1	1	0	1	1
8	5	0	17	2	3	3	2	1	3	0	10	1	0	1	1	1	0	0
8	5	0	11	2	1	1	2	4	3	2	16	4	3	0	0	0	0	0
1	0	0	8	2	9	0	2	5	0	0	6	4	0	1	1	1	0	1
1	3	0	15	2	3	3	2	2	2	2	16	4	1	1	1	0	1	1
8	0	0	8	2	2	6	2	5	0	2	16	3	0	0	0	0	0	0
8	3	0	15	2	4	3	2	5	2	0	6	1	0	1	1	1	0	1
8	6	0	21	1	3	3	1	5	1	2	16	4	0	1	0	0	0	0
8	0	9	8	2	2	6	3	3	0	2	16	3	0	1	1	1	0	1
8	5	2	7	2	2	1	2	5	3	2	16	4	3	0	0	0	0	0
1	3	0	12	2	4	3	2	5	2	0	9	1	0	1	1	1	1	1
8	7	0	19	2	4	3	2	1	5	0	15	1	0	1	1	1	1	0
1	3	0	21	1	5	3	1	0	2	0	2	1	0	1	1	1	0	1
8	3	0	17	2	3	3	2	4	2	0	8	1	0	1	1	1	0	0
8	11	2	9	2	4	3	2	4	5	0	15	1	0	1	1	1	0	1
1	7	0	21	1	3	2	1	5	4	0	8	1	0	1	1	1	0	1
1	7	10	26	1	3	3	2	0	4	0	2	0	0	1	1	0	0	0
1	7	0	17	2	5	3	2	0	4	0	11	1	0	1	1	1	1	1
1	6	2	4	2	2	1	2	5	1	2	2	4	0	0	0	0	0	0
8	0	14	8	2	3	6	3	3	0	0	10	0	0	0	0	1	0	0
1	3	0	21	1	3	3	1	5	2	0	6	1	0	1	1	1	1	0
1	0	9	8	2	9	6	3	5	0	2	16	4	3	0	0	0	0	0
8	3	2	8	2	1	3	3	5	2	0	12	1	0	1	1	0	0	1
8	0	0	10	2	1	6	2	3	0	0	4	4	0	1	1	0	0	0
1	3	0	20	1	5	3	1	5	2	0	5	1	0	1	1	1	0	0
8	0	12	3	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	0	22	1	2	3	1	5	2	2	16	3	2	1	0	1	0	0
8	3	13	1	1	2	5	1	5	5	0	2	0	0	1	1	1	1	1
1	7	0	9	2	4	3	2	0	4	0	15	1	0	1	1	1	1	1
1	9	2	5	2	1	0	3	5	5	0	2	3	0	1	1	0	0	1
1	3	0	10	2	3	2	2	0	2	0	8	1	0	1	1	1	0	1
7	3	6	9	2	3	3	3	4	2	2	16	4	3	0	0	0	0	0
1	3	0	12	2	4	3	2	5	2	0	8	1	0	1	1	1	0	1
8	6	2	4	2	2	1	2	5	1	2	16	4	3	0	0	0	0	0
1	7	8	8	1	4	3	1	0	5	0	11	1	0	1	1	1	0	1
1	7	10	30	0	1	7	2	2	5	2	2	4	0	0	0	0	0	0
1	7	10	18	2	5	3	3	2	4	2	1	4	1	0	0	0	0	0
8	5	3	7	2	1	1	2	4	3	0	15	1	0	1	1	0	0	1
8	5	0	15	2	4	3	2	4	3	0	8	3	1	1	1	0	0	1
8	5	8	5	2	1	1	2	5	3	0	7	2	0	1	1	1	0	1
8	3	0	8	2	1	3	2	4	2	2	16	4	3	0	0	0	0	0
1	5	2	9	2	2	1	2	5	3	2	16	4	3	0	0	0	0	0
1	1	10	23	1	2	3	3	5	5	0	5	1	0	1	1	0	0	1
8	0	0	9	2	3	6	2	3	0	2	1	0	0	0	0	0	0	0
1	7	0	23	1	4	3	1	5	4	0	7	1	0	1	1	1	0	1
8	7	2	17	1	2	1	2	1	5	0	3	4	1	0	0	0	0	0
1	3	0	29	1	8	2	1	0	2	0	4	0	0	1	1	0	0	1
1	0	0	8	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
8	0	0	10	2	1	6	2	5	0	0	3	4	1	0	1	0	0	0
8	3	0	13	2	1	3	2	4	2	2	0	0	1	1	0	0	0	0
8	3	0	13	2	7	3	2	1	2	0	10	3	2	0	0	0	0	0
1	7	0	25	1	4	3	1	5	5	0	5	0	1	1	1	1	0	1
8	0	0	9	2	1	6	2	3	0	2	16	0	1	1	1	0	0	1
8	5	2	11	2	3	3	2	4	3	0	4	1	0	1	1	1	0	0
1	6	3	20	1	2	3	3	2	1	2	16	4	3	0	0	0	0	0
8	3	0	19	2	2	4	2	4	2	2	16	4	3	0	0	0	0	0
8	6	2	11	2	1	6	2	1	1	2	16	4	1	0	0	0	0	0
1	7	2	5	2	3	6	2	2	4	2	16	4	3	0	0	0	0	0
1	7	2	13	1	1	3	2	2	5	0	1	2	0	1	1	1	1	1
8	3	8	12	2	4	1	3	4	2	0	15	1	0	1	1	1	0	1
8	6	3	10	2	4	1	3	4	1	0	9	1	0	1	0	0	0	0
1	6	0	13	2	2	2	2	0	1	0	5	3	1	1	1	0	0	1
1	0	0	8	2	1	0	2	5	0	0	3	0	0	1	1	1	1	1
8	7	2	20	1	4	3	3	5	4	0	1	1	0	0	0	0	0	0
8	3	0	18	2	4	5	2	1	2	0	1	1	0	1	1	0	0	0
1	3	0	11	2	3	3	2	5	2	0	4	1	0	1	1	1	0	1
8	3	0	20	1	2	2	1	5	2	0	9	1	0	1	1	0	0	1
8	5	6	1	2	6	3	2	4	3	0	10	1	0	1	1	1	0	1
1	5	2	6	2	6	3	2	2	3	0	15	1	0	1	1	0	0	1
8	3	5	6	2	3	2	2	4	2	0	1	1	1	1	1	0	0	1
1	5	11	15	1	4	3	2	5	5	0	14	1	0	1	1	0	0	1
8	3	6	2	2	1	3	2	4	2	2	1	4	2	1	1	0	0	0
1	8	5	3	1	5	3	1	2	5	0	13	1	0	1	0	0	0	0
8	6	0	20	1	2	3	1	4	1	0	6	0	0	1	1	1	1	1
8	7	2	10	2	2	4	3	4	4	2	2	0	0	1	1	0	1	1
1	0	0	8	2	2	0	2	5	0	0	4	4	0	1	1	1	0	0
8	6	0	20	1	4	3	1	4	1	0	15	1	0	1	1	1	0	1
1	3	0	10	2	3	2	2	0	2	0	1	4	0	0	1	0	0	0
1	11	2	5	1	1	3	2	2	5	0	15	3	0	1	1	0	0	1
8	3	9	14	2	1	4	3	5	2	2	1	3	0	1	1	0	0	0
1	8	5	28	1	3	3	3	5	5	2	1	4	0	0	0	0	0	0
8	5	6	2	1	1	1	2	4	3	0	2	1	0	1	1	0	1	1
1	3	0	8	2	5	3	2	5	2	0	5	4	0	1	1	1	1	1
1	3	0	20	1	3	3	1	0	2	0	9	1	0	1	1	1	0	1
8	3	11	2	1	6	3	1	5	2	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	11	2	3	1	1	1	2	2	5	2	16	4	3	0	0	0	0	0
1	9	2	7	2	1	1	3	0	5	0	1	2	0	0	1	0	0	0
8	0	9	9	2	1	6	3	3	0	2	16	4	3	0	0	0	0	0
8	3	0	10	2	2	2	2	5	2	2	16	4	3	0	0	0	0	0
1	6	10	15	2	3	1	2	2	1	2	16	4	3	0	0	0	0	0
1	5	0	20	1	2	3	1	2	3	0	2	1	0	1	1	0	0	1
1	7	0	18	2	3	3	2	2	5	0	10	1	0	1	1	0	0	1
1	1	0	23	1	5	3	1	5	5	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	0	2	3	0	0	6	4	0	1	0	0	0	0
8	3	0	10	2	3	2	2	4	2	0	5	1	0	1	1	1	1	1
8	11	10	19	2	4	3	3	4	5	0	1	0	1	1	1	1	1	1
1	5	0	23	1	3	3	1	0	5	0	6	3	0	0	0	0	0	0
1	5	0	13	2	2	1	2	0	3	0	1	2	0	0	0	0	0	0
8	1	0	22	1	1	3	1	5	5	1	1	0	0	1	0	0	0	0
8	0	4	6	2	6	6	3	3	0	2	16	4	3	0	0	0	0	0
1	3	0	8	2	4	6	2	5	2	0	7	1	1	1	1	0	0	0
1	7	14	31	1	3	3	3	0	5	0	8	1	0	1	1	1	0	1
8	0	0	8	2	1	6	2	3	0	0	3	4	1	1	1	0	0	1
1	7	10	30	0	4	3	2	5	4	0	13	2	0	1	1	0	1	1
1	7	10	22	1	3	2	3	0	4	0	6	4	0	1	0	0	0	0
8	3	0	11	2	1	3	2	4	2	2	16	4	3	0	0	0	0	0
8	5	5	6	2	2	3	3	1	3	0	2	4	1	1	1	1	0	0
8	3	0	8	2	4	2	2	4	2	0	5	0	0	1	1	1	1	1
8	6	0	14	2	2	1	2	4	1	2	16	4	3	0	0	0	0	0
1	8	0	10	2	3	2	2	2	5	0	6	1	0	1	1	0	1	1
1	1	2	17	1	3	3	3	2	5	2	6	4	0	1	1	0	0	1
8	5	0	17	2	2	5	2	4	3	0	1	4	0	0	0	0	0	0
1	5	14	16	1	2	1	3	5	3	0	3	4	0	1	1	0	0	0
8	3	7	18	2	5	3	3	4	2	0	7	1	0	1	1	1	0	1
1	7	2	10	1	3	1	1	5	5	0	3	0	0	0	0	0	0	0
8	7	10	19	1	3	3	2	1	4	0	2	4	0	1	1	0	0	1
8	3	9	5	2	3	3	2	4	2	0	14	4	1	1	1	1	0	1
1	7	2	10	1	2	6	1	2	5	0	2	3	0	1	0	0	0	0
1	1	0	22	1	4	3	1	0	5	0	8	1	0	1	1	1	0	1
1	3	0	11	2	4	3	2	2	2	0	15	0	0	1	1	0	0	1
8	3	6	9	2	1	3	3	1	2	0	12	1	0	1	1	0	0	0
8	3	0	12	2	3	3	2	5	2	0	7	1	1	1	1	0	0	1
8	6	2	15	1	2	1	2	5	1	2	16	4	3	0	0	0	0	0
8	6	4	16	1	2	1	2	4	1	2	2	3	2	1	1	0	0	1
8	3	0	17	2	1	2	2	4	2	0	2	4	1	1	1	0	0	1
8	6	2	6	1	1	1	2	1	1	0	15	0	1	0	0	0	0	0
1	3	0	12	2	3	3	2	0	2	0	15	4	0	1	1	1	0	0
8	13	0	16	2	2	3	2	4	5	2	16	4	3	0	0	0	0	0
1	11	2	5	2	1	3	2	2	5	2	16	0	1	0	0	0	0	0
1	7	2	11	1	2	1	2	0	4	0	4	1	1	1	1	0	0	0
1	6	2	6	2	1	3	2	2	1	2	16	4	3	0	0	0	0	0
1	3	0	16	2	2	3	2	5	2	0	8	1	0	1	1	1	0	0
1	2	9	9	2	2	0	3	2	5	2	16	4	3	0	0	0	0	0
1	3	14	7	2	1	3	3	0	5	0	1	3	0	1	1	0	0	0
1	0	0	9	2	5	6	2	5	0	0	5	0	0	1	0	0	0	0
8	3	0	11	2	5	3	2	1	2	2	16	4	3	0	0	0	0	0
1	7	5	4	2	2	3	2	0	4	0	10	0	0	1	0	0	0	0
1	3	0	22	1	2	3	1	0	2	0	1	1	0	1	0	0	0	0
1	1	0	26	1	2	3	1	2	5	0	4	1	0	1	1	1	0	0
8	6	6	5	2	1	3	2	1	1	0	10	1	0	1	1	1	0	1
8	3	14	6	2	3	6	2	1	2	2	16	4	3	0	0	0	0	0
8	6	13	2	2	1	1	2	4	1	0	2	4	1	1	1	1	0	0
1	5	11	23	1	3	3	2	0	5	0	8	1	0	1	1	1	0	1
5	0	9	8	2	3	6	3	3	0	2	16	4	3	0	0	0	0	0
1	5	0	23	1	4	3	1	0	3	0	15	1	0	1	0	1	0	1
8	3	14	17	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
8	6	9	4	2	2	2	3	4	1	0	15	1	0	1	1	1	0	1
8	3	0	20	1	2	3	1	1	2	0	6	1	0	1	1	0	0	1
8	3	9	17	1	4	3	2	4	2	0	4	3	0	1	0	1	0	0
8	2	12	7	2	3	0	3	4	5	0	6	4	1	1	1	0	0	0
8	3	5	3	2	2	3	2	4	2	0	2	4	0	1	1	0	1	1
1	9	2	21	1	3	3	3	0	5	0	7	1	1	1	1	0	0	1
8	0	12	4	2	2	6	3	3	0	2	16	4	3	0	0	0	0	0
1	7	0	17	2	6	3	3	5	4	0	15	4	0	1	1	0	0	1
8	2	12	5	2	1	0	2	4	5	2	16	4	3	0	0	0	0	0
1	5	0	27	1	2	3	1	0	5	2	2	3	2	0	0	0	0	0
8	7	13	15	1	2	6	2	1	4	0	3	1	0	1	1	0	1	1
1	5	0	13	2	7	3	2	0	3	0	15	1	0	1	1	1	0	0
8	6	2	5	2	1	1	2	5	1	0	5	0	0	0	0	0	0	0
8	7	0	12	2	2	3	2	4	4	0	12	1	1	1	1	0	0	1
1	5	2	12	1	3	3	2	2	3	0	3	4	0	1	1	0	0	1
8	3	0	13	2	2	3	2	1	2	0	2	1	0	1	0	1	0	0
8	3	0	21	1	5	3	1	5	2	0	13	3	0	1	0	0	0	0
8	3	10	16	2	1	5	2	4	5	2	3	0	1	1	1	0	0	1
8	5	8	1	2	1	6	2	5	3	2	16	4	3	0	0	0	0	0
1	8	0	13	2	3	3	2	0	5	0	6	1	0	1	1	1	1	1
1	8	2	4	1	1	1	1	0	5	0	1	4	0	1	1	1	0	1
1	9	10	31	1	4	3	2	2	5	0	13	1	0	1	1	0	0	1
1	3	0	9	2	2	3	2	5	2	0	5	4	0	1	1	0	0	1
1	7	5	11	2	1	2	3	5	4	2	16	4	3	0	0	0	0	0
8	2	12	5	2	3	0	2	4	5	2	16	4	3	0	0	0	0	0
1	6	2	4	1	1	1	2	5	1	0	8	1	1	1	1	0	0	1
8	7	4	1	2	4	2	3	4	4	0	1	1	1	1	1	1	0	0
7	3	0	19	2	4	4	2	1	2	0	9	1	0	1	1	1	0	1
1	0	9	20	1	2	1	3	0	5	0	8	0	0	1	0	0	0	1
1	7	0	19	2	6	3	2	2	4	0	7	1	0	1	1	0	0	0
8	3	0	9	2	4	2	2	1	2	1	9	4	0	1	0	0	0	0
8	5	13	16	1	4	3	2	1	5	0	8	1	0	1	1	1	1	1
1	8	5	4	1	2	3	1	2	5	2	16	4	3	0	0	0	0	0
8	3	0	12	2	1	1	2	5	2	2	1	0	1	1	1	0	0	1
8	3	5	2	2	2	3	2	5	2	0	5	3	0	1	1	1	0	1
1	7	2	13	1	1	1	2	2	4	0	7	0	2	1	1	0	0	1
8	7	2	16	2	3	1	3	4	4	0	6	4	0	1	1	0	0	1
8	3	0	10	2	2	1	2	4	2	2	16	4	3	0	0	0	0	0
1	3	2	9	2	3	3	2	2	2	0	4	2	0	1	1	0	0	1
8	9	2	21	1	4	1	2	1	5	0	8	1	0	1	1	0	0	0
8	11	11	9	2	1	1	3	4	5	0	3	0	2	1	1	0	0	1
1	7	0	17	2	3	3	2	0	4	0	7	1	0	1	1	1	0	1
1	0	12	4	2	3	2	3	5	0	0	12	1	0	1	1	1	0	1
1	0	9	9	2	9	6	3	5	0	2	16	4	3	0	0	0	0	0
8	5	0	24	1	4	2	1	1	5	0	15	1	0	1	1	1	0	0
8	5	0	27	1	1	3	1	1	5	2	16	3	1	1	1	0	0	1
8	7	14	20	1	3	3	2	4	4	0	10	1	0	1	1	1	0	1
8	6	9	8	1	5	6	2	1	1	2	3	0	0	1	1	1	0	0
1	4	2	10	1	1	1	1	2	5	0	8	1	0	1	0	0	0	0
1	5	1	4	1	4	3	1	5	5	0	1	4	0	0	0	0	0	0
8	0	9	8	2	2	6	3	3	0	2	16	4	3	0	0	0	0	0
8	5	0	19	1	4	3	1	4	5	0	7	1	0	1	1	1	0	1
1	0	14	10	2	4	6	2	5	0	0	9	2	0	1	0	0	0	0
1	11	0	30	1	3	2	1	5	5	0	4	4	0	1	0	0	0	0
1	3	2	4	1	3	3	2	5	2	0	6	1	0	1	0	0	0	0
1	6	10	21	1	1	1	3	2	1	0	3	0	0	1	0	1	0	0
1	5	2	4	2	3	3	2	0	3	0	14	4	1	1	1	0	0	1
8	8	2	5	1	4	6	2	1	5	0	9	0	1	1	1	0	0	0
8	3	0	20	1	4	3	1	4	2	0	10	1	0	1	1	1	1	1
8	7	1	13	2	1	3	3	5	4	2	1	4	0	0	0	0	0	0
8	3	2	18	1	3	2	3	5	2	0	15	1	0	1	1	1	0	1
1	9	0	30	1	2	3	1	0	5	0	2	4	0	1	1	0	1	1
8	3	9	11	2	3	3	3	4	2	0	13	1	0	1	1	1	1	1
1	7	0	11	2	4	2	2	2	4	2	4	4	1	0	0	0	0	0
8	11	13	18	2	3	3	3	4	5	2	16	4	3	0	0	0	0	0
1	7	2	6	1	2	3	2	2	4	2	16	4	3	0	0	0	0	0
1	1	0	24	1	5	3	1	5	5	0	12	1	0	1	1	0	0	0
1	7	0	18	2	1	3	2	2	4	0	1	1	1	1	1	0	0	1
8	1	9	13	1	4	3	3	1	5	0	2	0	1	1	1	1	0	1
8	3	7	9	2	3	2	3	1	2	2	16	4	3	0	0	0	0	0
8	7	0	16	2	3	7	2	4	4	2	16	4	3	0	0	0	0	0
8	6	11	2	2	1	1	2	4	1	2	16	4	3	0	0	0	0	0
1	5	0	12	2	3	3	2	5	3	0	15	1	0	1	1	0	0	0
8	3	2	10	2	1	1	2	1	2	2	16	0	0	1	1	0	1	1
8	3	13	3	2	2	3	3	4	2	0	1	1	0	1	1	0	0	1
8	5	0	9	2	2	2	2	4	3	2	16	4	3	0	0	0	0	0
8	5	5	18	1	2	6	3	1	5	2	16	4	3	0	0	0	0	0
8	3	11	1	2	4	3	2	5	2	0	9	1	0	1	1	1	0	0
8	6	5	12	2	2	1	2	1	1	0	8	0	1	1	1	1	1	1
8	11	4	3	2	1	1	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	20	1	2	3	1	4	2	0	5	1	0	1	1	0	0	1
8	2	13	6	2	1	0	3	4	5	2	16	4	3	0	0	0	0	0
8	6	0	18	1	3	3	1	4	1	0	3	3	1	1	1	0	0	1
1	5	0	20	1	5	3	1	0	3	2	3	4	1	1	0	0	0	0
8	8	5	3	1	2	3	1	1	5	2	16	4	0	1	1	0	0	1
8	7	9	13	2	3	1	3	4	4	2	16	3	0	1	1	0	0	0
1	3	0	11	2	4	3	2	2	2	0	14	1	0	1	1	0	0	1
1	5	0	8	2	1	2	2	5	3	2	1	4	2	0	0	0	0	0
8	0	0	10	2	1	0	2	3	0	0	1	0	1	1	1	0	0	0
8	6	13	13	2	1	3	2	4	1	2	16	4	3	0	0	0	0	0
1	6	14	3	2	3	3	2	0	1	0	8	1	0	1	1	1	1	1
8	3	8	16	1	4	3	2	4	2	0	8	1	0	1	1	1	0	1
8	3	13	15	1	4	3	3	4	2	0	15	1	1	1	1	1	1	1
8	7	0	12	2	2	3	2	4	4	2	16	4	0	1	1	0	0	1
1	7	0	10	2	2	2	2	5	4	0	5	1	0	1	1	1	0	1
1	5	2	6	1	4	3	1	2	5	2	16	4	3	0	0	0	0	0
8	0	0	10	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
8	5	0	12	2	2	3	2	4	3	0	5	1	0	1	1	1	1	1
1	2	2	4	2	3	0	2	5	5	2	16	4	3	0	0	0	0	0
1	7	5	27	0	1	2	3	2	5	0	3	4	0	1	0	1	0	0
8	5	11	17	2	1	2	3	4	3	0	7	2	0	1	1	1	0	0
8	5	2	3	2	3	1	2	1	3	0	5	1	0	1	1	1	0	1
8	3	12	6	2	2	3	2	4	2	0	15	1	0	1	1	1	0	1
8	6	7	5	2	1	1	2	5	1	0	9	1	0	1	1	1	1	1
1	7	0	9	2	2	3	2	5	4	2	16	4	3	0	0	0	0	0
8	6	0	16	2	2	3	2	4	1	2	5	1	1	0	1	1	0	0
8	7	2	8	2	3	1	2	5	4	2	1	4	0	0	0	0	0	0
1	7	0	12	2	4	3	2	0	4	0	11	1	0	1	0	1	0	0
1	7	0	27	1	4	3	1	2	5	0	8	1	0	1	1	1	1	1
1	6	3	3	1	3	6	2	0	1	0	7	4	0	1	1	0	0	1
1	5	0	20	1	3	3	1	5	3	0	4	4	1	1	1	1	0	1
8	5	0	15	2	1	3	2	4	3	1	1	1	0	1	1	0	0	1
1	7	0	27	1	1	3	1	0	4	0	3	4	0	1	1	0	0	1
1	6	0	18	2	5	5	2	2	1	2	16	4	3	0	0	0	0	0
8	0	0	8	2	2	6	2	3	0	2	3	4	0	1	0	0	0	0
8	3	0	13	2	4	3	2	4	2	2	16	4	1	1	0	1	0	0
1	1	2	15	1	3	3	2	5	5	0	5	0	0	1	1	0	0	0
8	3	0	8	2	5	2	2	4	2	0	5	4	1	1	1	0	0	0
8	5	0	12	2	3	3	2	4	3	0	9	4	0	1	1	1	0	1
8	5	9	3	2	2	2	3	5	3	0	9	4	0	1	0	0	0	0
1	1	0	23	1	2	3	1	2	5	0	2	1	0	1	1	0	1	0
1	9	13	11	2	1	3	2	2	5	0	1	0	0	1	1	1	0	1
8	0	9	8	2	2	6	2	4	5	0	4	0	1	1	1	0	1	1
1	6	11	3	2	1	2	2	2	1	2	16	0	1	1	1	0	0	0
1	1	0	23	1	5	3	1	5	5	0	8	4	0	1	1	0	0	1
8	3	0	14	2	3	2	2	4	2	0	9	1	0	1	1	0	0	1
1	5	13	25	1	4	3	3	2	5	0	7	1	0	1	1	1	0	1
8	11	2	5	2	1	3	2	5	5	2	0	0	1	1	1	0	0	1
8	3	0	13	2	3	3	2	4	2	0	6	1	0	1	1	1	0	0
1	6	7	9	2	4	3	3	2	1	0	15	0	0	1	1	1	0	0
1	3	7	1	2	3	5	2	2	2	2	7	4	0	1	1	0	0	0
1	6	2	11	1	2	1	2	2	1	2	1	4	0	0	0	0	0	0
1	3	0	20	1	3	3	1	5	2	0	11	1	0	1	1	0	0	1
1	3	0	18	2	5	3	2	0	2	0	6	0	0	1	1	0	0	1
8	3	0	12	2	2	1	2	4	2	2	16	4	3	0	0	0	0	0
8	7	0	31	0	4	3	0	1	5	0	12	0	0	1	1	1	0	1
8	6	8	4	2	3	1	2	4	1	2	16	2	0	1	1	0	0	1
1	0	0	9	2	3	6	2	3	0	2	1	4	3	0	0	0	0	0
1	3	9	11	2	2	2	3	5	2	0	5	0	0	1	0	1	0	0
8	6	3	7	2	4	2	3	4	1	0	9	0	0	1	1	0	0	1
8	9	14	1	2	2	2	3	4	5	2	16	4	3	0	0	0	0	0
8	7	0	17	2	2	2	2	1	4	0	9	3	2	1	1	0	0	1
1	0	0	8	2	2	6	2	5	0	2	16	4	1	0	0	0	0	0
8	3	0	12	2	2	3	2	4	2	0	2	0	0	1	1	1	0	0
8	5	0	24	1	1	1	1	5	5	2	16	4	3	0	0	0	0	0
8	0	0	8	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	5	2	6	1	1	5	1	0	5	2	16	4	2	0	0	1	0	0
8	3	0	18	2	4	3	2	1	2	0	13	1	0	0	1	0	0	1
1	9	0	26	1	3	3	1	5	5	0	5	0	0	1	1	0	0	1
8	5	0	17	2	2	2	2	4	3	2	16	4	3	0	0	0	0	0
8	3	14	2	2	3	3	2	4	2	2	4	4	0	1	0	1	0	0
8	5	0	14	2	3	3	2	1	3	0	7	1	0	1	1	0	1	1
8	3	6	2	2	6	3	2	5	2	0	9	1	0	1	1	0	0	1
1	3	0	20	1	2	3	1	2	2	0	1	2	0	1	1	0	0	1
1	5	2	11	1	2	3	2	5	5	2	16	4	3	0	0	0	0	0
8	9	0	18	2	4	3	2	5	5	2	16	4	3	0	0	0	0	0
1	3	0	10	2	4	2	2	5	2	0	6	2	0	1	1	1	0	1
8	6	2	8	2	1	2	3	1	1	2	16	4	3	0	0	0	0	0
8	5	9	6	1	2	2	2	4	3	0	5	1	0	1	0	0	0	0
1	11	0	30	1	4	3	1	5	5	2	16	4	3	0	0	0	0	0
8	3	9	1	1	3	3	2	1	2	0	1	2	0	1	1	0	0	0
1	6	2	20	1	1	0	2	0	1	0	1	1	1	1	1	0	0	1
1	5	0	23	1	3	3	1	2	5	2	16	4	3	0	0	0	0	0
8	3	0	18	2	3	1	2	4	2	0	5	3	1	1	1	1	0	1
8	5	0	10	2	1	2	2	4	3	2	0	4	1	0	0	0	0	0
8	11	2	1	2	1	1	2	1	5	0	16	4	2	1	0	0	0	0
8	12	14	7	1	3	1	1	1	5	0	6	4	0	1	0	0	0	0
8	7	0	11	2	3	3	2	1	4	2	1	1	0	0	0	0	0	0
8	5	0	11	2	2	1	2	4	3	0	3	3	0	1	0	0	0	0
1	11	0	8	2	4	2	2	5	5	2	16	4	3	0	0	0	0	0
8	7	2	7	1	1	1	1	1	5	0	2	0	0	1	1	1	0	0
1	7	2	19	1	2	1	2	5	5	2	16	4	3	0	0	0	0	0
8	3	0	19	2	3	3	2	4	2	0	6	0	0	1	1	1	1	1
1	3	0	21	1	4	3	1	2	2	0	6	1	0	1	1	1	1	1
8	6	6	8	1	1	1	2	5	1	0	7	0	0	1	1	1	1	1
1	5	0	24	1	4	3	1	5	5	0	8	1	0	1	1	0	0	1
8	3	5	8	2	3	3	2	4	2	0	5	4	1	1	1	0	0	1
8	6	0	10	2	3	3	2	4	1	0	11	4	0	1	1	0	0	1
8	0	0	10	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	7	2	13	1	2	6	2	2	5	0	14	0	0	1	1	0	0	1
1	1	0	24	1	4	3	1	0	5	0	5	1	1	1	1	0	0	1
8	2	13	1	2	3	6	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	12	2	2	3	2	4	2	0	11	3	0	1	1	0	0	1
8	11	2	4	2	1	2	2	5	5	0	2	4	0	0	0	0	0	0
1	4	14	1	1	2	3	1	2	5	0	15	4	1	1	1	1	0	1
1	3	1	17	2	2	3	2	2	2	0	1	1	0	1	1	1	1	1
1	0	2	7	1	1	6	1	0	5	1	16	4	1	0	0	0	0	0
8	3	0	10	2	3	2	2	5	2	0	5	0	0	1	1	1	0	1
1	7	12	1	1	4	3	1	5	5	2	5	3	0	0	0	0	0	0
1	3	11	3	2	4	5	2	2	2	0	8	2	0	1	1	0	1	1
8	11	13	7	2	3	3	2	4	5	2	16	4	3	0	0	0	0	0
8	0	0	8	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
8	6	7	9	2	4	3	2	4	1	0	5	0	0	1	1	1	0	1
1	5	0	25	1	5	3	1	2	5	0	9	1	1	1	1	1	1	1
8	11	2	12	2	2	1	2	4	5	1	4	1	0	1	1	0	0	0
8	5	5	26	1	4	1	3	1	3	2	1	4	0	0	0	0	0	0
1	6	2	9	1	2	0	1	2	5	0	3	1	0	0	0	0	0	0
1	7	12	1	1	5	6	1	2	5	0	5	1	0	1	1	1	0	1
8	3	0	14	2	2	3	2	4	2	0	1	1	0	1	1	0	0	1
8	3	0	17	2	3	3	2	5	2	0	5	1	0	1	1	0	0	1
8	7	0	16	2	1	2	2	4	4	0	11	1	0	1	1	1	0	1
1	7	10	31	1	2	3	3	2	5	0	4	1	0	1	1	0	0	0
1	3	2	4	1	2	3	2	5	2	0	9	4	0	1	1	0	0	1
1	3	0	13	2	3	3	2	0	2	0	5	1	0	1	1	0	0	1
8	6	2	4	2	1	5	2	1	1	2	16	4	3	0	0	0	0	0
1	7	10	13	2	1	3	3	2	4	0	13	2	0	1	1	1	0	1
1	0	0	9	2	2	6	2	5	0	2	4	4	0	0	0	0	0	0
8	5	10	20	1	3	1	2	4	3	0	6	0	1	0	1	0	0	1
1	11	2	30	0	2	3	3	0	5	0	6	0	0	1	1	1	0	1
8	0	11	10	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	14	20	1	2	3	2	4	2	0	1	1	0	0	0	0	0	0
8	2	2	9	2	1	0	2	4	5	2	16	0	2	1	1	0	0	0
1	5	0	20	1	2	3	1	5	3	0	5	1	0	1	1	0	0	1
8	3	3	6	2	1	1	2	4	2	0	15	2	0	1	1	1	1	1
1	0	0	9	2	8	6	2	5	0	2	16	4	3	0	0	0	0	0
1	7	0	13	2	4	3	2	0	4	0	8	1	0	1	1	0	1	1
8	0	11	6	2	1	6	3	5	0	0	1	0	1	1	1	1	0	0
1	6	5	4	1	2	3	2	2	1	0	5	1	0	1	1	0	0	1
8	7	0	9	2	3	3	3	5	4	0	4	1	0	1	1	1	0	0
8	3	0	10	2	5	2	2	4	2	0	9	0	0	1	1	1	0	1
8	7	2	5	1	1	6	2	1	4	2	1	4	1	0	0	0	0	0
1	3	6	19	1	4	2	2	0	2	0	6	4	1	1	0	1	0	0
8	0	9	9	2	3	6	3	3	0	2	16	4	3	0	0	0	0	0
1	3	0	14	2	5	3	2	0	2	0	13	1	0	1	1	0	0	1
8	6	1	14	2	3	1	3	4	1	2	4	4	0	0	0	0	0	0
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	12	9	2	5	1	3	1	2	0	3	1	0	1	1	0	0	0
1	3	0	8	2	4	3	2	2	2	0	3	0	0	1	1	1	1	1
1	7	0	8	2	1	2	2	5	4	0	15	1	0	1	1	1	0	0
8	7	6	1	1	9	7	2	4	4	2	16	4	0	0	0	0	0	0
1	3	0	10	2	5	2	2	0	2	0	13	0	0	1	1	1	0	0
8	3	0	17	2	4	3	2	4	2	0	3	3	0	1	1	1	1	1
1	0	9	7	2	8	6	3	5	0	2	16	4	3	0	0	0	0	0
1	7	2	4	2	1	1	2	2	4	0	12	0	1	1	1	0	0	0
8	3	0	11	2	2	3	2	4	2	0	3	4	0	1	1	0	0	1
1	7	0	13	2	5	3	2	2	4	0	15	1	0	1	1	0	0	1
8	3	0	17	2	3	0	2	5	2	2	0	4	0	0	0	0	0	0
1	1	6	21	1	6	3	3	0	5	0	15	4	1	1	1	0	0	1
8	2	2	12	1	2	0	2	4	5	2	16	4	3	0	0	0	0	0
8	3	11	18	2	2	3	2	4	2	2	16	4	0	1	1	0	0	1
8	7	0	10	2	2	3	2	4	4	0	5	1	1	1	1	0	1	1
8	5	0	9	2	3	3	2	4	3	0	13	1	0	1	1	1	0	1
8	11	4	2	2	3	2	3	4	5	2	16	4	3	0	0	0	0	0
8	3	6	1	1	4	3	1	4	2	0	12	1	0	1	1	1	1	1
1	7	0	8	2	1	2	2	5	4	2	0	4	0	0	0	0	0	0
8	6	2	8	1	2	1	1	1	5	2	16	4	3	0	0	0	0	0
8	3	0	15	2	2	3	2	5	2	0	4	1	0	1	1	0	0	0
1	3	2	14	1	2	2	3	2	2	2	2	4	1	0	0	0	0	0
1	0	2	15	1	2	1	2	2	5	0	4	4	1	1	0	0	0	0
8	3	11	1	2	4	3	2	4	2	0	3	0	0	1	0	1	0	0
8	3	0	14	2	2	3	3	4	2	0	5	0	2	1	1	0	0	1
8	3	5	2	1	1	6	1	5	2	0	2	0	0	1	1	0	0	0
1	7	2	12	1	2	3	1	5	5	2	16	4	3	0	0	0	0	0
8	3	0	9	2	3	2	2	4	2	0	3	1	0	1	1	1	0	1
8	6	2	11	2	4	3	3	1	1	0	3	0	0	1	1	1	0	1
8	11	14	8	2	3	3	2	4	5	2	1	4	0	0	0	0	0	0
1	0	7	1	2	2	6	2	5	0	0	5	1	0	1	0	0	0	0
8	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
8	11	0	20	1	3	3	1	1	5	0	5	1	1	1	1	1	1	1
8	0	0	10	2	1	6	2	3	0	0	12	1	0	1	1	0	1	1
8	3	0	11	2	2	3	2	5	2	0	15	1	0	1	1	0	0	1
1	1	0	23	1	4	3	1	5	5	0	13	4	0	1	1	0	0	1
8	3	0	10	2	3	2	2	4	2	0	15	1	0	1	1	1	0	1
8	5	0	19	2	4	3	2	4	3	0	6	0	0	1	1	0	0	1
1	5	11	2	2	4	2	2	2	3	0	10	1	0	1	1	0	0	1
1	3	2	6	2	3	2	2	2	2	2	5	2	0	1	1	0	0	1
8	0	0	9	2	2	6	2	3	0	0	15	1	0	1	1	1	0	1
1	3	11	16	1	4	3	3	0	2	0	15	4	0	1	1	1	1	1
1	3	0	11	2	3	2	2	2	2	0	15	1	0	1	1	0	0	0
1	6	14	6	1	1	3	2	5	1	0	4	0	0	1	1	0	0	1
1	3	0	19	2	7	3	2	2	2	0	3	1	1	1	1	0	0	0
8	5	13	20	1	1	1	3	5	3	0	15	1	0	1	1	1	1	1
1	3	9	6	2	1	6	3	2	2	2	16	4	3	0	0	0	0	0
1	5	0	24	1	1	0	1	5	5	2	2	4	0	0	0	0	0	0
1	3	5	17	1	2	3	3	0	2	0	1	4	0	0	0	1	0	0
8	7	0	20	1	1	4	1	4	4	2	2	4	1	0	0	0	0	0
8	3	0	15	2	1	1	2	4	2	2	16	4	3	0	0	0	0	0
1	5	0	21	1	4	3	1	5	3	0	15	1	0	1	1	1	0	0
8	7	3	6	2	4	3	3	5	4	0	8	1	0	1	1	0	1	1
8	3	0	20	1	1	3	1	4	5	0	15	0	0	1	1	1	0	1
8	3	0	15	2	5	3	2	4	2	0	6	0	0	1	1	1	0	1
1	3	0	17	2	3	3	2	0	2	0	10	1	0	1	1	0	0	1
1	3	9	7	2	4	2	3	2	2	0	5	1	1	1	1	0	0	1
8	3	6	1	2	3	3	2	4	2	2	8	0	1	1	1	0	0	0
8	7	5	10	2	1	3	2	5	4	2	16	4	3	0	0	0	0	0
8	9	2	8	1	3	1	1	1	5	0	4	1	0	0	0	0	0	0
1	3	0	13	2	1	3	2	5	2	2	16	4	3	0	0	0	0	0
8	11	2	5	2	2	3	2	4	5	2	16	4	3	0	0	0	0	0
8	5	3	2	1	1	1	2	5	3	2	16	4	3	0	0	0	0	0
1	5	0	26	1	2	2	1	0	5	0	12	1	0	1	1	0	0	1
8	5	0	11	2	1	1	2	1	5	0	12	2	0	1	1	0	0	1
8	5	2	4	2	5	2	3	4	3	0	15	1	0	0	0	0	0	0
8	1	6	1	1	4	1	1	1	5	0	10	0	1	0	1	0	0	0
1	7	2	5	2	6	6	2	0	4	2	16	4	3	0	0	0	0	0
8	0	0	10	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	5	2	14	1	2	1	2	2	5	0	4	3	0	1	1	0	0	0
8	3	2	11	1	2	1	2	1	2	2	16	4	3	0	0	0	0	0
1	3	7	19	1	2	5	3	5	2	0	12	2	0	1	1	0	1	1
8	0	0	10	2	2	6	2	3	0	0	3	4	0	1	1	0	0	1
8	3	0	10	2	5	2	2	1	2	0	4	4	1	1	1	0	0	0
8	0	12	3	2	2	6	2	3	0	0	1	2	0	1	1	0	0	0
8	3	0	8	2	4	2	2	1	2	0	6	1	0	1	1	1	0	1
8	6	1	5	2	3	3	2	5	1	0	5	1	0	1	1	1	0	1
1	0	0	8	2	4	6	2	5	0	2	1	4	1	0	0	0	0	0
8	5	0	20	1	3	3	1	1	3	0	9	1	0	1	1	1	1	1
1	7	2	6	1	2	1	2	0	4	2	16	4	0	0	1	0	0	0
8	3	0	18	2	4	3	2	4	2	0	6	1	1	1	1	1	1	1
1	0	0	9	2	4	6	2	5	0	2	16	4	3	0	0	0	0	0
1	3	0	14	2	3	3	2	5	2	0	2	1	0	0	0	0	0	0
1	3	0	15	2	5	1	2	5	2	2	16	4	3	0	0	0	0	0
8	7	0	15	2	2	3	2	5	4	0	3	2	0	1	1	0	0	1
8	3	3	14	2	2	3	3	5	2	2	16	4	3	0	0	0	0	0
1	5	9	26	1	3	3	3	2	5	0	6	0	0	1	1	0	1	1
8	7	14	2	2	3	1	2	1	4	1	2	1	0	0	0	0	0	0
8	3	2	7	2	1	1	2	5	2	0	6	1	0	1	1	1	0	1
1	0	9	8	2	8	6	3	5	0	2	16	4	3	0	0	0	0	0
1	7	0	9	2	4	3	2	5	4	0	11	1	0	1	1	0	1	1
8	5	0	19	1	2	3	1	1	3	0	1	1	1	0	0	0	0	0
8	11	0	14	2	4	3	2	4	5	0	2	4	0	0	0	0	0	0
8	3	0	15	2	4	3	2	4	2	0	1	1	1	1	1	0	1	1
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	5	2	10	1	2	0	2	1	5	0	5	0	0	0	0	0	0	0
8	3	13	3	2	2	0	3	4	2	2	16	4	3	0	0	0	0	0
8	7	0	9	2	5	2	2	5	4	0	10	1	0	1	1	1	0	0
1	6	9	2	1	2	6	2	0	1	0	15	1	0	1	1	1	0	1
1	5	2	5	2	1	2	2	2	3	2	0	3	0	0	0	0	0	0
1	7	14	13	1	3	3	2	5	4	0	5	1	0	1	0	0	0	0
8	3	0	12	2	5	3	2	4	2	0	9	4	0	0	0	0	0	0
8	3	0	8	2	3	2	2	5	2	0	10	1	0	1	0	0	0	0
1	5	0	13	2	2	3	2	0	3	0	3	4	0	1	1	0	0	1
1	6	10	13	2	3	1	3	0	1	0	6	0	1	1	0	0	0	0
8	3	6	10	2	3	3	3	5	2	0	10	1	0	1	1	0	0	1
1	11	13	1	2	2	1	2	2	5	0	3	2	0	1	1	1	1	0
1	7	5	7	2	3	3	2	2	4	0	6	2	0	1	1	0	0	1
1	7	0	9	2	2	2	2	2	5	2	1	4	2	0	0	0	0	0
6	3	5	8	1	3	3	2	4	2	0	1	1	0	1	1	0	0	0
1	7	2	14	0	3	1	2	2	5	0	3	2	0	1	0	0	0	0
1	0	12	14	1	2	0	2	5	5	1	6	1	0	1	1	0	0	0
8	9	9	4	1	1	0	2	1	5	2	16	4	3	0	0	0	0	0
8	3	3	5	2	2	3	2	1	2	2	16	4	3	0	0	0	0	0
1	6	6	8	2	1	1	2	2	1	0	12	1	0	1	1	1	0	0
8	3	0	20	1	6	3	1	4	2	0	6	1	0	1	1	0	0	1
8	0	0	9	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
8	7	2	4	2	2	1	2	1	4	2	16	4	3	0	0	0	0	0
8	11	2	4	2	1	1	2	4	5	2	16	4	3	0	0	0	0	0
8	11	0	15	2	3	3	2	5	5	2	16	4	3	0	0	0	0	0
1	3	4	15	2	2	1	2	0	2	0	3	0	0	1	1	0	0	0
1	7	4	4	2	2	6	2	5	4	0	5	0	0	0	0	0	0	0
1	8	0	13	2	2	3	2	5	5	0	5	0	0	1	1	1	0	1
8	7	6	1	1	3	3	2	5	4	0	5	1	0	1	0	0	0	0
1	3	5	1	2	6	3	2	2	2	0	15	1	1	1	0	0	0	0
1	3	0	20	1	1	0	1	5	2	0	7	0	0	1	1	0	0	1
8	3	0	16	2	3	3	2	1	2	0	9	2	0	1	1	0	0	1
1	7	10	31	0	9	7	2	2	5	2	16	4	3	0	0	0	0	0
8	3	13	8	1	1	1	2	5	5	1	1	0	1	1	1	0	0	1
8	3	0	20	1	6	3	1	4	2	2	16	4	3	0	0	0	0	0
3	8	9	3	1	1	3	1	5	5	0	7	4	1	1	1	1	0	1
8	0	0	14	2	1	6	2	5	5	2	16	4	3	0	0	0	0	0
8	3	0	11	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
8	7	2	18	0	2	1	2	1	5	0	1	3	0	1	0	0	0	0
8	5	0	9	2	3	2	2	1	3	2	1	4	1	0	0	0	0	0
8	3	12	6	2	2	3	2	4	2	0	6	4	0	1	1	1	0	1
8	3	11	10	2	2	3	2	4	2	0	1	1	0	1	1	0	1	1
8	12	14	2	1	4	3	1	1	5	2	16	4	3	0	0	0	0	0
1	6	0	23	1	5	3	1	5	1	0	10	3	0	1	1	0	0	0
8	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	3	4	2	4	2	3	2	2	0	7	4	1	1	1	0	0	1
1	3	0	9	2	5	6	2	5	2	0	9	1	0	1	1	0	0	1
1	7	0	22	1	3	3	1	2	4	0	4	1	0	1	1	0	0	0
8	7	13	12	2	4	3	3	1	4	0	6	1	0	1	1	1	1	1
8	0	9	7	2	2	6	3	3	0	2	16	4	3	0	0	0	0	0
1	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	9	8	2	1	3	3	5	2	2	16	4	3	0	0	0	0	0
1	6	8	16	1	5	3	2	5	5	2	16	4	0	0	0	0	0	0
1	7	0	13	2	3	3	2	2	4	0	11	1	0	1	1	1	0	1
1	6	14	12	2	2	3	2	2	1	0	4	1	0	1	1	0	0	1
8	6	2	5	2	2	3	2	5	1	2	16	4	3	0	0	0	0	0
8	3	0	20	1	4	3	1	4	2	0	12	1	0	1	1	1	0	1
8	0	0	9	2	3	6	2	3	0	1	0	0	0	0	0	0	0	0
1	11	0	16	2	8	6	2	2	5	2	16	4	3	0	0	0	0	0
1	5	10	18	2	1	3	3	5	3	2	16	4	3	0	0	0	0	0
8	6	0	18	2	3	3	2	4	1	0	5	1	0	1	1	0	0	0
8	3	2	15	2	2	3	3	4	2	2	1	1	1	0	0	0	0	0
1	3	6	3	2	6	3	2	0	2	0	15	4	0	1	1	1	0	0
1	7	0	16	2	3	2	2	2	4	0	4	1	0	1	1	0	0	0
1	6	9	21	1	5	3	3	2	1	0	6	1	1	1	1	1	1	1
1	0	14	6	1	3	6	1	2	5	0	15	0	0	1	1	1	0	0
1	0	0	6	2	4	6	2	2	5	2	16	4	3	0	0	0	0	0
1	1	0	23	1	5	3	1	2	5	0	13	1	0	1	1	1	1	1
8	0	9	8	2	2	6	3	5	0	2	16	4	3	0	0	0	0	0
1	0	0	9	2	8	6	2	5	0	2	16	4	3	0	0	0	0	0
1	5	0	8	2	1	2	2	5	3	2	16	4	3	0	0	0	0	0
8	5	2	9	2	1	1	2	4	3	0	4	4	1	1	0	1	0	0
1	9	2	5	2	1	1	2	2	5	2	3	4	1	0	0	0	0	0
1	5	0	14	2	2	2	2	0	3	0	3	1	0	1	1	1	1	1
1	3	2	18	2	6	3	3	2	2	0	4	2	0	1	1	0	0	1
1	3	0	12	2	2	3	2	5	2	2	14	4	0	1	1	0	0	0
1	2	2	9	2	3	0	3	2	5	0	6	0	1	1	1	0	0	1
8	6	2	9	1	3	2	2	1	1	0	3	4	0	1	1	0	0	0
1	7	0	20	1	3	3	1	5	4	0	4	1	0	1	1	0	1	0
8	9	2	9	2	3	5	2	1	5	2	1	3	1	0	0	0	0	0
8	0	0	10	2	1	6	2	3	0	0	9	3	0	1	1	0	0	1
1	6	2	16	1	2	3	1	0	5	2	16	4	3	0	0	0	0	0
1	7	0	21	1	2	3	1	5	4	0	5	4	1	1	1	1	1	0
8	7	4	13	2	1	3	3	4	4	2	16	4	3	0	0	0	0	0
1	6	2	10	1	3	3	1	2	5	0	8	1	0	1	1	1	0	0
1	3	0	20	1	3	3	1	2	2	0	11	1	0	1	1	0	0	0
1	5	0	11	2	6	3	2	2	3	0	15	0	0	1	1	0	0	1
8	3	0	18	2	4	3	2	4	2	0	5	0	1	1	1	0	0	1
8	5	0	17	2	4	3	2	4	3	0	15	1	0	1	1	0	0	1
1	3	0	23	1	3	3	1	5	2	0	5	2	0	1	0	1	0	0
1	0	0	6	3	2	0	3	5	0	0	4	3	1	1	0	0	0	0
8	5	3	19	1	3	3	3	4	3	0	4	4	0	1	0	0	0	0
1	8	14	2	1	3	3	1	2	5	0	4	1	0	1	1	0	0	1
1	3	1	11	1	4	3	2	0	2	0	3	4	1	1	1	0	0	0
1	5	0	20	2	2	3	2	2	3	2	16	3	1	1	1	0	0	1
8	5	2	5	2	2	1	2	5	3	0	16	4	0	0	0	0	0	0
1	0	0	9	2	4	6	2	5	0	0	8	0	0	0	0	1	0	0
8	3	0	12	2	3	3	2	5	2	0	3	4	0	1	1	1	0	0
1	3	0	13	2	4	3	2	2	2	0	3	1	0	1	1	1	0	0
1	0	0	10	2	1	0	2	3	0	0	3	1	0	1	1	0	0	0
8	3	0	19	1	2	2	1	4	2	2	16	4	3	0	0	0	0	0
8	6	0	10	2	4	2	2	1	1	0	15	1	0	1	1	1	0	1
1	5	0	13	2	3	0	2	2	3	0	5	0	0	1	1	0	0	1
8	2	5	6	2	1	0	3	5	5	0	2	1	1	1	0	0	0	0
1	5	0	23	1	4	2	1	2	5	0	15	1	0	1	1	1	0	1
1	6	11	5	2	1	1	2	5	1	2	2	4	0	0	0	0	0	0
1	3	0	13	2	4	3	2	0	2	0	9	4	0	1	1	0	1	1
8	3	5	5	2	2	2	2	4	5	0	2	3	2	1	1	0	0	1
8	5	2	8	2	1	2	3	4	3	0	8	0	2	1	1	0	0	1
1	3	0	12	2	6	3	2	0	2	0	15	4	1	1	1	1	0	1
1	6	13	4	1	3	3	2	5	1	0	3	0	0	1	1	0	0	0
8	6	6	3	2	2	2	2	4	1	0	16	0	0	0	0	0	0	0
8	0	9	10	2	3	0	3	3	0	2	3	3	0	0	0	0	0	0
8	7	6	1	2	2	2	2	1	4	0	15	1	0	1	1	0	0	0
8	3	2	9	1	5	3	2	1	2	0	11	4	2	1	1	0	0	1
1	5	9	26	1	1	3	2	0	5	2	1	0	0	0	0	0	0	0
1	6	9	20	1	3	3	2	2	5	0	12	1	0	1	1	1	1	0
8	7	2	9	0	2	3	1	1	5	2	16	4	3	0	0	0	0	0
1	1	0	25	1	3	3	1	5	5	2	16	4	3	0	0	0	0	0
1	5	0	24	1	4	2	1	5	5	0	7	0	0	1	0	1	0	0
1	11	0	31	1	4	3	1	5	5	0	4	1	0	1	1	0	0	1
1	7	0	32	0	2	3	0	5	5	0	5	2	0	1	1	1	0	0
1	3	9	12	2	1	1	3	5	2	2	16	4	3	0	0	0	0	0
8	3	0	12	2	2	1	2	5	2	0	1	4	1	0	0	0	0	0
8	3	12	10	1	2	2	2	4	2	0	13	2	0	1	1	1	0	1
1	5	0	25	1	4	3	1	2	5	0	9	3	0	1	1	0	0	1
1	2	9	14	2	3	0	3	0	5	2	6	1	0	1	1	0	0	1
8	0	9	10	2	1	0	3	3	0	0	15	0	0	1	0	0	0	0
8	5	8	8	1	1	3	2	5	3	0	7	0	0	1	1	1	0	1
8	7	9	11	2	3	3	3	5	4	0	5	2	0	1	1	0	0	1
8	8	2	8	1	1	2	2	5	5	0	2	4	0	0	0	0	0	0
1	7	2	8	2	1	1	3	0	4	2	16	4	3	0	0	0	0	0
1	7	0	16	2	2	3	2	5	4	2	1	3	0	0	0	0	0	0
8	3	0	19	1	4	3	1	5	2	0	7	4	1	1	1	0	0	1
1	6	2	17	2	4	0	3	2	1	2	4	4	1	1	0	0	0	0
8	3	6	1	1	5	3	2	4	2	2	16	4	3	0	0	0	0	0
1	5	0	26	1	2	3	1	5	5	0	6	1	0	1	1	1	1	1
8	0	0	8	2	2	0	2	3	0	0	1	4	1	0	0	0	0	0
8	7	0	16	2	3	3	2	4	4	1	0	0	0	0	0	1	0	0
8	6	11	19	2	4	1	2	4	1	0	8	1	0	1	1	1	1	1
1	3	0	24	1	4	3	1	5	5	0	13	4	0	1	1	1	0	0
8	5	0	26	1	1	1	1	1	5	2	1	4	1	0	0	0	0	0
1	1	0	24	1	4	3	1	0	5	0	14	4	0	1	1	1	0	1
1	5	0	24	1	4	3	1	2	5	0	13	1	0	1	1	0	0	1
1	5	1	9	1	2	3	2	2	3	2	16	3	0	0	0	0	0	0
8	7	11	9	0	3	3	1	1	5	0	15	1	0	1	1	1	0	1
8	9	0	20	1	3	3	1	5	5	0	7	4	0	1	1	0	1	0
1	3	2	18	1	2	0	2	5	5	0	6	1	0	1	0	0	0	0
8	7	2	15	1	2	3	2	1	5	2	16	4	3	0	0	0	0	0
1	5	0	12	2	3	3	2	5	3	0	5	3	1	1	1	0	1	1
1	3	0	9	2	2	2	2	0	2	0	4	4	0	1	1	0	0	1
1	9	3	5	2	1	1	2	0	5	2	16	4	3	0	0	0	0	0
1	0	9	9	2	1	6	3	5	0	0	4	0	0	1	0	0	0	0
8	7	0	14	2	1	3	2	5	4	0	2	4	1	1	1	0	0	1
1	3	13	7	2	5	3	3	5	2	0	11	4	0	1	0	0	0	0
1	6	6	27	1	5	3	2	5	5	0	12	1	0	1	1	0	0	1
1	0	5	4	2	3	6	3	5	0	2	4	4	0	1	0	1	0	0
1	3	0	20	1	2	3	1	2	2	0	2	0	0	1	1	0	0	0
1	1	10	21	1	3	3	2	5	5	0	7	1	0	0	1	1	0	1
8	11	0	30	1	3	3	1	1	5	0	8	0	0	1	1	1	1	1
8	5	5	4	2	1	3	2	5	3	2	16	4	3	0	0	0	0	0
1	7	0	24	1	4	3	1	5	4	0	10	1	0	1	1	1	0	0
1	7	14	3	2	3	1	2	2	4	0	8	3	1	0	1	0	0	1
1	5	2	7	1	2	1	2	2	5	0	5	3	0	0	0	0	0	0
8	3	0	9	2	3	3	2	4	2	0	15	1	0	1	1	1	0	1
8	3	0	8	2	3	3	2	4	2	0	12	4	0	1	1	1	0	1
8	7	5	5	2	4	1	2	1	4	2	16	4	3	0	0	0	0	0
1	5	5	11	1	1	6	2	5	5	0	13	0	0	1	1	1	0	0
8	3	2	17	2	2	1	3	1	2	0	5	1	1	1	1	1	0	0
1	5	0	24	1	4	3	1	2	5	0	15	1	0	1	0	1	0	0
1	3	0	20	1	2	3	1	2	2	1	11	0	1	1	1	0	0	1
8	0	9	8	2	2	0	3	3	0	0	12	0	0	1	1	0	0	1
1	1	0	23	1	5	3	1	0	5	0	2	4	0	1	1	0	0	0
8	5	0	13	2	3	6	2	1	3	0	3	1	0	1	1	1	0	0
1	0	14	9	2	3	0	3	5	0	2	1	3	0	0	0	0	0	0
8	11	2	10	2	1	1	3	5	5	2	16	4	3	0	0	0	0	0
8	2	2	10	2	5	0	3	4	5	2	16	4	3	0	0	0	0	0
8	3	0	20	1	2	3	1	4	2	0	8	2	0	1	1	1	0	1
8	6	2	6	1	4	3	1	1	5	0	2	2	0	1	1	0	0	1
1	3	6	15	2	4	5	3	5	2	0	8	0	2	1	0	0	0	0
1	11	13	5	2	2	1	3	0	5	0	8	1	0	1	1	1	0	1
1	7	0	12	2	1	1	2	5	4	0	15	4	0	1	1	1	0	1
1	3	0	13	2	3	3	2	0	2	0	15	4	0	1	1	0	0	1
1	6	0	9	2	2	0	2	0	1	0	6	0	1	1	1	1	0	0
1	7	12	6	1	2	6	1	0	5	0	4	3	1	1	1	0	0	1
1	5	0	25	1	1	3	1	2	5	2	16	4	3	0	0	0	0	0
8	3	5	3	2	1	6	2	0	2	2	16	4	3	0	0	0	0	0
8	0	9	8	2	3	0	3	3	0	0	10	1	0	1	1	0	0	1
8	7	0	31	1	4	3	1	1	5	2	16	4	3	0	0	0	0	0
8	7	13	19	0	2	3	2	1	4	2	16	4	3	0	0	0	0	0
8	3	0	9	2	5	2	2	4	2	0	12	1	1	1	1	1	0	1
1	6	0	12	2	2	3	2	2	1	2	16	4	3	0	0	0	0	0
8	7	0	20	1	3	3	1	1	5	0	5	1	0	1	1	0	0	1
1	3	0	8	2	4	2	2	0	2	0	14	0	0	1	1	1	1	1
8	3	7	10	1	3	1	2	1	2	2	16	4	3	0	0	0	0	0
1	3	0	11	2	4	2	2	2	2	0	6	1	1	1	1	0	0	1
1	3	0	20	1	4	3	1	2	2	0	15	1	0	1	1	0	0	1
8	7	0	13	2	2	3	2	4	5	0	4	0	0	1	1	0	1	1
1	3	2	9	2	2	3	3	2	2	0	15	3	0	1	1	1	0	1
1	5	2	10	1	3	5	2	5	5	2	16	4	3	0	0	0	0	0
8	7	2	6	1	2	3	2	1	4	0	12	0	0	0	0	0	0	0
1	0	9	10	2	1	0	3	5	0	0	7	4	0	1	1	0	0	1
8	0	0	9	2	1	6	2	3	0	0	15	4	0	1	1	1	0	1
8	7	0	14	2	3	3	2	5	4	0	4	1	0	1	1	1	1	1
1	6	3	2	2	1	3	2	0	1	0	4	0	0	1	1	0	0	1
1	3	6	3	2	2	3	2	0	2	0	1	0	1	1	0	0	0	0
8	6	9	2	1	3	3	2	1	1	2	2	3	0	0	0	0	0	0
8	3	0	20	1	3	4	1	1	2	0	8	1	0	1	1	1	1	1
8	3	2	9	1	4	3	2	5	2	0	8	1	0	1	1	0	0	1
1	3	6	1	1	6	3	1	5	2	0	15	1	0	1	1	1	1	0
1	0	0	8	2	3	0	2	5	0	0	11	1	0	1	1	0	0	0
8	3	14	5	2	1	1	3	5	2	2	16	4	3	0	0	0	0	0
1	5	2	4	1	4	1	1	2	5	0	15	0	0	1	1	0	0	0
1	3	3	4	1	4	3	2	0	2	0	6	1	0	1	1	1	1	0
8	3	2	11	2	1	1	3	4	2	2	16	4	3	0	0	0	0	0
8	3	10	19	1	4	3	2	1	2	0	15	1	0	1	1	1	0	1
1	6	2	7	1	1	1	1	2	5	2	1	0	1	1	1	0	0	1
1	0	0	9	2	3	0	2	5	0	2	1	1	0	1	1	0	0	0
1	11	2	18	1	2	1	3	2	5	2	1	3	1	0	0	0	0	0
0	6	5	6	1	3	1	2	5	1	2	6	4	2	1	1	0	1	1
8	3	2	6	2	3	1	2	4	2	0	11	1	0	1	1	1	0	1
8	3	0	12	2	3	2	2	4	2	0	4	4	0	1	1	0	0	1
1	6	0	8	2	5	3	2	0	1	2	16	4	3	0	0	0	0	0
3	0	14	9	2	1	6	2	5	0	0	13	1	0	1	1	1	0	0
8	11	4	4	2	1	1	2	4	5	2	16	4	3	0	0	0	0	0
1	3	0	13	2	3	3	2	5	2	0	4	4	0	1	1	0	0	1
1	3	0	22	1	4	3	1	5	2	2	16	4	3	0	0	0	0	0
1	7	0	9	2	2	2	2	0	4	0	2	2	0	1	1	1	0	1
1	11	2	3	2	3	6	2	5	5	2	16	4	3	0	0	0	0	0
1	3	9	7	2	4	3	2	2	2	0	1	0	0	1	1	0	1	1
1	3	2	9	1	2	3	1	5	5	0	15	1	0	0	1	1	0	0
1	2	5	12	2	3	1	3	2	5	2	3	0	0	0	0	0	0	0
8	6	5	5	2	2	1	2	4	1	2	16	4	3	0	0	0	0	0
8	0	5	2	3	2	0	3	1	5	2	16	4	1	0	0	0	0	0
8	6	2	9	2	5	1	2	4	1	2	16	4	3	0	0	0	0	0
1	3	0	30	1	3	3	1	2	2	0	13	0	1	1	1	1	1	1
8	0	11	3	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
8	0	9	8	2	9	6	3	3	0	2	16	4	3	0	0	0	0	0
1	3	6	17	2	4	5	2	2	2	0	15	1	0	1	1	1	0	0
8	3	0	9	2	2	3	2	4	2	0	4	2	0	1	1	0	0	1
8	7	14	15	1	2	3	3	1	4	0	1	0	0	1	0	0	0	0
1	9	10	12	2	3	7	3	2	5	2	16	4	3	0	0	0	0	0
1	7	12	5	2	4	2	2	0	4	0	15	1	0	1	1	0	0	0
1	3	0	11	2	4	3	2	0	2	0	11	1	1	1	1	0	1	1
8	6	5	14	2	3	1	3	4	1	0	6	1	0	1	1	1	0	1
8	7	0	15	2	1	5	2	4	4	2	16	4	3	0	0	0	0	0
8	5	2	7	2	3	3	2	5	3	0	9	2	0	0	1	0	0	0
1	7	0	20	1	3	1	1	0	4	2	16	4	3	0	0	0	0	0
8	5	7	5	2	2	3	2	1	3	2	16	4	3	0	0	0	0	0
8	5	11	1	2	1	1	2	4	3	2	16	4	2	1	1	0	0	0
1	5	0	19	1	2	0	1	5	3	0	9	4	0	1	1	0	1	1
1	3	0	21	1	4	3	1	2	2	0	8	4	0	1	1	0	1	1
1	11	0	28	1	6	3	1	5	5	0	2	1	0	1	0	0	0	0
1	6	3	7	1	2	1	2	2	1	0	15	1	1	1	1	0	0	0
1	6	2	10	2	2	1	3	0	1	0	5	1	0	1	1	0	0	1
8	3	6	1	1	5	3	2	4	2	0	11	1	0	1	1	0	0	1
8	5	5	3	2	1	2	2	5	3	2	16	4	3	0	0	0	0	0
5	3	5	3	2	2	3	2	4	2	0	3	1	0	1	0	0	0	0
8	3	0	11	2	4	3	2	1	2	0	9	1	0	1	1	0	0	1
8	7	0	19	1	1	3	1	4	4	2	16	4	3	0	0	0	0	0
1	3	0	10	2	4	2	2	5	2	2	16	4	3	0	0	0	0	0
1	7	0	15	2	2	3	2	0	4	0	1	3	1	1	1	0	0	1
8	6	5	4	2	1	2	3	5	1	2	16	4	3	0	0	0	0	0
8	5	0	11	2	4	3	2	4	3	0	6	1	0	1	1	0	1	1
8	7	2	9	1	2	1	1	1	5	0	8	4	0	0	0	0	0	0
1	3	0	14	2	5	3	2	5	2	2	16	4	3	0	0	0	0	0
8	3	9	12	2	4	3	3	4	2	0	10	1	0	1	1	1	0	1
1	5	0	12	2	2	3	2	5	3	0	5	1	0	1	1	0	0	1
5	0	0	9	2	0	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	6	8	2	3	6	3	5	2	2	16	4	3	0	0	0	0	0
8	0	0	9	2	3	6	2	3	0	0	7	0	0	1	1	0	0	1
8	5	0	15	2	3	3	2	5	3	0	3	1	0	1	1	1	0	1
3	6	9	7	1	5	3	2	5	1	0	7	0	0	1	1	0	0	1
8	7	0	12	2	1	3	2	4	4	2	0	1	1	0	0	0	0	0
8	0	14	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	5	0	26	1	3	3	1	2	5	0	5	1	0	1	1	1	1	1
8	3	0	19	2	6	7	2	4	2	0	15	1	0	1	1	0	0	1
8	6	0	12	2	3	3	3	5	1	0	4	1	0	1	1	0	0	0
1	7	2	14	0	3	6	2	5	5	2	16	4	3	0	0	0	0	0
1	3	0	14	2	2	3	2	5	2	0	5	1	0	1	1	1	0	1
8	5	0	22	1	2	2	1	1	5	0	5	3	0	1	1	0	1	1
8	6	3	2	2	1	1	2	5	1	2	1	0	0	0	0	0	0	0
1	7	0	32	0	3	3	0	0	5	0	5	0	0	1	1	0	0	1
1	0	5	3	2	1	6	3	3	0	2	4	0	0	0	0	0	0	0
0	3	6	10	2	1	2	3	2	2	2	16	4	3	0	0	0	0	0
8	3	0	17	2	3	3	2	1	2	0	15	1	0	1	1	1	0	0
8	3	0	10	2	1	2	2	1	2	0	2	1	0	0	0	0	0	0
8	3	0	11	2	1	3	2	4	2	0	1	0	1	1	1	0	1	1
8	0	9	7	2	5	0	3	3	0	0	7	1	0	0	0	0	0	0
1	3	0	20	1	1	2	1	0	2	0	11	0	0	1	1	1	0	1
1	9	0	13	2	1	3	2	2	5	2	1	4	2	0	0	0	0	0
8	5	0	12	2	3	3	2	4	3	0	8	4	1	1	1	0	0	1
8	7	2	6	1	5	3	2	1	4	2	16	4	3	0	0	0	0	0
1	3	0	12	2	5	3	2	5	5	0	6	1	0	1	1	0	0	0
1	5	0	11	2	4	2	2	2	5	0	8	1	1	1	1	0	0	1
8	6	2	9	1	1	5	2	1	1	2	2	0	1	0	0	0	0	0
8	11	2	3	2	1	1	2	4	5	0	4	3	0	1	1	0	0	0
8	3	0	12	2	2	3	2	4	2	0	3	0	0	1	1	1	1	1
8	6	2	7	1	3	0	2	1	1	2	16	4	3	0	0	0	0	0
1	3	0	20	1	3	3	1	0	2	0	4	0	0	1	0	0	0	0
8	7	0	20	1	4	3	1	4	4	0	15	1	0	1	1	0	0	1
8	3	0	15	2	3	3	2	5	2	0	2	3	0	1	1	0	0	0
8	6	0	16	2	3	2	2	5	1	0	9	1	0	1	1	0	0	1
8	3	5	7	1	2	3	2	1	2	0	5	1	0	1	1	1	0	1
8	6	2	22	1	3	0	2	1	1	0	6	1	0	1	1	0	0	0
1	3	5	6	2	1	2	3	5	2	0	1	0	0	1	1	0	0	0
0	3	5	2	2	1	2	2	4	5	0	8	1	0	1	1	0	0	1
8	5	0	16	2	2	3	2	4	3	0	5	1	0	1	1	1	1	1
1	3	0	8	2	2	6	2	5	2	2	16	4	3	0	0	0	0	0
1	9	13	8	2	2	3	3	0	5	0	5	1	0	1	1	0	0	1
1	6	0	21	1	2	3	1	0	1	0	6	1	0	1	1	1	0	0
8	3	0	10	2	1	2	2	1	2	0	3	4	0	1	1	0	0	0
8	7	0	16	2	4	3	2	5	4	0	2	4	0	1	0	1	0	0
8	0	0	9	2	9	6	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	9	2	1	3	2	4	2	0	3	2	0	1	1	1	0	1
8	0	0	15	2	2	0	2	4	5	2	3	1	0	1	1	0	0	0
1	1	10	24	1	3	3	2	2	5	0	3	1	0	1	1	0	0	1
8	0	0	9	2	1	0	2	3	0	2	16	4	3	0	0	0	0	0
1	7	0	9	2	3	6	2	5	4	0	3	3	0	1	1	0	0	1
8	0	12	9	2	3	6	3	3	0	2	16	4	3	0	0	0	0	0
8	0	14	13	1	9	6	3	5	5	2	16	4	3	0	0	0	0	0
1	5	5	3	1	4	3	1	2	5	2	16	4	3	0	0	0	0	0
1	3	0	11	2	3	3	2	4	2	0	3	0	0	1	0	0	0	0
1	3	14	18	2	2	2	3	2	2	0	1	4	0	1	1	1	0	0
1	3	2	8	1	2	3	2	5	2	2	2	3	1	0	0	0	0	0
8	3	0	15	2	3	3	2	1	2	0	1	2	0	1	0	0	0	0
1	5	2	7	1	2	6	1	5	5	2	16	4	3	0	0	0	0	0
1	7	0	31	1	3	3	1	0	5	0	5	1	0	1	1	1	1	1
8	3	0	11	2	1	1	2	4	2	2	16	4	1	0	0	0	0	0
8	9	2	9	2	2	0	3	1	5	0	10	2	0	1	1	0	0	0
8	3	0	20	1	1	2	1	1	2	0	8	2	0	1	0	1	0	0
8	3	10	14	2	2	3	2	5	2	0	10	4	0	0	1	0	0	0
1	5	3	7	1	1	3	2	2	5	2	3	0	0	1	1	1	0	0
1	9	2	15	1	2	1	2	0	5	2	16	4	3	0	0	0	0	0
8	3	0	14	2	4	3	2	4	2	0	15	1	0	1	1	0	0	0
8	3	0	9	2	2	3	2	4	2	0	4	0	0	1	1	1	0	1
1	7	0	10	2	3	2	2	2	4	0	13	0	0	1	1	1	0	0
1	3	0	22	1	2	3	1	2	2	2	16	4	3	0	0	0	0	0
8	5	11	11	2	1	1	3	4	3	2	16	4	3	0	0	0	0	0
8	3	12	17	1	4	3	3	4	2	0	10	1	1	1	1	1	1	1
1	7	3	6	2	6	2	3	2	4	0	6	1	0	1	1	0	0	1
8	6	2	9	1	2	1	2	1	1	0	15	1	0	1	0	1	0	0
8	5	0	27	1	3	2	1	1	5	2	4	0	0	1	0	0	0	0
8	3	0	9	2	3	3	2	4	2	0	9	1	0	1	1	0	0	1
1	9	2	6	2	2	3	3	0	5	0	7	4	2	0	0	0	0	0
1	7	2	4	2	1	1	2	2	4	2	16	4	3	0	0	0	0	0
8	9	0	12	2	1	1	3	5	5	2	16	4	3	0	0	0	0	0
8	7	2	7	2	2	1	2	4	4	2	16	4	3	0	0	0	0	0
1	9	9	9	2	3	3	3	2	5	2	16	4	3	0	0	0	0	0
8	3	7	1	2	2	3	2	5	2	0	2	0	0	1	1	1	0	1
1	3	0	10	2	1	2	2	5	2	2	16	4	3	0	0	0	0	0
1	5	11	8	2	3	1	2	5	3	2	16	4	3	0	0	0	0	0
1	5	0	23	1	2	3	1	0	3	0	4	1	0	0	0	1	0	0
8	2	12	8	2	4	0	3	5	5	0	10	1	0	1	1	1	0	1
1	7	0	10	2	3	2	2	5	4	2	1	4	0	0	0	0	0	0
8	6	0	10	2	4	4	2	4	5	2	16	4	3	0	0	0	0	0
1	3	0	19	2	5	3	2	5	2	2	15	1	0	1	1	1	0	1
1	3	5	12	2	2	3	3	5	2	2	7	4	1	0	0	0	0	0
8	3	0	17	2	3	3	2	4	2	0	1	4	0	1	1	0	0	1
1	9	0	21	1	3	3	1	5	5	0	5	1	0	1	1	1	0	1
1	3	0	18	2	4	2	2	2	2	0	3	1	1	1	0	1	0	0
8	6	12	10	2	3	3	2	4	1	0	15	4	0	1	1	1	0	0
1	7	0	8	2	3	3	2	5	5	0	6	1	0	1	1	1	0	1
8	3	0	14	2	1	3	2	1	2	2	2	1	1	0	0	0	0	0
8	5	9	12	1	1	3	2	5	3	0	3	0	0	1	1	0	0	1
8	5	1	4	2	4	6	2	1	3	2	16	4	3	0	0	0	0	0
8	3	2	7	1	3	1	2	5	2	0	6	4	0	1	1	1	0	0
8	12	5	4	1	1	3	1	5	5	0	13	0	0	1	1	1	0	1
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	11	2	4	2	2	3	2	5	5	2	16	4	3	0	0	0	0	0
1	0	9	8	2	1	6	3	5	0	2	1	4	1	0	0	0	0	0
1	7	0	23	1	2	0	1	5	4	0	3	1	0	1	0	0	0	0
1	3	2	9	2	2	0	3	4	5	2	16	4	3	0	0	0	0	0
8	7	0	17	2	4	1	2	4	4	0	2	1	0	1	1	1	0	0
8	3	2	13	2	1	3	3	5	2	0	16	3	0	1	1	0	0	1
1	6	9	8	2	3	2	3	5	1	0	3	1	0	1	1	1	1	1
1	5	0	23	1	4	3	1	0	5	0	15	4	0	1	1	1	0	0
1	5	0	10	2	1	2	2	5	3	2	16	4	3	0	0	0	0	0
1	3	2	6	2	2	6	3	2	2	0	3	1	0	1	0	1	0	0
1	9	10	31	0	2	3	3	2	5	0	2	1	0	1	1	0	0	1
8	3	0	11	2	3	3	2	5	2	0	5	1	0	1	1	0	0	0
8	3	0	9	2	3	2	2	1	2	2	2	3	1	0	0	0	0	0
1	7	5	1	1	5	2	1	2	5	0	15	1	0	1	1	1	0	1
8	1	0	23	1	2	3	1	5	5	0	3	4	0	0	1	0	0	0
8	5	7	3	2	3	5	2	5	5	0	4	3	0	1	1	0	0	1
1	5	0	20	1	3	3	3	2	3	0	1	1	1	1	1	1	1	1
1	7	0	17	2	3	0	2	2	4	0	4	1	0	1	1	0	0	0
1	9	13	15	1	1	6	2	5	5	0	6	1	0	1	1	1	0	1
8	7	0	16	2	2	3	2	5	4	2	1	3	0	1	0	0	0	0
8	3	0	15	2	1	3	2	4	2	0	3	4	2	1	1	0	0	1
1	6	10	17	2	1	2	3	2	1	0	9	4	1	1	1	1	1	1
8	3	11	20	1	4	3	3	4	2	0	7	1	0	1	1	1	1	1
1	3	0	13	2	3	1	2	2	2	1	5	1	0	1	1	1	0	1
1	0	0	9	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
8	7	11	7	2	2	3	2	4	4	2	16	4	3	0	0	0	0	0
8	3	0	12	2	3	2	2	4	2	0	14	1	0	1	1	0	0	1
8	6	9	3	2	2	3	2	4	1	0	3	1	1	1	1	0	1	1
8	3	0	9	2	4	3	2	1	2	0	15	1	0	1	1	1	0	1
1	9	13	8	2	4	3	2	2	5	2	1	4	1	0	0	0	0	0
8	0	9	10	2	1	6	3	3	0	2	16	4	3	0	0	0	0	0
6	7	5	4	2	1	5	3	5	4	0	9	1	0	1	1	0	0	1
8	7	4	3	2	4	3	2	4	4	2	16	4	3	0	0	0	0	0
1	5	0	27	1	3	3	1	5	3	0	5	1	0	1	1	1	0	0
1	3	0	8	2	1	2	2	0	2	0	15	1	0	1	1	1	1	1
8	5	14	13	2	5	3	2	4	3	0	9	1	1	1	1	0	1	1
1	3	6	2	2	3	3	2	2	2	0	5	1	0	1	1	1	1	1
8	7	5	11	2	2	3	2	5	4	0	5	0	2	1	0	0	0	0
8	8	0	11	2	4	1	2	4	5	0	7	1	0	1	1	1	1	1
1	3	0	20	1	1	3	1	0	2	0	4	0	0	1	1	0	1	1
1	9	10	8	2	2	2	3	2	5	0	9	0	1	1	1	0	0	1
1	6	14	1	2	3	6	2	0	1	0	9	4	0	1	1	1	0	0
1	3	7	3	1	3	2	1	5	5	0	6	4	1	1	1	0	0	1
8	2	5	11	2	2	0	3	4	5	2	16	4	3	0	0	0	0	0
1	7	0	31	1	4	1	3	2	5	0	3	2	0	1	1	1	1	1
8	11	6	11	2	3	3	3	4	5	2	16	4	3	0	0	0	0	0
1	3	6	3	2	4	3	2	2	2	0	4	1	0	1	1	0	0	1
1	1	0	23	1	4	3	1	5	5	2	16	4	3	0	0	0	0	0
8	7	0	9	2	2	2	2	1	4	2	16	4	3	0	0	0	0	0
1	5	0	27	1	2	3	1	0	5	0	15	1	0	1	1	1	1	1
1	3	12	11	1	2	3	2	2	2	0	6	4	0	1	1	0	0	0
1	5	2	6	1	1	1	2	2	5	2	3	1	1	0	0	0	0	0
8	5	0	17	2	1	2	2	4	3	2	16	4	3	0	0	0	0	0
1	11	0	11	2	3	3	2	0	5	0	15	1	0	1	1	1	0	0
8	6	2	6	2	3	1	2	4	1	2	2	4	3	0	0	0	0	0
1	3	0	21	1	4	3	1	5	2	0	2	0	1	1	1	1	0	0
8	7	2	4	2	1	6	2	1	4	0	8	4	1	1	1	0	0	1
1	9	3	15	1	3	3	2	5	5	0	15	0	0	1	1	1	0	1
8	6	2	6	2	1	1	3	5	1	2	3	4	0	0	0	0	0	0
8	1	2	18	1	2	3	3	1	5	0	4	2	0	1	1	0	1	1
1	6	0	20	1	1	2	1	5	1	1	2	1	0	1	0	0	0	0
8	9	2	4	2	3	2	3	4	5	2	16	4	3	0	0	0	0	0
8	6	6	2	2	4	3	2	1	1	0	9	1	0	1	1	1	0	1
8	6	9	11	2	1	2	3	1	1	0	15	1	0	1	1	0	0	1
1	0	0	10	2	2	6	2	5	0	0	4	4	0	1	1	1	0	1
1	3	2	8	2	2	3	2	5	2	0	1	3	0	1	1	0	0	0
8	3	0	12	2	2	5	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	12	2	2	2	2	4	2	0	1	1	1	1	1	0	0	1
8	7	10	15	2	2	3	3	1	4	0	1	3	1	1	0	0	0	0
8	0	9	9	2	1	0	3	3	0	2	16	4	3	0	0	0	0	0
8	3	0	9	2	2	2	2	4	2	2	16	4	3	0	0	0	0	0
1	3	3	4	2	6	6	2	5	2	0	1	0	1	1	1	0	0	1
8	5	2	9	2	1	1	3	1	3	0	6	2	0	0	0	0	0	0
8	7	0	18	1	3	6	3	4	5	0	6	0	0	1	1	0	0	0
1	5	2	4	1	2	0	1	2	5	2	16	4	3	0	0	0	0	0
1	9	0	8	2	2	2	2	2	5	2	0	4	0	0	0	0	0	0
1	3	2	6	2	3	2	2	2	2	0	11	4	1	1	1	0	1	1
1	8	11	5	1	2	1	1	5	5	0	6	1	0	1	0	0	0	0
1	9	6	1	1	0	7	1	2	5	2	16	4	3	0	0	0	0	0
8	3	0	20	1	2	3	1	4	2	0	2	1	0	1	1	0	0	1
1	11	2	9	2	3	1	2	2	5	0	6	1	1	1	1	1	0	1
1	11	0	16	2	2	3	2	5	5	0	4	2	0	1	1	0	0	1
1	0	9	10	2	1	6	3	5	0	2	0	4	0	0	0	0	0	0
1	6	5	2	2	1	3	2	5	1	2	16	4	3	0	0	0	0	0
8	6	2	16	2	1	3	3	4	1	0	5	0	2	1	1	0	0	1
1	6	0	11	2	2	2	2	5	1	0	3	1	0	1	1	1	1	0
8	6	2	4	2	2	3	2	4	1	0	15	1	0	0	0	0	0	0
1	2	12	9	2	2	0	3	0	5	2	3	1	0	0	0	0	0	0
1	3	6	6	2	5	2	2	5	2	0	7	1	0	1	1	0	0	1
1	1	0	20	1	1	3	1	0	5	0	6	2	0	1	1	1	0	1
1	6	0	11	2	5	2	2	2	1	0	3	1	0	1	1	1	0	0
8	0	0	9	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
8	0	0	8	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
1	7	2	9	1	2	2	1	0	5	0	15	3	2	0	0	0	0	0
8	3	0	12	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
8	0	0	8	2	3	6	2	5	0	2	16	0	0	1	0	0	0	0
1	5	0	20	1	3	3	1	2	3	0	5	1	0	1	1	1	1	1
1	1	14	1	1	2	6	1	0	5	0	4	1	0	1	0	1	0	0
8	11	0	9	2	3	2	2	4	5	0	7	4	0	1	1	0	1	1
8	3	0	12	2	2	3	2	4	2	0	3	1	0	1	1	0	0	1
1	5	5	4	2	1	2	3	5	3	0	1	4	1	1	1	0	0	1
1	0	2	12	1	2	7	2	0	5	2	16	4	3	0	0	0	0	0
8	5	0	18	2	2	4	2	4	3	0	3	2	0	1	1	0	1	1
1	3	0	20	1	2	3	1	5	2	0	3	4	0	1	1	1	0	1
8	3	7	1	2	5	2	2	4	2	0	5	1	0	1	1	0	0	1
2	0	9	8	2	1	6	3	3	0	0	13	1	0	1	1	0	0	0
1	6	12	10	2	4	5	3	2	1	0	5	0	0	0	0	0	0	0
8	0	0	9	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
1	8	2	12	1	2	1	1	2	5	0	1	1	1	1	1	0	0	0
1	11	10	20	1	1	2	3	2	5	2	16	4	3	0	0	0	0	0
8	3	5	15	1	2	1	2	5	2	2	16	4	3	0	0	0	0	0
8	5	0	9	2	1	4	2	4	3	2	16	4	3	0	0	0	0	0
8	6	5	12	2	3	3	3	4	1	0	5	1	0	1	1	1	0	1
1	3	0	13	2	4	3	2	0	2	0	6	2	0	1	1	0	0	0
1	7	0	18	2	4	3	2	5	4	0	8	4	0	1	1	0	0	1
8	3	2	7	1	2	2	2	4	2	2	16	4	3	0	0	0	0	0
1	9	12	25	1	1	7	3	2	5	2	16	4	3	0	0	0	0	0
1	7	0	20	1	2	1	1	0	4	0	15	4	0	1	1	0	0	0
1	3	12	15	1	3	3	2	1	2	2	16	4	3	0	0	0	0	0
1	3	0	20	1	5	3	1	2	2	0	15	1	0	1	1	0	0	0
1	6	9	11	2	2	3	3	5	1	0	4	0	0	1	1	0	0	0
8	3	9	15	2	4	6	3	1	2	2	16	4	3	0	0	0	0	0
8	3	0	20	1	3	3	1	1	2	0	3	1	0	1	1	1	0	0
1	5	2	20	1	1	1	3	5	5	0	3	4	0	0	0	0	0	0
1	7	14	14	2	2	3	3	5	4	2	16	4	1	1	1	0	0	1
1	6	2	9	1	3	1	2	2	1	0	8	0	0	1	1	0	0	1
1	2	1	1	2	3	0	2	5	5	2	3	0	1	1	0	0	0	0
1	3	2	10	2	3	3	3	5	2	0	6	1	0	1	1	0	0	1
1	7	2	12	1	1	1	1	5	5	0	6	1	1	0	0	0	0	0
8	7	0	31	1	2	3	1	1	5	0	1	1	0	1	1	0	0	0
8	0	11	9	2	2	5	3	5	0	0	9	4	1	0	0	0	0	0
8	6	5	2	2	3	2	2	4	1	2	16	4	3	0	0	0	0	0
8	0	9	9	2	9	6	3	3	0	2	16	4	3	0	0	0	0	0
8	5	2	8	1	2	6	2	1	5	0	2	1	0	1	1	0	0	0
8	0	0	14	2	1	6	2	1	5	0	7	0	0	1	1	1	0	1
8	0	5	1	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
8	7	6	4	1	3	3	1	1	5	0	6	1	0	1	1	1	0	1
8	3	0	11	2	4	3	2	4	2	0	15	1	0	1	1	0	0	1
8	5	0	21	1	2	3	1	4	3	0	5	1	0	1	1	1	0	1
8	0	0	10	2	2	6	2	3	0	0	4	1	1	1	0	0	0	0
8	0	0	10	2	2	6	2	3	0	0	4	3	0	1	1	0	0	1
8	3	0	15	2	4	3	2	5	2	0	8	0	0	1	1	1	0	1
1	5	9	13	2	1	3	3	0	3	0	15	1	0	1	1	0	0	1
8	3	0	16	2	4	3	2	1	2	0	4	1	0	0	0	1	0	0
8	0	9	8	2	1	6	3	5	0	0	6	1	0	1	1	0	0	0
8	6	2	18	2	2	1	3	1	1	2	1	4	1	0	0	0	0	0
1	5	0	11	2	2	2	2	0	3	0	5	4	0	1	1	0	0	0
1	0	5	5	2	2	0	3	5	0	2	1	3	0	0	0	0	0	0
8	3	0	10	2	4	2	2	4	2	2	16	3	0	0	1	0	0	0
1	3	0	17	2	3	3	2	5	2	0	6	2	0	1	1	1	0	0
1	11	2	10	2	3	0	3	2	5	0	6	3	0	0	1	0	0	0
8	3	0	12	2	4	3	2	4	2	2	16	4	3	0	0	0	0	0
8	6	14	2	2	3	1	2	4	1	2	16	4	3	0	0	0	0	0
1	3	0	19	1	2	2	1	2	2	2	1	3	0	0	0	0	0	0
1	6	0	20	1	5	3	1	0	1	2	16	4	3	0	0	0	0	0
1	9	0	31	1	4	3	1	2	5	0	6	1	0	1	1	1	0	1
8	6	2	9	2	1	3	3	1	1	0	3	4	1	1	1	0	0	1
8	3	0	20	2	2	3	2	4	2	0	6	0	0	1	1	0	0	1
8	3	0	9	2	2	2	2	1	2	0	8	1	0	0	1	0	0	0
8	3	0	12	2	1	3	2	4	5	0	3	3	0	1	1	0	1	1
1	3	12	4	2	3	3	2	2	2	0	4	1	0	1	1	0	1	1
8	7	0	12	2	2	3	2	4	4	2	1	4	0	0	0	0	0	0
1	3	0	19	1	5	3	1	5	2	0	15	1	0	1	1	1	1	1
1	11	0	19	2	2	4	2	5	5	0	10	1	0	1	1	0	0	1
8	3	0	12	2	2	3	2	4	2	0	3	1	0	1	1	1	0	1
8	3	6	1	1	2	3	2	5	2	0	1	2	0	1	1	0	1	1
1	3	0	20	1	5	3	1	5	2	0	11	1	0	1	1	1	1	1
8	7	2	6	1	1	1	1	1	5	2	2	1	1	0	0	0	0	0
1	3	14	1	2	1	6	2	5	2	0	2	0	0	1	1	0	0	1
8	11	7	6	2	1	2	3	4	5	2	16	4	3	0	0	0	0	0
1	1	0	25	1	2	3	1	5	5	0	3	3	0	1	0	0	0	0
8	0	0	9	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	5	0	8	2	3	2	2	0	3	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	0	2	3	0	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	6	2	3	0	0	15	1	1	1	1	0	0	1
1	5	11	9	2	3	1	3	5	3	0	3	4	1	1	1	0	0	1
1	0	12	16	1	2	0	3	2	5	0	2	2	0	1	1	0	0	1
1	3	0	15	2	3	3	2	5	2	0	4	1	0	1	1	1	1	1
8	7	2	14	1	2	3	2	5	5	2	5	3	0	0	0	0	0	0
8	2	5	4	2	2	0	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	18	2	3	3	2	5	2	0	15	1	0	1	1	1	0	1
8	0	0	8	2	3	0	2	3	0	2	6	4	0	1	1	0	0	1
1	3	11	10	2	7	2	2	0	2	0	12	1	0	1	1	0	0	1
1	7	0	13	2	1	0	2	5	4	2	16	4	3	0	0	0	0	0
1	9	2	15	1	2	3	2	0	5	0	4	4	0	1	1	0	0	0
8	6	2	10	1	1	3	2	1	1	0	4	4	0	1	1	0	0	1
1	6	0	18	2	1	3	2	2	1	0	5	1	0	0	0	1	0	0
1	11	11	12	2	3	4	2	5	5	2	16	4	3	0	0	0	0	0
8	5	2	9	2	1	1	3	4	3	0	3	4	1	1	1	0	0	1
1	7	0	20	1	4	3	1	5	4	2	16	4	3	0	0	0	0	0
1	3	0	12	2	3	3	2	5	2	0	4	2	0	1	0	1	0	0
8	5	2	6	2	2	3	2	5	3	2	0	4	0	0	0	0	0	0
8	0	0	8	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	5	0	8	2	5	3	2	2	3	0	15	2	0	1	1	1	0	1
8	5	0	16	2	3	3	2	1	3	0	15	1	0	1	1	1	0	1
8	7	0	12	2	2	1	2	4	4	0	1	1	0	1	1	0	0	1
1	6	2	18	1	5	3	2	2	1	0	3	0	0	1	0	0	0	0
8	7	9	8	2	1	1	2	4	5	0	7	0	0	1	0	0	0	0
1	5	2	13	2	3	0	3	2	3	0	6	4	2	1	1	0	0	0
8	3	0	10	2	3	6	2	4	2	0	9	1	0	1	1	1	1	1
1	7	0	12	2	2	1	2	0	4	0	2	1	0	1	1	0	0	0
8	5	0	7	2	3	3	2	5	3	0	7	0	0	1	0	0	0	0
1	0	2	8	1	2	0	1	2	5	0	4	1	0	1	0	0	0	0
8	3	0	17	2	4	3	2	4	2	0	6	1	0	1	1	0	1	0
1	3	0	12	2	1	1	2	0	2	2	3	4	2	0	0	0	0	0
1	0	9	9	2	9	6	3	5	0	2	16	4	3	0	0	0	0	0
1	3	0	18	2	4	3	2	5	2	0	10	4	0	0	1	0	0	0
1	3	0	18	2	3	3	2	0	2	0	8	2	0	1	1	1	1	1
1	7	9	24	1	3	3	3	5	4	2	16	4	3	0	0	0	0	0
1	9	6	3	2	4	6	2	5	5	0	7	4	0	1	1	0	0	1
8	6	0	11	2	3	3	2	4	1	2	0	4	0	0	0	0	0	0
8	3	14	10	2	3	3	3	4	2	0	4	1	0	1	1	0	0	1
1	3	0	13	2	3	3	2	5	5	0	5	1	0	1	1	1	0	0
8	3	0	16	2	4	3	2	1	2	0	13	1	0	1	1	1	1	1
8	3	0	10	2	4	2	2	1	2	0	15	1	0	1	1	0	0	1
1	3	0	11	2	1	3	2	5	2	1	5	1	0	0	0	1	0	0
8	6	2	9	2	1	5	3	5	1	1	16	0	0	0	0	0	0	0
8	3	6	2	1	4	6	2	5	2	0	15	1	0	1	1	1	0	1
8	3	9	17	2	2	3	2	4	2	0	3	4	2	1	1	0	1	1
1	7	0	10	2	2	2	2	5	4	0	4	2	0	1	1	0	0	1
1	3	0	10	2	2	2	2	0	2	0	3	2	0	1	1	0	0	1
1	1	0	23	1	3	3	1	5	5	2	9	1	0	1	1	0	0	1
1	3	8	7	2	2	0	3	0	2	0	0	1	0	1	1	0	0	1
8	6	2	8	1	1	1	2	1	1	0	10	1	0	1	1	1	0	1
1	3	0	21	1	2	3	1	2	2	0	12	4	1	1	1	0	0	1
8	5	0	12	2	4	3	2	1	3	2	2	0	0	1	1	0	0	0
8	0	9	7	2	9	6	3	3	0	0	11	0	0	1	1	0	0	1
1	3	0	8	2	1	2	2	5	2	0	1	4	1	1	1	0	0	0
1	7	0	22	1	3	3	1	2	4	0	6	1	0	1	1	0	1	1
1	3	0	10	2	2	2	2	0	2	0	8	1	0	1	1	1	0	1
1	3	0	20	1	4	0	1	0	2	0	8	1	0	1	1	1	0	1
1	7	11	9	2	2	2	3	2	4	2	16	4	0	1	1	0	0	1
8	7	0	20	1	3	3	1	4	4	2	16	4	3	0	0	0	0	0
1	3	1	7	1	6	3	2	2	2	0	3	1	0	0	0	1	0	0
1	11	8	30	1	2	3	3	2	5	0	1	2	0	1	1	1	0	0
1	6	7	6	1	3	6	2	5	1	0	5	1	0	0	0	1	0	0
8	11	14	1	2	3	3	2	4	5	2	16	4	3	0	0	0	0	0
8	8	0	10	2	4	2	2	1	5	0	4	0	0	1	1	0	0	1
1	11	11	21	1	3	3	2	0	5	0	4	1	0	1	1	1	0	1
8	7	5	17	1	4	3	3	5	4	2	16	4	3	0	0	0	0	0
1	7	0	12	2	2	1	2	5	4	0	4	4	1	0	1	0	0	0
1	3	0	11	2	3	2	2	2	2	2	16	4	3	0	0	0	0	0
1	3	12	9	2	2	3	3	2	2	2	16	4	3	0	0	0	0	0
1	7	2	29	0	4	2	2	2	5	2	16	4	3	0	0	0	0	0
8	3	0	19	1	3	2	1	4	2	0	11	1	0	1	1	0	0	1
1	3	0	21	1	4	3	1	5	2	0	7	0	1	1	1	1	0	1
1	3	14	15	2	3	3	2	2	2	0	4	4	0	1	1	1	0	1
8	11	10	18	2	5	3	2	5	5	0	15	4	0	1	1	1	0	1
1	3	0	11	2	3	3	2	2	2	0	5	1	0	1	1	0	0	0
8	11	0	31	1	2	2	1	1	5	2	16	4	3	0	0	0	0	0
8	3	10	20	1	1	3	2	4	2	2	0	4	0	0	0	0	0	0
1	5	13	14	1	3	3	3	2	5	0	6	1	0	1	1	0	0	1
8	9	14	18	2	1	3	2	4	5	0	5	4	1	1	1	0	0	1
1	0	0	10	2	5	6	2	5	0	0	15	1	0	1	1	0	0	0
8	0	9	8	2	4	6	3	3	0	2	1	1	0	0	0	0	0	0
1	3	3	20	1	3	3	2	5	2	0	8	1	0	1	1	0	0	0
1	3	9	16	2	1	1	3	5	2	2	16	4	3	0	0	0	0	0
8	3	14	12	2	4	3	3	4	2	0	7	1	0	1	1	1	1	1
1	5	0	8	2	1	6	2	5	3	0	1	2	0	1	1	0	0	0
1	7	9	10	1	4	6	1	5	5	0	7	4	1	1	1	1	0	1
8	3	5	1	2	2	3	2	4	2	0	6	3	1	1	1	0	0	0
1	5	0	24	1	1	3	1	2	5	0	3	0	0	1	1	0	0	0
8	3	0	17	2	2	3	2	4	2	0	1	1	0	1	1	0	0	0
1	7	0	19	2	6	4	2	0	4	0	12	1	0	1	1	0	0	0
1	11	2	7	2	1	0	2	2	5	2	16	4	3	0	0	0	0	0
8	3	9	7	2	3	3	2	1	2	0	1	1	0	1	1	0	1	1
8	5	9	4	1	3	3	2	1	3	2	16	4	3	0	0	0	0	0
1	5	5	11	1	4	3	2	0	5	2	1	3	0	1	0	1	0	0
8	3	0	8	2	4	3	2	4	2	0	13	4	0	1	1	1	0	1
8	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	0	23	1	2	3	1	2	2	0	3	4	1	1	1	0	0	1
8	7	2	8	1	1	2	1	1	5	2	5	4	0	0	0	0	0	0
1	5	3	10	1	7	3	2	2	5	0	15	0	0	1	1	0	0	1
8	0	9	8	2	9	6	3	3	0	0	4	0	2	1	1	0	0	0
1	3	3	1	2	2	2	2	5	2	0	5	1	0	0	0	0	0	0
8	3	0	25	1	1	3	1	1	2	2	16	4	3	0	0	0	0	0
1	3	0	13	2	3	3	2	5	2	2	0	1	0	0	0	1	0	0
1	3	0	13	2	1	0	2	5	2	0	1	2	0	1	1	0	0	0
8	0	9	8	2	2	0	3	3	0	0	16	4	0	1	1	0	0	0
1	5	2	5	1	1	2	2	5	5	2	16	4	3	0	0	0	0	0
2	3	2	9	2	3	3	3	2	2	2	0	4	0	0	0	0	0	0
8	5	2	6	1	2	3	2	4	3	0	2	3	0	1	1	0	0	1
1	6	6	19	1	5	1	2	2	5	0	15	1	0	1	1	0	0	0
1	7	2	6	2	3	0	2	5	4	0	8	1	0	1	0	0	0	0
1	2	3	6	1	4	0	2	2	5	1	16	0	0	1	0	0	0	0
1	7	0	20	1	4	3	1	5	4	0	9	1	0	1	1	1	1	1
8	3	6	1	1	4	3	2	4	2	0	11	2	0	1	1	1	0	1
8	11	14	2	2	4	2	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	10	2	3	3	2	5	2	2	16	4	0	1	1	0	0	1
1	7	2	7	2	1	3	3	5	4	2	16	4	3	0	0	0	0	0
1	7	0	23	1	2	3	1	0	4	2	16	4	3	0	0	0	0	0
8	3	0	17	2	4	3	2	4	2	0	11	4	0	1	1	0	0	1
8	5	0	14	2	1	1	2	4	3	0	7	1	0	1	1	0	0	1
8	9	1	5	1	2	3	2	4	5	0	2	1	0	1	1	0	0	1
8	3	5	12	1	1	3	2	4	2	0	10	1	0	1	1	1	0	1
8	7	0	12	2	3	3	2	5	4	0	6	4	0	0	1	0	0	1
1	7	0	17	2	2	3	3	0	4	0	1	4	0	1	1	0	0	1
1	0	0	9	2	4	0	2	5	0	2	1	0	0	0	0	0	0	0
1	7	0	14	2	2	0	2	2	4	0	3	0	1	1	1	0	0	1
1	5	0	25	1	1	3	1	5	5	2	16	4	3	0	0	0	0	0
1	7	8	3	2	2	1	2	2	4	0	2	0	0	1	0	0	0	0
1	3	0	18	2	4	3	2	0	2	0	9	2	0	1	0	1	1	0
8	9	11	8	2	3	2	3	4	5	0	2	4	0	1	1	0	1	1
8	5	0	30	1	3	3	1	1	3	0	2	1	0	1	1	0	0	1
8	7	6	13	1	3	3	2	1	5	2	16	4	3	0	0	0	0	0
8	0	14	10	2	2	0	3	3	0	0	5	4	0	1	0	0	0	0
4	0	5	8	1	3	3	1	5	5	0	16	2	0	1	0	0	0	0
8	0	0	8	2	2	0	2	3	0	2	16	4	3	0	0	0	0	0
1	5	2	15	1	2	1	2	0	5	0	4	3	0	1	1	0	0	0
8	3	0	8	2	2	3	2	4	2	2	16	3	0	0	0	1	0	0
8	0	9	9	2	2	6	3	5	0	0	11	4	0	0	0	0	0	0
1	5	0	20	2	3	3	2	2	3	0	7	1	0	1	1	0	0	0
1	3	0	17	2	1	1	2	0	2	2	1	4	0	0	0	0	0	0
8	2	2	6	2	2	0	2	5	5	2	1	3	1	0	0	0	0	0
8	3	0	13	2	4	1	2	4	2	2	16	4	3	0	0	0	0	0
0	3	5	9	2	1	3	3	4	2	0	10	1	0	1	0	1	1	0
8	5	0	21	1	4	3	1	4	5	0	4	0	0	1	1	0	0	1
8	3	0	12	2	2	3	3	4	2	0	2	2	0	1	1	0	0	1
1	3	0	20	1	2	3	1	2	2	0	16	0	0	1	1	0	1	1
1	3	7	5	2	1	5	2	5	2	2	16	4	3	0	0	0	0	0
1	3	0	10	2	4	2	2	2	2	0	6	0	0	1	1	1	0	0
1	5	3	11	2	4	3	3	2	3	0	1	1	0	1	1	0	0	0
8	3	9	22	1	4	3	2	5	2	0	15	1	0	1	1	1	0	1
1	6	2	11	2	2	3	3	2	1	0	6	2	0	1	1	0	0	1
1	7	0	13	2	4	1	2	5	4	0	14	3	1	1	0	1	0	0
8	3	2	5	2	1	0	3	4	5	2	16	4	3	0	0	0	0	0
8	3	0	15	2	3	3	2	4	2	0	4	4	0	1	0	0	0	0
8	3	0	16	2	3	3	2	4	2	0	2	0	0	1	1	0	0	0
1	6	0	8	2	3	3	2	2	1	0	3	4	0	1	0	0	0	0
1	7	0	15	2	1	1	2	5	4	0	3	2	0	1	1	0	0	1
8	6	0	11	2	1	3	2	5	1	0	3	0	0	0	0	0	0	0
8	9	2	8	2	3	3	2	4	5	0	10	1	1	1	1	1	0	1
8	5	2	5	1	3	2	2	1	3	0	7	0	0	1	0	0	0	0
8	3	0	11	2	3	3	2	1	2	0	2	1	0	1	1	0	0	1
8	6	2	9	2	4	2	3	1	1	2	16	4	3	0	0	0	0	0
8	3	0	13	2	2	3	2	4	2	0	2	1	0	1	1	1	1	1
1	0	0	10	2	1	6	2	5	0	2	16	4	0	0	0	0	0	0
8	5	5	7	1	2	3	2	5	3	2	16	4	3	0	0	0	0	0
1	11	10	31	1	2	3	3	2	5	0	15	1	0	1	1	0	1	1
1	7	0	12	2	2	3	2	0	4	0	15	0	0	1	1	0	0	0
8	3	0	18	2	4	3	2	4	2	0	2	1	0	1	1	0	1	1
8	13	0	12	2	2	3	2	4	5	0	3	0	0	1	1	1	1	1
8	3	0	11	2	3	3	2	5	2	0	7	2	0	1	0	0	0	0
8	5	6	10	2	1	2	3	4	3	0	2	1	0	1	1	1	1	1
8	5	0	11	2	1	1	2	4	3	2	16	4	3	0	0	0	0	0
8	5	3	15	1	2	1	3	1	3	0	6	0	0	1	1	1	0	0
8	0	9	8	2	2	0	3	3	0	2	16	4	3	0	0	0	0	0
1	7	0	16	2	3	3	2	5	4	0	4	1	0	1	1	1	0	1
8	7	0	13	2	1	3	2	4	4	0	15	1	0	1	1	0	1	1
1	2	12	12	2	2	6	3	2	5	0	4	0	2	1	1	0	0	1
8	6	5	7	2	1	0	3	5	1	0	15	0	0	1	1	0	1	1
8	5	3	5	1	5	2	2	1	3	0	11	0	1	1	0	0	0	0
8	0	9	7	3	2	0	3	3	0	0	4	1	0	1	1	0	0	0
8	8	0	10	2	4	4	2	5	5	2	16	4	3	0	0	0	0	0
1	0	0	8	2	2	6	2	5	0	0	5	4	0	1	1	0	0	1
8	7	2	6	1	1	3	2	5	4	2	16	4	3	0	0	0	0	0
8	0	9	9	2	2	6	3	5	0	2	16	4	3	0	0	0	0	0
1	3	0	14	2	2	2	3	5	2	0	1	4	1	1	1	0	0	1
8	6	2	20	1	1	3	3	5	1	2	16	4	0	1	0	0	0	0
8	0	0	10	2	3	6	2	5	0	0	6	3	0	0	0	0	0	0
1	11	2	22	1	3	3	2	2	5	0	6	1	1	1	1	0	1	1
1	0	5	5	2	3	2	3	5	0	0	7	1	0	1	0	1	0	0
1	11	2	7	2	2	6	2	2	5	2	1	3	0	1	1	0	0	0
8	5	0	10	2	2	2	2	4	3	0	5	2	0	1	1	0	0	1
8	3	0	14	2	1	3	2	4	2	0	4	4	0	1	1	0	1	1
1	3	0	12	2	3	3	2	0	2	0	5	1	0	1	1	1	0	0
8	0	0	8	2	4	6	2	3	0	0	2	1	1	1	1	0	0	1
1	3	13	14	1	4	3	3	0	2	0	4	1	0	1	1	1	0	1
1	7	2	11	1	4	5	2	2	5	2	16	4	3	0	0	0	0	0
1	6	4	5	1	1	3	2	5	1	2	16	3	2	1	1	0	0	0
8	3	1	7	1	1	1	2	4	2	0	10	0	0	1	1	0	0	1
1	5	2	14	2	2	1	2	2	3	2	2	0	1	1	0	0	0	0
1	0	0	10	2	1	6	2	5	0	0	15	1	0	1	1	0	0	1
1	3	2	17	1	4	3	2	5	5	2	5	2	0	1	1	0	0	0
1	5	0	17	2	2	3	2	2	3	0	4	2	0	1	1	0	0	1
8	1	0	23	1	3	3	1	5	5	0	3	2	0	1	1	1	0	1
8	6	0	18	2	3	3	2	4	1	0	8	1	0	1	1	1	0	1
1	5	2	4	1	1	3	1	2	5	0	5	1	0	1	1	0	0	0
8	7	10	20	2	4	2	2	4	4	2	16	4	3	0	0	0	0	0
8	3	0	9	2	4	2	2	5	2	2	7	4	0	1	1	1	0	0
8	5	0	11	2	2	3	2	4	3	0	7	1	0	1	1	1	0	1
3	7	2	11	1	1	1	1	2	5	2	16	4	3	0	0	0	0	0
8	0	9	9	2	1	6	3	5	0	2	1	4	0	0	0	0	0	0
8	6	0	21	1	2	3	1	5	1	0	3	1	0	1	1	0	1	1
8	11	11	4	2	1	3	2	5	5	2	16	4	3	0	0	0	0	0
1	7	2	14	1	2	2	2	2	5	0	5	3	0	1	1	0	1	0
8	0	0	10	2	2	6	2	3	0	2	3	3	0	1	0	0	0	0
1	0	0	10	2	1	6	2	5	0	0	6	0	1	1	1	1	0	1
1	11	9	11	2	3	1	2	5	5	0	8	1	0	1	1	0	1	1
8	6	0	18	2	4	3	2	4	1	0	15	1	0	1	1	1	0	1
8	11	8	17	2	2	3	2	4	5	0	3	1	0	1	1	0	0	1
8	5	2	7	1	1	2	2	5	5	2	3	1	0	1	1	1	0	0
1	7	2	12	0	3	2	1	5	5	2	16	3	0	0	0	0	0	0
8	5	0	17	2	1	4	2	5	3	0	13	3	0	1	1	0	1	1
3	0	12	19	1	4	0	3	2	5	0	3	0	0	1	1	1	1	1
1	5	2	6	1	3	1	1	2	5	2	16	4	3	0	0	0	0	0
8	5	2	7	1	1	6	2	1	5	0	11	2	0	1	1	1	1	1
1	7	10	18	2	4	2	2	2	4	0	8	1	0	1	1	0	0	0
8	0	14	19	1	2	6	3	1	5	2	16	4	3	0	0	0	0	0
8	3	0	8	2	6	3	2	5	2	0	15	1	0	1	1	1	0	1
1	8	0	25	1	3	3	3	5	5	2	16	4	3	0	0	0	0	0
8	3	13	14	2	6	0	3	1	2	0	15	0	0	1	1	1	0	1
8	9	0	17	2	1	1	2	4	5	0	6	0	2	1	1	0	0	1
8	5	0	9	2	3	3	2	4	3	0	12	1	0	1	1	1	1	1
8	6	9	11	2	1	3	3	5	1	0	11	2	0	1	1	0	0	0
8	6	14	6	2	2	3	2	1	1	0	6	1	1	1	1	0	0	1
1	3	0	13	2	1	7	2	5	2	2	16	4	3	0	0	0	0	0
8	3	0	15	2	2	3	2	4	2	2	16	3	0	1	0	0	0	0
1	11	7	1	1	3	3	1	2	5	0	4	1	1	1	1	0	0	0
1	3	0	13	2	3	3	2	5	2	0	2	1	0	0	0	0	0	0
8	3	5	6	2	1	2	3	1	2	0	8	0	0	1	1	1	1	1
8	11	12	9	2	1	1	2	4	5	0	10	0	0	1	1	1	0	1
1	11	2	3	2	1	3	2	2	5	2	16	4	3	0	0	0	0	0
8	3	0	13	2	1	3	2	5	2	2	16	0	0	1	1	0	0	1
1	7	11	8	2	3	6	2	5	4	0	6	1	0	1	1	1	0	1
8	3	0	21	1	1	3	1	4	5	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
8	5	0	14	2	4	3	2	4	3	0	12	1	0	1	1	0	0	1
1	6	2	3	2	3	1	2	2	1	0	5	1	0	1	1	0	0	0
8	7	0	13	2	4	3	2	5	4	0	7	0	1	1	1	1	1	1
1	3	0	9	2	2	3	2	5	2	0	3	2	0	1	1	1	0	1
1	3	0	9	2	3	3	2	0	2	2	16	4	3	0	0	0	0	0
8	5	0	17	2	2	3	2	4	3	0	7	0	0	1	1	0	1	1
1	3	0	21	1	4	3	1	5	2	0	7	1	0	1	1	1	0	1
1	1	10	24	1	4	3	3	0	5	0	11	1	0	1	1	0	0	1
0	3	12	12	2	4	3	3	4	2	2	16	4	3	0	0	0	0	0
1	7	0	30	1	3	3	1	0	5	0	6	1	0	1	1	1	1	1
1	0	0	9	2	1	0	2	5	0	0	15	1	0	1	1	0	0	1
1	0	0	8	2	3	6	2	5	0	0	6	1	0	1	1	0	0	0
1	5	9	7	2	2	3	3	5	3	0	1	1	0	1	1	0	0	1
1	7	2	4	2	1	1	2	5	4	0	5	0	1	1	1	0	0	0
8	9	0	18	2	4	0	2	4	5	0	11	1	0	1	1	1	1	1
8	7	2	14	1	3	3	1	1	5	2	2	3	0	0	0	0	0	0
1	5	0	20	1	2	3	1	2	3	0	10	0	0	1	1	0	0	0
1	6	3	7	1	1	1	1	5	5	2	16	4	3	0	0	0	0	0
8	3	10	21	1	1	3	2	5	2	2	0	4	0	0	0	0	0	0
8	3	11	12	2	1	1	3	4	2	0	6	2	0	1	1	1	1	1
1	7	0	15	2	3	3	2	5	4	0	6	1	0	1	1	1	1	1
1	9	10	21	1	1	3	3	2	5	0	4	1	0	1	1	1	0	1
1	5	0	26	1	2	3	1	0	5	0	2	1	0	1	1	1	0	0
1	5	0	10	2	3	2	2	0	3	0	11	1	0	1	1	1	0	1
1	5	11	1	1	4	1	2	2	3	0	1	4	0	0	0	0	0	0
1	7	2	15	0	3	6	2	2	5	1	1	1	0	0	0	1	0	0
8	8	0	11	2	5	3	2	1	5	0	4	1	0	1	1	0	0	1
1	7	0	17	2	4	0	2	5	4	0	8	1	0	1	1	1	0	1
1	7	2	18	0	2	6	1	2	5	2	3	4	0	1	1	0	0	0
8	3	0	17	2	3	3	2	5	2	0	6	1	0	1	1	0	0	1
1	11	2	17	2	2	0	3	2	5	2	3	4	0	0	0	0	0	0
8	6	14	4	2	1	1	2	5	1	0	9	1	0	1	1	0	0	1
1	1	0	22	1	2	3	1	0	5	2	16	4	3	0	0	0	0	0
1	5	14	18	1	3	3	2	5	3	0	4	1	0	1	1	0	0	0
8	1	0	21	1	1	3	1	5	5	0	16	4	0	1	1	0	0	1
1	7	0	12	2	5	3	2	2	4	0	9	2	0	1	0	0	0	0
8	5	6	2	1	3	3	1	5	5	2	16	4	3	0	0	0	0	0
8	5	12	9	1	3	4	2	4	3	0	5	1	0	1	1	0	1	1
8	3	11	14	1	2	3	2	4	2	2	2	0	0	1	1	0	0	1
8	6	0	14	2	2	3	2	5	1	0	6	1	1	1	1	0	0	1
1	2	13	12	1	2	5	2	2	5	0	6	4	1	1	0	0	0	0
8	0	0	7	3	2	0	3	3	0	2	16	4	3	0	0	0	0	0
8	3	0	16	2	2	1	3	4	2	2	16	4	3	0	0	0	0	0
1	3	9	12	2	5	2	3	5	2	0	6	0	0	1	1	0	0	0
8	3	0	9	2	3	2	2	4	2	2	2	1	1	1	1	0	1	1
8	9	4	2	2	2	2	2	4	5	2	16	4	3	0	0	0	0	0
8	7	0	13	2	3	3	2	4	4	2	0	4	1	0	0	0	0	0
8	7	0	8	2	3	2	2	4	4	2	16	4	3	0	0	0	0	0
1	9	14	1	2	2	3	2	2	5	2	16	4	3	0	0	0	0	0
8	7	14	31	1	2	2	2	1	5	2	0	4	1	0	0	0	0	0
1	9	1	8	2	1	6	3	2	5	1	0	0	0	1	0	0	0	0
8	7	6	17	2	2	3	3	5	4	2	16	4	3	0	0	0	0	0
8	7	9	20	1	2	3	3	1	4	0	9	1	0	0	0	0	0	0
8	3	0	12	2	2	3	2	4	2	0	2	0	0	1	1	0	0	1
1	11	0	20	1	3	1	1	0	5	0	6	1	1	1	1	1	1	1
1	0	0	10	2	3	6	2	5	0	0	6	0	1	1	1	0	0	0
8	0	0	24	1	4	6	1	5	5	2	16	4	3	0	0	0	0	0
8	0	12	10	2	2	0	3	1	0	0	5	1	0	1	1	0	0	0
1	11	10	23	1	2	3	3	2	5	2	16	4	3	0	0	0	0	0
1	3	0	17	2	4	3	2	2	2	0	15	1	0	1	1	0	0	1
1	3	0	8	2	3	2	2	5	2	2	16	4	3	0	0	0	0	0
1	7	6	13	2	8	7	3	5	4	0	5	4	0	1	1	1	0	1
1	7	2	12	2	1	1	3	1	4	2	1	4	1	1	0	0	0	0
8	3	0	16	2	4	1	2	4	2	2	1	0	2	1	0	0	0	0
8	3	10	16	2	4	3	2	4	2	2	16	4	3	0	0	0	0	0
8	0	9	9	2	1	6	3	5	0	2	2	3	1	1	1	0	0	1
8	7	2	7	1	2	1	2	4	5	2	16	4	3	0	0	0	0	0
8	6	6	2	2	3	3	2	5	1	0	8	4	0	1	1	0	0	0
8	7	7	7	2	2	1	2	4	4	2	16	4	3	0	0	0	0	0
8	3	0	19	2	5	3	2	1	2	0	15	1	0	1	1	1	0	1
8	6	0	21	1	3	3	1	4	1	2	16	4	3	0	0	0	0	0
8	6	0	14	2	1	0	2	5	1	0	13	1	0	1	1	0	0	1
1	6	2	12	2	3	2	2	2	1	0	4	1	0	1	1	0	0	1
8	6	2	6	2	1	1	3	4	1	0	8	3	2	1	1	0	1	1
1	6	2	4	1	1	3	1	0	5	2	16	4	3	0	0	0	0	0
8	6	12	6	2	1	2	2	4	1	2	1	1	0	0	1	0	0	0
1	5	2	7	1	2	1	2	0	3	0	4	4	0	1	1	0	0	0
8	7	0	16	2	3	0	2	4	4	2	2	4	0	1	0	0	0	0
3	1	5	16	1	2	2	3	1	5	0	1	1	0	0	0	0	0	0
8	3	0	19	1	4	3	1	4	2	2	16	4	3	0	0	0	0	0
1	2	9	7	2	4	3	2	2	5	0	5	1	0	1	1	0	1	1
3	1	0	22	1	3	3	1	2	5	0	4	1	0	1	1	1	0	1
8	7	0	14	2	3	3	2	4	4	2	5	4	0	1	1	0	1	0
1	6	6	2	2	1	1	2	2	1	0	5	1	0	1	0	1	0	0
8	3	3	14	2	2	1	2	4	2	0	4	1	1	0	0	0	0	0
8	7	2	4	2	1	1	2	1	4	2	16	4	3	0	0	0	0	0
8	7	8	18	1	1	1	1	4	4	2	0	3	0	1	0	0	0	0
8	3	10	13	2	1	3	3	5	5	2	16	4	3	0	0	0	0	0
8	7	0	10	2	4	2	2	4	4	2	16	4	3	0	0	0	0	0
8	5	5	4	2	1	1	2	5	3	0	4	0	0	1	1	0	0	1
8	5	0	16	2	2	3	2	4	3	0	2	3	0	1	1	0	0	1
1	0	9	8	2	1	0	3	5	0	2	16	4	3	0	0	0	0	0
1	5	8	24	1	3	3	3	0	5	0	11	1	0	1	1	1	0	1
8	5	0	16	2	2	5	2	1	3	2	1	4	1	0	0	0	0	0
1	3	0	13	2	6	3	2	5	2	0	13	1	0	1	1	0	0	1
8	0	9	8	2	9	6	3	3	0	2	16	4	3	0	0	0	0	0
8	0	14	1	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	5	4	2	1	3	2	5	2	2	1	4	1	0	0	0	0	0
8	5	2	6	2	3	1	2	5	3	2	16	4	3	0	0	0	0	0
8	0	0	8	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
8	5	14	2	2	3	3	2	5	3	0	2	1	0	1	1	1	1	1
1	6	11	12	2	3	1	3	5	1	2	5	2	0	1	1	0	1	0
8	3	12	14	0	2	5	2	1	5	0	11	1	0	1	0	0	0	0
1	6	2	7	1	3	3	2	5	1	0	2	3	0	0	1	0	0	0
8	3	0	17	2	3	1	2	4	2	0	5	0	0	1	1	1	0	0
8	6	0	16	2	2	3	2	5	1	0	6	0	0	1	1	0	0	1
1	7	0	11	2	2	3	2	0	5	0	5	1	0	1	1	1	0	1
8	5	2	17	1	2	3	3	5	3	2	2	0	0	0	1	0	0	0
8	6	14	10	2	2	1	2	4	1	2	16	4	3	0	0	0	0	0
8	6	6	1	1	2	6	1	1	1	0	4	1	0	1	1	0	1	1
8	11	2	11	2	3	0	2	5	5	0	5	1	0	1	0	0	0	0
1	6	2	21	1	3	1	3	0	1	0	15	0	0	1	1	1	0	1
8	3	5	16	1	3	3	2	5	2	0	8	4	0	1	1	1	0	1
8	7	0	16	2	4	3	2	1	4	2	16	4	3	0	0	0	0	0
8	8	2	20	1	4	1	2	1	5	0	13	1	1	1	0	1	0	0
8	0	0	9	2	2	0	2	5	0	2	7	2	0	0	0	0	0	0
1	7	0	13	2	4	3	2	2	4	2	16	4	3	0	0	0	0	0
1	7	2	18	1	2	3	2	2	5	2	16	4	3	0	0	0	0	0
1	3	0	9	2	2	3	2	2	2	0	7	1	0	1	1	1	0	1
1	3	0	21	1	4	2	1	0	2	0	3	0	0	1	1	0	0	1
8	3	0	11	2	3	3	2	4	2	0	2	1	0	1	1	0	1	1
1	0	0	8	2	4	6	2	5	0	0	14	1	0	1	1	0	0	0
8	3	0	19	2	1	3	2	1	2	0	12	1	0	1	1	0	0	1
1	5	7	15	1	2	3	3	0	3	2	3	4	1	0	1	0	0	0
8	7	10	17	2	4	3	2	4	4	0	10	2	0	0	0	0	0	0
1	9	2	6	2	1	1	2	5	5	2	16	4	3	0	0	0	0	0
8	3	0	20	1	2	3	1	4	2	2	2	0	0	1	1	0	0	1
1	3	9	22	1	3	3	3	5	2	0	2	2	0	1	1	0	0	1
8	7	0	16	2	4	3	2	4	5	0	15	2	0	1	1	1	0	1
1	3	0	9	2	2	3	2	5	5	0	2	0	0	1	0	0	0	0
8	11	10	12	2	2	1	2	1	5	2	16	4	3	0	0	0	0	0
1	3	0	30	1	2	3	1	5	2	0	3	1	0	1	1	1	0	1
8	3	0	11	2	3	2	2	4	2	0	7	1	1	1	1	1	0	1
1	1	0	23	1	2	3	1	0	5	0	8	0	0	1	1	1	1	1
1	5	0	27	1	3	3	1	5	5	2	4	4	0	0	0	1	0	0
1	6	0	15	2	5	3	2	5	1	0	1	3	0	1	0	0	0	0
8	3	0	12	2	2	1	2	5	2	2	1	1	0	1	1	0	0	1
1	5	0	11	2	2	2	2	0	3	0	11	1	0	1	1	1	1	1
8	7	11	13	2	2	3	3	4	4	2	2	1	0	1	1	0	1	1
8	7	14	31	1	2	2	3	1	5	0	10	4	1	1	0	0	0	0
1	11	0	12	2	2	3	2	5	5	0	2	0	0	1	1	0	1	1
0	3	0	8	2	1	3	2	5	2	0	2	1	0	1	1	0	1	1
1	1	0	23	1	6	3	1	2	5	0	15	1	0	1	1	1	0	1
1	7	0	16	2	4	3	2	4	4	2	8	1	1	1	0	1	0	0
8	6	2	10	2	3	1	3	4	1	0	6	4	0	1	1	0	0	1
1	3	5	18	2	1	3	2	2	2	0	2	0	1	1	1	0	0	1
8	3	0	16	2	6	3	2	4	2	0	15	1	0	1	1	1	0	1
1	7	0	26	1	2	3	1	0	4	2	2	3	0	0	0	0	0	0
1	7	0	31	1	3	3	1	5	5	0	15	1	0	1	1	1	1	1
8	3	9	5	2	1	3	2	1	2	2	16	4	3	0	0	0	0	0
8	3	0	19	2	2	3	2	4	2	0	3	2	0	1	1	0	0	1
1	3	0	13	2	4	2	2	2	2	0	2	1	0	0	0	1	0	0
8	3	0	20	1	3	3	1	1	2	0	5	1	0	1	1	1	0	1
1	7	3	17	2	1	3	3	5	4	2	16	4	3	0	0	0	0	0
8	0	0	6	3	3	0	3	3	0	0	3	1	0	1	0	0	0	0
1	3	0	8	2	4	5	2	2	2	0	6	4	0	1	1	0	1	1
8	6	0	12	2	2	3	2	4	1	2	4	2	0	0	0	0	0	0
1	5	0	25	1	4	3	1	5	5	0	6	1	0	1	1	1	0	0
8	6	6	2	2	2	3	2	4	1	2	16	4	3	0	0	0	0	0
8	7	10	19	2	4	4	2	5	5	2	16	4	3	0	0	0	0	0
1	7	10	31	1	4	2	3	2	5	2	16	4	3	0	0	0	0	0
8	3	9	9	2	4	0	3	5	2	0	4	1	0	1	1	0	0	1
1	7	10	12	2	4	3	2	5	5	2	16	4	3	0	0	0	0	0
1	3	0	10	2	3	2	2	5	2	0	6	4	1	1	1	0	0	1
1	5	9	22	1	4	3	3	2	5	0	13	1	0	1	1	0	1	0
1	3	0	21	1	4	2	1	5	2	0	15	1	0	1	1	0	0	1
1	5	2	4	2	5	2	2	5	3	2	16	4	0	0	0	0	0	0
1	5	0	9	2	3	2	2	4	3	2	16	4	3	0	0	0	0	0
1	11	3	10	2	1	1	3	2	5	0	10	1	0	1	1	1	0	0
1	3	2	11	2	3	2	3	0	2	0	5	0	0	1	1	0	0	0
8	5	8	7	1	7	3	2	5	3	2	4	0	0	1	0	1	0	0
1	7	8	21	1	2	6	2	0	5	0	10	2	0	1	1	0	0	1
1	6	5	3	2	2	1	2	2	1	2	7	1	1	1	0	0	0	0
1	7	0	30	1	1	3	1	5	4	2	16	4	3	0	0	0	0	0
1	7	0	31	0	3	3	0	2	5	0	8	1	0	1	1	1	1	1
8	5	2	15	2	4	3	2	4	3	2	16	4	3	0	0	0	0	0
8	7	3	3	2	2	1	2	4	4	2	16	4	3	0	0	0	0	0
1	6	0	29	1	3	3	1	5	1	0	5	2	0	1	1	0	0	0
1	0	9	15	2	2	0	3	5	5	0	10	0	0	1	1	0	1	0
8	3	0	19	1	3	0	1	1	2	0	3	1	0	1	0	0	0	0
8	3	0	13	2	3	3	2	5	2	2	2	2	0	0	0	0	0	0
8	0	0	8	2	2	6	2	5	0	0	11	2	0	1	0	0	0	0
8	0	0	10	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
1	3	14	2	2	3	2	3	5	2	0	3	1	1	1	1	1	0	0
1	3	2	8	2	1	1	3	0	2	2	16	4	3	0	0	0	0	0
1	9	0	17	2	2	3	2	0	5	0	5	4	0	0	0	0	0	0
1	3	0	8	2	1	2	2	4	2	2	0	4	1	0	0	0	0	0
1	3	3	14	1	2	3	2	2	2	0	7	4	0	1	0	0	0	0
1	6	2	8	2	3	3	3	2	1	0	7	4	0	0	0	0	0	0
1	3	6	2	1	2	3	1	0	2	0	4	1	0	1	1	0	0	1
8	5	9	8	1	2	3	2	1	3	0	2	1	0	1	1	0	0	1
8	5	0	27	1	4	3	1	1	5	2	16	4	3	0	0	0	0	0
1	11	2	4	2	2	3	2	2	5	2	16	4	3	0	0	0	0	0
8	3	0	11	2	5	3	2	1	2	0	4	0	0	1	1	1	0	1
1	7	14	9	2	3	3	3	5	4	2	1	4	0	0	0	0	0	0
1	3	0	9	2	3	3	2	2	2	0	3	0	0	1	1	0	0	0
8	11	10	13	2	3	1	2	5	5	0	5	4	0	1	0	0	0	0
8	2	2	11	2	3	0	2	5	5	2	16	4	3	0	0	0	0	0
8	5	6	1	2	3	3	2	4	3	0	9	1	0	1	1	1	0	1
8	5	0	9	2	4	3	2	4	3	0	15	1	0	1	1	1	1	1
8	1	0	23	1	5	3	1	1	5	0	15	0	0	1	1	1	0	1
8	3	0	8	2	5	2	2	4	2	2	16	4	3	0	0	0	0	0
8	11	2	8	2	1	1	2	4	5	2	16	4	3	0	0	0	0	0
8	7	0	31	0	3	3	0	1	5	0	1	0	0	1	1	0	0	1
8	9	4	3	2	5	2	3	4	5	2	16	4	3	0	0	0	0	0
1	6	2	11	1	2	5	2	0	1	0	4	1	0	1	1	1	0	1
8	5	0	18	2	2	3	2	4	3	0	4	1	1	1	1	0	0	1
1	3	0	15	2	1	3	2	2	5	0	3	4	0	1	1	0	1	1
8	3	0	9	2	4	2	2	4	2	0	14	1	0	1	1	1	0	1
8	5	0	14	2	2	3	2	4	3	0	5	1	0	1	1	0	0	1
8	6	12	5	2	1	1	2	4	1	0	3	1	1	1	1	0	0	1
8	6	0	13	2	2	1	2	4	1	2	16	4	3	0	0	0	0	0
1	3	2	7	1	1	1	2	0	2	0	15	4	0	1	1	1	0	1
1	0	14	7	2	2	6	3	5	0	0	4	1	0	1	1	0	0	1
1	6	2	4	2	1	1	2	2	1	2	1	0	1	0	1	0	0	1
1	3	9	12	1	3	3	2	2	2	0	2	1	0	1	1	0	1	1
8	3	0	14	2	1	1	2	5	2	2	16	4	3	0	0	0	0	0
8	7	0	19	1	2	3	1	4	4	0	15	0	1	1	1	1	0	1
8	3	0	10	2	4	3	2	4	2	0	4	1	0	1	1	1	0	0
8	7	5	3	1	3	6	2	1	4	0	6	1	0	1	1	1	0	1
1	3	0	15	2	4	3	2	2	2	0	15	1	0	1	1	1	1	1
1	3	0	8	2	4	6	2	5	5	0	6	1	0	1	1	0	0	0
8	7	14	9	2	5	3	2	1	4	0	9	3	0	1	1	0	0	0
1	3	0	10	2	4	2	2	0	2	0	7	1	0	1	1	1	0	1
8	0	12	6	2	4	0	3	1	5	2	16	4	3	0	0	0	0	0
1	0	9	9	2	3	0	3	5	0	0	1	2	0	0	1	0	0	0
8	3	12	12	2	3	3	3	1	2	0	15	1	0	1	1	1	0	0
8	6	11	18	1	1	3	2	4	1	0	2	3	0	1	1	0	1	1
1	5	2	5	1	1	1	2	2	3	0	5	1	0	1	1	1	0	1
1	5	4	21	1	1	3	1	2	5	0	7	1	0	1	1	1	1	1
8	6	0	18	2	3	3	2	1	1	0	5	1	0	1	1	1	1	1
8	11	2	21	1	3	5	3	1	5	0	6	1	0	1	1	0	0	1
1	0	0	8	2	1	0	2	5	0	2	16	4	3	0	0	0	0	0
8	6	5	6	2	3	1	2	5	1	0	1	1	0	1	1	0	0	0
1	7	0	31	1	3	7	1	2	5	0	7	1	0	1	1	1	0	1
8	3	12	11	2	1	3	3	4	2	0	5	1	0	1	1	1	1	1
8	5	7	7	1	1	6	2	5	5	0	3	3	0	1	1	0	1	1
1	3	10	10	2	3	6	3	2	2	2	16	4	3	0	0	0	0	0
1	5	3	4	1	3	6	2	5	3	0	6	3	0	1	1	1	0	1
1	5	2	5	2	2	1	2	2	3	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	8	15	2	2	3	2	4	2	0	1	1	0	1	1	0	0	1
8	7	0	10	2	4	2	2	5	5	2	16	4	3	0	0	0	0	0
8	0	9	8	2	2	6	3	3	0	2	2	4	0	0	0	0	0	0
8	7	14	19	1	3	3	3	4	4	2	16	4	0	0	0	0	0	0
1	7	11	13	1	2	2	1	2	5	0	4	2	0	1	1	1	0	0
1	6	5	5	2	1	1	2	0	1	2	16	4	3	0	0	0	0	0
1	3	0	19	1	2	3	1	5	2	2	4	4	1	0	0	0	0	0
8	5	0	17	2	1	7	2	4	3	2	16	4	3	0	0	0	0	0
8	6	2	8	2	2	1	3	4	1	0	15	1	0	1	1	1	0	1
8	6	3	5	2	1	3	2	5	1	2	16	4	3	0	0	0	0	0
1	3	0	13	2	2	3	2	0	2	0	2	1	0	1	1	0	1	1
8	8	0	17	2	2	2	2	5	5	0	2	1	0	1	1	0	1	0
8	7	0	18	2	1	3	2	5	4	2	16	4	3	0	0	0	0	0
1	7	8	4	0	3	1	1	2	5	0	5	4	1	1	1	1	0	0
8	5	0	18	2	1	3	2	5	5	0	8	1	0	1	1	1	1	1
8	0	0	7	2	3	0	2	3	0	2	16	4	3	0	0	0	0	0
1	3	0	14	2	5	3	2	0	2	0	15	1	1	1	1	0	0	1
1	11	10	12	2	2	3	3	5	5	1	7	1	1	1	0	0	0	0
1	3	7	10	2	1	1	3	4	2	2	1	3	0	0	0	0	0	0
1	3	0	12	2	4	3	2	5	2	2	6	2	0	1	1	0	0	1
1	5	0	25	1	3	0	1	0	5	0	6	0	0	1	1	1	0	0
1	3	0	24	1	3	3	1	5	2	0	6	1	1	1	1	0	0	1
8	5	5	5	2	2	2	3	4	3	0	5	4	1	1	1	0	0	1
1	3	0	17	2	1	2	2	0	2	0	0	4	1	0	0	0	0	0
8	3	0	8	2	3	2	2	4	2	0	15	0	0	1	1	0	0	1
1	5	5	21	1	2	3	3	0	3	2	16	4	3	0	0	0	0	0
8	11	2	18	1	1	1	2	1	5	2	0	4	0	0	0	0	0	0
1	3	0	16	2	5	3	2	2	2	0	14	1	0	1	1	1	0	1
8	7	5	3	2	1	0	3	5	4	0	7	4	1	1	1	0	0	1
8	7	0	8	2	4	3	2	4	4	0	8	4	0	1	1	0	1	1
1	7	2	11	1	2	1	1	2	5	2	2	3	1	1	1	1	0	1
1	3	0	16	2	2	3	2	0	2	0	4	1	0	0	0	0	0	0
8	3	0	9	2	2	3	2	4	2	0	1	0	0	1	0	0	0	0
1	7	0	20	2	5	3	2	2	4	0	15	1	0	1	1	0	0	1
8	3	0	11	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
1	6	6	24	1	3	3	3	2	5	0	10	1	0	1	0	0	0	0
1	3	0	11	2	1	3	2	2	2	0	3	0	0	1	1	0	0	1
8	3	0	16	2	5	3	2	5	2	0	11	0	1	1	1	0	0	0
8	6	9	1	2	3	3	2	5	1	0	5	1	0	1	1	0	0	0
1	9	2	15	1	3	3	2	2	5	0	3	4	0	1	0	0	0	0
8	3	12	20	1	2	3	3	5	2	0	6	0	0	1	1	1	0	1
8	0	0	8	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	6	2	4	2	1	1	2	0	1	0	1	0	0	0	1	0	0	1
8	5	2	12	1	1	1	2	4	3	0	2	0	1	1	0	0	0	0
8	3	0	13	2	5	1	2	4	2	0	10	4	0	1	1	0	0	1
8	6	2	5	2	1	0	2	1	1	2	16	4	3	0	0	0	0	0
1	6	2	8	1	1	1	2	5	1	0	6	0	1	1	1	0	0	1
8	5	0	11	2	4	3	2	4	3	0	15	1	0	1	1	1	0	1
1	7	2	6	2	1	1	2	2	4	0	15	0	0	0	1	0	0	1
8	11	1	18	1	3	3	2	5	5	0	5	1	0	1	0	0	0	0
8	3	4	2	2	2	1	2	5	2	2	16	4	3	0	0	0	0	0
1	0	0	8	2	1	0	2	5	0	2	1	4	0	0	0	1	0	0
1	11	5	12	1	2	3	2	2	5	0	1	1	0	1	0	0	0	0
1	7	6	10	1	2	3	1	5	5	0	3	1	0	1	1	0	1	1
1	3	0	20	1	4	3	1	2	5	0	12	0	1	1	1	0	0	1
8	0	9	9	2	2	6	3	3	0	0	4	4	1	0	1	0	0	0
1	3	0	10	2	4	3	2	2	2	0	10	1	0	1	1	0	1	1
1	0	9	8	2	9	6	3	5	0	2	16	4	3	0	0	0	0	0
1	6	0	14	2	1	3	2	2	1	0	4	0	0	1	1	1	0	1
8	6	12	4	2	3	3	2	4	1	0	5	3	0	1	1	0	0	1
1	3	0	21	1	5	6	1	2	2	0	7	1	0	1	1	0	1	1
1	6	13	6	2	3	3	2	2	1	0	6	1	0	1	1	1	0	0
8	0	0	8	2	4	6	2	3	0	2	16	4	3	0	0	0	0	0
8	6	0	10	2	1	2	2	1	1	2	16	4	3	0	0	0	0	0
1	5	0	8	2	3	6	2	5	3	0	6	1	0	1	1	0	0	1
0	1	6	22	1	3	3	3	5	5	2	16	4	3	0	0	0	0	0
1	3	0	12	2	6	3	2	2	2	0	15	1	0	1	1	0	0	1
8	5	0	19	1	3	3	1	5	3	0	3	2	0	1	1	1	0	0
1	3	0	10	2	3	3	2	0	2	0	15	0	1	1	1	1	0	0
8	3	11	2	2	4	3	2	5	2	0	15	0	0	1	1	1	0	1
1	7	0	20	1	4	2	1	5	4	0	7	1	0	1	1	0	0	1
1	5	11	7	2	3	3	2	0	3	0	6	4	0	1	0	1	0	0
1	6	14	18	2	2	4	3	0	1	2	16	4	3	0	0	0	0	0
1	3	12	2	1	3	3	2	5	2	0	5	1	0	1	1	0	0	1
8	3	0	18	2	2	3	2	5	2	0	3	1	0	1	1	1	0	1
1	3	0	8	2	2	3	2	0	2	0	6	1	0	1	1	1	0	1
8	9	0	10	2	1	2	3	4	5	0	15	4	0	1	1	1	1	1
8	9	2	5	2	2	1	2	4	5	2	16	4	3	0	0	0	0	0
1	7	0	8	2	4	2	2	5	4	0	7	1	0	1	1	1	1	1
8	0	0	9	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
1	7	0	14	2	2	3	2	5	4	2	16	4	3	0	0	0	0	0
8	7	13	8	1	2	3	2	4	4	0	4	4	0	1	1	0	0	1
8	11	6	6	1	3	3	2	5	5	0	8	1	0	1	1	0	1	1
8	6	2	10	1	5	7	2	4	1	2	16	4	3	0	0	0	0	0
1	11	14	3	2	2	1	2	2	5	2	16	4	3	0	0	0	0	0
8	3	0	17	2	5	3	2	4	2	0	7	1	0	1	1	0	0	1
8	6	7	11	1	2	7	2	5	1	0	8	4	1	1	1	1	0	1
8	7	2	7	2	2	1	2	4	4	0	2	1	1	1	0	0	0	0
8	0	11	9	2	7	6	3	3	0	2	16	4	3	0	0	0	0	0
8	7	0	13	2	1	3	2	4	4	2	16	4	3	0	0	0	0	0
8	3	9	16	2	2	3	3	4	2	0	4	4	1	0	0	0	0	0
1	7	0	9	2	3	3	2	2	4	0	7	1	0	1	1	0	1	0
8	3	0	20	1	5	5	1	1	2	0	15	1	0	1	1	1	1	1
8	0	0	8	2	1	0	2	3	0	0	3	0	0	0	1	0	0	0
8	11	0	12	2	2	2	3	4	5	2	16	0	0	1	1	0	0	1
8	9	0	15	2	2	3	2	4	5	2	0	3	0	0	0	0	0	0
1	3	2	18	1	3	3	3	5	2	0	4	0	1	1	1	0	0	1
8	7	2	2	2	5	3	2	4	4	2	16	4	3	0	0	0	0	0
1	9	0	18	2	4	1	2	5	5	0	15	0	0	1	1	1	0	1
8	3	0	20	1	4	1	1	4	2	0	12	1	0	1	1	0	0	1
1	0	0	6	1	1	0	3	5	0	0	2	1	0	1	1	0	0	1
1	7	14	20	1	3	3	2	2	4	0	10	1	0	1	1	0	0	0
1	3	0	25	1	2	3	1	0	2	0	4	0	1	1	1	0	0	0
1	0	9	9	2	7	6	3	3	0	0	12	1	0	1	0	0	0	0
8	0	14	10	2	9	6	3	3	0	2	16	3	0	0	1	0	0	1
8	5	9	1	2	1	1	2	1	3	2	16	4	3	0	0	0	0	0
1	0	0	9	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
1	7	2	4	1	1	2	2	0	4	2	16	4	3	0	0	0	0	0
8	3	0	15	2	1	1	2	4	2	0	13	4	0	1	1	0	0	1
8	5	2	7	2	1	3	2	5	3	2	2	3	0	0	0	0	0	0
8	7	0	11	2	1	1	3	4	4	2	16	4	3	0	0	0	0	0
8	3	6	3	2	4	3	2	4	2	0	5	1	0	1	1	1	0	1
8	5	0	16	2	3	3	2	5	3	0	6	1	0	1	1	1	0	0
1	5	3	12	1	1	2	2	0	5	2	16	4	3	0	0	0	0	0
8	3	0	20	1	4	3	1	4	2	0	15	1	0	1	1	1	0	0
8	3	11	19	1	1	1	2	4	5	2	16	4	3	0	0	0	0	0
8	2	2	4	2	3	0	3	4	5	2	16	4	3	0	0	0	0	0
1	5	0	13	2	1	3	2	5	3	0	9	4	0	1	1	0	0	1
8	0	12	9	2	2	6	3	3	0	2	16	4	3	0	0	0	0	0
1	7	0	7	2	1	3	3	2	4	2	16	4	3	0	0	0	0	0
1	3	0	8	2	6	5	2	5	2	2	16	4	3	0	0	0	0	0
1	0	9	8	2	2	6	3	5	0	0	1	1	1	1	1	0	1	1
1	6	11	3	2	1	1	2	5	1	0	10	1	0	1	1	0	0	0
1	11	14	19	2	3	1	2	2	5	2	2	4	0	0	0	0	0	0
1	0	7	1	2	3	0	2	5	0	0	8	1	0	1	1	0	0	1
1	7	0	21	1	5	3	1	2	4	2	1	3	0	1	1	0	0	1
8	3	5	14	2	2	2	3	1	2	0	5	4	0	1	0	1	0	0
8	3	7	12	1	2	3	2	1	2	2	3	1	1	1	1	0	0	1
1	5	2	23	1	1	3	3	2	5	2	16	0	1	1	1	1	0	1
8	3	0	16	2	3	3	2	4	2	0	9	1	0	1	1	0	1	1
8	5	0	18	1	2	1	1	4	3	2	0	1	0	0	0	0	0	0
1	3	0	20	1	7	3	1	0	2	0	15	1	0	1	1	0	1	1
1	5	2	5	2	1	1	2	2	3	2	16	4	3	0	0	0	0	0
1	5	5	4	2	1	2	2	2	3	0	11	0	0	1	0	0	1	0
8	3	0	14	2	3	3	2	5	2	2	6	1	1	1	1	1	0	1
8	3	0	9	2	3	3	2	5	2	0	4	1	0	1	1	0	1	1
8	5	13	11	2	1	1	3	5	3	2	16	3	0	0	0	0	0	0
8	3	14	8	2	5	4	2	5	2	0	15	0	1	1	1	0	0	0
8	7	2	4	2	1	1	2	4	4	2	16	4	3	0	0	0	0	0
1	5	12	18	1	4	4	3	0	5	0	8	0	0	1	1	1	0	1
8	3	0	12	2	3	3	2	1	2	0	3	1	0	1	1	1	0	1
1	7	9	14	1	4	5	2	5	5	2	0	4	0	0	0	0	0	0
8	6	0	8	2	3	2	2	4	1	0	5	2	0	1	1	1	0	0
8	7	0	9	2	3	2	2	4	4	0	1	4	0	1	0	0	0	0
8	3	10	16	2	1	3	3	4	2	2	16	4	0	1	0	0	0	0
8	7	13	21	1	4	3	2	1	5	2	16	4	3	0	0	0	0	0
1	7	0	12	2	1	1	2	2	5	2	16	4	3	0	0	0	0	0
8	3	0	15	2	3	3	2	4	2	2	0	4	0	0	0	0	0	0
8	6	9	11	2	2	1	3	1	1	0	9	0	0	1	0	0	0	0
8	3	2	8	2	3	3	3	5	2	2	16	4	3	0	0	0	0	0
1	11	0	23	1	2	2	1	0	5	2	6	2	0	1	1	0	0	1
8	3	1	5	1	9	5	2	5	2	2	16	4	3	0	0	0	0	0
1	5	2	12	1	4	3	2	5	5	0	6	0	0	1	1	0	1	1
1	5	0	8	2	2	5	2	5	5	0	4	2	0	1	1	0	0	1
8	1	8	14	1	1	3	2	5	5	2	16	4	3	0	0	0	0	0
1	11	3	5	2	1	2	2	2	5	2	1	4	0	0	0	0	0	0
1	7	7	11	1	2	3	2	5	5	0	6	1	0	1	1	0	0	1
1	7	5	16	2	4	3	3	5	4	0	15	1	0	0	0	0	0	0
1	5	0	21	1	1	3	1	2	3	0	2	4	0	1	1	0	0	1
8	5	3	11	1	4	3	2	4	3	2	16	4	3	0	0	0	0	0
8	5	6	4	1	3	3	1	1	3	2	16	4	3	0	0	0	0	0
1	3	0	16	2	5	3	2	5	2	0	14	1	0	1	1	1	1	1
1	6	0	19	2	4	3	2	2	1	2	8	4	1	1	1	0	1	0
8	15	1	3	1	1	2	1	1	5	2	4	4	1	1	0	0	0	0
8	6	5	2	2	1	3	2	5	1	2	4	0	0	1	0	1	0	0
8	7	0	16	2	2	7	2	4	4	0	3	4	0	1	1	0	0	1
1	5	2	10	1	4	3	2	0	5	0	15	1	1	1	1	0	0	0
8	7	0	11	2	5	3	2	4	4	0	9	2	0	1	1	0	1	1
8	3	2	11	2	2	3	3	4	5	2	16	4	3	0	0	0	0	0
8	7	2	10	2	1	3	3	4	4	0	2	2	0	1	1	0	0	1
8	5	13	15	1	1	2	3	4	3	0	9	4	0	1	1	0	0	1
1	0	0	10	2	2	6	2	5	0	0	5	1	0	0	0	0	0	0
8	0	0	10	2	4	6	2	3	0	0	1	1	0	0	0	0	0	0
1	4	11	29	1	3	3	3	2	5	0	15	0	0	1	1	0	0	1
1	6	2	8	1	1	3	2	5	1	0	4	2	0	1	1	0	0	1
8	3	0	17	2	2	3	2	1	2	0	6	1	0	1	1	1	0	0
8	5	5	10	2	2	3	3	1	3	2	1	1	1	0	0	0	0	0
8	3	3	7	1	3	3	2	5	2	0	2	2	0	0	0	0	0	0
8	0	9	9	2	1	6	3	3	0	2	16	4	3	0	0	0	0	0
8	7	12	1	0	2	3	1	1	5	2	16	4	3	0	0	0	0	0
1	6	6	5	2	2	3	2	2	1	0	2	0	0	1	1	1	0	1
1	3	3	8	2	1	6	3	5	5	0	9	1	0	1	1	1	0	1
1	12	7	4	1	3	3	1	5	5	0	6	0	1	1	1	0	0	0
1	7	2	3	1	1	1	2	5	4	2	16	4	3	0	0	0	0	0
8	7	0	12	2	2	3	2	4	4	0	6	1	0	1	1	0	0	1
1	0	0	8	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
8	9	2	4	2	1	1	2	5	5	0	15	0	1	0	0	0	0	0
1	5	8	17	2	4	3	2	0	3	0	15	1	0	1	1	0	0	1
1	1	0	24	1	4	3	1	5	5	0	14	1	0	1	0	1	0	0
8	0	0	10	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
1	6	9	15	2	3	3	3	2	1	0	7	1	0	1	1	1	0	1
1	3	9	3	2	7	3	2	2	2	0	15	0	0	1	1	0	0	1
8	5	2	5	2	1	1	2	5	3	0	3	0	0	1	1	0	0	1
1	7	2	10	2	4	3	2	0	4	0	15	1	0	1	1	1	0	1
8	8	0	12	2	1	2	2	4	5	0	8	4	0	1	0	0	1	0
1	6	0	14	2	1	6	2	2	5	0	15	1	0	1	1	1	0	1
8	3	9	7	2	6	2	2	4	2	0	16	1	0	0	1	0	0	0
8	3	0	16	2	2	3	2	5	2	0	16	0	0	1	1	1	0	1
8	6	0	18	2	1	3	2	1	1	2	16	4	1	1	1	0	0	1
1	11	0	11	2	1	0	2	2	5	0	11	0	0	1	1	0	1	1
8	3	0	18	2	1	3	2	4	2	0	1	1	0	1	1	0	0	1
1	11	2	4	2	2	1	2	5	5	0	3	0	0	0	0	0	0	0
1	7	0	31	1	3	2	2	0	5	2	16	4	3	0	0	0	0	0
8	9	13	6	2	2	3	2	1	5	0	6	2	0	1	1	1	0	0
8	0	0	9	2	2	6	2	5	0	0	15	1	0	1	1	1	0	0
1	6	12	11	1	3	3	2	2	1	0	6	1	0	1	1	1	0	1
8	3	0	13	2	6	3	2	5	2	0	5	1	0	1	1	0	0	0
1	5	0	20	1	3	3	1	5	3	2	1	3	0	0	0	0	0	0
8	0	9	8	2	3	6	3	3	0	0	5	0	0	1	1	0	0	0
1	6	0	13	2	1	1	2	2	1	2	2	3	0	0	1	0	0	1
1	11	5	1	2	1	5	2	5	5	0	2	1	1	0	0	0	0	0
1	5	5	7	1	3	3	2	2	5	0	15	1	0	1	1	1	0	0
8	11	10	21	1	3	3	2	5	5	0	6	1	0	1	0	0	0	0
8	7	1	15	2	3	3	3	4	4	0	4	1	1	1	0	0	0	0
1	6	10	19	2	2	3	2	5	1	2	2	2	0	0	0	0	0	0
8	3	0	19	1	4	3	1	4	2	0	12	4	0	1	1	1	0	1
1	5	2	6	2	2	1	3	5	3	2	0	4	0	0	0	0	0	0
8	3	0	12	2	2	3	2	5	2	0	15	1	0	1	1	0	0	1
8	5	0	18	1	3	3	1	5	3	2	16	4	3	0	0	0	0	0
1	9	10	12	2	4	1	3	5	5	2	1	0	0	0	0	0	0	0
1	5	2	3	1	1	1	1	2	5	0	5	0	0	1	0	1	0	0
1	7	12	1	1	3	6	1	5	5	0	4	3	0	1	1	0	0	1
8	3	0	9	2	2	2	2	4	2	1	4	4	1	0	0	1	0	0
8	7	13	14	1	1	3	3	5	4	0	5	0	1	1	1	0	0	1
8	3	0	15	2	5	3	2	4	2	0	13	1	0	1	1	0	0	0
8	3	0	11	2	1	2	3	5	2	2	7	2	0	0	0	0	0	0
8	3	14	14	2	1	1	2	4	2	2	16	4	3	0	0	0	0	0
8	0	0	7	3	2	0	3	3	0	2	16	4	3	0	0	0	0	0
3	3	0	11	2	4	3	2	2	2	0	7	4	0	1	1	1	1	0
1	0	0	9	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
1	3	0	20	1	6	3	1	2	2	0	15	1	0	1	1	0	0	1
8	5	14	12	2	5	3	3	4	3	2	2	4	3	0	0	0	0	0
8	7	0	32	0	5	3	0	1	5	0	15	1	0	1	1	1	0	1
1	1	0	23	1	5	3	1	0	5	0	13	4	1	1	1	0	0	1
1	5	13	4	2	2	6	2	2	3	2	16	4	3	0	0	0	0	0
1	7	8	18	2	3	3	3	2	4	2	9	0	1	1	1	0	0	0
1	3	0	10	2	3	3	2	5	2	0	5	1	1	1	1	0	1	1
8	3	2	5	2	3	3	2	4	2	0	7	1	1	1	1	0	0	1
1	7	5	5	2	5	3	2	0	4	0	2	4	0	1	0	0	0	0
1	6	3	6	1	1	1	2	5	1	2	16	0	0	1	1	0	0	1
1	3	0	8	2	6	6	2	5	2	0	1	2	0	1	1	1	0	0
1	7	0	18	2	2	3	2	5	4	0	2	1	0	1	1	0	0	1
1	7	9	6	2	5	0	2	5	4	2	16	4	3	0	0	0	0	0
8	3	0	15	2	2	3	2	1	2	0	2	4	0	1	1	0	0	1
1	7	10	31	1	4	3	2	0	5	0	4	1	0	1	1	0	0	1
8	0	9	9	2	1	6	3	3	0	0	6	4	2	1	1	0	0	1
8	3	3	4	2	4	3	2	1	2	0	15	1	0	1	1	0	0	1
1	7	6	8	2	2	2	3	5	4	2	16	4	3	0	0	0	0	0
8	3	0	10	2	4	3	2	1	2	0	1	1	0	1	1	0	1	1
1	3	14	1	2	2	3	2	5	2	0	4	0	0	1	1	0	0	1
1	5	0	14	2	2	3	2	2	3	0	3	1	0	1	1	1	0	0
1	3	0	8	2	2	3	2	5	2	0	1	1	0	1	1	0	1	1
8	0	9	8	2	2	5	3	3	0	2	16	4	3	0	0	0	0	0
1	7	2	11	2	7	1	3	2	4	0	15	1	1	0	1	0	0	1
8	0	5	1	3	2	0	3	5	0	0	5	4	0	1	1	0	0	0
8	3	0	14	2	5	1	2	4	2	0	7	1	0	1	0	0	1	0
1	5	0	12	2	3	3	2	5	3	0	4	2	0	1	1	0	0	1
0	3	6	7	2	1	2	3	4	2	2	16	4	3	0	0	0	0	0
1	9	2	10	2	2	1	3	5	5	2	16	4	3	0	0	0	0	0
8	7	0	20	1	4	3	1	4	4	2	16	4	3	0	0	0	0	0
8	3	0	30	1	4	1	1	1	2	2	3	4	1	0	0	0	0	0
8	7	4	30	1	3	3	3	1	5	2	16	4	0	0	0	0	0	0
1	7	2	7	0	8	5	1	5	5	2	16	4	3	0	0	0	0	0
8	3	2	8	1	1	0	1	5	5	0	4	4	0	1	1	0	0	1
8	1	0	24	1	3	3	1	1	5	0	5	1	0	1	1	1	0	1
1	0	14	10	2	2	6	2	5	0	2	16	4	1	1	1	0	0	1
1	3	0	9	2	4	2	2	2	2	0	12	1	0	1	1	0	0	1
8	0	0	10	2	2	6	2	5	0	2	5	2	0	1	0	1	0	0
8	11	0	17	2	2	3	2	4	5	0	2	1	0	1	1	0	0	1
1	9	11	16	2	3	3	3	2	5	0	3	4	0	0	0	0	0	0
8	1	2	16	1	2	3	2	5	5	1	2	0	0	1	1	0	0	0
1	11	0	31	1	2	3	1	5	5	2	0	4	0	0	0	0	0	0
1	6	0	20	1	3	3	3	0	1	0	4	4	0	1	1	1	0	1
1	3	0	21	1	3	3	1	0	2	0	3	4	0	1	1	1	1	1
8	3	3	9	2	2	1	3	4	2	2	2	3	0	0	0	0	0	0
8	3	2	11	2	3	3	2	4	2	2	16	4	0	1	1	0	0	0
8	5	0	14	2	3	2	2	4	3	2	6	4	1	1	0	0	0	0
1	9	13	14	1	3	3	2	0	5	0	5	0	0	1	1	0	0	1
1	11	0	31	0	4	3	0	2	5	0	7	0	0	1	1	0	0	1
1	11	0	21	1	2	3	1	2	5	0	4	0	0	1	1	1	0	1
1	11	10	31	1	3	3	2	2	5	2	16	4	3	0	0	0	0	0
1	9	14	2	2	4	2	2	0	5	2	16	4	3	0	0	0	0	0
1	7	0	12	2	1	2	3	0	4	0	3	0	0	1	1	0	0	1
8	3	0	17	2	1	3	2	4	2	2	1	3	0	1	1	0	0	1
8	3	0	20	1	3	3	1	4	2	0	6	1	0	1	1	1	0	1
1	3	0	11	2	2	3	2	2	2	0	2	4	0	1	1	0	0	1
8	5	0	21	1	4	1	1	1	3	0	8	0	0	0	1	0	0	0
1	3	0	10	2	2	2	2	2	2	2	16	4	3	0	0	0	0	0
1	0	2	6	2	3	0	3	2	5	2	16	4	3	0	0	0	0	0
1	9	5	3	2	4	3	2	2	5	0	6	1	0	1	1	0	0	0
8	3	0	20	1	1	3	1	1	2	2	16	4	3	0	0	0	0	0
8	7	4	2	2	3	2	3	4	4	2	16	4	3	0	0	0	0	0
1	7	0	18	2	2	3	2	5	5	0	4	1	0	1	1	1	0	0
1	0	0	9	2	2	0	2	3	0	0	3	1	0	1	1	0	0	1
1	0	0	9	2	1	6	2	5	0	0	6	0	0	1	1	0	0	1
4	5	6	19	1	3	6	3	1	3	2	1	4	0	0	0	0	0	0
8	1	0	23	1	4	2	1	5	5	0	15	1	0	1	0	1	0	1
8	5	0	16	2	1	3	2	4	3	2	16	4	3	0	0	0	0	0
8	3	7	1	2	3	1	2	5	2	0	9	1	0	1	1	0	0	1
1	3	11	10	2	2	2	3	2	2	2	16	4	3	0	0	0	0	0
1	3	0	11	2	3	2	2	0	2	0	5	1	0	0	1	1	0	1
1	3	0	10	2	2	2	2	0	2	0	5	1	0	1	1	0	0	1
8	6	9	16	2	2	3	3	1	1	2	16	4	3	0	0	0	0	0
8	3	2	18	1	4	3	3	4	2	0	1	4	0	1	1	0	0	1
1	6	0	13	2	4	3	2	0	5	2	16	4	3	0	0	0	0	0
1	3	0	11	2	4	3	2	5	2	0	7	0	1	1	1	0	0	1
1	7	10	18	2	4	1	2	2	4	0	15	1	0	1	1	1	0	1
8	8	2	5	1	2	3	2	1	5	0	6	1	1	1	0	0	0	0
1	6	0	13	2	1	1	2	5	1	2	0	1	0	0	0	0	0	0
8	3	11	3	2	3	6	2	5	2	2	16	4	3	0	0	0	0	0
1	8	11	4	1	3	3	2	0	5	2	16	4	1	0	0	0	0	0
1	5	2	21	1	1	3	2	1	3	0	10	1	0	1	1	0	0	1
1	3	2	5	2	6	3	2	5	2	0	14	4	0	1	1	0	1	1
8	3	0	9	2	4	3	2	4	2	0	15	1	0	1	1	0	1	1
1	3	0	25	1	4	3	1	0	2	0	15	1	0	1	1	1	0	1
1	5	2	21	1	4	3	2	5	3	0	9	1	0	1	1	1	1	1
1	7	5	30	0	4	1	2	5	5	0	6	3	2	1	1	0	0	0
8	3	0	14	2	2	3	2	1	2	0	3	4	1	1	1	0	1	1
8	7	0	12	2	1	3	3	4	5	0	2	3	0	1	1	0	0	1
8	7	2	16	1	2	2	2	1	5	0	3	1	0	1	1	0	0	1
8	7	2	9	2	2	1	3	1	4	0	6	0	0	1	1	0	0	0
8	0	0	8	2	1	0	2	3	0	2	16	4	3	0	0	0	0	0
1	3	0	16	2	2	3	2	0	2	0	8	1	0	1	1	0	0	1
1	6	0	20	1	2	3	1	2	1	0	4	2	0	1	1	1	0	1
1	3	5	20	1	3	3	3	0	2	0	8	1	0	1	1	0	0	0
1	6	7	25	0	3	2	2	0	1	2	16	4	3	0	0	0	0	0
8	7	9	2	2	1	3	2	5	4	0	15	1	0	1	1	0	0	1
8	2	5	7	2	2	6	2	1	5	0	5	4	0	0	0	0	0	0
8	3	0	19	2	4	3	2	4	2	0	3	4	0	1	1	0	1	1
8	0	0	8	2	1	6	2	5	0	2	1	4	0	0	0	0	0	0
1	9	0	31	0	2	3	0	0	5	0	3	4	0	1	1	0	1	1
8	3	7	10	1	5	3	2	4	2	0	15	1	0	1	1	1	1	1
8	6	4	2	2	1	1	2	5	1	2	16	4	3	0	0	0	0	0
1	3	14	14	1	3	3	2	5	2	0	5	1	0	1	1	0	1	1
8	3	0	9	2	2	2	2	4	5	0	4	4	2	1	1	0	0	1
8	7	0	9	2	0	7	2	4	4	0	8	0	0	1	1	0	0	0
8	1	0	19	2	5	1	2	1	5	2	4	3	0	0	0	0	0	0
8	7	0	16	2	1	3	2	4	5	2	0	4	0	0	0	0	0	0
8	9	0	13	2	3	3	2	1	5	2	16	4	3	0	0	0	0	0
1	0	0	9	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
1	3	2	6	1	6	2	2	5	2	0	6	4	1	0	0	0	0	0
1	3	0	11	2	1	1	2	5	2	2	3	4	1	0	0	0	0	0
8	7	0	15	2	4	3	2	4	4	0	5	1	0	1	1	1	1	1
1	3	11	12	1	1	3	2	0	2	0	8	0	0	1	1	0	0	1
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	0	19	1	3	3	1	4	2	0	7	1	0	1	1	0	0	1
8	5	3	11	1	2	3	2	1	5	0	9	1	0	1	1	1	0	1
1	7	0	12	2	4	3	2	5	4	0	8	2	0	1	1	0	1	0
1	12	14	29	1	2	3	1	0	5	0	1	1	0	1	1	1	1	1
8	5	2	12	2	1	3	3	5	3	2	16	3	0	0	0	0	0	0
8	5	0	18	2	2	3	2	5	3	0	10	0	0	1	1	0	0	1
8	3	7	4	2	4	3	2	4	2	0	6	1	0	1	1	0	1	1
1	0	12	9	2	2	6	3	5	0	2	2	0	1	1	0	0	0	0
8	7	7	10	1	3	3	1	1	5	2	16	4	3	0	0	0	0	0
8	6	0	14	2	1	1	2	1	1	2	1	4	0	0	0	0	0	0
8	3	0	11	2	2	2	2	4	2	2	16	4	3	0	0	0	0	0
8	7	10	12	2	2	1	3	4	4	0	6	4	0	1	1	0	0	1
8	5	0	12	2	2	3	2	1	3	0	3	0	0	1	1	0	0	1
8	1	0	25	1	4	3	1	1	5	0	3	1	0	1	1	1	0	0
8	6	2	27	1	4	3	3	1	1	0	15	1	0	1	0	0	0	0
1	11	0	16	2	2	2	2	2	5	2	16	4	1	1	1	0	0	1
8	3	0	11	2	3	3	2	4	2	0	15	1	0	1	1	0	0	1
1	6	6	1	1	2	3	1	5	5	0	16	4	0	1	1	0	0	1
1	11	0	29	1	4	6	1	2	5	0	9	0	0	1	1	0	0	1
1	0	0	8	2	2	6	2	5	0	2	16	4	3	0	0	0	0	0
1	6	10	16	2	4	3	2	2	1	0	6	1	0	1	1	0	0	0
8	5	0	11	2	3	3	2	4	3	0	3	4	2	1	1	1	0	0
1	3	0	8	2	3	6	2	5	2	0	6	0	0	1	1	1	0	1
1	3	0	11	2	6	3	2	0	2	0	9	1	1	1	1	1	1	1
8	5	5	9	1	2	3	2	1	3	0	5	4	0	1	1	0	0	1
1	1	10	23	1	3	3	3	0	5	0	4	1	0	1	1	1	0	1
1	3	5	16	2	1	1	2	2	2	2	16	0	2	1	1	0	0	0
8	3	12	8	1	3	6	2	1	2	2	16	4	3	0	0	0	0	0
8	7	0	15	2	2	3	2	4	4	0	4	3	1	1	1	0	1	1
1	5	0	10	2	4	3	2	2	3	0	15	1	0	1	1	0	0	0
1	9	1	2	1	1	2	1	0	5	0	15	0	0	1	1	1	0	1
8	7	0	12	2	3	5	2	1	4	2	1	4	0	0	0	0	0	0
8	5	0	25	1	5	3	1	1	5	0	12	1	0	1	1	0	0	0
1	1	0	24	1	1	3	1	5	5	0	7	0	1	1	1	0	0	1
8	0	14	5	2	2	6	2	1	5	2	16	4	3	0	0	0	0	0
8	11	0	17	2	1	3	2	4	5	0	7	4	0	1	1	0	0	1
8	3	6	1	2	3	3	2	1	2	2	6	4	0	1	1	0	0	0
1	6	0	17	2	2	3	2	5	1	0	5	1	0	1	0	0	0	0
1	3	2	11	2	5	3	2	5	2	0	3	1	0	1	1	1	0	0
1	3	8	2	1	1	6	2	5	2	0	1	4	1	1	1	0	0	1
1	6	13	18	2	3	3	3	2	1	0	0	1	0	0	1	0	0	0
8	6	0	20	1	2	3	1	5	1	0	3	4	0	1	1	1	1	1
1	7	12	3	0	2	2	1	2	5	0	3	0	0	1	1	0	1	1
8	3	0	12	2	2	3	2	4	2	0	4	1	0	1	1	0	0	1
8	5	2	14	2	2	1	2	4	3	2	16	4	3	0	0	0	0	0
8	5	0	12	2	1	1	2	4	3	0	7	0	0	1	1	0	0	1
8	0	0	9	2	2	6	2	5	0	2	2	2	0	1	1	0	0	0
8	5	0	10	2	4	2	2	4	3	0	9	0	1	1	1	1	0	1
8	0	0	14	2	1	3	2	4	5	2	16	4	3	0	0	0	0	0
1	3	9	2	2	3	1	2	5	2	0	4	1	1	1	1	0	0	1
1	5	9	20	2	5	3	3	2	3	0	11	4	2	1	1	0	0	0
1	7	0	20	1	3	3	1	5	4	0	3	1	0	1	1	1	0	1
8	7	2	11	1	3	3	1	1	5	0	7	1	0	0	0	0	0	0
8	3	14	9	2	1	3	3	4	2	0	7	0	1	1	1	0	0	1
8	5	6	11	2	2	5	3	4	3	2	1	4	0	1	1	0	0	1
1	6	6	8	1	2	3	2	2	1	0	3	1	1	1	1	0	0	1
1	5	2	13	1	7	3	2	5	5	0	15	4	1	1	1	0	0	1
8	3	0	20	1	4	3	1	5	2	0	8	1	0	1	1	1	0	1
8	2	3	2	2	1	0	2	1	5	2	16	4	3	0	0	0	0	0
1	6	0	10	2	3	3	2	5	1	0	3	1	0	1	0	0	0	0
1	2	2	8	2	2	0	2	2	5	2	16	4	3	0	0	0	0	0
8	3	0	17	2	2	3	2	1	2	0	3	1	0	1	1	0	0	0
1	7	0	21	1	1	3	1	5	4	2	16	4	3	0	0	0	0	0
8	5	9	12	2	2	3	3	5	3	0	1	0	0	1	1	0	0	0
1	6	2	7	2	3	1	3	0	1	0	11	2	0	1	0	0	0	0
8	3	14	3	1	1	3	2	4	2	2	16	4	3	0	0	0	0	0
8	3	0	11	2	1	3	2	4	5	2	16	4	3	0	0	0	0	0
8	3	2	6	2	1	0	3	4	5	0	15	0	0	1	1	0	0	1
8	11	4	2	2	2	3	3	4	5	2	16	4	3	0	0	0	0	0
1	6	2	5	2	1	6	2	0	1	2	16	4	3	0	0	0	0	0
1	11	9	19	2	1	3	3	5	5	0	0	0	1	1	1	0	0	1
1	0	0	7	3	1	0	3	5	0	2	1	4	3	0	0	0	0	0
8	6	6	9	1	2	3	2	5	1	0	6	1	0	1	1	0	0	1
8	3	1	1	2	1	1	2	4	2	0	1	4	0	1	1	0	1	1
1	3	0	15	2	1	3	2	5	2	2	16	4	3	0	0	0	0	0
1	7	7	16	2	2	3	3	0	4	0	4	4	0	1	0	1	0	0
8	6	6	6	2	5	2	3	5	1	0	12	1	0	1	1	1	0	1
1	7	14	21	1	4	3	2	5	4	2	16	4	3	0	0	0	0	0
8	0	0	8	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	0	0	6	3	3	6	3	5	0	2	16	4	3	0	0	0	0	0
8	3	0	14	2	3	3	2	1	2	0	6	1	0	0	1	1	0	0
8	3	11	7	2	4	2	3	5	2	2	16	4	3	0	0	0	0	0
8	2	12	12	1	4	0	2	4	5	0	10	1	0	1	1	1	0	1
8	3	0	15	2	2	3	2	4	2	0	2	1	0	1	1	0	1	1
1	0	14	12	1	1	0	1	5	5	0	5	2	0	1	1	1	0	0
1	3	6	2	2	3	3	2	5	2	0	11	1	0	1	0	0	0	0
1	11	0	31	1	3	3	1	5	5	0	5	1	0	1	1	1	0	1
8	3	0	21	1	4	1	1	5	2	0	8	0	0	1	0	0	0	0
1	0	11	10	2	9	6	3	5	0	0	10	3	0	1	0	0	0	0
8	5	6	1	2	2	3	2	5	3	2	5	4	1	1	0	1	0	0
1	3	8	14	2	3	1	2	0	2	0	4	1	0	1	0	0	0	0
1	0	0	9	2	2	6	2	5	0	0	15	1	0	1	0	0	0	0
8	3	0	8	2	3	2	2	4	2	0	2	0	0	1	1	0	0	1
8	3	0	21	1	4	3	1	5	5	0	7	1	0	1	1	1	1	1
8	7	14	15	2	2	0	2	1	4	0	14	4	0	0	0	0	0	0
8	3	6	3	1	4	1	2	4	2	0	5	1	0	1	1	0	0	1
8	5	0	11	2	2	3	2	4	3	0	4	2	0	1	1	1	0	1
8	3	0	21	1	3	3	1	1	2	0	14	4	1	0	0	0	0	0
1	5	6	14	2	6	7	3	0	3	2	16	4	3	0	0	0	0	0
8	3	0	8	2	5	2	2	4	2	0	6	4	2	1	1	0	1	1
8	5	0	19	2	2	3	2	5	3	0	3	2	0	1	1	0	0	0
8	6	2	6	2	1	1	3	1	1	2	0	3	1	1	0	0	0	0
8	5	0	12	2	3	2	2	5	3	0	2	3	0	0	0	0	0	0
8	3	0	11	2	3	1	2	1	2	0	1	1	0	0	1	0	0	1
8	5	14	23	1	2	3	1	1	5	0	2	1	1	1	1	0	0	1
8	0	9	7	2	3	0	3	3	0	0	5	3	1	1	1	0	0	1
8	2	4	3	2	4	5	2	5	5	0	15	0	0	1	0	0	0	0
1	5	5	15	2	1	1	3	0	3	2	16	4	3	0	0	0	0	0
1	3	13	13	1	3	3	3	2	2	0	11	1	0	1	1	1	0	0
8	3	10	18	2	2	3	3	4	2	0	5	4	0	0	0	0	0	0
1	3	0	19	1	2	2	1	5	2	2	1	1	0	0	0	0	0	0
8	3	0	19	1	4	3	1	4	2	0	10	1	0	1	1	1	0	1
1	3	2	6	2	3	2	3	2	2	0	4	2	0	1	0	1	0	0
8	7	0	8	2	3	2	2	5	4	0	6	1	0	1	1	1	0	1
8	3	11	15	2	2	2	2	4	2	2	16	4	3	0	0	0	0	0
8	3	0	18	2	4	4	2	5	2	0	8	1	0	1	1	1	0	1
1	0	0	1	3	2	0	3	5	0	0	4	0	0	0	0	0	0	0
8	3	0	12	2	4	3	2	4	2	0	3	2	0	1	1	1	0	0
1	5	5	4	2	2	3	3	2	3	0	1	1	1	1	0	0	0	0
8	7	2	8	2	2	3	3	5	4	2	2	3	1	1	0	1	0	0
1	3	0	8	2	4	2	2	5	2	0	8	4	0	1	0	0	0	0
8	5	0	11	2	3	3	2	1	3	0	5	1	0	1	1	0	0	1
8	3	6	14	2	6	3	3	5	2	0	15	1	0	1	1	1	0	1
8	6	8	1	2	2	1	2	4	1	0	8	0	0	1	1	0	0	1
8	9	0	11	2	2	3	2	5	5	0	5	1	1	1	1	0	0	0
1	11	2	4	1	2	0	2	0	5	2	16	4	3	0	0	0	0	0
3	7	0	19	2	2	2	2	2	4	0	13	1	0	1	1	1	0	1
8	6	0	19	2	2	3	2	4	1	0	3	2	0	1	1	0	0	1
1	7	13	15	1	3	4	3	5	4	0	7	4	0	1	1	0	0	1
8	6	6	2	2	4	3	2	5	1	2	16	4	3	0	0	0	0	0
8	9	11	5	2	1	1	2	1	5	2	2	4	0	0	0	0	0	0
1	9	8	19	1	2	3	3	2	5	2	16	4	3	0	0	0	0	0
1	3	0	20	1	4	6	1	5	2	2	16	4	3	0	0	0	0	0
1	6	14	19	1	3	3	2	5	1	0	9	4	0	1	1	1	0	1
8	3	2	9	2	2	2	2	5	2	0	5	0	0	1	0	0	0	0
8	7	0	20	1	2	3	1	4	4	0	5	1	0	1	1	1	1	1
8	7	0	8	2	4	2	2	1	5	0	3	3	0	0	0	0	0	0
1	5	0	9	2	3	2	2	5	3	0	8	1	0	1	1	1	0	1
8	11	0	12	2	3	2	2	4	5	2	16	4	3	0	0	0	0	0
8	7	0	10	2	2	3	2	4	4	2	16	4	3	0	0	0	0	0
1	3	9	2	2	2	2	2	5	2	0	5	1	0	1	1	0	0	1
8	6	2	7	2	1	3	2	1	1	0	5	1	0	1	1	0	0	1
1	6	0	11	2	2	2	2	2	1	2	16	4	3	0	0	0	0	0
1	7	3	19	1	1	1	2	2	4	0	5	4	1	1	1	0	0	1
8	5	0	16	2	3	1	2	1	3	2	16	4	3	0	0	0	0	0
1	1	6	1	1	5	6	1	5	5	2	1	2	0	0	0	0	0	0
1	7	2	9	1	2	3	2	5	4	0	1	3	0	1	0	0	0	0
8	3	0	19	1	3	3	1	4	2	2	16	4	3	0	0	0	0	0
8	0	0	10	2	1	6	2	3	0	2	4	4	0	1	1	0	0	1
8	5	0	26	1	3	3	1	5	5	0	10	1	0	1	1	1	0	0
8	3	0	13	2	3	3	2	4	2	0	11	1	0	1	1	1	0	1
1	3	7	7	1	5	3	2	2	2	0	3	1	0	1	1	1	1	1
1	7	5	25	1	3	3	2	2	4	0	13	1	0	1	1	1	0	1
8	6	6	1	1	4	3	2	4	1	0	15	1	0	1	1	1	1	1
8	6	0	19	1	4	3	1	5	1	0	9	1	0	1	1	0	0	1
8	11	3	1	1	4	3	1	4	5	0	4	1	1	1	1	0	1	1
8	3	0	20	1	6	1	1	4	2	0	13	0	1	1	1	0	0	1
8	7	2	20	1	5	3	2	5	4	0	11	2	0	1	0	0	0	0
1	7	0	31	1	2	3	1	5	5	0	6	1	0	1	1	0	0	1
8	3	7	16	1	2	3	3	4	2	0	4	1	0	1	1	1	1	1
1	7	12	12	2	2	1	3	5	4	2	4	2	0	1	1	1	0	0
8	0	3	3	2	1	6	3	3	0	0	6	0	0	1	1	1	0	1
1	5	10	23	1	4	3	3	2	5	0	2	2	0	0	0	0	0	0
1	3	0	15	2	5	1	2	2	2	0	15	1	0	1	1	1	0	1
1	5	2	8	1	2	0	2	2	5	0	4	2	0	1	1	1	0	1
1	3	0	15	2	1	1	2	0	2	2	1	4	0	0	0	0	0	0
8	3	0	12	2	6	3	2	5	2	2	2	4	0	1	1	0	0	1
1	0	0	9	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
1	5	0	9	2	3	2	2	5	3	0	7	1	0	1	1	0	0	0
8	3	0	14	2	5	3	2	4	2	0	7	1	0	1	1	0	0	1
8	6	2	15	1	1	3	2	5	1	2	1	1	1	1	1	0	0	1
8	5	0	9	2	3	2	2	5	3	0	6	1	0	1	1	0	0	1
1	3	2	7	1	1	1	2	5	2	2	2	4	2	0	0	0	0	0
1	7	10	22	1	2	3	3	5	4	0	7	2	0	1	1	0	0	1
1	6	2	5	2	3	0	2	2	1	0	13	0	0	1	1	0	0	1
8	5	9	4	1	1	3	2	1	3	0	15	1	0	1	1	0	0	1
8	6	2	5	2	3	5	2	5	1	2	16	4	3	0	0	0	0	0
1	3	0	12	2	3	3	2	5	2	2	16	4	3	0	0	0	0	0
8	7	6	12	2	2	3	3	4	4	2	16	4	3	0	0	0	0	0
8	3	13	9	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
8	6	10	11	2	2	1	3	1	1	0	3	0	0	0	1	0	0	1
8	3	7	2	1	5	3	2	4	2	0	15	0	0	1	1	0	0	1
1	7	2	11	1	2	1	1	5	5	2	3	4	0	0	0	0	0	0
8	5	13	20	1	4	3	2	1	5	0	14	1	0	1	1	1	1	1
8	3	0	13	2	3	3	2	5	2	0	13	1	0	1	1	1	0	1
8	3	2	6	2	2	1	2	4	2	2	16	4	3	0	0	0	0	0
8	5	0	9	2	2	2	2	4	3	0	3	0	0	1	0	0	0	0
1	3	0	14	2	5	3	2	2	2	0	15	1	0	1	1	1	0	1
8	3	3	8	2	6	3	2	1	2	2	16	4	3	0	0	0	0	0
1	3	8	18	2	1	0	2	0	2	2	6	4	0	1	1	1	0	1
8	7	0	31	1	3	3	1	1	4	2	16	4	3	0	0	0	0	0
8	3	0	19	1	6	1	1	5	2	0	2	1	0	1	1	0	0	1
1	3	6	2	2	2	3	2	5	2	0	15	1	0	1	1	0	0	1
8	3	0	21	1	1	3	1	1	2	2	16	4	3	0	0	0	0	0
8	6	13	2	2	1	6	2	1	1	0	1	0	0	1	1	0	0	1
8	6	9	5	1	1	3	2	1	1	0	1	3	0	1	1	0	0	1
0	3	0	8	2	3	2	2	5	2	2	16	4	3	0	0	0	0	0
1	7	0	24	1	4	3	1	5	4	0	8	1	0	1	1	1	1	1
1	5	0	20	1	7	1	1	0	3	0	15	1	0	1	1	0	0	0
8	3	5	1	2	1	0	2	4	2	0	7	1	0	1	1	0	0	0
8	3	0	9	2	4	2	2	4	2	0	12	1	0	1	1	0	0	1
8	3	0	17	2	1	1	2	4	2	2	16	4	3	0	0	0	0	0
1	9	2	6	2	1	1	2	2	5	2	16	4	3	0	0	0	0	0
8	5	2	8	2	3	1	2	4	3	0	4	2	0	1	1	0	0	1
1	3	0	16	2	4	3	3	2	2	0	10	1	0	1	1	0	0	0
8	3	6	1	1	3	0	1	5	2	2	2	4	0	0	0	0	0	0
8	3	0	17	2	3	3	2	1	2	0	2	1	0	1	1	1	0	1
8	7	11	8	2	2	1	3	4	4	2	16	4	3	0	0	0	0	0
8	0	9	9	2	9	6	3	3	0	2	16	4	3	0	0	0	0	0
1	3	0	31	1	4	3	1	0	2	0	11	1	0	1	1	0	0	1
8	5	0	11	2	3	3	2	5	3	0	15	0	0	1	1	0	0	0
1	3	0	19	1	3	1	1	5	2	0	4	1	0	1	1	1	0	0
1	3	6	3	1	3	3	2	5	2	0	4	2	0	1	1	1	0	1
1	5	10	25	1	4	0	2	0	5	0	10	1	0	1	1	0	0	1
1	3	0	30	1	2	3	1	2	2	0	4	2	0	1	1	0	0	0
8	7	11	25	0	3	3	2	1	5	0	7	1	0	1	1	1	1	1
1	9	9	2	1	5	3	1	0	5	2	16	4	3	0	0	0	0	0
1	3	13	10	2	3	3	3	0	2	0	6	1	0	1	1	1	0	0
8	1	6	14	1	1	3	2	5	5	0	1	4	1	1	1	0	1	1
8	2	12	3	2	3	0	2	1	5	2	16	4	3	0	0	0	0	0
1	11	0	17	2	2	3	2	0	5	2	16	4	3	0	0	0	0	0
8	6	2	6	1	5	3	2	1	1	0	8	0	0	1	1	1	1	1
7	0	9	8	2	1	0	3	3	0	2	16	4	3	0	0	0	0	0
1	5	6	3	2	2	3	2	2	3	0	5	2	0	1	1	1	0	1
8	0	0	8	2	2	0	2	3	0	0	9	1	0	1	1	1	0	1
1	0	14	10	2	2	6	2	5	0	0	5	1	0	0	1	0	0	0
8	3	6	2	2	2	5	2	1	2	0	6	1	0	1	1	0	0	0
8	0	0	8	2	9	6	2	3	0	2	2	4	0	0	0	0	0	0
1	5	11	14	1	2	3	2	5	5	0	4	1	0	1	1	0	0	0
8	5	3	8	1	2	3	2	1	5	0	4	1	0	1	1	1	1	1
1	5	13	5	2	4	3	2	2	3	0	1	0	0	1	1	0	0	1
1	11	2	18	2	2	1	3	2	5	2	2	0	0	1	1	0	0	1
8	5	14	11	2	4	1	3	4	3	2	16	4	1	1	1	0	0	0
1	5	2	11	1	2	3	2	5	5	0	5	4	1	1	1	0	0	1
8	6	2	4	1	1	1	2	1	1	2	3	0	1	0	1	0	0	0
8	6	14	1	2	1	3	2	4	1	2	16	4	3	0	0	0	0	0
1	3	13	6	2	3	2	2	5	2	0	3	4	0	1	1	1	0	1
8	0	0	8	2	1	6	2	3	0	0	6	4	1	1	1	1	0	0
8	6	9	3	1	3	2	2	5	1	0	6	1	1	1	1	0	0	0
1	0	0	9	2	1	6	2	5	0	2	4	0	0	1	0	0	0	0
1	6	0	12	2	4	3	2	2	1	0	15	1	0	1	1	0	0	0
8	5	7	8	1	5	3	2	5	3	0	7	4	0	1	1	0	0	1
8	3	0	15	2	2	3	2	5	2	0	3	0	0	1	1	0	0	1
8	6	12	14	2	2	1	3	4	1	2	4	4	0	1	1	0	0	1
1	7	0	8	2	2	2	2	5	4	0	4	1	1	1	1	0	0	1
1	5	0	20	1	3	5	1	2	3	0	15	1	0	1	1	0	0	1
1	6	0	17	2	1	3	2	2	5	2	1	1	0	0	0	0	0	0
1	6	13	3	2	1	1	2	5	1	0	5	4	0	1	1	1	0	1
1	3	0	21	1	4	3	1	2	2	0	8	1	0	1	1	0	1	1
8	5	0	15	2	3	3	2	4	3	0	8	2	0	1	0	0	0	0
1	5	0	19	1	3	3	1	2	3	0	7	0	0	0	1	0	0	0
1	9	0	21	1	2	1	1	5	5	0	1	0	0	1	1	0	0	1
8	0	0	8	2	9	6	2	3	0	2	16	0	1	1	1	0	0	1
8	7	0	19	1	4	3	1	4	4	0	15	1	0	1	1	0	0	1
8	5	0	27	1	3	3	1	1	5	0	4	1	0	1	1	1	0	0
1	5	2	13	2	3	1	3	5	3	0	6	0	1	0	0	0	0	0
8	3	11	14	2	2	3	3	4	2	0	1	1	0	1	1	0	0	1
8	6	14	9	2	1	2	2	4	1	2	16	4	3	0	0	0	0	0
1	3	0	18	2	1	3	2	2	2	2	16	4	3	0	0	0	0	0
1	6	10	20	1	2	3	2	2	1	0	4	2	0	1	1	0	0	1
1	3	0	21	1	2	2	1	0	2	0	6	1	0	1	1	1	1	1
1	3	0	20	1	2	2	1	5	2	0	3	2	0	1	1	0	0	1
1	0	2	13	1	1	6	2	2	5	2	16	4	3	0	0	0	0	0
8	7	0	9	2	2	2	2	4	4	2	0	1	0	0	0	0	0	0
1	5	6	1	2	3	3	2	5	3	0	6	1	0	1	1	0	0	0
8	0	0	10	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	11	0	31	1	3	3	1	0	5	0	12	4	1	1	1	1	0	1
8	3	0	19	1	2	3	2	4	2	2	16	4	3	0	0	0	0	0
8	3	12	19	1	2	3	3	4	2	0	6	4	2	1	0	0	0	0
8	5	6	6	1	4	3	1	1	5	0	11	1	0	1	1	0	1	1
8	5	0	12	2	2	3	2	4	3	0	12	1	0	1	1	1	1	1
1	6	0	10	2	3	2	2	2	1	0	5	1	0	1	1	0	0	0
8	3	0	12	2	5	3	2	4	2	0	15	1	0	1	1	1	0	1
1	6	0	22	1	3	3	1	0	1	0	1	4	0	1	1	0	0	1
8	7	0	16	2	3	1	2	4	4	0	6	1	0	1	0	0	0	0
8	6	0	10	2	3	2	2	4	1	0	6	1	0	1	1	1	1	1
8	3	10	19	1	6	3	2	5	5	2	16	4	3	0	0	0	0	0
8	0	12	10	2	1	6	2	3	0	0	3	4	1	1	0	0	0	0
8	3	9	10	1	2	3	2	4	2	0	3	1	0	1	0	1	1	0
8	3	0	18	1	3	3	1	4	2	2	16	4	3	0	0	0	0	0
8	6	2	16	1	2	3	3	1	1	1	2	0	1	1	0	0	0	0
8	0	0	10	2	2	0	2	3	0	2	2	0	0	1	0	0	0	0
8	7	2	7	2	1	6	3	1	4	0	7	4	1	1	1	0	0	0
1	1	0	24	1	1	3	1	2	5	2	16	4	3	0	0	0	0	0
8	2	2	5	2	2	6	2	1	5	0	2	0	0	1	1	1	0	1
1	3	6	17	1	3	3	2	5	5	0	7	0	0	1	1	0	1	1
8	0	8	2	2	2	0	2	5	5	2	16	4	3	0	0	0	0	0
8	5	2	5	1	1	5	2	1	3	2	0	4	0	0	1	0	0	0
8	8	14	24	1	3	3	2	5	5	0	3	0	0	1	1	1	1	1
8	5	0	19	1	4	1	1	4	3	0	15	1	0	1	1	0	0	1
8	11	2	9	2	2	3	3	4	5	2	16	4	3	0	0	0	0	0
8	11	0	15	2	2	3	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	13	2	2	3	2	4	2	0	14	0	0	1	1	1	0	0
1	3	2	13	2	3	1	3	5	2	0	6	4	0	1	1	0	0	0
1	0	0	8	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
7	6	14	15	2	4	3	3	1	1	0	9	1	0	1	1	1	0	1
1	5	2	5	1	2	3	1	5	5	2	1	4	3	0	0	0	0	0
8	0	0	10	2	3	6	2	5	0	1	1	0	1	0	0	0	0	0
8	2	2	7	2	2	0	2	5	5	2	2	4	1	1	1	0	0	1
1	0	0	9	2	2	0	2	5	0	0	2	1	0	1	1	1	0	1
8	3	0	13	2	2	1	2	4	2	0	4	0	2	1	0	0	0	0
8	3	2	6	2	5	0	3	1	2	0	15	2	0	1	0	0	0	0
8	6	0	13	2	2	1	2	5	1	0	3	1	0	0	1	0	0	0
1	5	0	14	2	3	3	2	5	3	2	2	0	0	0	0	0	0	0
8	3	0	12	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
1	7	0	21	1	4	3	1	2	4	2	16	4	3	0	0	0	0	0
1	6	9	8	1	2	3	2	5	1	0	15	4	0	1	1	1	1	1
1	7	0	22	1	2	3	1	2	4	0	4	2	0	1	1	1	0	1
8	3	4	19	1	2	3	2	4	2	0	12	0	0	1	1	0	0	1
8	9	0	21	1	2	3	1	4	5	0	6	1	0	0	0	0	0	0
1	7	2	6	2	2	3	2	2	4	0	7	2	0	0	1	0	0	0
8	0	0	9	2	2	6	2	5	0	0	3	0	0	1	1	0	0	0
8	5	6	3	1	1	1	2	5	3	2	16	4	3	0	0	0	0	0
1	11	2	5	1	2	1	1	2	5	2	16	4	3	0	0	0	0	0
1	3	0	11	2	4	7	2	5	2	0	8	1	0	1	1	0	0	0
8	5	0	12	2	2	3	2	5	5	0	2	1	0	1	1	0	1	1
1	0	0	8	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
1	8	13	21	1	3	3	3	5	5	0	5	0	0	1	1	1	0	1
1	5	14	15	1	2	2	2	5	3	0	2	0	0	1	1	0	0	1
8	6	5	13	2	2	6	3	4	1	2	16	4	3	0	0	0	0	0
1	7	2	7	1	1	1	2	2	4	0	7	1	0	1	1	1	0	1
8	8	2	6	1	2	1	2	1	5	0	6	0	1	0	1	0	0	0
8	3	6	1	1	3	3	2	1	2	0	12	1	0	1	1	1	1	1
1	11	0	24	1	2	3	1	2	5	0	4	1	0	1	0	1	0	0
1	5	5	6	2	2	0	3	0	3	0	11	0	1	1	1	0	0	1
1	7	10	19	2	3	3	3	5	4	0	7	2	0	1	1	0	0	0
8	3	2	6	1	1	1	2	5	2	2	16	4	3	0	0	0	0	0
1	7	2	13	0	1	6	2	2	5	2	16	4	2	1	1	0	0	1
1	3	7	5	2	2	3	2	2	2	0	15	1	0	1	1	1	0	0
1	7	0	20	1	3	3	1	0	4	2	16	4	3	0	0	0	0	0
8	6	0	19	1	2	3	1	4	1	2	16	4	3	0	0	0	0	0
1	7	0	31	1	2	3	1	2	5	2	16	4	3	0	0	0	0	0
1	3	0	21	1	5	3	1	0	2	0	12	4	0	1	1	1	1	1
1	3	0	14	2	4	2	2	2	2	0	1	1	0	1	1	0	0	0
1	3	0	12	2	3	3	2	0	2	0	15	1	0	1	1	1	1	1
8	9	12	4	2	1	2	3	4	5	0	3	4	0	1	1	0	0	1
1	8	0	24	1	2	3	1	0	5	0	9	1	0	1	1	1	1	1
8	1	14	10	1	1	2	2	5	5	2	16	4	3	0	0	0	0	0
1	6	0	22	1	3	3	1	2	1	0	12	1	1	1	0	1	0	0
3	7	5	13	1	1	3	2	1	5	2	16	4	0	0	0	0	0	0
8	5	0	12	2	2	3	2	4	3	2	0	4	0	0	0	0	0	0
1	11	2	7	1	4	2	2	2	5	2	1	3	0	1	1	0	0	0
1	11	0	19	2	3	2	3	5	5	0	5	4	0	1	1	0	0	1
8	3	0	12	2	2	3	2	4	2	0	2	4	0	1	1	0	0	1
1	3	14	29	1	4	3	2	2	2	0	14	1	0	1	1	1	0	0
8	6	3	1	2	1	6	2	1	1	0	9	2	0	1	1	0	0	1
8	1	0	23	1	3	2	1	1	5	2	16	4	3	0	0	0	0	0
1	5	0	18	2	4	0	2	2	3	0	8	0	1	1	0	0	0	0
1	3	14	20	1	2	3	2	5	2	2	16	4	3	0	0	0	0	0
8	0	9	7	2	9	6	3	3	0	2	16	4	3	0	0	0	0	0
8	3	9	9	2	2	3	3	4	2	0	10	1	0	1	1	0	1	1
8	6	0	13	2	4	1	2	4	1	0	9	3	0	1	0	0	0	0
1	6	0	13	2	4	3	2	2	1	2	16	4	3	0	0	0	0	0
8	10	10	12	2	4	1	2	4	5	2	16	4	3	0	0	0	0	0
8	7	5	1	2	3	1	2	4	4	2	5	0	0	0	0	0	0	0
8	6	2	8	2	3	1	3	4	1	0	5	2	0	1	1	0	0	1
1	6	2	12	1	3	3	2	0	1	0	5	1	0	1	1	1	0	1
1	5	5	9	2	1	6	2	5	3	2	16	4	3	0	0	0	0	0
1	3	0	14	2	6	3	2	0	2	0	3	1	0	1	1	0	0	1
1	3	0	20	1	3	3	1	0	2	0	5	1	0	1	1	1	1	1
8	6	2	8	2	1	7	2	4	1	2	16	4	3	0	0	0	0	0
1	6	6	1	2	4	3	2	2	1	0	9	0	1	1	1	0	0	1
1	7	2	13	2	6	4	3	0	4	0	15	0	1	1	1	1	0	0
8	7	2	8	2	1	1	3	5	4	0	16	0	0	1	0	0	0	0
8	6	2	5	2	1	1	2	1	1	2	16	4	3	0	0	0	0	0
1	3	0	18	2	3	3	2	0	2	0	10	1	0	1	1	1	0	1
1	7	0	20	1	1	3	1	0	4	0	3	2	0	1	1	1	0	1
8	11	0	12	2	2	1	2	1	5	0	6	0	0	1	1	0	1	1
1	5	2	7	2	1	1	2	2	3	2	16	4	3	0	0	0	0	0
1	7	0	14	2	3	3	2	0	4	0	7	1	0	1	1	1	0	1
1	6	4	6	2	1	1	3	2	1	2	16	4	3	0	0	0	0	0
1	11	2	5	2	1	1	2	2	5	2	16	4	3	0	0	0	0	0
1	7	0	20	1	6	3	1	0	4	0	10	2	0	1	1	0	0	0
8	7	10	20	1	2	3	2	4	4	0	1	1	0	0	0	0	0	0
1	3	0	21	1	2	3	1	5	2	0	4	4	0	1	1	0	0	1
8	5	11	4	1	1	3	1	1	3	0	5	1	0	1	1	1	0	1
8	3	0	15	2	1	3	2	4	2	2	16	4	3	0	0	0	0	0
1	3	7	11	1	4	3	2	5	2	0	6	4	0	1	1	1	0	0
1	6	2	4	2	3	1	2	0	1	2	16	4	3	0	0	0	0	0
8	5	0	21	1	3	3	1	4	3	0	1	4	0	1	1	0	0	1
1	3	8	3	2	4	3	2	0	2	0	3	1	0	1	1	1	1	1
8	3	13	17	1	4	3	2	1	2	2	1	1	0	1	1	0	0	0
8	7	0	10	2	4	3	2	4	4	0	3	4	0	1	1	1	1	1
8	6	8	11	2	4	3	3	1	1	0	8	4	0	1	0	0	1	0
8	6	0	15	2	3	1	2	4	1	0	8	1	0	1	1	0	0	0
8	11	2	4	2	1	1	2	4	5	2	4	0	1	0	0	0	0	0
8	6	0	17	2	2	3	2	5	1	0	1	2	0	1	1	0	0	1
1	7	0	12	2	1	1	2	5	4	2	16	0	0	1	1	0	0	1
1	5	0	20	1	4	3	1	2	3	0	15	1	0	1	1	1	1	1
8	9	0	20	1	4	3	1	1	5	0	11	1	0	1	1	0	0	0
1	7	10	15	2	1	2	2	2	4	2	16	4	3	0	0	0	0	0
8	3	13	15	2	4	3	3	4	2	0	7	2	0	1	1	1	0	1
1	5	11	15	2	4	1	3	0	3	2	16	4	3	0	0	0	0	0
8	11	0	8	2	1	2	2	4	5	0	3	0	0	1	1	0	0	1
1	6	12	16	2	3	3	2	2	1	0	5	2	0	1	1	0	0	1
1	1	14	22	1	1	2	2	5	5	0	0	1	0	1	0	0	0	0
1	3	0	22	1	3	3	1	5	2	0	6	1	0	1	1	0	0	1
8	7	2	8	1	2	2	1	1	5	0	4	0	0	1	1	0	0	0
1	6	0	8	2	1	2	2	0	1	0	15	0	1	1	1	1	0	1
8	7	0	18	2	3	3	2	4	4	0	6	1	0	1	1	0	0	1
1	7	0	31	1	3	1	1	2	5	0	7	0	0	1	1	0	0	1
8	3	9	1	1	2	3	2	4	2	2	16	4	3	0	0	0	0	0
8	3	0	20	1	4	3	1	4	2	0	8	4	0	1	1	1	0	1
1	6	0	11	2	3	2	2	0	1	0	15	2	0	1	1	0	0	0
1	1	0	23	1	3	3	1	2	5	0	15	3	2	1	1	0	1	0
8	3	14	31	0	3	3	1	1	2	0	15	1	0	1	1	1	0	1
1	7	2	13	0	3	3	2	5	5	0	5	1	0	1	1	1	0	1
8	0	0	8	2	9	6	2	3	0	0	16	0	1	0	0	0	0	0
8	3	0	9	2	1	2	2	4	2	2	16	4	3	0	0	0	0	0
1	7	0	28	1	4	3	3	2	4	0	8	2	0	1	1	1	1	1
8	11	0	30	1	2	3	1	1	5	0	15	1	0	1	1	1	0	1
8	6	0	18	1	3	3	1	5	1	0	7	1	0	1	1	0	0	0
1	3	9	8	2	3	3	2	5	5	2	3	4	1	0	0	0	0	0
1	1	2	21	1	3	3	3	0	5	0	6	4	0	1	1	0	0	1
1	5	7	1	2	3	2	2	5	3	0	3	4	0	1	0	0	0	0
1	6	6	3	1	3	3	1	2	1	0	11	1	0	1	1	0	0	0
1	9	9	10	2	3	3	3	5	5	0	1	1	0	0	0	0	0	0
8	11	11	11	2	3	1	3	1	5	0	3	1	0	1	1	1	0	1
8	3	6	3	2	2	3	2	4	2	0	1	4	0	1	1	0	0	1
8	0	9	10	2	2	6	3	3	0	2	16	4	3	0	0	0	0	0
8	7	0	9	2	1	2	2	4	5	0	15	0	0	1	1	1	0	1
1	7	11	19	2	3	1	2	0	4	0	5	0	1	1	1	0	0	1
1	6	0	20	1	2	3	1	0	1	0	2	3	0	0	0	0	0	0
8	3	0	11	2	5	3	2	1	2	0	8	1	0	1	1	1	1	1
1	7	0	11	2	5	3	2	0	5	0	12	3	0	1	0	0	0	0
8	3	0	9	2	4	3	2	5	2	0	8	1	0	1	1	0	0	1
8	5	2	18	1	2	2	2	1	5	0	5	2	0	0	0	0	0	0
1	3	5	2	2	6	2	2	2	2	0	10	0	1	1	1	0	0	1
8	5	6	20	1	3	3	3	5	3	2	16	4	3	0	0	0	0	0
1	6	12	7	2	4	1	2	4	1	0	9	4	0	1	0	0	0	0
1	6	0	20	1	2	2	1	0	1	0	7	1	0	1	1	1	0	0
8	5	0	9	2	1	3	2	4	5	2	16	4	3	0	0	0	0	0
8	5	12	27	1	3	3	2	1	3	0	6	1	0	1	1	1	0	0
8	3	0	12	2	4	1	2	4	2	0	15	0	1	1	1	1	0	0
8	11	0	17	2	1	3	2	1	5	0	2	1	0	1	1	0	0	1
1	7	0	13	2	2	1	2	5	4	2	16	4	1	0	0	0	0	0
1	7	0	16	2	3	1	2	5	4	0	7	0	0	1	0	1	0	0
1	0	2	8	1	3	0	2	2	5	0	3	1	0	1	1	0	0	1
8	3	0	9	2	4	3	2	4	2	0	15	1	0	1	1	1	0	1
1	11	3	11	1	2	3	2	2	5	0	10	0	1	1	1	0	0	0
8	6	2	7	1	1	1	2	5	1	2	16	4	0	0	0	0	0	0
1	11	2	4	2	3	3	2	2	5	0	15	1	0	1	1	0	0	1
1	11	0	20	1	4	2	1	5	5	0	3	2	0	1	1	1	0	1
1	3	0	12	2	4	3	2	5	2	0	6	1	0	1	1	0	0	1
4	3	5	17	1	4	3	3	4	2	2	16	4	3	0	0	0	0	0
1	3	0	20	1	3	3	1	5	2	0	15	1	1	1	1	0	0	1
8	6	0	10	2	2	3	2	4	1	0	1	1	0	1	1	0	0	1
1	3	0	20	1	3	4	1	0	2	0	6	2	0	1	1	1	0	1
1	6	14	12	2	3	1	3	2	1	0	5	0	1	0	0	0	0	0
8	3	2	14	2	4	3	3	5	2	0	8	0	0	1	1	1	1	1
1	11	5	20	1	2	3	2	5	5	0	2	1	0	1	1	1	1	1
8	7	0	19	2	1	1	2	1	5	2	0	4	0	0	0	0	0	0
1	3	14	1	1	2	6	2	5	2	2	16	4	3	0	0	0	0	0
8	3	14	10	2	5	3	2	5	2	0	11	2	0	1	1	0	0	1
1	7	0	24	1	4	3	1	0	4	0	5	1	0	1	1	0	0	1
1	1	10	23	1	3	3	2	5	5	0	4	4	0	1	0	0	0	0
8	3	0	8	2	3	2	2	4	2	0	8	1	0	1	1	1	0	1
1	0	0	9	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
1	3	0	17	2	2	3	2	0	2	2	2	3	0	1	1	0	0	0
1	3	0	19	2	1	6	2	0	2	2	1	2	0	1	1	0	0	1
8	3	6	1	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
1	8	0	16	2	1	1	2	2	5	2	16	4	3	0	0	0	0	0
1	0	0	6	3	1	0	3	5	0	0	5	0	0	0	0	0	0	0
1	11	2	3	2	2	1	2	2	5	0	5	0	1	1	1	0	0	0
1	5	0	9	2	3	2	2	2	3	0	3	1	0	1	1	1	0	0
1	7	0	21	1	1	1	1	2	4	1	2	0	0	1	1	0	0	1
8	3	0	17	2	3	3	2	1	2	0	6	0	1	1	1	0	1	1
8	5	2	17	1	3	3	2	4	3	0	2	4	0	0	0	0	0	0
8	3	0	12	2	4	1	2	4	2	0	5	0	0	1	1	0	0	1
8	7	4	5	2	3	1	2	1	4	0	5	0	0	0	0	0	0	0
1	6	9	11	1	5	3	2	5	5	0	12	2	0	1	1	0	0	1
8	3	0	19	2	3	3	2	5	2	0	9	1	0	1	1	0	0	0
8	7	0	9	2	4	4	2	4	4	2	16	4	3	0	0	0	0	0
8	1	0	22	1	3	3	1	1	5	0	9	1	0	1	1	1	1	1
1	9	6	14	1	3	3	2	5	5	0	7	3	0	1	0	0	1	0
8	3	9	12	1	1	6	2	1	2	0	6	1	0	1	1	0	1	1
1	5	0	23	1	3	5	1	5	5	0	7	1	0	1	1	0	0	1
8	5	0	14	2	3	3	2	4	3	0	7	4	0	1	1	0	0	1
1	0	0	9	2	2	6	2	5	0	2	16	4	3	0	0	0	0	0
1	5	0	14	2	3	3	2	2	3	0	7	1	0	1	1	1	0	0
1	9	0	23	1	3	3	1	2	5	2	0	4	0	0	0	0	0	0
1	11	11	1	2	4	6	2	2	5	2	16	4	3	0	0	0	0	0
1	7	2	16	1	3	4	2	2	5	2	16	4	3	0	0	0	0	0
8	5	14	3	2	1	1	2	4	3	0	11	1	0	1	1	1	1	1
1	0	0	9	2	2	5	2	5	0	2	1	3	1	0	0	0	0	0
1	7	14	21	1	1	1	1	5	5	2	1	4	3	0	0	0	0	0
8	0	0	8	2	1	6	2	3	0	2	2	3	0	0	0	0	0	0
8	3	0	20	1	1	3	1	4	2	2	16	4	3	0	0	0	0	0
1	3	0	9	2	2	6	2	2	2	0	12	1	0	1	1	1	0	1
1	7	0	21	1	3	1	1	2	4	2	16	4	3	0	0	0	0	0
1	6	0	14	2	3	1	2	0	1	0	11	1	0	1	1	0	0	1
1	3	13	14	2	4	3	3	2	2	0	5	1	1	1	1	0	0	1
1	1	0	24	1	3	2	1	0	5	2	1	1	0	1	0	0	0	0
1	7	0	16	2	2	3	2	0	4	0	4	4	0	1	1	1	0	0
1	7	10	16	2	4	3	3	0	4	0	9	2	0	1	0	0	0	0
8	7	2	12	2	3	1	2	1	4	2	16	4	1	0	0	0	0	0
8	7	5	13	2	3	1	2	4	4	2	16	4	3	0	0	0	0	0
8	3	6	10	2	1	2	3	1	2	0	3	3	0	1	0	0	0	0
8	3	0	9	2	2	2	2	4	2	0	2	4	0	1	0	0	0	0
8	7	2	8	2	1	6	2	1	4	2	16	4	3	0	0	0	0	0
8	0	0	9	2	2	6	2	3	0	0	15	1	0	1	1	0	0	1
1	7	0	9	2	1	3	2	0	4	1	1	0	1	1	1	0	0	0
1	3	0	18	2	2	3	2	5	2	2	16	4	3	0	0	0	0	0
8	3	0	11	2	3	3	2	4	2	0	4	1	0	1	0	0	0	0
8	2	2	5	2	2	0	2	5	5	0	5	1	1	1	1	1	0	0
1	3	0	12	2	4	3	2	0	2	0	4	4	0	1	1	0	1	1
1	7	2	29	1	4	3	3	0	5	2	0	0	0	0	1	0	0	0
8	11	0	24	1	2	7	2	5	5	2	16	4	3	0	0	0	0	0
1	6	2	5	1	1	3	1	5	1	2	16	4	3	0	0	0	0	0
8	5	0	18	2	5	3	2	4	3	2	16	4	3	0	0	0	0	0
1	6	2	10	1	1	1	2	5	1	0	15	0	1	1	1	0	0	1
1	7	0	31	0	3	3	0	2	5	0	5	1	0	1	1	0	1	1
8	3	5	10	2	1	3	2	4	2	2	16	4	3	0	0	0	0	0
8	6	5	8	1	1	6	2	5	1	2	16	4	3	0	0	0	0	0
1	5	0	22	1	2	3	1	2	5	2	16	4	3	0	0	0	0	0
5	3	5	3	2	1	3	2	5	2	2	16	4	3	0	0	0	0	0
1	7	0	9	2	3	2	2	0	4	2	16	4	3	0	0	0	0	0
1	3	0	11	2	5	3	2	5	2	0	13	0	0	1	1	1	1	1
8	5	2	7	2	1	1	2	4	3	0	6	0	0	0	0	0	0	0
8	5	13	8	2	3	3	3	1	3	0	4	0	1	1	1	0	1	1
1	7	0	31	0	2	3	0	0	5	0	1	0	1	1	0	0	0	0
8	7	0	22	1	1	1	1	1	4	0	15	1	0	1	1	0	0	1
8	6	2	17	1	3	1	3	4	1	0	13	1	0	1	1	0	0	0
8	3	0	12	2	1	1	2	5	2	0	2	0	1	1	1	0	0	1
8	6	0	9	2	2	3	2	4	1	0	3	1	1	1	1	1	0	1
1	3	0	20	1	7	3	1	5	2	0	15	1	0	1	1	1	0	1
1	0	0	8	2	0	6	2	5	0	0	5	1	0	1	1	1	0	1
8	3	2	4	2	1	0	3	4	2	0	7	4	0	1	1	0	0	0
1	7	10	31	1	5	3	2	5	5	2	16	4	3	0	0	0	0	0
8	5	0	19	1	1	1	1	5	3	0	10	4	1	1	1	0	0	1
1	0	9	10	2	2	0	3	5	0	0	4	1	0	1	1	0	0	0
8	11	4	2	2	3	3	2	4	5	2	16	4	3	0	0	0	0	0
8	5	0	9	2	4	3	2	1	3	0	6	2	0	1	1	0	0	1
8	7	2	7	1	1	6	1	1	5	2	16	4	1	0	0	0	0	0
8	6	2	4	2	6	2	3	1	1	1	1	1	0	1	0	0	0	0
1	6	2	7	2	1	3	3	0	1	2	16	4	3	0	0	0	0	0
1	0	0	9	2	2	6	2	5	0	0	12	2	0	1	1	0	0	0
8	3	0	8	2	3	2	2	4	2	2	6	0	1	1	0	1	0	0
8	5	0	10	2	1	3	2	4	3	1	0	1	0	0	0	0	0	0
1	3	0	8	2	5	3	2	0	2	2	16	4	3	0	0	0	0	0
8	6	5	10	2	3	1	3	4	1	2	16	4	3	0	0	0	0	0
8	3	5	9	2	5	3	3	4	2	0	4	0	0	1	1	1	0	0
1	3	0	21	1	5	3	1	2	2	2	16	4	3	0	0	0	0	0
1	5	0	24	1	5	2	1	2	5	0	15	1	0	1	1	0	0	1
8	3	0	10	2	3	3	2	1	2	0	4	0	0	1	1	1	0	1
1	3	0	16	2	4	3	2	5	2	0	9	1	0	1	1	0	1	0
8	6	0	8	2	3	3	2	4	1	2	16	4	3	0	0	0	0	0
8	6	2	6	2	1	1	2	4	1	2	16	0	0	1	1	0	0	0
1	7	0	19	2	4	3	2	0	4	0	2	3	1	0	0	0	0	0
1	7	0	19	1	5	3	1	5	4	0	15	1	0	1	1	1	1	1
1	2	2	4	2	2	0	2	5	5	2	3	4	1	0	0	0	0	0
8	0	0	9	2	0	6	2	3	0	0	6	1	0	1	1	0	1	1
8	3	0	9	2	2	2	2	4	2	0	5	1	0	1	1	0	0	1
8	3	0	8	2	1	0	2	4	5	0	6	0	2	1	0	0	0	0
1	5	1	16	2	1	3	3	2	3	0	3	2	0	1	1	0	0	0
8	7	5	3	1	1	1	1	5	5	2	16	4	1	0	0	0	0	0
1	7	0	15	2	4	2	2	0	4	2	16	4	3	0	0	0	0	0
8	7	2	7	2	2	1	2	5	4	2	16	4	3	0	0	0	0	0
1	3	0	16	2	2	3	2	5	2	0	2	1	0	1	1	1	0	1
8	3	0	15	2	5	3	2	4	2	0	12	4	0	1	1	1	0	1
8	6	2	5	2	1	1	2	4	1	2	16	4	3	0	0	0	0	0
8	7	2	28	0	2	3	3	1	5	0	3	0	0	1	0	0	0	0
1	5	0	25	1	2	0	1	0	5	0	12	1	0	1	1	1	0	0
8	9	0	9	2	2	2	2	4	5	2	16	4	3	0	0	0	0	0
8	3	3	11	2	3	1	3	5	2	0	4	1	1	0	1	0	0	1
8	3	0	20	2	2	3	2	5	2	2	16	4	3	0	0	0	0	0
0	3	5	2	2	2	3	2	0	2	0	4	1	0	1	1	0	0	0
8	6	0	14	2	2	3	2	1	1	2	16	4	3	0	0	0	0	0
1	5	5	14	1	3	3	3	2	3	0	9	1	0	1	1	0	0	1
1	5	0	11	2	3	3	2	5	3	0	6	1	0	1	1	1	0	0
1	9	0	20	1	2	1	1	5	5	0	4	2	0	1	1	0	0	1
1	11	5	13	2	3	3	3	5	5	0	4	1	0	1	1	0	0	1
1	3	0	19	2	6	1	2	5	2	0	3	1	0	1	0	1	0	0
8	6	0	12	2	1	1	2	5	1	2	16	4	3	0	0	0	0	0
8	3	6	1	2	4	3	2	1	2	0	5	1	0	1	1	1	0	1
1	7	2	12	2	5	2	3	2	4	0	15	1	0	1	1	1	0	0
8	3	2	7	2	3	3	2	5	2	0	5	0	0	1	0	0	0	0
8	3	2	4	1	2	1	2	5	2	0	12	0	0	1	1	0	0	1
1	3	0	11	2	4	3	2	0	2	0	3	1	0	1	1	1	0	1
1	3	0	13	2	4	2	2	2	2	0	15	0	1	1	0	1	0	0
1	3	2	6	2	2	3	2	5	2	0	5	2	0	1	1	1	0	1
1	3	0	11	2	4	2	2	5	2	0	15	1	0	1	1	1	1	0
1	11	11	16	2	2	3	2	0	5	0	3	4	0	1	1	0	0	1
8	9	7	6	2	2	3	2	4	5	0	6	0	0	1	1	0	0	1
1	9	12	31	1	4	3	3	2	5	0	15	1	0	1	1	0	0	1
1	7	6	4	1	3	3	2	5	4	0	1	4	0	0	0	0	0	0
1	3	0	16	2	2	2	2	5	2	0	6	1	0	1	0	1	0	0
1	5	0	28	1	2	3	1	2	3	0	4	0	0	1	1	1	0	1
1	1	0	24	1	5	3	1	0	5	0	6	1	0	1	1	0	0	1
1	5	9	10	2	1	3	2	2	3	0	1	0	0	1	1	0	0	1
1	5	2	13	2	3	1	3	5	5	0	5	0	0	1	1	0	0	0
8	0	0	8	2	8	6	2	3	0	2	16	4	3	0	0	0	0	0
8	7	0	5	2	2	3	3	4	4	0	2	2	0	1	1	0	0	1
1	6	2	7	1	1	1	2	2	1	0	3	0	1	1	1	0	0	1
8	0	11	10	2	2	0	2	4	5	2	16	4	3	0	0	0	0	0
8	3	2	13	1	2	3	2	4	5	0	4	1	0	1	1	1	1	1
1	0	0	4	3	2	0	3	5	0	2	2	0	1	0	0	0	0	0
8	3	0	14	2	1	3	2	5	2	0	16	3	0	1	1	0	0	1
1	6	0	13	2	5	3	2	5	1	0	3	1	0	1	1	0	0	0
8	7	0	21	1	3	2	1	4	4	0	11	4	0	1	1	0	0	1
8	6	14	16	2	4	1	2	1	1	2	16	4	3	0	0	0	0	0
1	3	0	10	2	6	2	2	2	2	0	15	1	0	1	1	1	0	1
8	7	2	4	2	4	3	2	5	4	0	10	2	0	1	1	0	0	0
1	3	0	8	2	4	6	2	5	2	0	15	1	0	1	1	0	0	0
8	3	5	3	2	3	3	3	4	2	0	10	1	0	1	0	1	0	0
1	7	0	28	1	4	3	1	0	5	0	15	0	0	1	1	0	0	1
8	6	13	1	2	2	2	2	4	1	2	3	4	1	1	1	0	1	1
1	5	5	8	1	2	3	2	2	5	0	8	4	0	1	1	0	0	0
1	8	0	9	2	5	2	2	5	5	0	14	1	0	1	1	1	1	0
1	0	9	8	2	1	6	3	5	0	2	16	4	3	0	0	0	0	0
8	0	9	2	2	1	5	2	3	0	0	2	4	0	0	1	1	0	0
1	5	2	17	1	3	3	2	5	5	0	6	4	2	0	1	0	0	0
1	9	6	2	1	2	3	1	2	5	0	6	4	0	1	1	0	0	1
1	0	0	10	2	2	6	2	3	0	0	4	3	1	1	1	0	0	0
8	11	2	4	2	3	2	2	4	5	0	8	0	0	1	1	1	0	1
8	11	5	1	2	1	1	2	4	5	2	1	4	3	0	0	0	0	0
8	3	3	9	2	3	1	3	5	2	0	15	4	0	0	0	0	0	0
1	0	0	9	2	1	6	2	5	0	0	6	2	0	1	1	1	0	1
1	5	6	17	2	5	3	3	4	3	0	5	0	0	1	1	0	0	0
1	3	0	9	2	2	2	2	0	2	0	10	3	2	1	1	0	1	1
8	3	0	9	2	3	2	2	1	2	0	4	1	0	1	1	1	0	0
8	5	0	11	2	5	3	2	5	3	0	13	1	0	1	1	1	0	1
1	6	3	5	1	1	1	2	2	1	0	15	1	0	0	0	0	0	0
1	0	5	9	2	2	6	3	5	0	2	3	4	0	0	0	0	0	0
1	3	10	19	2	2	1	2	2	2	2	16	4	3	0	0	0	0	0
1	6	2	9	1	1	1	1	2	5	0	15	0	0	1	1	1	0	1
1	7	0	13	2	3	1	2	0	4	0	4	1	0	1	1	0	0	0
8	7	0	20	1	4	3	1	4	4	0	5	3	1	1	1	1	0	1
1	3	0	26	1	3	3	1	5	2	0	4	1	0	1	1	1	1	1
1	3	12	19	1	3	3	1	5	5	0	4	1	0	1	1	1	1	1
1	3	0	20	1	5	3	1	0	2	0	7	4	0	1	1	1	1	1
8	3	14	1	2	5	3	2	5	2	2	16	4	3	0	0	0	0	0
1	0	0	9	2	8	6	2	5	0	2	16	4	3	0	0	0	0	0
8	2	2	3	2	2	0	2	1	5	2	16	4	3	0	0	0	0	0
8	3	0	17	2	4	1	2	1	2	0	8	4	0	1	1	0	0	1
8	9	6	10	2	2	2	3	1	5	2	16	4	3	0	0	0	0	0
8	11	0	12	2	4	3	2	4	5	2	0	0	0	1	0	0	0	0
8	0	0	11	2	1	6	3	4	5	2	16	4	3	0	0	0	0	0
1	7	0	30	1	2	7	1	2	5	2	16	4	3	0	0	0	0	0
8	9	0	31	0	4	3	0	1	5	0	14	1	0	1	1	1	0	0
0	0	0	8	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
1	5	0	13	2	4	3	2	5	3	0	8	0	0	1	0	1	0	0
1	3	0	12	2	4	1	2	5	2	0	10	2	0	0	1	1	0	0
1	3	14	17	2	4	3	2	2	2	0	7	1	0	1	1	1	0	1
8	11	12	20	1	4	2	2	1	5	2	16	4	3	0	0	0	0	0
8	0	0	8	2	3	0	2	5	0	0	4	0	1	1	0	0	0	0
1	7	14	25	1	3	3	3	0	5	0	3	1	0	1	1	1	0	1
1	11	2	9	2	3	1	2	2	5	0	15	1	0	1	1	0	0	1
1	3	7	11	2	1	3	3	5	2	2	16	4	3	0	0	0	0	0
8	6	3	11	2	2	3	3	4	1	2	16	4	3	0	0	0	0	0
8	3	0	18	2	2	3	2	4	2	2	16	0	2	1	1	0	0	1
1	6	0	20	2	2	4	2	5	1	0	1	1	0	1	1	0	0	0
8	11	8	28	0	2	3	3	1	5	0	3	4	0	0	0	0	0	0
1	0	0	9	2	8	6	2	5	0	2	16	4	3	0	0	0	0	0
1	3	0	19	1	1	3	1	0	5	0	3	1	0	1	1	0	0	1
1	5	0	16	2	3	3	2	5	3	2	16	4	3	0	0	0	0	0
8	5	0	23	1	5	3	1	1	5	2	2	1	0	0	0	0	0	0
8	5	6	15	2	2	1	3	1	3	0	2	4	0	1	1	0	0	0
8	0	0	10	2	3	6	2	5	0	0	6	2	0	1	1	0	0	1
1	7	0	13	2	1	2	2	0	4	0	6	0	0	1	1	0	0	1
1	0	2	16	1	5	0	3	5	5	0	15	1	0	1	0	0	0	0
8	3	14	15	2	3	3	2	4	2	0	6	1	0	1	1	1	1	1
1	3	2	12	2	2	5	3	2	2	0	4	2	0	1	1	0	0	0
1	7	1	2	2	2	2	2	2	4	2	3	1	0	1	1	0	0	1
8	6	9	11	2	3	3	3	1	1	0	7	4	0	1	1	0	0	1
8	3	4	1	2	1	1	2	4	5	0	10	2	0	1	0	0	0	0
8	5	2	8	1	1	1	2	1	3	2	2	0	1	1	1	1	0	1
1	0	12	15	2	1	0	3	5	5	2	2	4	0	0	0	0	0	0
1	11	0	24	1	3	0	2	5	5	0	7	4	0	1	0	0	0	0
8	3	9	12	1	2	3	2	4	2	0	6	1	0	1	1	1	0	0
1	3	0	9	2	2	2	2	0	2	0	3	4	0	1	1	1	1	1
1	5	0	27	1	3	2	1	5	5	0	11	0	0	1	1	0	0	0
8	3	0	20	1	1	1	1	1	2	0	15	1	0	1	1	1	0	1
1	6	2	8	2	2	2	3	5	1	0	4	1	0	1	0	0	0	0
1	5	11	24	1	3	3	2	2	5	0	5	1	0	1	0	1	0	1
1	1	0	23	1	4	3	1	5	5	0	10	4	0	1	1	1	0	1
8	3	0	21	1	4	3	1	5	2	0	4	1	0	1	1	0	0	0
8	6	0	11	2	5	2	2	4	1	2	16	4	3	0	0	0	0	0
1	6	10	15	2	1	1	3	2	1	2	16	4	3	0	0	0	0	0
1	3	0	10	2	4	2	2	5	5	0	1	1	0	1	1	0	0	1
8	5	2	8	2	3	1	2	5	3	0	6	1	1	1	0	0	0	0
1	11	10	31	0	4	3	2	2	5	0	7	1	0	1	1	1	0	0
8	3	0	19	1	1	2	1	4	2	2	16	4	3	0	0	0	0	0
8	6	2	4	2	4	1	2	4	1	2	2	3	1	0	0	0	0	0
8	2	9	6	1	2	0	2	1	5	2	16	4	0	1	0	1	0	0
1	5	0	12	2	2	3	2	2	3	0	2	1	0	1	1	1	0	1
8	3	0	16	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
1	0	0	8	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
1	7	0	18	2	3	1	2	5	4	0	7	2	0	1	1	0	0	1
8	3	0	12	2	2	1	2	1	2	2	16	4	3	0	0	0	0	0
8	5	0	15	2	4	3	2	4	3	0	9	4	0	1	1	1	0	1
8	3	2	5	1	2	0	1	1	5	0	7	1	1	0	1	0	0	0
1	3	0	17	2	4	3	2	5	2	0	15	1	0	1	1	0	0	1
8	6	5	5	2	1	3	3	4	1	0	7	2	0	1	1	0	0	0
8	5	12	9	2	3	3	3	1	3	0	3	1	0	1	1	0	0	1
1	3	5	22	1	1	4	2	5	2	0	3	1	0	1	1	0	0	1
8	11	10	14	2	1	1	3	4	5	2	16	4	3	0	0	0	0	0
1	3	14	1	2	5	3	2	5	2	2	16	4	3	0	0	0	0	0
0	3	9	9	1	1	3	2	4	2	2	16	0	2	1	0	0	0	0
8	6	2	9	2	2	1	3	5	1	2	16	4	3	0	0	0	0	0
8	0	0	9	2	2	6	2	1	5	2	16	4	3	0	0	0	0	0
1	3	13	5	1	6	3	2	5	2	0	9	1	1	1	1	0	0	0
8	3	9	5	2	4	2	3	4	2	2	16	4	3	0	0	0	0	0
8	3	14	2	1	3	6	2	1	2	0	8	1	1	0	0	0	0	0
1	6	0	12	2	2	3	2	2	1	0	3	1	0	1	1	1	0	1
8	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	5	2	8	1	3	3	2	2	5	0	5	1	0	1	1	1	1	1
1	11	3	30	1	3	0	3	5	5	0	4	1	1	0	1	1	0	0
1	3	9	4	1	3	1	2	5	5	0	8	1	1	1	0	0	0	0
8	6	2	12	2	1	1	3	5	1	0	11	1	0	1	1	1	0	0
1	9	2	6	1	2	1	1	5	5	2	16	4	3	0	0	0	0	0
8	7	14	4	2	1	7	2	5	4	0	12	1	1	1	1	0	1	1
8	11	2	3	2	2	5	3	4	5	2	16	4	3	0	0	0	0	0
8	2	5	5	2	2	0	2	4	5	0	10	0	0	1	1	0	1	1
1	7	0	8	2	2	0	2	5	4	0	3	0	1	1	1	1	0	1
1	7	11	10	1	3	5	2	2	5	2	16	4	1	0	0	1	0	0
0	6	2	8	2	5	3	2	5	1	0	9	1	0	1	1	1	1	1
8	7	9	21	1	6	3	3	1	4	0	9	1	1	1	0	1	1	0
8	5	0	9	2	3	2	2	5	3	2	16	4	3	0	0	0	0	0
8	3	11	11	2	1	3	2	5	2	0	8	4	1	1	1	1	0	1
8	3	0	16	2	3	3	2	4	2	0	3	1	0	1	1	0	0	1
1	7	2	6	1	1	1	2	0	4	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	0	2	3	0	0	7	4	0	0	1	0	0	0
1	3	0	13	2	2	2	2	0	2	0	6	1	0	1	1	0	0	1
8	11	9	22	1	2	1	2	5	5	2	16	4	3	0	0	0	0	0
8	3	6	10	2	4	3	2	4	2	0	6	0	0	1	1	0	1	1
8	7	0	20	1	3	3	1	1	4	2	16	4	3	0	0	0	0	0
8	3	0	16	2	3	2	2	1	2	0	7	1	0	1	1	0	1	1
8	5	0	9	2	3	3	2	4	3	0	6	1	0	1	1	0	0	1
1	3	0	10	2	5	2	2	2	2	0	15	1	0	1	1	1	0	1
8	6	13	14	1	1	1	3	4	1	0	5	0	0	1	1	0	0	1
8	6	0	12	2	2	1	2	5	1	0	4	0	0	0	0	0	0	0
8	7	12	5	2	3	2	2	5	4	2	16	4	3	0	0	0	0	0
8	3	2	9	2	3	1	3	4	2	2	1	4	0	0	0	0	0	0
1	3	0	10	2	3	2	2	0	2	0	13	1	0	1	1	1	0	1
8	6	9	2	2	3	3	2	1	1	0	15	1	0	1	1	0	0	1
8	8	0	22	1	2	3	1	1	5	0	1	1	1	0	0	0	0	0
1	0	2	5	2	2	0	2	2	5	0	7	0	1	0	0	0	0	0
8	11	2	6	2	1	1	2	4	5	0	15	4	2	1	0	1	0	0
1	1	0	25	1	4	3	1	5	5	0	15	1	0	1	1	0	0	1
8	9	2	6	1	1	6	2	1	5	0	15	1	0	1	1	1	0	0
1	7	0	31	0	4	3	1	2	5	0	15	1	0	1	1	0	0	1
1	5	0	9	2	2	2	2	5	3	0	10	2	0	1	1	0	0	1
8	2	12	8	2	2	0	3	4	5	2	16	4	3	0	0	0	0	0
8	2	5	12	2	1	0	2	4	5	2	16	4	3	0	0	0	0	0
1	5	0	25	1	4	3	1	5	5	0	4	4	0	1	0	0	0	0
8	3	10	19	1	5	3	2	4	2	0	12	1	1	1	1	0	0	1
8	3	0	13	2	4	3	2	4	2	0	8	2	0	1	1	1	1	1
8	3	5	12	1	2	5	2	5	2	0	13	0	0	1	1	0	1	1
1	5	1	2	1	3	1	2	5	3	2	16	3	0	0	0	1	0	0
8	3	0	15	2	2	3	2	4	2	0	6	1	0	1	1	0	1	1
1	0	0	8	2	2	6	2	5	0	2	1	4	0	0	0	0	0	0
0	7	0	9	2	2	2	2	5	4	0	1	1	0	1	1	0	0	1
1	5	0	11	2	3	2	2	2	3	0	14	1	0	1	1	1	1	1
8	3	0	20	1	4	3	1	4	2	0	5	4	1	1	1	0	0	1
8	7	2	16	1	4	7	3	1	4	2	16	4	3	0	0	0	0	0
8	11	14	14	2	1	1	3	5	5	2	4	3	0	1	0	0	0	0
1	7	0	22	1	4	3	1	0	4	0	15	1	0	1	1	1	1	1
1	7	0	8	2	3	2	2	5	4	2	16	4	3	0	0	0	0	0
8	3	2	7	2	7	2	3	1	2	0	4	1	0	1	1	1	0	1
8	3	0	12	2	2	3	2	4	2	0	6	1	0	1	1	1	0	1
1	3	0	12	2	4	5	2	2	2	0	13	1	0	1	1	0	0	1
8	6	2	18	1	2	3	2	5	1	0	8	1	0	1	1	1	0	0
8	5	9	5	2	3	3	2	1	3	2	5	4	1	0	0	0	0	0
1	3	0	10	2	1	2	2	0	2	2	16	4	3	0	0	0	0	0
1	3	0	12	2	2	3	2	0	2	0	4	1	0	1	1	1	0	1
1	7	3	5	2	1	2	2	0	4	2	16	4	3	0	0	0	0	0
8	5	2	28	1	3	3	3	1	5	0	15	1	0	1	1	1	0	1
8	5	11	2	1	2	3	2	4	3	0	3	1	0	1	1	1	1	1
8	5	0	19	1	4	1	1	4	3	2	16	4	3	0	0	0	0	0
1	5	0	21	1	3	2	1	5	3	0	7	1	0	1	1	1	1	0
1	11	14	2	2	1	6	2	2	5	2	2	0	0	1	1	0	0	1
1	3	0	12	2	7	3	2	0	2	0	4	0	1	1	1	1	0	1
1	3	0	11	2	6	3	2	2	2	0	15	1	0	1	1	0	1	1
8	6	0	12	2	2	1	2	4	1	2	16	4	3	0	0	0	0	0
8	7	0	29	1	9	7	1	1	5	2	16	4	3	0	0	0	0	0
8	3	3	12	2	2	3	2	5	2	0	5	1	1	1	1	1	0	1
1	3	0	12	2	3	3	2	0	2	2	16	4	3	0	0	0	0	0
1	7	0	14	2	3	3	2	5	4	0	5	1	0	1	1	1	1	0
3	7	0	16	2	1	1	2	2	4	0	16	0	1	0	1	0	0	1
8	11	2	4	2	2	1	2	5	5	2	15	0	0	0	1	0	0	0
1	7	0	8	2	2	2	2	5	4	0	3	1	0	1	1	1	0	1
8	5	2	8	1	1	3	2	1	5	0	5	0	0	1	0	0	0	0
8	5	0	19	2	2	0	2	1	3	1	2	3	0	1	1	0	0	1
8	5	0	11	2	2	2	2	4	3	2	2	4	1	1	1	0	1	1
8	7	2	12	1	2	1	1	1	5	2	16	4	3	0	0	0	0	0
8	5	1	6	1	1	3	2	5	5	0	3	2	0	1	0	1	0	0
1	7	0	25	1	2	3	1	5	5	0	4	1	0	1	1	1	0	1
1	6	2	9	1	2	3	1	5	5	2	5	2	0	0	0	0	0	0
1	5	6	1	2	3	2	2	5	3	0	2	1	0	1	0	1	0	0
8	3	0	11	2	2	2	2	4	2	0	3	1	0	1	1	1	0	1
8	0	0	9	2	3	6	2	3	0	2	1	4	0	0	0	0	0	0
1	6	0	13	2	3	6	2	2	1	0	1	1	0	1	1	0	1	1
1	6	3	5	2	4	6	2	2	1	0	15	1	0	0	1	0	0	0
1	3	0	13	2	3	3	2	0	2	0	5	4	0	1	1	1	0	1
8	3	0	21	1	3	3	1	1	2	0	2	1	1	1	1	1	0	1
8	9	0	12	2	5	3	2	4	5	0	11	1	0	1	1	1	0	1
1	5	0	26	1	1	2	1	5	5	0	7	0	0	1	1	1	1	1
8	7	0	31	0	5	1	0	1	5	0	2	0	1	0	0	0	0	0
1	6	2	27	1	4	3	3	2	5	0	12	1	0	1	1	1	0	1
8	3	6	1	2	1	2	2	5	2	2	1	4	1	0	0	0	0	0
1	3	6	2	1	3	3	1	5	2	0	9	0	1	1	1	1	0	1
8	7	10	20	1	2	3	2	1	4	0	3	4	0	1	1	0	0	0
1	7	2	21	1	3	6	2	0	5	0	13	0	0	1	1	0	0	1
1	3	0	14	2	2	3	2	5	2	0	1	4	1	0	0	0	0	0
8	7	5	10	1	1	0	2	1	4	2	16	4	3	0	0	0	0	0
8	6	0	14	2	3	3	2	1	1	0	15	1	0	1	1	1	0	1
8	1	0	24	1	5	3	1	1	5	0	1	0	1	1	0	0	0	0
8	7	2	7	1	2	1	2	1	4	0	4	4	1	1	1	1	0	0
8	7	0	16	2	2	3	2	1	5	2	2	1	0	1	0	0	0	0
7	7	7	6	2	1	2	3	4	4	2	2	1	1	1	1	0	0	1
8	5	2	9	1	1	5	2	1	5	2	16	4	3	0	0	0	0	0
6	3	0	13	2	2	3	2	4	2	0	10	0	0	1	1	1	0	1
8	7	0	11	2	5	2	2	1	4	0	9	2	0	1	1	0	1	0
1	8	0	18	2	2	3	2	5	5	2	0	0	0	0	0	0	0	0
1	3	0	9	2	3	3	2	5	2	0	6	1	0	1	0	1	0	0
1	3	0	11	2	2	2	2	0	2	0	3	0	0	1	1	0	1	1
8	8	0	9	2	1	2	2	4	5	0	1	0	0	1	1	0	0	1
1	5	0	29	1	2	3	1	5	5	0	15	0	0	1	1	1	1	0
1	6	0	17	2	4	3	2	2	1	0	15	1	0	1	1	1	0	1
1	7	14	17	2	1	1	2	2	5	0	6	2	0	1	1	0	1	0
8	6	9	20	1	4	2	3	1	1	0	15	1	0	1	0	0	0	0
1	0	9	8	2	2	6	3	5	0	0	6	0	0	0	0	0	0	0
1	6	0	8	2	2	0	2	5	1	2	16	4	3	0	0	0	0	0
1	7	2	10	0	4	2	1	2	5	0	4	0	0	0	0	0	0	0
8	3	0	19	1	3	3	1	4	2	0	6	4	0	1	1	0	0	1
1	5	0	12	2	6	1	2	0	5	0	8	0	0	1	1	0	0	1
8	0	14	9	2	2	6	3	1	5	0	5	2	0	1	1	0	0	0
1	7	12	20	1	3	3	3	2	4	2	2	4	0	0	0	0	0	0
8	7	0	28	1	8	4	1	1	5	2	16	4	3	0	0	0	0	0
1	0	14	10	2	2	0	2	5	0	2	16	4	3	0	0	0	0	0
1	0	0	8	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
8	3	0	20	1	3	3	1	4	2	0	5	3	0	1	1	0	0	1
1	5	2	11	1	4	3	2	5	5	0	6	4	1	1	1	1	0	0
8	6	1	17	2	1	1	2	4	1	0	15	1	0	1	1	0	0	1
8	6	2	6	1	2	1	2	1	1	2	16	4	3	0	0	0	0	0
8	7	5	8	1	3	3	2	5	4	2	16	4	3	0	0	0	0	0
1	6	4	6	1	1	1	2	5	1	2	16	4	3	0	0	0	0	0
8	5	0	10	2	3	2	2	4	3	0	9	4	0	1	1	0	0	0
8	5	2	22	1	4	1	2	1	5	0	12	0	0	1	0	0	0	0
8	5	0	15	2	4	3	2	1	3	1	4	0	0	1	1	0	0	0
1	3	7	3	2	2	2	2	5	2	0	3	2	0	1	1	0	0	1
8	5	12	13	2	3	3	3	5	3	0	6	0	0	1	1	0	0	1
8	0	9	9	2	2	6	3	3	0	0	16	4	0	1	1	0	0	0
1	5	14	1	2	4	3	2	5	3	0	15	1	0	1	1	1	0	1
1	3	9	4	2	4	3	2	0	2	0	15	1	0	1	1	1	0	0
6	3	6	20	1	2	3	3	4	2	2	16	4	3	0	0	0	0	0
1	11	4	1	2	1	7	2	2	5	2	3	4	1	1	1	0	0	1
8	3	0	12	2	2	3	2	4	2	2	0	4	0	0	0	0	0	0
8	0	9	9	2	1	0	3	3	0	2	2	4	1	1	0	0	0	0
8	3	0	8	2	1	3	2	4	2	0	4	0	0	1	1	0	0	1
1	3	5	14	2	2	3	3	2	2	0	2	1	1	1	1	0	0	1
1	3	9	16	2	1	0	3	5	2	0	1	4	0	1	1	0	0	1
8	3	0	9	2	3	3	2	4	2	0	1	1	1	1	1	1	0	1
8	11	6	1	1	3	3	1	1	5	0	5	1	0	1	0	0	0	0
8	3	2	7	2	1	1	2	5	2	2	16	4	3	0	0	0	0	0
1	3	0	10	2	6	2	2	0	2	1	0	0	1	1	1	0	0	0
1	4	2	6	1	1	0	1	2	5	2	16	4	0	0	0	0	0	0
8	11	14	18	2	4	1	2	5	5	0	12	3	2	1	1	0	0	1
8	3	0	11	2	1	3	2	4	2	2	16	4	1	0	0	0	0	0
1	0	0	9	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
8	6	13	14	2	2	3	3	4	1	0	4	4	0	1	0	0	0	1
8	6	13	15	1	3	3	2	1	1	0	7	1	0	1	1	0	0	1
8	5	0	17	2	1	3	2	4	3	2	16	4	0	1	1	0	0	1
1	6	3	13	1	2	3	3	5	1	2	16	4	3	0	0	0	0	0
1	0	5	6	2	3	0	3	0	5	0	4	1	0	1	1	0	0	1
1	3	0	20	2	1	3	2	2	2	2	16	4	0	0	0	0	0	0
8	6	4	7	2	5	1	2	4	1	2	16	4	3	0	0	0	0	0
1	7	2	10	1	2	3	2	5	4	0	3	1	0	1	1	1	1	1
8	6	2	7	2	1	1	2	1	1	0	8	3	1	1	0	0	0	0
1	7	0	16	2	3	3	2	5	4	0	5	0	1	1	0	0	0	0
1	3	0	26	1	5	3	1	0	2	0	15	1	0	1	1	1	0	1
1	7	0	30	1	3	3	1	0	5	0	14	1	0	0	1	0	0	1
1	3	0	22	1	3	3	1	5	2	0	11	1	0	1	1	0	0	1
8	11	2	9	1	2	2	2	1	5	2	3	0	0	1	0	0	0	0
8	6	0	13	2	5	3	2	5	1	0	15	1	0	1	1	0	0	1
8	3	2	15	1	4	0	2	1	2	0	1	0	0	0	1	0	0	0
1	11	2	5	2	3	3	2	2	5	2	16	4	3	0	0	0	0	0
1	6	9	10	2	1	1	3	0	1	0	4	0	1	1	1	0	0	1
1	5	6	20	1	5	3	3	2	5	0	12	1	0	1	1	1	0	1
1	6	11	15	2	3	3	2	2	1	0	15	0	0	1	1	0	0	1
1	3	0	8	2	1	3	2	0	2	2	16	0	1	1	1	0	1	1
8	8	0	13	2	3	3	3	1	5	0	3	4	0	1	1	0	0	1
8	6	0	20	1	2	3	1	5	1	0	2	4	1	1	1	1	1	1
8	3	14	11	2	1	1	2	4	2	0	8	1	0	1	1	1	0	1
1	0	14	1	2	2	0	2	0	5	2	2	0	1	0	0	0	0	0
8	3	0	10	2	2	3	2	4	2	0	4	1	0	1	1	1	1	0
8	0	0	8	2	3	6	2	5	0	2	2	1	1	0	0	0	0	0
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	0	15	2	2	3	2	4	2	0	3	4	1	1	1	0	1	1
8	3	9	13	2	1	3	3	4	2	2	16	4	3	0	0	0	0	0
1	5	0	8	2	2	5	2	5	5	0	3	1	0	1	0	0	0	0
8	9	2	5	2	1	3	2	1	5	2	16	4	3	0	0	0	0	0
1	0	0	9	2	2	6	2	5	0	0	5	3	1	1	1	0	0	1
8	3	9	16	2	1	1	3	1	2	0	15	1	0	1	1	1	0	1
1	5	7	12	1	3	3	2	1	3	0	1	1	0	1	1	0	1	1
1	3	0	21	1	4	3	1	5	2	0	10	1	0	1	1	1	0	1
1	6	0	13	2	1	1	2	0	1	2	16	4	0	0	0	0	0	0
8	7	0	31	1	2	3	1	5	5	0	2	0	0	0	1	0	0	1
8	3	0	17	2	2	3	2	1	2	0	3	1	0	1	1	0	0	1
1	6	14	16	2	3	1	2	5	1	2	1	4	1	0	0	0	0	0
8	7	2	26	1	5	3	1	1	5	1	6	1	0	1	1	1	0	1
1	0	0	5	3	1	0	3	5	0	2	16	4	3	0	0	0	0	0
8	3	14	20	1	5	3	2	4	2	0	10	1	0	1	1	1	1	1
8	5	3	5	1	1	1	2	1	5	0	15	1	0	1	1	0	0	0
8	3	10	16	2	3	1	2	5	2	0	8	1	0	1	0	0	0	0
1	5	0	27	1	3	3	1	0	3	0	3	2	0	1	1	0	1	1
1	7	0	31	1	4	3	1	0	5	2	1	3	0	0	0	0	0	0
8	3	0	19	1	4	3	1	4	5	0	15	1	0	1	1	0	0	1
1	0	0	8	2	1	0	2	5	0	2	1	0	0	1	1	0	0	1
1	7	0	31	1	8	6	1	5	5	2	16	4	3	0	0	0	0	0
1	5	0	13	2	3	3	2	0	3	0	9	0	1	1	1	1	0	1
1	6	0	11	2	2	2	2	0	1	0	2	1	0	1	1	0	1	1
8	6	5	4	2	1	3	2	5	1	0	8	4	1	1	1	1	0	0
8	2	14	1	2	3	0	2	1	5	2	16	4	3	0	0	0	0	0
8	3	0	10	2	3	3	2	4	2	0	6	1	0	1	1	1	0	1
1	0	0	8	2	2	6	2	5	0	0	6	2	0	1	1	0	0	0
1	7	2	10	2	2	1	3	2	4	0	15	4	0	1	1	1	0	0
8	3	0	8	2	2	3	2	4	2	0	4	1	0	1	0	0	0	0
1	5	0	25	1	4	3	1	0	5	0	5	1	0	1	1	1	0	1
1	0	0	9	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
1	6	6	2	1	6	5	1	5	1	0	12	1	0	1	1	0	1	1
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	7	6	20	1	6	3	1	0	4	0	8	4	1	1	0	0	1	0
8	6	12	21	1	2	1	2	5	1	2	16	4	3	0	0	0	0	0
1	5	4	24	1	2	3	1	2	5	2	16	4	3	0	0	0	0	0
1	0	0	10	2	3	6	2	5	0	0	7	1	0	1	1	0	0	0
8	7	9	1	2	3	3	2	4	4	2	2	3	0	0	0	0	0	0
1	5	2	17	1	4	3	2	5	5	2	16	4	3	0	0	0	0	0
1	7	0	9	2	1	2	2	5	4	2	16	4	3	0	0	0	0	0
1	6	4	8	2	1	3	2	5	1	0	3	4	1	1	1	0	0	1
8	0	9	8	2	2	6	3	5	0	0	4	4	0	1	1	0	0	0
8	0	0	8	2	6	6	2	3	0	2	3	0	0	0	0	0	0	0
8	3	2	8	2	2	1	2	5	2	0	4	1	1	1	0	0	0	0
1	7	2	8	2	2	1	3	2	4	0	4	0	1	0	1	0	0	0
8	0	0	9	2	3	6	2	3	0	2	2	3	0	1	0	0	0	0
8	11	13	4	2	3	6	2	5	5	2	16	4	3	0	0	0	0	0
1	7	6	12	1	4	7	2	2	5	2	16	4	3	0	0	0	0	0
1	11	14	3	2	2	1	2	2	5	2	16	4	3	0	0	0	0	0
8	6	0	12	2	3	3	2	4	1	2	3	3	0	1	1	1	0	0
8	3	2	18	1	3	2	3	1	2	2	6	2	0	1	1	1	0	1
8	3	0	16	2	2	3	2	4	2	0	6	1	0	1	0	0	0	0
8	3	0	22	1	4	3	3	5	2	0	8	1	0	0	0	0	0	0
1	3	3	8	1	5	3	1	5	5	0	11	1	0	1	1	0	0	0
1	7	14	1	1	3	2	1	2	5	0	4	4	0	1	1	1	0	1
8	3	4	14	2	5	3	2	4	2	0	2	0	0	1	1	0	1	0
1	0	0	8	2	3	6	2	5	0	0	5	1	0	1	1	1	0	1
1	7	9	9	1	2	3	1	0	5	0	4	1	0	1	1	1	0	0
1	7	0	17	2	2	2	2	0	4	0	12	2	0	1	1	0	1	1
1	3	0	20	1	4	2	1	0	2	0	15	1	0	1	1	1	0	1
1	1	0	24	1	1	3	1	2	5	0	8	0	0	1	1	1	1	1
8	3	0	16	2	3	3	2	4	2	0	9	1	0	1	1	0	0	1
1	1	6	2	1	5	3	1	0	5	0	11	4	0	1	1	0	0	0
8	3	0	9	2	3	3	2	1	2	0	11	1	1	1	1	1	0	0
1	6	0	20	1	2	3	1	2	1	0	3	2	0	1	1	1	0	1
1	7	2	9	1	3	1	2	2	5	2	1	1	1	0	0	0	0	0
1	2	2	6	2	4	0	2	0	5	2	16	4	3	0	0	0	0	0
8	3	0	19	1	1	1	1	4	2	2	16	4	1	1	1	0	0	1
8	5	0	20	1	1	3	1	4	3	2	1	3	1	1	1	1	1	1
8	3	0	10	2	2	3	2	4	5	0	4	1	0	1	0	0	0	0
1	9	2	5	1	1	3	2	5	5	0	14	0	0	1	1	0	1	1
8	8	0	16	2	2	3	3	4	5	2	16	4	3	0	0	0	0	0
8	5	0	26	1	3	3	1	1	5	0	5	1	1	1	1	0	0	0
8	5	9	9	1	2	5	2	5	5	0	4	0	0	1	1	0	0	1
8	5	0	10	2	2	3	2	1	3	0	16	4	1	0	0	0	0	0
1	7	0	16	2	1	2	2	5	4	0	13	2	0	1	1	0	0	1
8	1	0	23	1	2	3	1	5	5	0	1	4	0	0	0	0	0	0
8	7	2	9	2	1	0	3	4	4	0	15	4	0	1	1	1	1	1
8	6	2	5	2	4	1	2	4	1	2	5	0	0	1	0	0	0	0
8	0	0	9	2	2	0	2	3	0	1	3	0	1	1	0	0	0	0
1	0	0	9	2	2	6	2	5	0	0	4	0	1	1	1	1	0	1
1	7	3	15	1	5	2	2	2	5	0	15	1	0	1	1	0	0	0
1	7	2	8	1	4	3	2	0	4	2	16	4	3	0	0	0	0	0
8	5	2	6	1	2	6	2	1	5	0	8	0	0	1	1	0	0	1
1	7	2	11	1	3	1	1	2	5	0	3	4	1	1	1	0	0	0
8	3	0	10	2	6	3	2	4	2	0	5	4	0	1	1	1	0	0
1	5	2	6	2	2	1	3	5	3	2	16	4	3	0	0	0	0	0
8	3	0	19	1	5	7	1	4	2	2	16	4	2	0	0	0	0	0
8	5	0	17	2	3	3	2	4	3	0	6	0	0	1	1	0	1	1
8	6	0	10	2	2	3	2	4	1	0	3	1	0	1	1	1	1	1
8	7	0	30	1	2	3	1	1	5	0	12	2	0	1	1	1	0	1
8	5	5	4	2	3	3	3	5	3	2	0	4	0	0	0	0	0	0
1	6	6	7	1	2	3	2	2	1	0	3	1	0	1	1	0	0	0
8	5	0	8	2	2	2	2	5	3	0	1	1	0	1	0	0	0	0
3	11	2	16	2	2	2	2	2	5	2	16	0	0	1	1	0	0	1
8	5	2	15	1	3	3	2	1	5	0	4	4	1	1	1	0	0	0
1	7	14	14	1	8	6	2	5	5	2	16	4	3	0	0	0	0	0
1	3	0	19	1	2	2	1	5	2	0	12	1	0	1	1	0	0	1
8	3	5	2	2	1	3	2	4	2	2	1	0	1	1	1	0	0	1
8	3	0	13	2	4	0	2	4	5	2	2	1	0	0	0	0	0	0
8	11	6	16	2	2	7	3	4	5	0	5	0	0	1	1	1	1	1
1	3	0	11	2	1	3	2	0	2	2	16	0	1	1	1	0	0	1
1	6	2	3	2	3	3	2	2	1	0	6	2	0	1	1	1	0	0
8	3	5	14	1	2	3	2	4	2	1	0	1	1	0	0	0	0	0
1	0	14	10	2	1	0	3	5	0	2	16	4	3	0	0	0	0	0
8	3	0	17	2	3	3	2	4	2	0	6	2	0	1	1	1	1	1
1	7	9	5	1	2	3	1	0	5	0	4	1	0	1	1	1	0	1
8	3	0	19	1	1	3	1	4	2	2	16	4	3	0	0	0	0	0
1	3	0	9	2	2	2	2	0	2	0	11	1	0	0	0	0	0	0
1	5	2	6	2	3	3	2	2	3	0	13	1	0	1	1	0	0	0
1	6	2	8	2	1	1	2	2	1	1	1	0	1	1	1	0	0	0
1	6	2	13	1	3	1	2	5	5	2	3	2	0	1	0	0	0	0
1	3	2	11	2	6	1	3	5	2	0	7	2	0	0	1	0	0	0
8	3	8	17	2	4	3	2	5	2	0	6	0	0	1	1	0	0	0
1	5	7	8	2	2	1	3	5	3	0	15	2	0	1	1	1	0	0
1	6	9	8	1	4	3	1	2	5	0	15	0	0	1	1	1	0	0
1	11	3	12	1	4	3	1	5	5	0	15	1	0	1	1	0	0	1
8	3	2	8	2	1	3	2	5	2	2	1	4	1	0	0	0	0	0
1	3	0	12	2	4	3	2	0	2	0	9	1	0	1	1	0	0	0
8	13	0	23	1	2	1	1	1	5	2	16	4	3	0	0	0	0	0
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	7	0	14	2	3	3	2	2	4	0	11	1	0	1	1	1	0	1
1	3	0	20	1	4	3	1	5	2	2	16	4	3	0	0	0	0	0
1	0	0	9	2	1	6	2	5	0	0	7	0	0	1	1	0	0	0
8	11	4	2	2	2	2	3	4	5	2	16	4	3	0	0	0	0	0
8	3	3	1	2	3	3	2	5	2	2	16	4	3	0	0	0	0	0
1	3	0	13	2	5	5	2	2	2	0	16	0	1	1	0	0	0	0
8	3	3	13	2	1	1	3	5	2	2	16	4	3	0	0	0	0	0
1	7	2	4	2	2	3	2	2	4	2	14	0	0	1	1	1	0	1
8	5	5	8	2	3	2	3	4	3	2	16	4	3	0	0	0	0	0
1	13	0	8	2	4	2	2	4	5	0	6	4	1	1	1	1	0	1
8	0	0	9	2	4	6	2	3	0	0	7	1	0	1	1	0	0	1
1	9	2	10	2	2	3	3	5	5	0	9	0	0	1	1	0	0	1
8	3	6	4	2	5	6	2	1	2	2	16	4	3	0	0	0	0	0
1	0	1	2	1	2	0	1	5	5	2	4	0	0	0	1	1	0	1
1	5	3	4	1	1	6	2	2	3	0	7	0	0	1	1	1	0	1
1	3	0	17	2	4	3	2	5	2	0	8	1	0	1	1	0	0	1
1	0	0	9	2	1	6	2	5	0	0	6	0	0	1	1	0	0	1
1	8	14	2	1	1	1	1	2	5	2	16	4	3	0	0	0	0	0
8	6	3	7	1	2	1	2	5	1	0	3	1	1	1	0	0	0	0
8	0	0	9	2	1	6	2	5	0	2	3	4	0	0	0	0	0	0
1	6	3	3	1	7	2	2	0	1	0	15	1	0	1	1	1	0	1
8	5	0	10	2	2	3	2	4	3	0	4	1	0	1	1	0	0	1
1	11	0	29	1	8	7	1	5	5	2	16	4	3	0	0	0	0	0
1	7	9	12	1	1	3	1	0	5	2	16	4	3	0	0	0	0	0
8	6	0	9	2	2	2	2	4	1	2	16	4	3	0	0	0	0	0
1	6	5	5	2	5	6	2	5	1	0	8	0	0	1	1	1	0	1
8	7	2	5	2	3	2	2	1	4	0	7	4	0	0	0	0	0	0
1	7	0	31	1	3	2	1	2	5	0	8	1	0	1	1	1	1	1
8	7	0	20	1	2	1	1	5	4	0	4	4	0	1	1	0	0	1
8	3	0	12	2	4	3	2	1	2	0	4	1	0	1	1	0	0	1
8	0	0	9	2	1	0	2	5	0	2	9	4	0	0	0	0	0	0
8	8	0	12	2	1	3	2	5	5	2	1	3	1	0	0	0	0	0
8	1	0	23	1	2	3	1	1	5	0	3	1	0	1	1	1	0	0
1	5	0	13	2	3	3	3	5	3	0	8	0	1	1	1	0	0	0
1	3	2	10	2	3	1	3	0	2	2	7	0	1	0	0	0	0	0
8	2	14	2	1	2	6	1	1	5	2	16	4	3	0	0	0	0	0
8	7	0	14	2	2	3	2	1	5	2	16	4	3	0	0	0	0	0
1	6	0	13	2	1	2	2	0	1	0	15	1	0	1	1	0	0	1
1	3	3	3	2	1	2	2	2	2	2	16	4	3	0	0	0	0	0
8	3	0	19	1	1	1	2	4	2	2	16	4	3	0	0	0	0	0
1	3	0	22	1	4	2	1	5	2	0	8	1	0	1	1	0	0	0
8	7	0	11	2	1	3	2	5	4	0	2	1	0	1	0	1	0	0
1	7	0	11	2	2	2	2	5	4	0	6	1	0	1	1	1	0	1
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	5	0	17	2	4	3	2	5	3	0	8	0	0	1	1	0	0	1
8	3	11	17	2	2	1	3	1	2	2	16	4	3	0	0	0	0	0
8	7	0	14	2	2	3	2	4	4	2	16	4	3	0	0	0	0	0
1	7	0	17	2	1	3	2	5	4	0	6	1	1	1	1	0	1	1
1	5	0	12	2	2	3	2	0	3	0	2	3	0	1	1	0	1	1
8	6	0	21	1	3	1	1	1	1	0	5	1	0	1	1	1	0	0
8	9	0	13	2	2	6	2	5	5	1	5	0	0	0	0	0	0	0
8	0	0	9	2	1	0	2	3	0	2	16	4	3	0	0	0	0	0
1	3	9	9	2	1	1	2	2	2	2	2	4	0	0	0	0	0	0
1	5	3	9	1	2	3	2	2	3	0	4	2	0	1	1	0	1	0
1	3	0	15	2	3	3	2	0	2	0	10	1	0	1	1	1	0	1
8	5	0	17	2	3	2	2	1	5	0	9	1	0	1	1	1	1	0
1	3	0	15	2	3	2	2	5	2	2	16	4	3	0	0	0	0	0
8	11	4	4	2	2	1	2	4	5	2	16	4	3	0	0	0	0	0
1	3	6	7	2	5	3	3	0	2	0	15	1	0	1	1	1	0	0
8	5	0	10	2	4	7	2	5	3	0	7	0	0	1	1	0	1	1
8	3	0	8	2	5	3	2	4	2	0	7	1	0	1	1	1	1	1
8	0	0	9	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
1	5	3	22	1	2	3	3	0	5	2	3	4	3	0	0	0	0	0
1	3	8	12	2	3	3	3	2	2	2	16	4	3	0	0	0	0	0
1	7	0	18	2	3	3	2	2	4	0	3	1	0	1	1	0	0	1
8	3	0	8	2	4	2	2	4	2	0	10	1	0	1	0	0	0	0
8	6	2	5	2	1	2	2	5	1	0	16	3	1	0	0	0	0	0
8	6	0	18	2	1	1	2	4	5	0	11	1	0	1	1	1	0	1
1	9	2	2	2	3	3	2	5	5	2	16	4	3	0	0	0	0	0
1	0	0	9	2	3	6	2	5	0	0	6	0	0	1	0	0	0	0
8	1	5	16	1	1	1	3	5	5	2	16	4	3	0	0	0	0	0
8	3	5	6	2	1	3	3	1	2	0	8	0	1	1	1	0	0	0
1	7	3	6	2	5	3	2	2	4	0	12	4	1	1	1	0	0	1
1	7	0	12	2	2	3	2	5	4	0	2	4	0	1	1	0	0	0
1	3	0	12	2	3	3	2	0	2	0	4	1	0	1	1	1	0	0
8	7	2	12	1	2	3	1	1	5	0	4	1	1	1	0	1	0	0
1	0	0	9	2	2	6	2	5	0	0	4	0	0	1	0	0	0	0
8	6	14	6	2	5	1	2	5	1	0	9	1	0	1	1	0	0	0
8	6	0	19	2	2	3	2	1	1	0	2	1	0	1	1	0	1	1
8	3	0	13	2	2	3	2	4	2	0	4	0	1	1	1	0	0	1
8	3	11	16	2	5	1	2	4	2	0	1	3	0	1	0	0	0	0
1	3	4	2	2	4	1	2	5	2	2	16	4	3	0	0	0	0	0
8	7	0	15	2	4	1	2	1	4	0	11	1	0	1	1	1	0	0
8	0	0	9	2	4	6	2	5	5	0	11	0	0	1	1	1	0	1
8	3	0	11	2	4	3	2	4	2	2	16	4	3	0	0	0	0	0
1	7	0	13	2	4	2	2	0	4	0	8	0	0	0	1	0	0	1
8	5	5	1	1	1	1	2	5	3	2	4	4	0	1	1	0	0	1
8	3	2	11	2	1	1	3	4	2	2	16	4	3	0	0	0	0	0
8	3	14	20	1	2	3	3	4	2	0	4	4	2	1	0	0	0	0
8	3	0	17	2	4	3	2	1	2	0	7	2	0	1	1	0	0	0
8	5	12	3	2	3	3	2	4	3	2	16	4	3	0	0	0	0	0
8	7	14	14	2	2	3	2	4	4	2	0	4	0	0	0	0	0	0
1	7	7	23	1	3	3	2	2	5	0	9	0	0	1	1	0	1	1
1	9	0	31	1	4	3	1	5	5	0	4	1	0	1	1	1	0	1
1	0	0	9	2	2	0	2	5	0	2	16	4	3	0	0	0	0	0
1	5	2	8	1	2	3	2	2	5	0	3	1	0	1	1	0	0	1
8	9	9	20	1	2	3	2	5	5	0	2	1	0	1	1	0	0	0
8	7	2	13	2	4	3	3	4	4	0	15	1	0	1	0	0	0	0
1	7	14	4	2	7	3	2	5	4	2	1	4	3	0	0	0	0	0
8	2	2	3	1	2	6	2	1	5	0	4	1	0	0	1	0	0	0
8	7	0	9	2	1	2	2	4	4	2	16	4	3	0	0	0	0	0
1	5	2	4	1	3	1	2	2	3	0	11	0	2	1	1	0	0	1
1	0	0	4	3	2	0	3	5	0	0	1	0	0	1	1	0	0	1
1	11	0	12	2	2	2	2	0	5	0	10	0	0	1	0	1	0	1
1	11	2	3	2	1	2	3	2	5	2	16	4	3	0	0	0	0	0
1	7	0	22	1	3	3	1	5	4	0	6	3	1	1	1	0	0	1
1	5	0	21	1	4	3	1	0	3	0	2	4	0	1	0	0	0	0
8	5	0	11	2	1	2	2	4	3	0	3	4	0	1	0	0	0	0
8	3	0	20	1	4	3	1	4	2	0	5	1	0	1	1	1	0	1
8	11	0	15	2	2	3	2	1	5	2	2	3	1	0	0	0	0	0
1	3	0	21	1	4	3	1	5	2	0	10	1	0	1	1	1	0	1
1	7	10	17	2	1	1	2	2	4	1	1	0	1	0	1	0	0	0
8	3	0	20	1	2	3	1	4	2	0	1	1	1	1	1	0	0	0
1	3	0	10	2	3	2	2	5	2	0	12	2	0	1	1	1	1	1
1	5	5	5	2	4	3	2	0	3	0	14	0	1	1	1	1	0	0
1	0	5	15	1	1	3	2	5	5	2	3	4	0	0	0	0	0	0
8	0	0	9	2	2	0	2	5	0	2	16	4	3	0	0	0	0	0
1	11	2	3	2	3	1	2	2	5	0	6	1	0	0	1	0	0	0
1	7	0	8	2	2	7	2	5	4	0	10	2	0	1	1	1	0	1
1	5	9	8	1	5	3	2	5	5	0	2	1	0	1	1	1	0	0
1	6	5	16	2	3	5	2	2	1	2	16	4	3	0	0	0	0	0
1	7	0	18	2	4	4	2	0	5	0	7	1	0	1	1	1	0	1
8	3	0	16	2	5	3	3	5	2	0	12	2	0	1	1	1	1	1
1	6	2	6	2	1	1	2	0	1	0	2	0	0	1	1	1	0	1
1	0	9	10	2	1	6	3	5	0	2	16	4	3	0	0	0	0	0
1	6	0	16	2	2	3	2	5	1	0	3	0	0	1	1	0	0	1
1	5	0	27	1	4	3	1	5	3	0	10	1	0	1	1	0	0	1
8	2	5	8	2	1	0	2	4	5	0	5	1	1	1	1	0	0	0
1	3	14	19	1	2	3	2	4	2	0	1	4	1	1	1	0	1	1
8	5	0	19	2	4	3	2	1	3	2	16	4	1	0	0	0	0	0
8	3	0	22	1	5	3	3	5	5	0	14	0	0	1	1	1	0	1
1	5	0	22	1	3	3	1	2	3	0	15	1	0	1	1	0	0	0
1	7	0	21	1	3	1	1	5	4	0	6	0	0	1	1	0	0	0
8	3	0	15	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
8	3	14	17	2	2	3	2	1	2	0	1	0	1	1	1	0	0	1
1	5	2	7	1	4	3	2	2	3	0	6	1	0	1	1	0	0	1
1	11	14	28	1	2	3	3	2	5	2	16	4	3	0	0	0	0	0
1	6	2	5	1	4	3	2	2	1	0	4	1	0	1	1	1	0	1
8	11	2	8	2	2	6	2	1	5	2	16	4	0	0	0	0	0	0
1	3	0	16	2	2	3	2	5	2	0	3	3	0	1	0	1	0	0
1	1	0	22	1	4	3	1	5	5	0	7	2	0	1	1	1	0	0
1	3	0	13	2	5	3	2	0	2	0	3	1	0	1	1	1	0	1
8	1	14	24	1	1	3	1	5	5	2	1	3	0	1	1	0	1	1
1	9	2	13	2	5	1	3	4	5	2	16	4	3	0	0	0	0	0
1	7	0	19	2	3	3	2	0	4	2	16	4	3	0	0	0	0	0
8	7	0	21	1	3	3	1	5	5	0	1	4	0	0	0	0	0	0
8	3	0	12	2	4	3	2	4	2	0	11	0	0	1	1	1	0	1
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	0	0	8	2	3	0	2	5	0	2	16	4	3	0	0	0	0	0
1	1	8	1	1	7	3	1	2	5	0	6	1	0	1	1	0	0	1
8	0	12	8	1	3	3	1	5	5	2	8	0	1	1	0	0	0	0
3	4	2	2	1	2	0	1	2	5	2	16	4	3	0	0	0	0	0
1	0	9	8	2	8	6	3	5	0	2	16	4	3	0	0	0	0	0
1	9	10	31	1	2	2	3	2	5	0	3	4	1	1	1	0	1	1
1	11	2	5	2	4	1	2	0	5	0	6	1	1	1	1	0	1	1
1	3	0	8	2	2	1	2	2	2	0	1	3	0	1	1	0	0	1
1	0	0	10	2	2	5	2	5	0	2	16	4	3	0	0	0	0	0
8	11	0	14	2	2	3	2	4	5	0	2	2	0	1	1	0	1	1
8	3	0	11	2	2	3	2	4	2	0	3	2	0	1	1	1	0	1
8	3	0	18	1	3	3	1	4	2	0	3	1	1	1	1	1	1	1
8	7	12	11	2	3	3	3	5	4	0	5	1	0	1	1	0	0	0
8	5	2	9	1	3	1	2	1	5	0	1	1	0	1	0	0	0	0
8	0	0	9	2	3	0	2	3	0	0	9	4	0	1	0	1	0	0
8	5	13	7	2	2	3	2	4	3	0	2	0	0	1	1	1	1	1
8	3	6	20	1	5	3	3	5	2	2	16	4	3	0	0	0	0	0
8	0	5	1	2	2	6	2	3	0	2	16	0	0	0	1	0	0	1
1	7	0	11	2	4	2	2	5	5	0	7	1	0	0	0	0	0	0
1	6	11	11	2	2	3	2	0	1	0	5	1	0	1	1	0	0	0
8	5	6	6	1	5	5	1	1	5	0	8	1	0	1	1	0	0	1
1	0	0	10	2	1	6	2	5	0	0	12	4	1	1	1	0	0	1
1	7	14	24	1	1	3	3	2	4	0	9	1	0	1	1	0	0	0
8	3	3	14	1	3	3	2	5	2	0	7	1	0	1	1	0	0	1
1	1	0	24	1	3	3	1	0	5	0	3	1	0	1	1	1	0	1
8	7	2	9	1	1	3	1	1	5	2	1	3	0	0	0	0	0	0
8	3	0	19	1	1	1	1	5	2	2	16	4	3	0	0	0	0	0
1	6	9	16	2	4	3	2	0	1	2	10	4	0	1	1	1	1	1
1	3	0	19	2	3	3	2	0	2	2	16	4	1	0	0	1	0	0
8	0	14	2	3	2	0	3	5	0	2	16	4	3	0	0	0	0	0
8	7	0	8	2	2	7	2	4	4	2	16	4	3	0	0	0	0	0
8	0	14	20	1	4	6	3	1	5	0	6	4	0	1	0	0	0	0
1	11	0	32	0	6	4	0	2	5	0	2	3	0	0	0	1	0	0
1	7	5	11	2	1	1	3	5	4	0	11	0	0	1	1	1	1	1
1	3	0	13	2	8	3	2	2	2	0	7	0	1	1	1	0	0	1
1	7	0	14	2	3	3	2	5	4	0	4	4	0	1	1	0	0	1
8	5	9	15	2	1	1	3	4	3	2	16	4	3	0	0	0	0	0
1	11	7	1	2	4	0	2	5	5	0	15	1	1	0	0	1	0	0
1	11	0	30	1	2	0	1	0	5	0	3	1	0	0	1	0	0	0
1	1	12	11	1	2	3	2	5	5	0	4	2	0	1	1	1	0	0
1	7	14	17	2	3	3	3	5	4	2	16	4	3	0	0	0	0	0
1	11	10	12	2	2	3	3	2	5	2	0	4	0	0	0	0	0	0
8	5	5	3	2	1	6	2	1	3	2	1	4	2	0	0	0	0	0
1	5	5	25	1	4	3	2	2	5	0	9	1	0	0	0	1	0	0
8	3	5	3	2	3	3	2	1	2	0	7	0	0	1	1	0	0	0
8	6	6	5	1	3	6	2	5	1	0	2	1	0	1	1	1	0	0
8	3	0	8	2	3	2	2	4	2	0	8	2	0	1	1	1	0	1
8	3	0	19	1	1	3	1	5	2	0	6	4	0	0	0	0	0	0
8	6	7	2	2	3	3	2	4	1	0	2	3	0	1	1	0	1	1
8	7	0	19	2	3	4	2	5	4	0	15	0	0	1	1	1	0	0
1	5	2	8	1	1	3	1	0	5	2	16	4	3	0	0	0	0	0
1	5	0	31	1	5	3	1	5	3	0	15	1	0	1	1	0	0	1
8	6	10	16	2	1	1	3	4	1	0	9	1	1	1	1	0	0	1
8	3	14	6	1	1	6	2	5	5	0	3	0	0	1	1	0	0	1
8	7	0	24	1	2	3	1	5	4	0	1	3	0	0	0	0	0	0
8	2	2	8	2	2	0	2	4	5	2	16	4	1	0	0	0	0	0
1	6	0	9	2	1	0	2	2	1	0	3	3	1	1	1	0	1	1
1	11	10	21	1	2	2	2	0	5	0	15	1	0	1	1	1	0	1
8	6	1	7	1	2	3	2	1	1	2	1	3	1	0	0	0	0	0
8	3	8	13	1	3	5	2	1	2	0	6	1	1	1	1	0	1	1
1	1	0	23	1	3	2	1	5	5	0	6	1	0	1	1	1	1	0
3	2	2	7	2	1	5	3	2	5	2	16	4	3	0	0	0	0	0
8	6	13	3	2	2	2	2	4	1	0	1	4	0	0	0	0	0	0
8	11	0	12	2	3	2	2	4	5	0	3	1	0	1	1	1	1	1
1	5	0	14	2	4	3	2	5	3	0	7	2	0	1	1	0	0	1
1	6	2	5	2	1	1	2	5	1	0	15	1	0	1	1	1	0	0
1	7	0	14	2	3	3	2	0	4	0	2	1	0	1	1	0	0	1
8	7	0	15	2	2	1	2	4	5	2	16	4	3	0	0	0	0	0
1	9	5	5	2	1	3	2	0	5	2	16	4	3	0	0	0	0	0
8	6	0	9	2	2	1	2	4	1	0	6	1	1	1	1	0	0	0
8	0	14	9	2	2	0	3	5	5	2	16	4	3	0	0	0	0	0
1	6	6	10	2	3	3	3	2	1	2	16	4	3	0	0	0	0	0
8	3	5	5	1	2	3	2	5	2	0	2	4	0	1	1	0	1	1
1	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	2	12	8	2	3	0	3	2	5	2	2	3	1	0	0	0	0	0
8	9	0	14	2	2	3	2	4	5	0	7	1	0	1	1	0	0	1
1	9	13	6	2	3	3	2	5	5	0	4	1	0	1	1	0	0	1
1	5	0	27	1	2	3	1	2	5	0	2	1	0	1	1	0	1	1
1	3	0	20	1	5	2	1	5	2	0	15	1	0	1	1	0	0	1
1	3	0	13	2	3	2	2	2	5	0	6	4	1	1	1	0	0	1
1	5	2	6	1	1	1	1	5	5	0	4	0	0	0	0	0	0	0
8	6	12	10	2	2	1	2	5	1	0	2	4	0	1	1	0	0	1
1	0	9	8	2	9	6	3	5	0	2	16	4	3	0	0	0	0	0
1	3	2	21	1	3	3	3	5	2	0	6	4	1	1	0	0	0	0
8	3	2	17	1	1	5	3	5	2	2	16	4	3	0	0	0	0	0
1	0	12	11	1	2	6	3	5	5	2	16	4	3	0	0	0	0	0
1	7	5	17	1	1	3	3	0	4	0	0	4	0	0	0	0	0	0
1	6	0	16	2	2	3	2	2	1	2	8	2	0	1	1	0	0	0
8	11	2	5	2	1	1	2	1	5	2	5	0	0	0	0	0	0	0
1	11	9	1	2	4	3	2	5	5	0	7	4	0	1	1	1	0	0
1	11	5	13	2	4	1	3	5	5	0	5	1	0	1	1	0	0	0
1	0	9	8	2	8	6	3	5	0	2	16	4	3	0	0	0	0	0
8	7	0	18	2	2	3	2	4	4	0	4	4	0	1	1	0	0	1
8	3	13	14	1	3	5	2	5	2	0	12	0	0	1	0	0	0	0
1	6	7	8	2	2	2	3	5	1	0	2	1	0	1	1	0	1	1
8	5	12	1	2	2	1	2	4	3	2	16	4	3	0	0	0	0	0
1	6	0	16	2	1	1	2	0	1	0	1	0	2	1	1	0	0	1
1	5	2	8	2	3	1	3	2	3	0	7	0	0	0	1	0	0	0
8	0	0	9	2	1	6	2	3	0	1	4	1	0	0	0	0	0	0
1	3	0	17	2	6	3	2	5	2	0	11	1	0	1	1	0	1	1
8	7	0	9	2	2	2	2	1	4	0	14	1	0	1	1	0	0	0
1	5	0	19	2	2	3	2	5	3	0	5	0	0	1	0	0	0	0
8	5	2	12	1	4	3	2	5	5	0	11	0	0	1	1	1	0	1
1	5	14	20	1	3	3	2	2	5	0	4	1	0	1	0	1	0	0
1	0	0	8	2	5	5	2	5	0	0	6	3	1	1	0	0	0	0
8	3	0	7	2	2	0	2	1	2	0	5	0	1	0	1	0	0	0
1	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	14	13	2	2	3	3	2	2	0	8	1	0	1	1	1	0	1
1	5	10	12	2	4	3	3	2	3	0	9	4	0	1	1	1	1	0
1	7	13	9	1	1	3	1	0	5	0	13	1	0	1	1	1	0	1
8	3	0	19	1	5	3	1	1	2	0	5	1	0	1	1	1	1	1
8	3	0	22	1	5	3	1	5	5	0	2	1	0	1	0	0	0	0
8	5	0	11	2	2	2	2	1	3	0	5	1	0	1	1	0	0	1
8	5	0	14	2	4	1	2	5	3	0	3	1	0	1	1	1	1	1
8	7	0	19	1	3	1	1	1	4	2	16	4	3	0	0	0	0	0
8	0	9	9	2	4	6	3	3	0	2	16	4	3	0	0	0	0	0
1	1	8	24	1	2	3	2	0	5	0	2	2	0	1	0	0	0	0
8	3	0	12	2	2	2	2	4	2	0	13	1	0	1	1	1	0	1
3	6	10	14	2	3	2	2	2	5	2	16	4	3	0	0	0	0	0
1	0	0	9	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
8	3	0	14	2	1	3	2	5	2	0	9	1	0	1	1	0	0	0
8	6	11	17	1	2	1	2	5	1	0	14	2	0	0	1	0	0	0
1	5	0	25	1	3	3	1	2	5	2	16	4	3	0	0	0	0	0
1	3	0	15	2	2	3	2	5	2	0	5	1	0	1	0	0	0	0
1	7	2	5	1	3	3	2	5	4	2	2	1	0	0	0	0	0	0
3	9	2	4	2	1	1	2	2	5	2	16	4	3	0	0	0	0	0
8	5	2	25	1	1	1	2	5	5	2	2	3	0	1	0	0	0	0
8	3	11	10	2	4	3	3	1	2	0	5	1	0	1	1	0	0	1
8	9	14	17	2	2	3	2	4	5	0	1	0	1	0	1	0	0	0
1	7	0	15	2	2	3	2	2	4	2	16	4	3	0	0	0	0	0
8	3	0	14	2	7	3	2	1	2	0	15	0	1	1	1	0	0	1
1	0	12	3	2	1	5	3	5	0	0	13	0	1	1	1	0	0	1
8	5	13	24	1	2	3	3	5	3	2	16	4	3	0	0	0	0	0
1	3	0	15	2	2	3	2	0	2	0	2	2	0	1	1	1	0	1
8	0	0	8	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	5	2	11	2	2	3	2	5	3	0	4	0	0	1	1	0	0	1
1	7	0	9	2	4	3	2	5	5	0	7	1	0	1	1	1	0	1
8	5	9	7	1	1	5	2	1	5	0	15	1	0	1	1	1	0	1
8	3	0	9	2	2	2	2	5	2	0	2	3	0	0	0	0	0	0
8	6	2	6	2	2	6	3	1	1	0	4	2	0	0	1	0	0	0
1	3	0	14	2	4	3	2	0	2	0	4	1	1	1	0	1	0	0
8	3	3	5	1	1	3	2	4	2	0	10	2	0	1	1	0	0	0
1	3	0	10	2	3	3	2	0	2	2	16	4	3	0	0	0	0	0
8	3	0	20	1	2	3	1	4	2	0	2	1	0	0	0	0	0	0
1	5	2	5	2	4	1	2	0	3	2	5	1	0	0	1	0	0	0
8	0	0	9	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	7	0	11	2	2	3	2	5	4	2	16	4	3	0	0	0	0	0
8	0	0	8	2	3	6	2	5	0	0	5	2	0	1	1	1	0	1
8	0	14	19	2	2	3	3	4	5	2	1	4	2	1	1	0	0	0
8	6	6	1	2	3	6	2	5	1	0	5	4	1	1	1	0	0	1
8	6	13	8	2	2	1	2	4	1	2	16	4	3	0	0	0	0	0
8	0	14	1	2	2	6	3	3	0	2	2	1	0	1	1	0	0	0
8	11	13	8	2	3	3	2	1	5	2	1	1	1	0	0	0	0	0
1	3	4	11	2	2	3	3	2	2	0	4	1	0	1	1	0	0	1
1	7	0	9	2	4	3	2	0	5	0	10	4	0	1	1	1	0	0
1	5	0	25	1	4	3	1	2	5	0	15	1	0	1	1	1	0	1
1	6	5	11	2	1	3	3	0	1	0	1	4	0	1	1	0	0	1
1	3	0	21	1	5	3	1	5	2	0	9	0	0	1	0	0	0	0
8	5	0	12	2	2	3	2	4	3	0	9	1	0	1	1	0	1	1
1	3	0	20	1	3	2	1	0	2	0	3	1	0	1	1	1	0	1
1	11	5	15	1	1	3	2	2	5	0	7	1	0	1	0	0	0	0
1	3	0	16	2	1	3	2	0	2	0	3	4	0	1	1	0	0	1
1	2	2	10	2	2	0	2	2	5	0	4	0	0	1	1	0	0	1
1	11	14	4	2	2	2	2	2	5	2	16	4	3	0	0	0	0	0
8	0	0	9	2	4	6	2	5	0	0	15	0	1	1	1	0	0	0
1	3	2	17	2	3	3	3	2	2	0	7	1	0	1	1	0	0	0
1	5	0	23	1	1	3	1	0	5	0	9	1	0	1	1	1	1	1
8	5	10	15	2	3	3	3	4	3	0	4	1	1	1	1	1	0	1
1	5	2	7	1	2	1	1	5	5	2	3	4	0	0	0	0	0	0
1	6	10	23	1	1	3	3	5	1	2	16	0	2	1	1	0	0	1
8	11	14	18	2	3	1	3	4	5	2	16	4	3	0	0	0	0	0
1	6	5	10	2	1	3	2	0	1	2	16	0	1	1	1	0	0	0
1	0	0	9	2	2	6	2	5	0	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
1	11	0	9	2	1	0	3	5	5	2	16	4	1	1	1	0	0	1
8	3	3	6	2	1	2	2	5	2	0	7	1	0	1	1	1	0	1
8	11	5	10	1	1	3	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	8	2	3	3	2	4	2	0	7	0	0	1	1	1	1	1
8	7	2	6	1	3	5	2	1	4	0	6	0	1	1	1	1	0	0
1	9	0	26	1	3	3	1	2	5	0	13	1	0	1	1	1	0	1
8	3	0	9	2	2	3	2	1	2	2	16	4	3	0	0	0	0	0
8	3	14	7	2	3	3	3	4	2	0	7	4	0	1	1	0	1	1
1	0	0	10	2	2	6	2	5	0	2	16	4	3	0	0	0	0	0
1	6	0	10	2	4	2	2	5	1	0	10	1	1	1	1	0	0	1
8	3	0	8	2	4	2	2	4	2	0	14	3	1	1	1	0	0	1
8	5	1	14	2	3	3	3	5	3	0	6	1	0	1	1	1	1	1
8	3	2	11	2	2	5	2	1	2	1	3	0	0	1	1	1	0	0
8	6	0	13	2	2	1	2	5	1	0	15	2	0	1	1	1	0	1
8	0	0	9	2	1	0	2	3	0	2	16	4	3	0	0	0	0	0
1	6	6	2	1	2	3	2	5	1	2	2	2	0	1	0	0	0	0
8	9	10	15	2	6	1	3	5	5	0	12	2	0	0	1	1	0	0
1	3	0	20	1	5	2	1	0	2	0	15	1	0	1	1	1	0	1
1	3	2	18	2	6	3	3	5	2	2	6	0	0	1	1	1	0	0
1	3	0	23	1	5	3	1	2	5	0	15	1	0	1	1	1	0	1
8	6	7	21	1	3	3	2	4	1	0	5	4	2	1	1	0	0	1
8	3	0	20	1	3	3	1	4	2	0	15	0	0	1	1	0	0	1
8	0	0	9	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	0	16	2	1	3	2	4	2	2	0	4	1	0	0	0	0	0
8	7	5	10	2	2	5	3	1	4	0	12	1	0	1	0	0	0	0
8	3	3	7	2	3	1	3	5	2	0	6	1	1	1	1	1	0	1
8	6	5	12	2	1	1	3	5	1	2	16	4	3	0	0	0	0	0
8	3	2	16	1	2	1	3	1	2	2	16	4	3	0	0	0	0	0
8	5	0	20	1	3	3	1	1	5	0	15	1	1	1	1	0	1	1
8	3	0	17	2	3	3	2	4	2	0	3	0	0	1	1	0	1	1
1	11	0	18	2	3	4	2	0	5	0	15	0	0	1	1	1	0	0
1	5	0	8	2	2	0	2	5	3	0	3	1	0	1	1	1	0	1
1	3	6	2	2	4	3	2	5	2	0	11	0	0	1	1	0	0	1
8	3	0	20	1	3	3	1	1	2	0	3	1	0	1	0	1	0	0
1	5	9	4	2	2	3	2	5	3	0	4	1	0	1	1	0	0	0
8	3	0	14	2	6	3	2	4	2	0	15	0	0	1	1	1	1	1
1	3	0	21	1	2	3	1	5	2	0	2	1	0	1	1	1	0	1
8	3	8	6	2	3	3	2	4	2	0	6	4	0	1	1	0	0	1
1	7	0	22	1	2	0	1	0	4	0	1	4	0	1	1	0	0	1
8	7	0	10	2	4	2	2	4	4	0	3	1	0	1	1	1	0	1
8	0	12	19	1	2	6	3	1	5	0	2	2	0	1	1	0	0	1
8	7	4	7	1	1	3	2	5	4	2	16	4	3	0	0	0	0	0
1	3	0	21	1	5	3	1	5	5	2	16	4	3	0	0	0	0	0
8	3	5	17	1	3	3	3	5	2	0	13	1	0	1	1	0	0	1
8	7	0	9	2	2	2	2	4	4	2	16	4	3	0	0	0	0	0
8	0	0	9	2	2	6	2	5	0	0	15	1	0	1	1	0	1	0
1	6	9	22	1	2	3	2	0	1	2	16	4	3	0	0	0	0	0
8	0	0	9	2	2	6	2	5	0	2	16	4	1	0	0	0	0	0
8	6	0	12	2	5	3	2	4	1	0	11	1	1	1	1	1	1	1
8	3	0	9	2	4	2	2	4	2	0	15	0	0	1	1	0	0	1
1	11	0	13	2	4	5	2	2	5	0	6	1	0	1	0	1	0	0
1	0	0	10	2	3	6	2	5	0	0	15	1	0	1	1	0	0	1
8	11	0	12	2	1	3	2	4	5	2	16	4	3	0	0	0	0	0
1	3	2	5	2	2	2	2	0	2	0	6	0	0	1	1	1	1	1
8	7	1	13	1	3	3	2	5	5	0	6	4	1	0	0	0	0	0
1	7	10	31	1	3	6	2	5	5	0	8	3	0	1	1	0	1	1
1	3	0	20	1	2	2	1	0	2	0	2	1	0	1	1	1	0	1
1	3	0	8	2	3	3	2	2	5	0	4	1	0	1	1	0	0	0
1	7	9	12	2	2	3	3	4	4	0	5	0	1	1	1	0	0	0
8	3	2	7	2	2	3	2	5	2	2	16	4	3	0	0	0	0	0
1	7	0	31	1	5	3	1	2	5	0	5	1	0	1	1	1	0	1
1	3	0	13	2	3	3	2	0	2	0	15	0	1	1	1	1	1	1
1	3	5	11	2	5	2	3	2	2	0	3	4	0	1	1	0	0	1
1	3	14	26	1	4	3	3	2	2	0	13	1	0	1	0	1	0	0
8	5	5	4	2	1	1	2	4	3	0	6	2	0	1	1	1	0	0
8	3	0	10	2	4	3	2	4	2	0	4	1	0	1	1	1	1	1
1	0	0	9	2	8	6	2	5	0	2	16	4	3	0	0	0	0	0
1	0	0	9	2	1	6	2	5	0	2	16	4	2	1	1	0	0	1
8	3	0	11	2	2	3	2	1	2	2	16	4	3	0	0	0	0	0
1	7	2	7	1	1	2	1	2	5	0	3	2	0	1	1	0	0	1
1	7	0	25	1	2	3	1	5	4	2	1	3	0	0	0	1	0	0
1	7	0	28	1	3	6	1	2	5	2	5	4	0	1	1	1	1	1
8	0	0	8	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
8	0	12	1	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
8	0	0	9	2	4	6	3	5	0	0	6	1	0	1	1	1	0	1
8	3	6	17	1	2	3	3	4	2	2	16	4	3	0	0	0	0	0
8	5	14	11	2	1	3	3	4	3	0	15	1	0	1	1	1	0	1
8	0	12	1	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	1	0	22	1	3	3	1	5	5	2	16	4	3	0	0	0	0	0
1	0	5	17	1	2	0	3	0	5	0	5	0	0	1	1	1	0	1
1	3	12	13	1	3	3	3	2	2	0	12	1	0	1	1	1	0	0
1	2	2	11	2	1	0	3	2	5	2	16	4	3	0	0	0	0	0
1	3	5	7	2	1	2	2	0	2	0	3	4	0	1	1	1	0	0
8	6	12	16	2	3	3	2	4	1	2	16	4	0	1	0	1	0	0
8	9	6	14	2	2	1	2	1	5	2	16	4	3	0	0	0	0	0
1	7	8	11	1	5	3	1	5	5	2	16	4	0	0	0	0	0	0
1	5	0	20	1	2	2	1	5	3	0	4	3	0	1	1	0	0	1
1	5	0	26	1	3	3	1	5	5	0	5	1	0	1	1	1	0	0
1	5	0	19	2	4	3	2	5	3	0	6	1	0	1	1	1	0	1
1	7	14	4	0	2	3	1	2	5	0	5	1	0	1	1	0	0	1
1	9	0	11	2	3	6	2	2	5	2	16	4	3	0	0	0	0	0
1	3	0	13	2	2	3	2	2	2	2	16	4	3	0	0	0	0	0
1	7	2	3	2	2	1	2	2	4	2	16	4	3	0	0	0	0	0
1	7	0	31	0	4	3	0	2	5	0	4	1	0	1	1	1	1	1
1	7	0	32	0	1	3	0	2	5	0	8	1	0	1	1	1	1	1
8	5	0	8	2	4	3	2	5	5	0	6	1	0	1	1	0	0	0
8	11	4	2	2	4	3	2	4	5	2	16	4	3	0	0	0	0	0
8	6	11	7	2	4	3	2	5	1	2	16	4	3	0	0	0	0	0
1	8	11	6	1	9	4	1	5	5	2	16	4	3	0	0	0	0	0
8	3	8	1	2	3	3	2	4	2	0	4	0	0	1	1	1	1	1
1	3	0	16	2	2	0	2	5	2	2	16	4	3	0	0	0	0	0
1	7	2	11	2	2	2	3	2	4	0	6	4	0	0	0	0	0	0
1	3	7	7	2	1	2	3	5	2	2	16	4	3	0	0	0	0	0
8	7	0	12	2	2	3	2	1	5	0	5	1	0	1	0	1	1	0
8	3	10	22	1	5	3	2	5	2	0	13	1	0	1	1	0	0	1
8	5	9	17	2	2	4	2	4	3	0	5	0	1	1	1	1	1	1
8	7	2	9	1	3	3	1	1	5	0	12	0	0	1	1	0	0	1
8	3	0	18	2	5	4	2	5	2	0	15	1	0	1	1	1	0	0
1	7	5	18	0	3	2	2	0	5	0	13	1	0	1	1	1	0	0
8	3	0	11	2	1	3	2	4	2	0	3	0	0	1	0	0	1	0
1	11	0	19	2	7	7	2	5	5	2	16	4	3	0	0	0	0	0
1	6	2	12	2	2	6	3	2	1	0	4	3	0	1	1	0	0	0
8	11	0	14	2	4	3	2	5	5	0	8	1	0	1	1	1	1	1
1	0	0	10	2	2	6	2	3	0	0	2	4	1	1	1	1	0	1
1	7	2	11	1	2	6	1	0	5	0	2	0	1	0	0	0	0	0
1	5	2	8	1	3	1	2	0	5	2	5	4	0	0	0	1	0	0
8	6	0	18	2	6	3	2	1	1	0	15	1	0	1	1	1	0	0
1	5	0	25	1	1	3	1	0	5	2	16	4	3	0	0	0	0	0
8	6	2	14	1	3	2	3	1	1	0	5	1	0	1	1	1	0	1
1	0	0	9	2	2	0	2	5	0	0	4	2	0	1	1	0	0	1
8	0	12	5	2	2	6	3	3	0	0	11	2	0	0	0	0	0	0
8	6	9	6	2	3	0	2	4	1	2	16	4	3	0	0	0	0	0
8	11	1	8	1	2	3	2	4	5	0	5	4	0	0	0	0	0	0
8	5	2	6	1	1	3	2	1	5	0	11	1	0	1	1	1	0	0
8	7	2	7	1	1	1	2	1	4	0	15	1	0	1	1	1	0	0
1	11	0	11	2	4	2	2	0	5	0	6	3	0	0	0	0	0	0
8	6	2	4	2	1	1	2	4	1	2	1	1	1	0	1	0	0	1
8	3	14	17	2	5	3	2	4	2	0	15	1	0	1	1	1	0	1
8	3	0	20	1	3	3	1	5	2	0	6	0	0	1	0	0	0	0
8	3	6	3	1	2	6	2	1	2	0	5	1	0	1	1	1	0	0
8	3	2	6	2	1	2	2	1	2	2	2	0	1	1	1	0	0	1
1	3	0	17	2	5	1	2	0	2	0	15	3	0	1	1	1	0	1
8	5	12	4	2	2	2	3	4	3	0	4	3	0	1	0	0	1	0
1	1	0	24	1	2	3	1	0	5	0	12	1	0	1	1	0	0	1
1	11	2	19	1	1	1	3	5	5	0	3	2	0	1	1	1	0	1
8	9	0	12	2	3	3	2	1	5	0	7	1	0	1	0	1	0	0
1	5	11	11	2	1	7	2	2	3	2	16	4	3	0	0	0	0	0
8	0	0	8	2	1	6	2	3	0	0	6	0	1	1	1	0	0	0
8	5	0	19	1	2	2	1	4	3	0	16	1	0	0	0	0	0	0
1	3	5	8	2	2	3	3	5	2	0	16	4	0	1	0	0	0	0
1	5	8	1	2	6	5	2	2	3	0	15	2	0	1	1	0	0	0
8	3	5	7	2	5	3	3	4	2	0	12	1	0	1	1	1	0	0
8	6	2	6	2	1	2	3	1	1	0	12	1	0	1	1	0	0	0
1	7	0	20	1	4	0	1	0	4	2	16	4	3	0	0	0	0	0
8	3	0	11	2	2	3	2	4	2	0	5	4	0	1	1	0	1	1
8	3	0	17	2	3	3	2	4	2	0	11	1	0	1	1	1	1	1
8	3	0	18	2	2	3	2	4	2	0	4	1	0	1	1	1	0	1
1	7	12	8	0	2	3	1	2	5	0	2	0	1	1	1	0	1	1
8	3	0	15	2	3	3	2	4	2	0	7	1	0	1	1	1	0	1
1	6	0	13	2	3	3	2	5	1	0	5	0	0	1	1	0	0	1
8	6	5	5	2	1	2	2	4	1	0	15	1	0	1	1	0	0	1
8	5	8	12	2	3	3	3	5	3	0	4	2	0	1	1	0	1	1
1	0	9	8	2	8	6	3	5	0	2	16	4	3	0	0	0	0	0
1	3	14	12	2	2	3	2	2	2	0	2	1	0	1	0	0	0	0
1	7	0	11	2	1	2	2	0	4	2	16	4	3	0	0	0	0	0
8	3	2	8	2	1	5	2	4	2	0	4	2	0	0	0	0	0	0
1	3	0	16	2	4	3	2	0	2	0	11	1	0	1	1	1	0	1
8	5	0	19	1	1	1	1	4	3	0	3	1	0	1	1	0	0	1
1	7	12	29	1	1	1	1	2	5	2	16	4	3	0	0	0	0	0
1	11	10	25	1	4	3	3	5	5	0	9	1	0	1	1	1	1	1
8	3	0	10	2	4	2	2	4	2	0	6	1	0	1	1	1	1	1
1	5	2	15	1	4	1	2	2	5	0	15	1	0	0	0	0	0	0
1	0	0	6	2	5	0	3	5	0	0	4	0	0	0	1	1	0	0
1	3	0	12	2	3	2	2	2	2	2	16	4	3	0	0	0	0	0
8	5	2	8	2	1	1	3	5	3	2	1	4	0	0	0	0	0	0
1	7	0	10	2	3	3	2	2	4	2	16	4	3	0	0	0	0	0
8	6	0	21	1	3	2	1	4	1	0	11	0	1	1	1	0	0	1
1	5	0	23	1	2	3	1	2	5	0	1	2	0	1	1	0	1	1
8	5	13	13	2	2	1	3	4	3	0	9	4	1	1	1	1	1	1
1	6	9	19	1	5	3	3	5	1	0	10	1	0	1	1	0	0	1
1	6	2	6	1	2	6	2	2	1	2	16	0	0	1	1	0	0	0
8	3	0	17	2	1	3	2	4	2	0	3	1	1	1	1	0	0	0
8	1	0	22	1	1	3	1	5	5	2	16	4	3	0	0	0	0	0
1	5	5	7	1	4	3	1	2	5	0	8	1	0	1	1	1	0	1
8	5	0	11	2	2	3	2	5	3	2	0	4	1	0	0	0	0	0
1	3	0	17	2	4	3	2	5	2	0	8	1	0	1	1	1	1	0
1	3	0	22	1	2	3	1	5	2	0	4	1	0	1	1	1	0	1
8	3	2	3	2	3	3	2	1	2	0	7	2	0	1	1	1	0	1
8	5	0	16	2	4	3	2	1	3	2	16	4	3	0	0	0	0	0
8	5	0	23	1	2	1	1	1	5	0	15	1	0	1	1	0	1	1
1	3	0	10	2	2	2	3	5	2	0	2	1	0	1	1	0	0	1
8	6	5	5	2	4	1	2	1	1	2	16	4	3	0	0	0	0	0
1	11	2	8	2	2	1	3	0	5	2	3	4	3	0	0	0	0	0
1	3	0	7	2	5	3	2	0	2	0	15	1	0	1	1	1	0	0
1	3	0	12	2	2	3	2	5	2	0	1	4	0	1	0	0	0	0
1	6	5	16	2	6	1	3	5	1	0	10	0	0	1	1	0	0	0
1	7	5	15	2	2	3	3	5	4	2	3	4	0	0	0	0	0	0
8	3	0	9	2	3	3	2	4	2	0	5	1	0	1	1	1	0	1
1	5	10	11	2	2	1	3	5	3	0	2	2	0	1	1	1	0	1
8	3	0	13	2	4	3	2	4	2	0	8	4	0	1	0	0	0	0
1	6	2	5	1	2	1	2	2	1	2	16	4	3	0	0	0	0	0
8	3	0	15	2	3	3	2	4	2	0	8	1	0	1	1	1	1	1
8	3	3	8	1	4	3	2	5	2	0	11	1	0	1	1	1	0	1
1	11	6	6	1	2	2	2	5	5	0	5	1	0	1	0	0	0	0
8	5	0	17	2	2	3	2	5	3	2	16	4	0	0	1	0	0	0
1	5	2	4	2	1	6	2	2	3	2	1	2	0	0	1	0	0	1
1	8	2	5	1	2	0	1	5	5	2	16	4	3	0	0	0	0	0
8	6	8	11	2	6	1	2	1	1	0	15	4	0	0	1	0	0	0
8	7	0	20	1	1	3	1	5	4	2	1	1	0	0	1	0	0	0
1	5	0	14	2	2	3	2	2	3	0	4	1	1	1	1	1	1	1
1	6	11	20	1	5	3	2	0	1	0	15	1	0	1	1	1	0	0
8	5	0	20	1	3	1	1	1	3	0	3	1	0	1	1	0	0	0
1	8	13	1	1	1	6	1	2	5	0	4	0	0	1	0	0	0	0
1	3	0	15	2	4	3	2	5	2	2	16	4	3	0	0	0	0	0
8	0	9	8	2	1	6	3	3	0	0	4	4	2	1	1	0	0	0
8	7	2	5	1	2	6	2	5	4	0	2	1	0	1	1	0	0	1
8	0	9	9	2	4	6	3	5	0	2	16	4	3	0	0	0	0	0
1	6	5	14	2	4	1	3	2	1	0	2	0	0	1	1	0	0	0
1	1	0	24	1	5	3	1	0	5	0	12	1	0	1	1	1	1	1
1	7	14	2	1	2	1	1	2	4	2	16	4	3	0	0	0	0	0
8	5	6	20	1	2	0	1	1	5	0	8	0	0	1	1	0	0	1
8	3	9	12	2	4	3	3	1	2	0	12	1	0	1	1	1	1	1
8	0	0	10	2	5	6	2	3	0	2	16	4	3	0	0	0	0	0
1	11	0	31	0	3	3	0	2	5	0	7	1	0	1	1	0	0	1
1	3	10	20	1	2	3	3	0	2	2	0	4	0	0	0	0	0	0
1	3	1	10	2	2	3	3	0	2	1	2	0	0	1	0	1	0	0
1	5	0	20	1	5	3	1	5	3	0	3	1	0	1	1	0	0	1
8	5	0	15	2	4	3	2	4	3	0	2	0	2	1	1	0	1	1
8	3	14	3	2	3	3	2	4	2	0	2	0	0	1	0	0	0	0
8	3	0	9	2	4	3	2	4	5	0	13	4	1	1	1	1	0	1
1	3	9	28	1	3	3	3	5	2	0	2	1	0	0	0	1	0	0
1	3	0	15	2	4	2	2	5	2	0	8	1	0	1	1	1	0	1
8	5	2	6	1	1	5	1	1	5	2	6	2	0	0	1	0	0	1
8	0	0	8	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
8	6	2	9	2	1	1	3	4	1	0	5	1	1	1	1	0	0	1
1	7	5	6	2	3	3	2	5	4	0	8	1	0	1	1	0	0	1
8	3	12	8	1	4	7	2	4	2	2	16	4	3	0	0	0	0	0
8	5	5	16	2	2	6	2	1	3	0	4	1	0	1	1	0	0	1
8	7	12	9	2	1	3	3	5	4	2	16	4	3	0	0	0	0	0
1	0	9	8	2	3	6	3	5	0	0	9	0	0	1	1	0	1	0
8	3	0	15	2	4	3	2	4	2	0	6	1	0	1	1	1	0	1
8	3	0	18	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
8	3	12	4	2	2	2	3	5	5	0	3	3	0	1	1	0	0	1
1	3	0	11	2	3	2	2	0	2	2	16	4	3	0	0	0	0	0
1	1	11	22	1	6	3	3	5	5	2	1	1	0	0	1	0	0	0
8	5	0	21	1	3	3	1	4	3	0	11	1	0	1	1	0	0	0
8	0	0	8	2	1	0	2	3	0	0	15	4	0	1	1	1	0	1
8	3	0	14	2	5	3	2	5	2	0	11	1	0	1	1	0	0	0
8	3	5	1	2	2	2	2	1	2	0	3	1	0	1	1	0	1	1
8	6	3	1	2	3	3	2	4	1	0	2	4	1	1	1	0	0	0
1	7	0	31	1	3	3	1	2	5	2	16	4	3	0	0	0	0	0
8	3	0	19	1	2	2	1	1	2	0	4	0	0	1	1	0	0	1
1	11	10	21	1	2	2	3	0	5	0	6	1	0	1	0	0	1	0
1	0	0	10	2	1	6	2	5	0	0	15	1	1	1	1	0	0	1
8	3	0	12	2	2	3	2	0	2	2	1	4	0	0	0	0	0	0
8	7	3	15	1	3	3	3	4	4	0	11	1	0	1	1	0	0	1
1	7	9	13	1	2	3	1	0	5	0	4	1	0	1	1	1	1	1
8	3	8	5	2	1	1	2	4	2	2	16	4	1	0	0	0	0	0
1	5	8	18	1	2	3	3	2	3	0	4	1	0	1	1	1	0	1
1	5	0	13	2	2	3	2	2	3	0	15	1	0	1	1	1	0	1
1	5	0	27	1	2	3	1	2	5	0	3	1	1	1	0	0	0	0
8	3	0	12	2	4	3	2	5	2	0	15	1	0	1	1	1	0	1
0	11	0	31	1	2	2	1	0	5	2	16	4	3	0	0	0	0	0
8	6	2	6	2	2	1	2	1	1	2	4	1	0	1	1	0	0	0
8	6	0	10	2	1	2	2	4	1	2	16	4	3	0	0	0	0	0
1	5	9	9	2	2	3	3	2	3	0	4	1	0	1	1	0	0	0
8	6	0	9	2	3	3	2	4	1	0	5	1	0	1	1	1	0	1
8	3	0	26	1	3	3	1	1	2	0	2	2	0	1	1	0	0	1
1	1	0	23	1	3	2	1	5	5	2	1	1	0	0	0	0	0	0
1	5	0	23	1	2	3	1	5	5	0	3	4	1	1	1	0	1	1
1	6	0	24	1	4	3	1	0	1	0	7	0	1	1	1	1	0	1
1	7	0	21	1	2	3	1	5	4	2	1	1	0	0	0	0	0	0
1	6	2	6	1	1	6	2	0	1	0	2	4	2	1	1	0	0	1
8	0	0	8	2	4	6	2	3	0	0	12	1	0	0	0	0	0	0
1	5	0	11	2	1	2	2	0	3	2	2	0	2	1	1	0	0	1
1	0	11	9	2	2	6	3	5	0	0	15	1	0	1	1	1	0	1
1	3	0	20	1	4	4	1	5	2	0	8	1	0	1	1	1	0	1
1	2	2	2	1	4	0	1	0	5	2	16	4	3	0	0	0	0	0
8	8	0	9	2	2	2	2	4	5	0	6	4	0	1	1	0	0	1
1	0	0	9	2	2	6	2	5	0	0	4	0	1	1	1	0	0	0
8	7	0	11	2	3	1	2	4	4	0	3	1	0	1	0	0	0	0
1	5	0	25	1	2	3	1	0	5	0	11	4	1	1	1	1	0	0
8	1	3	3	1	1	6	1	5	5	2	2	3	0	1	1	0	0	1
1	3	0	12	2	2	1	2	5	2	0	3	4	0	1	0	1	0	0
8	5	12	3	2	2	0	2	4	3	2	16	4	3	0	0	0	0	0
8	3	2	11	1	5	3	2	4	2	2	16	4	3	0	0	0	0	0
1	7	0	28	1	5	3	1	2	5	0	6	1	0	1	1	0	0	1
1	5	9	2	1	3	6	2	5	3	0	5	1	0	1	0	1	1	0
8	3	2	5	1	5	3	2	1	2	0	7	1	1	1	1	1	0	1
1	1	0	21	1	3	3	1	5	5	0	5	4	1	1	1	1	1	1
8	2	5	6	2	2	0	2	4	5	2	0	4	0	0	0	0	0	0
1	1	0	23	1	5	3	1	2	5	0	15	1	0	1	1	0	0	1
1	5	11	20	1	3	3	2	5	3	2	16	4	3	0	0	0	0	0
8	7	0	30	1	4	3	1	5	5	0	3	1	0	1	1	1	0	0
1	11	2	3	2	4	1	2	5	5	0	8	4	0	1	1	0	0	1
8	0	0	10	2	3	6	2	5	0	0	5	1	1	0	1	0	0	0
1	7	5	15	1	3	3	2	0	4	0	3	0	0	1	1	1	1	1
8	3	0	20	1	3	0	1	4	2	0	15	1	0	1	1	1	0	0
8	1	0	24	1	1	3	1	5	5	2	16	4	3	0	0	0	0	0
8	3	0	8	2	3	3	2	5	2	0	8	1	0	1	1	0	0	1
1	3	14	11	1	6	3	2	5	2	0	15	4	1	1	1	1	0	1
1	0	9	9	2	4	0	3	5	0	0	15	4	1	0	0	0	0	0
1	5	2	8	1	1	2	2	2	5	0	10	0	0	1	1	0	0	0
8	0	2	4	1	2	0	2	1	5	0	5	1	0	1	1	0	0	1
8	3	0	19	2	3	1	2	5	2	2	16	0	1	1	1	0	0	0
8	3	2	10	2	3	1	3	1	2	2	16	4	3	0	0	0	0	0
8	5	1	16	1	1	1	2	5	3	0	7	1	0	1	1	0	1	0
1	1	0	25	1	7	3	1	2	5	0	6	4	0	1	1	0	1	1
1	6	2	9	1	2	2	2	0	1	0	15	1	0	1	1	0	0	0
8	0	14	8	2	1	0	3	3	0	0	15	1	0	1	1	1	1	1
1	6	2	4	2	1	3	3	5	1	0	7	2	0	1	1	1	1	1
8	8	0	18	2	1	3	2	5	5	2	5	0	0	0	0	0	0	0
8	3	0	16	2	5	3	2	4	2	0	15	1	0	1	1	0	0	1
1	3	0	8	2	3	6	2	5	2	0	6	1	0	1	1	1	0	0
1	3	2	14	2	4	1	3	0	2	0	10	0	0	1	0	0	0	0
4	7	5	5	2	1	1	2	4	4	2	0	4	0	0	0	0	0	0
8	6	2	5	2	3	5	2	1	1	0	4	4	1	1	1	0	0	1
8	0	9	19	1	1	6	3	4	5	0	1	4	1	1	1	0	0	1
8	5	0	13	2	1	3	2	4	3	2	16	4	3	0	0	0	0	0
8	6	9	6	2	1	3	2	4	1	0	9	0	2	0	1	0	0	1
8	9	12	20	1	2	3	3	5	5	0	8	2	0	1	1	1	0	1
1	6	10	11	2	4	2	3	2	1	2	16	4	3	0	0	0	0	0
1	6	0	13	2	3	2	2	2	5	0	5	1	0	1	1	1	0	1
8	0	9	10	2	1	6	3	3	0	2	16	4	3	0	0	0	0	0
8	3	0	9	2	1	2	2	4	2	0	10	1	0	1	1	1	1	1
1	11	0	27	1	2	7	1	2	5	2	16	4	3	0	0	0	0	0
1	0	0	8	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
8	0	0	8	2	1	6	2	3	0	0	4	0	0	1	1	0	0	1
1	0	0	9	2	2	6	2	5	0	0	3	1	0	1	1	0	1	1
8	3	6	1	1	3	6	1	5	2	0	11	1	0	1	1	1	1	1
1	7	0	25	1	2	2	1	5	4	0	5	2	0	1	1	0	0	0
1	5	0	16	2	3	3	2	0	3	0	5	1	0	1	1	1	0	1
8	0	11	6	2	2	6	3	3	0	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	0	2	5	0	2	1	3	1	0	0	0	0	0
8	3	2	8	2	1	3	3	4	2	0	15	0	1	1	1	1	0	0
8	3	11	12	2	2	3	2	4	2	0	15	0	0	1	1	0	0	1
1	8	1	9	1	3	3	2	2	5	0	5	1	0	1	1	1	1	1
1	0	0	9	2	1	6	2	5	0	0	15	1	0	1	1	0	0	0
8	0	14	9	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
8	6	14	1	1	5	3	2	4	1	0	13	1	0	1	1	1	1	1
1	6	0	20	1	4	6	1	2	1	2	16	4	1	1	1	0	0	1
8	9	2	6	2	1	1	2	1	5	2	16	4	3	0	0	0	0	0
1	7	0	10	2	3	2	2	5	4	0	6	4	1	1	1	0	0	1
1	7	10	16	2	2	1	3	2	4	0	4	1	1	1	0	0	0	0
8	0	9	7	2	9	6	3	3	0	2	16	4	3	0	0	0	0	0
1	5	0	12	2	1	2	2	5	3	2	16	4	3	0	0	0	0	0
1	3	0	8	2	3	3	2	5	2	0	5	1	0	1	1	0	0	0
1	3	0	20	1	3	3	1	5	2	0	3	1	0	1	1	1	0	1
1	3	9	19	1	3	3	2	5	2	0	5	1	0	1	1	1	0	1
1	5	0	17	2	2	3	2	0	3	0	6	1	1	1	1	0	0	1
1	3	0	13	2	3	3	2	5	2	0	6	2	0	1	1	0	0	1
1	11	11	16	1	2	1	2	2	5	2	1	4	0	0	0	0	0	0
8	6	0	12	2	1	7	2	5	1	2	16	4	3	0	0	0	0	0
1	7	0	10	2	5	2	2	0	5	0	6	1	0	1	1	1	1	1
1	3	10	13	2	4	3	3	0	2	0	15	4	0	0	0	0	0	0
1	5	0	12	2	3	3	2	2	3	0	15	1	0	1	1	1	0	1
8	5	0	20	1	3	3	1	4	3	0	3	1	0	1	1	1	0	1
8	3	0	19	1	5	3	1	4	2	0	1	1	0	1	1	0	0	1
8	7	0	13	2	4	3	2	4	4	0	14	4	1	0	0	0	0	0
8	3	0	9	2	2	2	2	4	2	2	16	0	0	1	1	0	1	1
1	5	2	6	1	2	1	2	2	5	2	16	4	1	0	0	0	0	0
1	0	0	9	2	1	6	2	5	0	0	3	0	1	1	1	0	0	1
8	7	0	22	1	5	3	1	1	4	2	0	4	0	0	0	0	0	0
8	9	0	20	1	3	3	1	4	5	0	15	2	0	0	1	0	0	0
1	3	0	10	2	3	2	2	0	2	0	6	1	1	1	1	0	0	1
8	5	6	1	2	4	3	2	1	3	0	7	0	0	1	1	0	0	1
8	5	6	20	1	4	5	1	5	3	0	14	1	0	1	1	0	0	1
1	11	2	6	1	1	3	2	0	5	2	4	4	0	0	0	0	0	0
1	0	0	8	2	1	6	2	5	0	0	15	1	0	1	1	0	0	0
1	7	12	13	1	1	1	2	5	5	2	16	4	3	0	0	0	0	0
8	6	0	19	1	4	3	1	1	1	2	1	4	1	0	0	0	0	0
1	0	0	8	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
1	3	2	15	1	4	3	2	5	2	2	16	4	1	1	1	0	0	1
1	6	0	11	2	5	3	2	0	1	0	11	1	0	1	1	1	0	1
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	0	9	8	2	1	6	3	3	0	2	16	0	0	1	1	0	0	0
1	5	0	18	2	4	3	2	2	5	0	9	0	0	1	1	0	0	1
8	0	9	7	3	1	0	3	3	0	0	9	4	0	1	1	0	1	1
1	7	0	17	2	3	2	2	5	4	0	6	0	0	1	1	0	1	1
1	0	0	9	2	1	6	2	5	0	1	0	0	0	0	0	0	0	0
8	7	0	8	2	5	3	2	1	4	0	15	4	1	1	1	1	0	1
1	0	12	4	2	2	6	3	5	0	0	5	4	2	1	0	0	0	0
8	0	9	8	2	9	6	3	3	0	2	16	4	3	0	0	0	0	0
8	3	1	9	2	4	3	2	1	2	0	15	1	0	1	1	1	1	1
1	7	0	21	1	5	3	1	5	4	0	13	0	0	1	1	1	0	1
8	3	11	5	2	1	2	3	4	2	0	7	4	1	1	1	0	0	1
1	6	0	19	2	2	3	2	5	1	2	16	4	3	0	0	0	0	0
8	3	0	8	2	3	2	2	5	2	2	16	4	3	0	0	0	0	0
1	3	2	22	1	4	3	2	5	2	0	6	2	0	1	0	1	0	0
8	5	2	13	2	1	1	3	5	3	2	16	4	3	0	0	0	0	0
8	3	0	20	2	2	3	2	1	2	0	4	0	0	1	1	1	0	0
8	3	0	9	2	2	2	2	5	2	2	3	0	1	1	1	0	1	0
8	3	0	15	2	2	3	2	4	2	0	4	3	0	0	0	0	0	0
1	6	2	4	2	2	1	2	2	1	0	3	1	0	1	1	0	0	0
8	3	7	7	2	5	3	3	5	2	0	9	1	0	1	1	1	1	1
8	3	0	13	2	1	3	2	4	2	1	6	1	0	1	1	0	0	1
1	3	0	8	2	5	3	2	0	2	2	0	1	0	0	0	0	0	0
8	5	0	20	1	4	3	1	5	3	0	9	0	0	1	1	0	0	0
1	2	3	10	2	2	0	2	2	5	0	4	2	0	1	0	0	0	0
1	3	2	12	1	1	0	1	5	5	0	1	4	2	0	0	0	0	0
1	7	2	14	1	2	1	3	5	4	2	16	4	3	0	0	0	0	0
1	11	14	2	2	2	3	2	2	5	2	16	4	3	0	0	0	0	0
8	0	13	2	2	1	6	2	3	0	0	5	1	1	1	1	0	0	1
1	3	0	12	2	4	3	2	0	2	2	16	4	3	0	0	0	0	0
1	3	0	12	2	2	3	2	0	2	0	3	1	1	1	1	0	1	1
8	6	2	10	2	2	1	3	1	1	0	6	1	1	1	1	0	0	1
8	7	2	8	2	2	3	2	4	4	0	2	2	0	1	1	1	0	1
8	3	0	15	2	3	3	2	5	2	2	16	4	3	0	0	0	0	0
1	11	1	16	1	2	3	3	2	5	0	5	1	0	1	1	1	0	0
1	5	2	10	1	1	5	2	2	5	0	15	1	0	1	1	0	0	1
1	0	0	10	2	2	0	2	5	0	0	10	1	0	1	1	1	0	0
8	5	3	8	2	3	3	2	4	3	0	7	2	0	1	1	0	0	0
1	7	2	9	2	5	6	2	2	4	0	15	4	1	1	1	0	0	1
1	6	0	20	1	3	1	1	2	1	0	4	1	0	1	1	0	0	0
1	9	9	20	1	4	3	2	0	5	0	7	3	0	1	1	0	1	1
1	11	2	15	1	4	3	3	0	5	0	8	2	0	1	1	0	1	0
8	3	0	12	2	5	3	2	1	2	0	8	1	0	1	0	0	0	0
1	3	0	9	2	2	2	2	0	2	2	16	4	3	0	0	0	0	0
8	3	0	14	2	4	0	2	4	2	0	3	1	1	1	1	0	0	1
8	3	0	20	1	6	3	1	1	2	0	14	1	0	1	0	0	0	0
8	3	3	12	1	3	3	2	4	2	2	16	4	3	0	0	0	0	0
1	5	12	12	1	2	2	2	5	3	2	5	4	1	1	0	0	0	0
8	11	2	6	2	5	3	3	4	5	0	5	3	0	1	1	0	0	0
1	5	0	18	2	3	4	2	0	3	0	4	1	0	1	1	1	0	1
1	3	0	18	1	2	2	1	0	2	0	0	1	0	1	1	0	1	1
1	3	0	16	2	4	1	2	0	5	0	15	1	0	1	1	1	0	1
8	0	0	9	2	1	6	2	3	0	0	12	0	0	1	1	0	0	1
1	6	10	10	2	9	4	3	2	1	2	16	4	3	0	0	0	0	0
8	6	5	2	2	1	1	2	1	1	2	16	4	3	0	0	0	0	0
1	5	2	5	1	4	1	2	2	5	2	1	1	1	0	1	0	0	0
3	5	0	2	3	1	3	3	2	3	2	16	4	3	0	0	0	0	0
1	0	0	9	2	2	0	2	5	0	2	16	4	3	0	0	0	0	0
1	9	2	8	1	2	6	1	0	5	0	15	4	0	1	1	0	0	0
8	5	0	13	2	3	3	2	1	3	0	11	0	0	1	0	0	0	0
8	7	0	11	2	5	2	2	1	4	0	15	4	0	1	1	0	1	0
1	7	0	13	2	3	3	2	5	4	0	6	1	0	1	1	1	0	1
8	5	8	16	2	2	3	3	1	3	0	2	1	1	1	1	0	0	1
8	11	2	6	2	1	1	2	4	5	2	16	4	3	0	0	0	0	0
1	9	0	13	2	1	3	2	5	5	2	1	4	0	0	0	0	0	0
1	5	2	17	1	1	3	3	2	3	0	15	1	0	1	1	1	1	1
1	6	2	9	2	1	3	3	0	1	0	4	4	1	1	1	1	0	1
1	3	0	17	2	1	3	2	0	2	2	16	4	3	0	0	0	0	0
1	5	10	19	2	5	3	2	5	3	0	6	1	0	1	1	0	0	1
1	0	0	8	2	1	6	2	5	0	0	5	0	1	1	1	1	0	1
1	11	14	2	2	2	3	2	2	5	2	16	4	3	0	0	0	0	0
1	11	12	12	2	1	3	2	2	5	2	16	4	3	0	0	0	0	0
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	14	6	2	2	3	2	5	2	0	2	4	1	1	0	0	0	0
8	7	0	9	2	3	2	2	5	4	2	0	4	0	0	0	0	0	0
3	11	5	8	2	2	6	3	2	5	2	16	4	3	0	0	0	0	0
1	3	5	13	2	2	3	3	5	2	0	6	1	0	1	1	0	0	0
1	0	0	8	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
8	3	0	16	2	4	3	2	4	2	0	10	1	0	1	1	0	1	1
8	5	9	20	1	2	3	1	4	5	0	5	4	0	1	1	1	1	1
1	8	2	5	1	1	1	1	2	5	2	16	4	3	0	0	0	0	0
8	3	5	8	2	2	1	2	4	2	0	3	4	2	1	1	0	0	1
8	3	0	12	2	3	3	2	4	2	0	6	1	0	1	1	0	0	1
8	11	2	9	2	1	1	2	1	5	0	4	0	0	0	0	0	0	0
8	3	10	20	1	4	2	2	1	2	0	3	1	1	1	1	1	0	1
1	7	3	7	1	1	6	2	2	4	0	5	3	2	0	1	1	0	1
8	3	0	20	1	4	6	1	4	2	0	1	1	0	1	0	0	0	0
8	7	2	8	1	1	0	1	1	5	2	16	4	3	0	0	0	0	0
8	5	0	12	2	5	3	2	4	3	0	15	1	0	1	1	1	0	1
8	3	2	12	2	2	2	3	5	2	0	4	1	1	1	1	0	0	1
8	3	2	7	2	1	1	2	1	2	2	16	4	3	0	0	0	0	0
1	9	0	30	1	9	4	1	5	5	2	16	4	3	0	0	0	0	0
8	3	0	18	2	2	3	2	5	2	0	1	2	0	1	1	0	0	1
1	3	0	8	2	2	2	2	5	2	0	2	4	0	1	1	1	1	1
1	3	0	8	2	4	6	2	5	2	0	7	2	0	1	1	0	0	1
8	0	5	7	2	2	6	3	3	0	0	14	1	0	1	1	0	0	1
1	7	5	31	0	5	3	3	2	5	0	1	2	0	1	1	0	0	1
1	3	0	20	1	2	3	2	2	2	0	6	1	0	1	1	1	1	1
8	11	10	10	2	9	7	3	5	5	2	16	4	3	0	0	0	0	0
1	2	2	16	2	1	0	2	5	5	0	3	0	0	1	1	0	0	1
1	7	0	7	2	1	2	2	0	4	2	16	4	3	0	0	0	0	0
1	5	0	16	2	5	3	2	0	3	0	12	3	0	1	1	0	0	1
1	3	0	12	2	4	3	2	5	2	0	15	1	0	1	1	1	0	1
1	3	0	17	2	1	3	2	2	2	2	1	0	1	1	0	0	0	0
1	5	0	21	1	2	3	1	2	3	0	1	1	0	1	1	1	1	0
8	5	2	3	1	2	1	2	4	3	2	16	3	1	0	0	0	0	0
1	0	0	9	2	9	6	2	5	0	2	3	0	1	0	0	0	0	0
1	7	6	16	0	3	3	2	5	5	0	10	1	0	1	1	1	0	1
8	3	0	20	1	2	3	1	4	2	0	4	1	0	1	1	0	0	1
8	3	0	20	1	3	3	1	4	2	0	6	1	0	1	1	0	0	1
1	6	0	20	1	4	1	1	0	1	0	5	0	1	0	1	0	0	0
1	6	2	9	1	3	2	1	0	5	0	12	1	0	1	1	1	0	1
1	7	0	31	1	4	3	1	5	5	2	0	2	0	0	0	0	0	0
3	11	2	3	2	2	4	3	2	5	2	16	4	3	0	0	0	0	0
8	3	0	13	2	3	3	2	4	2	0	6	1	0	1	0	0	0	0
8	6	5	5	2	2	3	2	1	1	2	0	4	0	0	0	0	0	0
1	11	10	28	1	1	2	2	2	5	0	1	0	1	0	1	0	0	1
8	7	0	10	2	1	2	2	5	4	0	5	0	1	1	1	0	0	1
1	1	0	25	1	4	3	1	5	5	0	7	1	0	1	1	0	0	1
1	3	0	13	2	6	3	2	2	2	0	15	0	0	1	1	0	1	0
8	3	7	1	2	1	3	2	4	2	0	1	0	0	1	0	0	0	0
1	7	0	31	1	3	3	1	5	5	0	13	1	0	1	1	1	0	1
8	3	5	7	2	3	2	3	4	2	0	5	1	0	1	0	1	0	0
8	0	0	4	3	2	5	3	3	0	2	16	4	3	0	0	0	0	0
8	5	3	14	2	1	3	2	1	3	0	4	1	0	1	1	0	0	1
8	3	0	17	2	3	1	2	4	2	0	4	4	0	1	0	0	0	0
8	11	0	13	2	3	3	2	4	5	0	4	2	0	1	1	0	0	1
1	11	2	8	1	1	1	2	5	5	0	3	4	0	1	0	0	0	0
1	3	0	8	2	2	6	2	5	2	0	4	4	1	1	1	0	1	1
8	5	2	9	1	1	2	2	1	5	2	16	4	3	0	0	0	0	0
1	0	0	9	2	4	6	2	5	0	2	16	4	3	0	0	0	0	0
1	5	0	10	2	3	3	2	2	3	0	5	1	1	1	1	0	0	1
1	7	0	13	2	1	3	2	5	4	0	3	4	1	1	1	0	0	1
1	3	0	9	2	3	3	2	5	2	0	5	1	0	1	1	0	0	1
8	7	0	18	2	3	3	2	4	5	0	3	3	0	1	1	0	0	0
8	3	0	12	2	3	3	2	5	2	0	8	0	0	1	1	0	0	1
1	7	2	7	1	1	5	2	5	4	2	16	4	3	0	0	0	0	0
8	7	0	8	2	2	3	2	4	4	2	16	4	3	0	0	0	0	0
1	5	0	9	2	5	2	3	5	5	0	11	1	0	1	1	0	0	1
8	5	0	9	2	4	2	2	1	3	2	1	2	0	1	0	1	0	0
8	11	0	31	1	2	3	1	1	5	2	4	0	0	0	0	0	0	0
8	6	5	4	2	1	1	2	5	1	0	6	1	1	1	1	0	0	0
8	3	5	1	2	5	2	2	4	2	0	11	0	0	1	1	0	1	1
8	6	2	16	1	1	3	3	5	1	2	2	3	0	0	0	0	0	0
8	5	0	9	2	2	2	2	4	3	0	4	0	0	1	1	0	0	1
1	9	13	31	0	2	3	3	0	5	0	4	4	0	1	1	1	1	1
8	3	0	8	2	1	2	2	5	2	0	16	0	0	1	0	0	0	0
8	3	5	4	2	1	2	2	4	2	0	11	1	0	1	1	1	0	1
8	1	0	23	1	4	3	1	5	5	2	16	4	0	1	0	1	0	0
8	3	0	16	2	4	3	2	1	2	0	15	1	0	1	1	1	0	0
8	6	12	16	1	1	3	2	5	1	2	16	4	0	0	0	0	0	0
0	3	0	7	2	2	6	2	5	2	0	2	1	0	1	0	0	0	0
8	6	2	11	2	1	1	3	4	1	2	16	4	3	0	0	0	0	0
1	5	0	24	1	3	3	1	2	5	2	6	1	0	1	0	0	0	0
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	7	0	7	2	4	0	2	5	4	0	15	0	0	1	1	1	0	1
1	3	0	12	2	3	3	2	0	2	0	15	1	0	1	1	0	0	1
8	3	0	20	1	3	3	1	4	2	2	16	4	3	0	0	0	0	0
8	5	0	14	2	3	3	2	1	5	0	9	1	0	1	1	1	0	1
8	3	2	13	2	2	2	3	4	2	0	4	4	0	1	1	1	0	1
8	7	0	9	2	2	2	2	4	4	1	2	0	0	1	0	0	0	0
1	3	0	14	2	4	3	2	5	2	0	5	1	0	1	1	1	1	1
8	3	0	8	2	5	7	2	4	2	0	3	1	0	1	1	0	0	1
8	6	2	10	2	1	3	3	4	1	0	2	1	0	1	1	0	0	0
8	3	6	1	2	3	3	2	1	2	0	4	1	0	1	1	0	0	0
1	3	0	11	2	5	3	2	5	2	0	12	1	0	1	1	1	0	0
3	5	9	21	1	2	3	3	2	5	0	3	3	0	1	1	0	0	0
1	7	0	14	2	3	3	2	5	4	0	7	1	0	1	1	0	0	1
1	1	10	22	1	2	3	2	2	5	2	16	4	3	0	0	0	0	0
1	7	5	2	1	2	3	1	5	5	2	1	3	0	1	1	0	0	0
8	7	0	12	2	3	3	2	1	4	0	5	1	0	1	1	1	0	1
1	7	0	22	1	1	3	1	5	5	0	3	1	0	1	1	1	0	1
1	6	6	2	1	3	3	1	2	5	0	8	0	1	0	1	0	0	0
1	6	4	15	1	4	3	3	5	1	0	7	1	0	1	1	0	0	0
1	7	0	22	1	2	3	1	0	4	2	4	1	0	1	1	0	0	1
8	5	2	7	1	1	2	2	4	3	2	16	3	1	0	1	0	0	0
1	3	6	4	1	6	3	1	5	5	0	15	0	1	1	1	0	0	0
1	3	0	15	2	3	1	2	0	2	0	7	2	0	1	1	0	0	1
8	3	0	9	2	1	2	2	5	2	2	16	0	2	1	1	0	0	0
8	3	0	11	2	4	3	2	4	2	0	10	1	1	1	1	1	0	1
1	5	11	19	1	3	2	3	5	5	2	16	4	3	0	0	0	0	0
8	7	0	13	2	2	1	2	4	4	2	16	4	3	0	0	0	0	0
8	3	0	18	2	3	1	2	4	2	0	2	1	0	1	1	1	1	1
8	6	2	13	1	3	5	2	1	1	0	15	0	0	1	1	1	0	0
8	3	13	10	2	4	4	2	1	2	0	7	1	0	1	1	1	0	1
8	7	0	13	2	4	3	2	5	4	0	10	2	0	1	1	1	0	1
8	5	14	13	2	3	1	2	4	3	0	10	1	0	0	0	0	0	0
8	0	0	9	2	4	0	2	3	0	2	2	3	1	0	0	0	0	0
1	3	0	21	1	3	3	1	5	2	2	2	4	3	0	0	0	0	0
1	0	0	25	1	4	3	1	5	5	0	4	1	1	1	0	0	0	0
8	3	14	12	1	3	3	3	1	2	0	4	1	0	1	1	0	0	0
8	3	0	20	1	4	3	1	4	2	0	9	4	0	1	1	1	0	1
1	6	0	9	2	3	2	2	5	1	2	16	4	3	0	0	0	0	0
1	6	7	2	2	3	3	2	0	1	0	10	1	0	1	1	1	0	1
8	11	0	24	1	3	3	1	1	5	0	4	3	0	1	1	1	0	1
1	7	10	17	2	1	3	3	0	5	0	16	0	1	1	1	0	0	1
8	5	2	14	2	2	1	3	4	3	0	11	0	1	1	1	0	1	1
8	3	0	10	2	1	2	2	4	2	0	3	1	1	1	1	0	0	1
8	7	0	21	1	1	3	1	4	4	0	10	1	0	1	1	1	1	1
1	1	0	23	1	3	3	1	0	5	0	6	1	0	1	1	1	1	1
8	3	0	13	2	2	3	2	4	2	0	4	2	0	1	1	0	0	1
8	11	0	31	1	2	2	1	1	5	0	4	1	0	1	1	0	0	0
8	1	0	21	1	3	5	1	1	5	0	7	0	0	1	1	1	0	0
1	3	0	8	2	1	6	2	2	2	2	16	4	3	0	0	0	0	0
1	7	12	1	0	6	6	1	5	5	1	9	0	0	1	0	0	0	0
1	7	0	15	2	3	3	2	0	5	0	7	1	0	1	1	1	0	1
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	0	14	2	1	3	2	5	2	2	16	4	3	0	0	0	0	0
1	8	2	9	1	2	3	2	5	5	1	12	1	0	1	1	1	0	1
1	6	4	18	1	2	3	3	2	1	2	16	4	3	0	0	0	0	0
8	3	2	13	2	2	0	3	5	2	0	6	1	0	1	1	0	0	1
8	7	0	18	2	4	2	2	1	4	0	2	1	0	1	1	0	1	1
1	6	14	21	1	2	2	2	5	1	0	2	1	0	1	1	0	0	0
8	6	2	5	2	2	2	2	4	1	2	2	4	0	0	0	0	0	0
8	1	2	20	1	3	2	2	1	5	0	4	1	0	1	1	1	0	0
8	0	0	8	2	9	6	2	3	0	0	2	1	1	1	1	1	0	1
8	7	0	20	1	2	3	1	1	4	0	6	1	0	1	1	1	0	1
1	7	5	3	2	4	3	2	0	4	0	15	1	0	1	1	0	0	1
8	6	2	7	1	4	1	2	4	1	2	16	4	3	0	0	0	0	0
1	7	0	29	1	9	4	1	2	5	2	16	4	3	0	0	0	0	0
1	7	0	32	0	2	3	0	5	5	0	3	3	0	1	0	0	0	0
1	0	9	8	2	1	6	3	5	0	2	16	4	3	0	0	0	0	0
1	7	10	21	1	4	3	2	2	4	2	16	4	3	0	0	0	0	0
1	5	0	16	2	2	3	2	2	3	0	1	4	0	1	0	0	0	0
8	5	0	18	2	2	3	2	1	3	2	0	0	0	0	0	0	0	0
1	7	2	11	1	2	1	1	2	5	0	3	1	0	1	1	0	0	1
8	6	8	4	2	4	3	2	1	1	0	4	0	0	1	0	0	0	0
1	7	2	6	2	1	1	2	2	4	0	15	4	1	1	1	0	0	1
8	6	14	1	2	4	2	2	1	1	2	16	4	3	0	0	0	0	0
8	9	0	11	2	2	2	3	5	5	2	16	4	3	0	0	0	0	0
8	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	11	6	1	2	1	3	2	2	5	2	16	4	3	0	0	0	0	0
1	5	2	5	1	3	2	2	5	5	0	9	4	0	1	1	0	0	1
1	6	0	14	2	2	3	2	0	1	2	16	4	3	0	0	0	0	0
8	0	9	8	2	2	0	3	3	0	0	4	1	1	1	0	0	0	0
1	0	0	9	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
8	7	0	16	2	3	1	2	5	4	0	6	2	0	1	0	0	0	0
8	3	0	16	2	4	3	2	5	2	0	9	1	1	1	1	1	1	1
8	3	0	24	1	4	3	1	5	2	0	4	1	1	1	1	0	0	1
8	3	0	16	2	2	3	2	4	2	0	2	1	0	1	1	0	0	1
8	7	1	18	1	2	1	2	1	5	2	16	4	3	0	0	0	0	0
1	9	9	15	1	3	3	2	5	5	2	16	4	3	0	0	0	0	0
8	0	9	5	1	2	0	1	5	5	2	16	4	3	0	0	0	0	0
8	5	0	10	2	2	3	2	1	3	2	3	1	0	1	0	0	0	0
1	11	0	16	2	3	1	2	0	5	0	4	0	0	1	1	1	0	1
8	7	0	10	2	2	3	2	5	4	0	1	1	0	1	1	0	1	0
8	6	0	13	2	5	3	2	4	5	0	10	3	0	1	1	1	1	1
8	7	0	8	2	6	3	2	4	4	0	3	1	0	1	1	0	1	1
1	0	0	8	2	8	6	2	5	0	2	16	4	3	0	0	0	0	0
1	5	9	9	2	2	2	3	2	3	2	16	4	3	0	0	0	0	0
8	7	11	6	2	4	6	3	5	4	0	15	1	0	1	1	1	1	1
8	7	0	21	1	2	3	1	1	4	2	2	2	0	0	0	0	0	0
3	3	0	12	2	4	3	2	2	2	2	16	4	3	0	0	0	0	0
8	3	0	12	2	4	1	2	4	2	0	10	1	0	1	1	1	0	1
8	3	13	20	1	1	3	3	1	2	0	10	1	0	1	1	0	0	1
8	3	0	10	2	4	2	2	4	2	0	9	1	0	1	1	1	1	1
1	0	0	8	2	1	6	2	5	0	0	11	4	0	1	1	0	0	1
1	7	2	5	1	2	1	2	2	4	0	2	4	1	1	0	1	0	0
1	5	2	5	1	1	1	1	5	5	0	6	1	0	1	1	1	0	1
1	3	8	1	2	4	2	2	2	2	0	3	2	0	1	1	1	1	1
1	6	0	31	1	4	3	1	2	1	0	1	1	0	1	1	0	0	0
8	3	0	9	2	4	2	2	5	2	0	15	0	0	1	1	0	1	0
8	3	0	10	2	6	3	2	4	2	0	4	1	1	1	1	1	0	0
8	6	13	13	2	2	3	3	4	1	2	16	4	3	0	0	0	0	0
1	5	2	6	1	2	1	2	5	3	2	2	0	0	1	0	0	0	0
1	7	12	3	0	4	6	1	0	5	0	6	3	0	1	1	1	0	1
8	0	5	6	2	2	6	3	3	0	2	16	4	3	0	0	0	0	0
1	0	9	9	2	2	0	3	5	0	2	16	4	3	0	0	0	0	0
8	5	3	16	1	1	3	3	4	3	2	0	1	1	0	0	0	0	0
8	0	0	15	2	4	6	2	1	5	0	6	1	0	1	1	1	0	0
8	0	0	8	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	5	14	24	1	4	3	3	2	5	0	11	1	0	1	1	1	1	1
1	3	7	12	2	1	3	3	2	2	2	16	4	3	0	0	0	0	0
1	3	13	14	2	2	6	3	5	2	0	8	2	0	1	1	0	0	1
8	10	0	11	2	4	2	2	1	5	2	16	4	3	0	0	0	0	0
1	7	0	11	2	4	2	2	0	4	0	8	1	0	1	1	1	0	1
1	0	9	8	2	3	6	3	5	0	2	2	3	1	1	0	0	0	0
1	3	8	24	1	3	3	2	5	5	0	3	1	0	0	0	0	0	0
1	9	0	31	1	1	2	1	5	5	2	1	3	0	0	0	0	0	0
1	6	9	5	2	1	3	2	0	1	2	16	4	3	0	0	0	0	0
2	8	2	9	1	2	3	2	2	5	0	2	2	0	1	1	0	0	1
1	3	0	20	1	4	1	1	5	2	2	16	4	3	0	0	0	0	0
8	5	0	14	2	2	2	2	4	3	0	4	4	0	1	1	0	0	1
8	9	14	3	2	3	3	2	4	5	2	16	4	3	0	0	0	0	0
8	7	14	1	2	3	3	2	4	4	0	3	1	0	1	1	0	1	1
8	6	0	15	2	1	1	2	4	1	0	11	4	0	1	1	0	0	1
8	6	0	12	2	4	3	2	1	1	0	5	4	0	1	1	0	0	0
8	3	0	19	1	3	6	1	1	2	0	2	4	1	0	0	0	0	0
8	7	2	5	3	1	1	3	5	4	2	16	4	3	0	0	0	0	0
1	0	0	8	2	2	6	2	5	0	0	3	2	0	0	0	0	0	0
1	5	5	2	2	2	3	2	5	3	1	2	0	0	1	0	0	0	0
1	3	0	18	2	1	2	2	2	2	0	14	0	0	1	1	1	1	1
1	8	0	25	1	2	3	1	5	5	0	2	4	0	1	0	0	0	0
1	7	3	21	1	2	3	3	2	5	0	7	4	1	1	1	1	0	1
1	0	0	8	2	2	6	2	5	0	0	15	1	0	1	1	1	0	1
1	7	0	31	1	1	3	1	2	5	0	3	1	0	0	1	1	0	0
8	0	0	8	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	6	13	2	2	3	3	4	2	0	5	2	0	0	0	0	0	0
1	3	0	8	2	4	6	2	5	2	0	7	2	0	1	1	0	0	1
1	7	6	22	1	4	3	2	5	5	0	1	1	0	1	1	0	0	1
1	3	6	2	1	6	3	1	2	2	0	15	1	0	1	1	0	0	1
1	5	14	6	1	3	3	1	5	5	0	7	1	0	1	1	0	0	0
8	3	1	17	1	4	3	2	5	2	0	10	1	0	1	1	1	0	1
1	6	2	7	1	1	3	2	2	1	0	15	0	0	1	1	1	0	0
1	0	0	10	2	2	6	2	5	0	2	16	0	0	0	0	0	0	0
1	7	0	25	1	5	3	1	2	4	0	12	1	0	1	1	0	1	1
8	7	2	11	1	3	3	1	1	5	0	5	3	1	0	0	0	0	0
1	6	5	4	2	3	3	2	2	1	2	16	4	3	0	0	0	0	0
8	3	0	16	2	1	3	2	5	2	2	16	4	3	0	0	0	0	0
8	0	0	9	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	0	15	2	5	3	2	2	2	0	15	0	1	1	1	0	0	1
1	3	2	10	1	1	6	2	2	2	2	16	4	3	0	0	0	0	0
1	7	0	12	2	3	3	2	0	4	0	9	1	0	1	1	0	0	1
1	7	0	25	1	1	1	1	0	5	0	8	1	0	1	1	0	0	1
1	8	2	9	1	1	1	2	2	5	0	7	1	1	1	1	0	0	1
1	7	0	31	1	5	3	1	5	5	0	4	1	0	1	1	0	0	1
8	5	6	12	2	1	3	3	4	3	2	16	4	3	0	0	0	0	0
8	3	9	16	2	1	2	3	4	2	2	16	4	3	0	0	0	0	0
8	6	0	11	2	1	1	2	4	1	0	3	4	2	1	1	0	0	1
8	3	0	20	1	4	3	1	5	2	0	9	1	0	1	1	0	0	1
8	3	13	11	2	2	3	3	4	2	0	5	1	0	1	1	0	0	1
1	7	2	6	1	3	1	1	2	5	2	2	4	0	0	0	0	0	0
1	3	0	11	2	3	3	2	0	2	0	11	4	0	1	1	1	1	1
8	7	0	8	2	2	7	2	4	4	0	4	1	1	1	1	0	0	1
8	9	2	8	1	2	3	2	1	5	0	15	1	0	1	1	1	0	1
8	6	0	9	2	3	2	2	4	1	0	6	1	0	1	1	1	0	1
1	3	0	11	2	4	2	2	0	2	2	16	4	2	0	0	0	0	0
8	7	0	16	2	4	0	2	1	4	2	16	4	1	1	1	0	0	0
1	3	2	13	2	2	1	2	5	2	2	16	4	3	0	0	0	0	0
5	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
8	5	2	7	2	3	2	3	4	3	2	16	4	3	0	0	0	0	0
8	5	0	12	2	3	3	2	1	3	0	4	1	0	1	1	0	0	1
8	3	0	20	1	4	3	1	4	2	0	1	1	0	1	1	1	1	1
8	7	0	19	1	2	3	1	4	4	2	2	3	0	0	0	0	0	0
3	5	7	7	2	1	6	2	2	3	0	3	1	1	1	0	1	0	0
1	3	13	17	2	3	3	3	5	2	0	5	1	0	1	1	1	0	1
1	5	0	14	2	2	3	2	0	3	2	0	0	0	1	1	0	0	1
8	6	0	18	2	4	3	2	5	1	0	10	1	0	1	1	1	0	1
1	5	0	10	2	3	2	2	2	3	0	5	1	0	1	1	1	0	1
1	5	0	8	2	1	6	2	5	3	0	4	1	0	1	1	0	1	0
8	6	3	15	2	2	3	2	5	1	2	16	4	3	0	0	0	0	0
1	6	7	5	2	3	2	3	2	1	0	9	1	0	1	1	0	0	0
1	6	2	5	1	2	1	1	2	5	2	16	4	3	0	0	0	0	0
1	7	0	12	2	4	3	2	0	4	0	7	0	0	1	0	0	0	0
8	7	5	3	2	1	2	3	4	4	2	16	4	3	0	0	0	0	0
1	2	2	6	1	3	0	2	2	5	0	4	3	1	1	1	1	0	0
8	5	0	12	2	3	3	2	4	3	0	4	2	0	0	0	0	0	0
1	1	2	9	1	3	3	2	2	5	0	15	4	1	1	1	0	0	0
8	7	5	10	2	2	3	3	4	4	0	3	3	0	1	1	0	1	1
8	0	9	9	2	4	6	3	3	0	0	2	3	1	0	0	0	0	0
1	7	0	15	2	1	1	2	5	4	0	6	0	0	1	1	0	0	0
1	3	0	17	2	3	3	2	1	2	2	16	4	3	0	0	0	0	0
1	9	2	7	1	2	1	1	5	5	0	15	3	0	1	1	0	0	1
1	6	0	20	1	3	3	1	5	1	0	6	4	0	1	1	1	1	1
1	6	11	15	1	3	3	3	2	1	0	15	1	0	1	1	0	0	0
1	6	2	5	2	1	1	2	0	1	0	15	1	0	1	1	0	0	1
8	0	9	8	2	1	6	3	5	0	0	0	0	1	0	1	0	0	0
1	5	0	11	2	2	2	2	0	3	0	4	1	0	1	1	1	1	1
1	3	0	10	2	2	3	2	2	2	0	4	3	0	1	1	0	0	0
3	3	14	1	2	1	1	2	5	2	2	16	4	2	0	0	0	0	0
1	7	0	12	2	2	1	2	2	4	0	16	0	1	1	0	0	0	0
1	0	3	9	2	1	0	2	2	5	2	5	1	1	1	1	1	0	1
8	5	0	12	2	2	3	2	4	3	2	16	4	3	0	0	0	0	0
1	5	10	24	1	2	3	3	2	5	0	1	1	0	1	1	1	0	1
8	7	0	10	2	1	0	2	5	5	0	1	4	0	0	1	0	0	0
1	3	0	16	2	2	3	2	0	2	0	9	1	0	1	1	1	0	1
1	2	2	10	2	3	0	3	5	5	0	15	0	0	1	0	0	0	0
1	3	9	1	2	1	3	2	0	2	0	3	4	0	1	0	0	0	0
8	11	14	1	2	2	2	2	4	5	2	16	4	3	0	0	0	0	0
8	3	9	13	2	4	2	2	4	2	0	8	1	1	1	0	0	0	0
1	9	6	20	1	4	3	3	2	5	2	16	4	3	0	0	0	0	0
1	1	0	22	1	2	3	1	0	5	0	2	2	0	1	1	0	0	1
8	5	0	13	2	4	3	2	5	3	2	16	4	3	0	0	0	0	0
1	5	2	5	1	1	1	2	5	5	2	16	4	3	0	0	0	0	0
8	3	0	20	1	4	3	1	4	2	0	12	1	1	1	1	1	1	1
8	3	0	20	1	1	3	1	5	2	0	1	0	0	1	1	0	0	1
8	6	3	10	2	1	1	2	4	1	0	3	4	1	1	0	0	0	0
8	7	0	12	2	4	3	2	5	4	0	6	4	0	1	1	1	0	1
1	5	2	12	1	4	3	2	2	5	2	1	4	0	1	0	1	0	0
8	0	9	8	2	9	6	3	3	0	2	16	4	3	0	0	0	0	0
1	6	0	18	2	1	3	2	2	1	2	16	4	3	0	0	0	0	0
8	6	2	7	2	1	1	3	5	1	0	9	1	0	1	1	0	0	1
8	7	14	17	2	4	1	2	1	5	0	11	4	0	1	1	0	1	1
8	6	3	8	2	1	1	3	5	1	0	6	1	0	1	1	0	0	1
1	6	9	15	2	1	1	3	5	1	0	4	2	0	1	1	0	0	0
8	0	0	9	2	4	6	2	3	0	0	7	1	0	1	1	1	0	1
1	1	0	23	1	4	3	1	5	5	0	15	0	0	1	1	1	0	1
1	11	2	5	2	3	1	2	2	5	2	16	4	3	0	0	0	0	0
1	7	0	9	2	2	1	2	5	5	2	2	4	0	0	0	0	0	0
1	5	0	8	2	3	2	2	5	3	0	3	1	0	1	1	0	0	0
8	0	10	9	2	2	6	3	3	0	0	3	3	0	1	0	0	0	0
8	7	11	15	2	1	1	2	4	4	2	16	4	3	0	0	0	0	0
1	5	14	19	2	2	3	2	2	3	0	1	1	0	1	1	1	1	0
8	0	0	8	2	2	0	2	3	0	2	2	3	0	0	0	0	0	0
8	3	6	1	2	3	3	2	4	2	0	6	1	0	1	1	0	0	0
8	5	0	19	2	1	1	2	5	3	2	16	4	3	0	0	0	0	0
1	6	1	1	2	2	3	2	2	1	0	4	2	0	1	1	0	0	0
8	3	0	13	2	4	3	2	4	2	2	16	4	3	0	0	0	0	0
8	7	13	29	1	4	3	3	5	5	2	16	4	3	0	0	0	0	0
8	6	0	10	2	3	2	2	4	1	0	9	2	0	1	1	0	0	0
1	3	3	8	2	1	3	3	5	2	2	16	4	3	0	0	0	0	0
8	3	0	12	2	2	1	2	4	2	2	1	0	2	1	0	0	1	0
8	3	2	5	2	1	2	2	4	2	0	15	1	0	1	1	1	0	1
1	3	0	13	2	3	3	2	2	5	0	1	1	0	1	1	0	0	0
1	7	0	9	2	1	2	2	2	4	0	2	3	0	0	0	0	0	0
8	0	12	7	2	1	2	3	1	0	2	16	4	3	0	0	0	0	0
8	3	9	9	2	2	3	2	4	2	0	5	1	0	1	1	0	0	1
8	3	0	22	1	4	3	1	5	2	0	7	1	0	1	1	0	0	1
8	3	0	19	1	3	3	1	1	2	0	5	1	0	1	1	0	1	1
8	3	0	15	2	4	1	2	2	2	2	16	4	3	0	0	0	0	0
1	6	4	6	1	3	6	1	5	5	0	2	0	0	1	0	0	0	0
1	0	11	8	2	2	6	3	3	0	2	1	3	0	0	0	0	0	0
8	5	0	25	1	3	3	1	1	5	0	14	1	0	1	1	1	1	1
1	5	14	2	2	5	3	2	5	3	0	15	1	0	1	1	0	0	1
8	7	0	11	2	3	3	2	4	4	0	0	4	3	0	0	0	0	0
1	9	6	10	2	4	1	2	5	5	0	9	4	1	1	1	1	0	0
1	5	0	16	2	1	3	3	2	3	0	1	2	0	1	1	1	0	1
1	5	1	14	1	3	3	2	2	5	0	4	1	0	1	1	0	0	1
1	3	0	16	2	3	3	2	2	2	0	12	1	0	1	1	0	1	1
8	11	2	4	2	2	1	2	4	5	2	16	4	3	0	0	0	0	0
1	5	11	7	1	1	3	1	2	5	0	15	1	0	1	1	0	0	0
1	11	2	30	1	2	1	3	5	5	2	3	1	1	1	1	0	0	0
1	7	0	10	2	2	4	2	5	4	2	16	4	3	0	0	0	0	0
1	5	0	22	1	1	1	1	5	3	2	16	4	3	0	0	0	0	0
1	0	0	9	2	3	6	2	5	5	0	8	1	0	1	1	0	0	1
1	7	0	21	1	2	1	2	5	4	0	15	1	1	1	1	1	1	1
1	5	0	22	1	1	2	1	5	5	2	16	4	3	0	0	0	0	0
1	0	0	8	2	2	0	2	5	0	0	15	0	0	1	1	1	0	1
1	3	0	10	2	2	2	2	5	2	0	5	1	0	1	1	1	0	1
1	3	0	13	2	3	3	2	0	2	2	7	4	1	1	1	0	0	1
8	0	9	9	2	9	6	3	3	0	2	16	4	3	0	0	0	0	0
8	6	8	2	2	3	3	2	1	1	0	6	2	0	1	0	0	0	0
1	3	0	18	2	5	2	2	5	2	0	12	4	1	1	1	0	1	1
1	6	14	15	2	4	3	2	0	1	0	15	1	0	1	1	1	0	0
1	3	0	12	2	4	3	2	0	2	0	2	1	1	1	1	1	0	1
8	3	5	5	1	4	3	2	5	2	2	16	4	3	0	0	0	0	0
1	7	11	19	2	3	4	2	2	4	2	16	4	3	0	0	0	0	0
8	7	14	22	1	3	3	2	1	5	0	15	2	0	1	1	0	1	1
1	11	10	20	1	6	3	3	5	5	2	16	4	3	0	0	0	0	0
1	3	0	8	2	2	2	2	4	2	0	6	1	0	1	1	1	1	0
8	0	0	9	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
1	7	0	13	2	3	4	2	2	5	0	6	3	1	1	1	0	1	1
8	0	0	9	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	12	14	1	2	3	3	1	2	2	16	4	3	0	0	0	0	0
8	3	6	13	2	2	1	3	5	2	2	16	4	3	0	0	0	0	0
8	5	8	14	1	2	0	3	1	3	0	9	2	0	0	0	0	0	0
8	6	10	20	2	1	1	2	4	1	2	16	4	3	0	0	0	0	0
8	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
8	7	6	13	1	5	3	2	1	5	0	10	4	1	1	1	0	1	1
1	8	0	20	2	3	3	2	5	5	0	2	2	0	1	1	0	0	0
8	3	2	7	1	1	6	2	5	5	2	16	4	3	0	0	0	0	0
3	6	5	11	1	1	6	2	5	1	0	1	1	0	0	0	0	0	0
8	7	5	25	1	4	3	2	1	5	1	1	1	0	1	1	0	1	0
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	6	3	7	2	2	3	3	0	1	0	1	4	0	1	1	0	0	1
1	0	0	9	2	1	6	2	5	0	0	2	4	2	0	0	0	0	0
1	5	5	11	2	3	3	2	2	3	2	16	4	3	0	0	0	0	0
8	7	9	10	2	1	2	2	5	4	0	16	0	2	1	1	0	0	1
1	2	12	3	2	1	0	2	5	5	2	2	4	1	0	0	0	0	0
8	3	10	17	2	3	2	3	4	2	0	8	1	0	1	1	1	1	1
1	3	0	16	2	5	3	2	2	2	0	14	1	0	1	1	1	0	1
8	3	0	18	2	3	5	2	1	2	0	9	4	1	1	1	0	0	1
8	5	12	1	2	1	6	2	1	3	0	10	0	0	1	1	0	0	1
8	5	2	5	2	2	6	2	5	3	0	12	0	0	1	1	0	0	0
8	3	0	16	2	2	3	2	4	2	0	2	4	0	1	1	0	0	0
8	3	6	8	2	4	3	3	5	2	2	16	4	3	0	0	0	0	0
1	6	6	10	1	3	6	2	2	1	0	6	1	1	1	1	0	0	1
1	0	0	9	2	1	6	2	3	0	0	8	4	0	0	0	1	0	0
8	3	2	20	1	4	3	2	4	2	0	13	0	0	1	1	0	1	1
8	11	4	1	2	3	3	2	4	5	2	16	4	3	0	0	0	0	0
1	6	0	17	2	4	3	2	5	5	0	8	1	0	1	1	0	1	1
1	7	0	29	1	3	3	1	2	4	0	4	0	0	1	1	0	0	0
8	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
8	7	9	10	2	3	3	2	4	4	0	8	1	0	1	1	0	0	1
8	3	0	20	1	3	3	1	1	2	0	6	0	0	1	1	1	0	1
8	3	0	8	2	5	2	2	4	2	0	9	1	0	1	1	0	0	1
8	11	0	10	2	3	1	2	4	5	2	16	4	3	0	0	0	0	0
1	8	0	13	2	5	3	2	5	5	0	15	0	0	1	1	0	0	1
8	5	2	10	2	2	3	2	4	3	2	16	4	3	0	0	0	0	0
1	5	0	25	1	3	1	1	2	5	0	13	1	0	1	1	0	0	0
1	3	0	12	2	3	1	2	2	2	2	16	4	3	0	0	0	0	0
8	7	2	5	2	2	5	2	4	4	2	16	4	3	0	0	0	0	0
8	1	0	22	1	1	3	1	1	5	0	4	4	0	1	1	1	0	1
8	5	1	2	1	2	3	1	5	3	0	2	2	0	1	0	0	0	0
1	6	11	20	1	1	3	2	0	1	2	16	4	3	0	0	0	0	0
1	9	0	31	0	2	1	0	5	5	0	8	0	0	1	1	0	0	1
1	5	2	7	2	1	3	2	5	3	2	16	4	3	0	0	0	0	0
8	3	10	14	2	3	2	3	5	2	0	8	3	0	0	0	0	0	0
8	6	4	4	2	1	1	2	5	1	2	0	4	1	1	0	1	0	0
1	1	0	24	1	2	3	1	2	5	0	1	2	0	1	1	0	0	1
8	7	0	8	2	3	2	2	4	5	0	1	1	0	1	1	0	0	1
8	6	0	10	2	4	1	2	4	1	2	3	4	0	0	0	0	0	0
7	5	6	11	2	4	2	3	2	3	0	10	4	1	1	1	0	1	1
8	3	8	1	2	1	3	2	4	2	0	1	4	0	1	1	0	0	1
8	6	0	20	2	1	1	2	1	1	0	15	2	0	1	1	1	0	1
8	5	11	7	2	2	1	2	5	3	0	10	4	0	1	1	0	0	0
8	5	9	8	2	1	2	3	4	3	2	16	4	3	0	0	0	0	0
1	7	0	8	2	3	2	2	5	4	0	8	1	0	1	1	0	1	1
8	11	2	5	2	1	1	2	4	5	0	8	0	0	1	1	1	1	1
8	5	2	13	2	3	1	3	0	3	2	5	0	0	1	1	1	0	0
8	6	14	9	2	2	2	2	5	1	2	16	4	3	0	0	0	0	0
1	11	9	11	2	1	3	2	5	5	0	9	1	0	1	1	0	0	1
8	0	6	9	2	2	6	3	5	0	2	16	4	3	0	0	0	0	0
8	12	8	1	1	2	2	1	1	5	2	16	4	3	0	0	0	0	0
8	7	0	19	1	4	3	1	4	4	2	16	4	3	0	0	0	0	0
8	6	0	22	1	1	3	1	5	1	0	11	1	0	1	1	1	1	1
1	1	0	21	1	5	3	1	5	5	0	10	1	0	1	1	0	0	0
8	3	0	10	2	3	3	2	4	2	2	16	4	3	0	0	0	0	0
1	7	0	10	2	3	2	2	5	4	0	3	0	0	1	1	0	0	0
1	7	0	18	2	4	3	2	2	4	0	3	0	0	1	1	1	0	1
8	3	8	9	2	5	3	3	1	2	0	11	2	0	1	1	1	1	0
8	5	14	8	2	4	0	3	1	3	0	4	4	1	1	1	1	0	1
8	3	0	12	2	4	3	2	5	2	0	8	4	1	1	1	0	0	1
8	5	0	9	2	3	2	2	5	5	0	2	4	0	1	1	1	0	1
8	3	0	9	2	4	2	2	1	2	0	9	1	0	1	1	1	0	0
1	11	13	13	1	3	3	3	5	5	0	7	2	0	1	1	1	1	1
8	5	5	8	2	2	3	2	4	3	2	16	4	3	0	0	0	0	0
1	6	2	5	1	2	2	2	2	1	0	10	0	0	1	1	1	0	1
8	6	5	1	2	1	0	2	3	1	2	2	3	1	0	0	0	0	0
8	7	0	12	2	2	3	2	1	4	0	5	1	0	1	1	1	0	1
8	6	0	14	2	4	1	2	1	1	0	10	4	1	0	0	0	0	0
1	11	2	11	2	3	1	2	0	5	0	4	0	0	1	0	1	0	0
8	6	5	3	2	1	1	2	5	1	2	16	4	3	0	0	0	0	0
1	5	14	16	1	3	2	2	5	3	2	5	3	0	0	0	0	0	0
1	6	2	4	2	1	1	2	5	1	2	16	4	3	0	0	0	0	0
8	3	2	7	2	1	0	3	4	5	2	16	4	3	0	0	0	0	0
8	3	0	8	2	4	3	2	5	2	0	5	1	0	1	1	0	0	0
8	3	0	8	2	3	2	2	4	2	0	16	4	1	1	0	1	0	0
1	3	6	3	2	2	3	2	0	2	2	16	4	3	0	0	0	0	0
8	0	0	8	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	0	16	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
1	7	2	3	2	1	1	2	2	4	2	16	4	3	0	0	0	0	0
8	5	1	1	2	4	3	2	4	3	0	9	1	0	1	1	1	0	1
8	7	0	22	1	2	1	1	1	4	0	11	2	0	1	0	0	0	0
1	5	0	12	2	4	3	2	0	3	0	7	4	0	1	1	1	0	1
8	3	10	22	1	2	3	2	1	2	2	16	4	0	0	0	0	0	0
1	6	0	23	1	3	1	1	2	1	2	0	1	0	0	0	0	0	0
8	6	0	11	2	1	2	2	4	1	0	3	1	0	1	1	0	0	1
8	5	0	20	1	3	3	1	4	3	0	9	4	0	1	1	0	0	1
1	5	2	8	1	3	3	2	2	5	0	15	0	0	1	1	0	0	0
8	5	6	7	2	2	3	3	4	3	0	9	1	0	1	1	0	0	1
1	3	13	8	2	6	3	2	2	2	0	15	1	0	1	1	1	0	1
8	5	5	21	1	3	3	2	5	3	0	4	2	0	1	1	0	0	1
1	11	0	24	1	4	3	1	0	5	0	1	3	0	0	1	0	0	1
8	2	14	2	2	3	0	2	5	5	2	16	4	3	0	0	0	0	0
8	0	0	9	2	4	6	2	3	0	0	15	1	0	1	1	0	0	1
8	5	0	8	2	2	2	2	4	3	0	14	2	0	1	1	0	1	1
8	5	7	9	2	2	1	3	4	3	0	2	2	0	1	1	0	1	0
1	3	0	10	2	4	3	2	2	5	0	15	4	0	1	1	1	1	1
8	0	0	9	2	3	6	2	3	0	0	9	3	0	1	1	0	0	1
1	6	9	7	2	2	1	2	2	1	1	2	0	1	1	1	1	0	1
8	5	0	19	1	6	3	1	4	3	0	7	1	0	1	1	0	0	1
8	7	0	24	1	2	1	1	5	4	0	6	0	0	1	1	0	0	1
8	1	11	19	1	7	3	3	1	5	0	1	0	0	1	1	0	1	1
8	6	2	7	2	1	1	2	4	1	0	12	1	0	1	1	1	0	1
1	0	9	8	2	4	6	3	5	0	2	16	4	3	0	0	0	0	0
1	0	0	11	2	2	6	2	0	5	0	1	0	0	1	1	0	1	1
8	6	0	13	2	3	3	2	4	1	0	6	1	1	1	1	0	0	1
8	3	0	15	2	5	0	2	4	2	0	13	1	0	1	1	0	1	1
1	5	2	8	2	4	0	3	5	3	0	5	2	0	0	0	0	0	0
1	6	2	8	1	1	1	2	0	1	2	1	4	0	0	0	0	0	0
8	6	0	13	2	2	3	2	4	1	0	9	0	0	1	1	1	1	1
1	7	0	16	2	4	3	2	0	4	0	8	1	0	1	1	1	0	1
1	1	0	22	1	1	3	1	5	5	2	0	4	0	0	0	0	0	0
8	5	14	17	1	2	3	3	5	3	0	10	4	0	1	1	1	0	1
1	5	0	17	2	5	3	2	0	3	2	16	4	3	0	0	0	0	0
8	2	2	7	2	3	0	2	4	5	2	3	3	0	0	0	0	0	0
8	0	0	4	3	1	0	3	3	0	2	16	4	3	0	0	0	0	0
1	3	0	13	2	5	3	2	0	2	2	3	3	0	1	1	0	0	0
1	7	0	31	1	3	6	1	5	5	0	6	1	0	1	1	0	0	1
8	7	0	14	2	4	3	2	4	4	0	10	1	0	1	1	1	0	1
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	6	11	5	1	2	3	2	2	1	0	3	1	1	1	1	0	0	1
1	3	2	15	1	4	3	2	5	2	0	7	1	1	1	1	1	0	0
8	5	0	19	2	1	1	2	4	3	0	4	0	2	1	1	0	0	1
1	3	0	10	2	2	2	2	2	2	0	6	1	0	1	1	1	1	1
8	3	0	19	2	3	3	2	4	2	0	12	0	1	1	1	0	0	0
8	6	2	16	2	3	2	3	5	1	2	16	4	1	0	0	0	0	0
1	6	2	5	1	1	1	2	2	1	2	16	4	3	0	0	0	0	0
8	3	0	17	2	3	3	2	4	2	1	8	0	1	0	0	0	0	0
1	3	0	12	2	3	3	2	0	2	0	14	1	0	1	0	0	0	0
8	6	0	16	2	4	3	2	4	1	2	16	4	3	0	0	0	0	0
8	3	0	18	2	2	4	2	4	2	0	7	1	0	1	1	1	0	0
1	0	0	9	2	1	6	2	5	0	0	6	4	1	1	0	0	0	0
1	11	0	20	1	2	3	1	0	5	0	3	0	0	1	1	1	1	1
1	0	0	10	2	2	6	2	5	0	0	4	0	0	1	0	1	0	0
8	7	6	6	1	3	3	2	4	4	0	8	1	0	1	1	1	0	1
1	2	12	10	2	2	0	2	3	5	2	1	3	0	0	0	0	0	0
1	7	7	31	1	4	2	2	5	5	0	9	1	0	1	1	1	0	1
8	7	0	9	2	2	1	2	4	4	0	2	4	1	1	1	0	0	0
1	6	0	14	2	3	3	2	0	1	0	3	4	0	1	1	1	0	1
8	7	2	10	2	1	1	3	1	4	0	5	4	0	1	0	0	0	0
8	3	0	13	2	1	6	2	1	2	2	16	3	0	1	0	0	0	0
8	3	5	2	2	3	3	2	5	2	0	8	1	0	1	1	1	0	1
1	3	0	7	2	2	5	2	5	2	0	15	1	0	1	1	0	0	0
8	3	6	9	2	4	3	3	4	2	2	16	4	3	0	0	0	0	0
0	3	0	7	2	3	3	2	5	5	0	15	1	0	0	1	1	0	0
1	11	2	4	2	3	1	2	2	5	2	16	4	3	0	0	0	0	0
1	1	0	24	1	5	3	1	2	5	0	11	1	0	1	1	1	1	1
8	3	0	15	2	3	3	2	1	5	0	8	1	0	1	1	1	1	1
8	3	0	8	2	1	3	2	4	2	2	16	4	3	0	0	0	0	0
1	3	0	19	1	2	3	1	5	5	2	16	4	3	0	0	0	0	0
1	7	0	22	1	3	3	1	0	4	0	3	2	0	1	1	0	1	1
8	11	0	10	2	1	3	2	4	5	0	3	4	1	1	1	0	0	0
1	3	9	13	2	3	2	3	5	2	0	5	3	0	1	1	0	0	0
1	7	13	29	0	3	3	3	0	5	0	4	1	0	1	1	0	0	1
8	3	5	4	2	4	3	2	4	2	0	8	1	0	1	1	0	0	1
1	5	10	20	1	2	1	3	2	3	0	5	4	2	1	0	0	0	0
1	3	1	2	1	1	3	1	0	5	1	2	1	0	1	1	1	1	0
1	11	5	3	2	5	3	2	2	5	0	14	1	0	1	1	1	0	1
1	5	0	14	2	3	3	2	5	3	0	8	1	0	1	1	1	0	1
1	3	6	1	1	3	3	1	5	2	0	7	1	0	1	1	0	0	0
1	0	9	8	2	1	6	2	3	0	0	2	1	1	1	1	1	0	1
1	7	12	11	1	4	3	2	5	4	2	16	4	3	0	0	0	0	0
8	0	0	8	2	1	5	2	5	0	2	1	3	2	0	0	0	0	0
1	6	11	14	2	1	3	3	4	1	0	13	1	0	1	1	1	0	1
8	0	0	9	2	3	6	2	3	0	0	1	2	0	1	0	0	0	0
1	0	2	5	2	1	0	2	2	5	2	16	4	3	0	0	0	0	0
1	0	11	2	2	1	6	2	5	0	0	5	0	2	1	0	0	0	0
1	3	0	9	2	2	2	2	0	2	0	6	1	0	1	1	0	0	1
1	6	11	11	2	3	3	3	2	1	2	5	4	1	1	1	1	0	1
8	3	6	1	2	1	6	2	5	2	2	16	4	3	0	0	0	0	0
1	0	2	12	2	2	0	3	2	5	2	2	2	0	1	1	0	0	0
1	3	0	20	1	2	6	1	5	2	0	4	0	0	1	1	1	0	1
8	3	0	17	2	3	3	2	4	5	0	2	0	0	1	1	0	0	1
8	3	0	13	2	2	3	2	5	2	0	4	1	0	1	1	1	0	0
1	11	0	31	1	2	3	1	0	5	0	4	1	0	1	1	0	1	1
1	7	2	4	1	1	3	2	2	4	2	16	4	3	0	0	0	0	0
8	3	7	10	2	1	4	2	5	2	0	15	1	0	1	1	1	0	1
1	7	2	4	1	1	3	2	5	4	2	16	4	0	0	0	0	0	0
8	3	0	10	2	2	2	2	1	2	0	7	1	0	1	1	0	0	1
1	3	2	22	1	2	3	2	5	2	2	16	4	3	0	0	0	0	0
1	5	3	9	2	2	3	3	5	3	0	4	1	0	1	1	1	0	0
1	7	11	8	2	1	3	3	2	4	0	3	2	0	1	1	0	0	1
8	3	0	12	2	6	3	2	1	2	0	10	1	1	1	1	0	0	1
8	0	9	8	2	2	6	3	3	0	2	16	4	3	0	0	0	0	0
8	3	2	7	1	2	3	2	4	2	0	2	1	0	0	0	0	0	0
8	2	13	3	2	3	0	2	4	5	2	16	4	3	0	0	0	0	0
1	5	0	15	2	3	3	2	2	3	0	4	1	0	1	0	0	0	0
1	5	0	10	2	5	2	2	5	3	2	16	4	3	0	0	0	0	0
8	3	0	15	2	3	3	2	4	2	0	4	1	0	1	0	1	0	0
8	2	2	5	2	2	0	2	4	5	2	16	4	3	0	0	0	0	0
8	3	5	8	2	3	6	2	1	5	2	16	4	2	0	0	0	0	0
8	6	6	4	1	5	3	2	4	1	0	12	1	0	1	1	0	1	1
1	7	3	10	1	3	3	1	2	4	2	16	4	3	0	0	0	0	0
8	3	0	8	2	1	2	2	4	2	2	16	4	1	0	0	0	0	0
1	3	0	10	2	4	2	2	5	5	0	7	1	0	1	1	0	0	1
8	6	0	16	2	3	3	2	4	1	0	4	4	0	1	1	0	1	1
8	3	2	16	2	2	1	3	5	2	2	16	4	3	0	0	0	0	0
8	5	2	25	1	2	3	3	1	5	0	5	0	0	1	0	0	0	0
1	3	6	7	2	2	2	3	5	2	0	6	1	0	1	1	0	1	1
8	7	0	31	1	1	3	1	1	5	2	16	4	0	1	0	1	0	0
1	5	1	10	2	1	1	3	0	3	0	15	1	0	1	1	0	0	0
8	6	6	1	1	2	7	2	4	1	2	16	4	3	0	0	0	0	0
1	3	0	8	2	4	3	2	0	2	0	15	1	0	1	1	1	0	1
1	7	0	14	2	5	1	2	5	5	0	10	1	0	1	1	0	0	0
1	6	2	6	2	1	1	3	2	1	0	3	4	0	1	1	0	0	1
8	3	0	31	1	4	3	1	1	2	0	4	3	0	1	1	0	0	1
1	9	0	12	2	1	0	2	0	5	0	2	3	0	1	1	0	0	1
8	3	0	18	2	2	3	2	4	2	0	2	1	0	0	0	0	0	0
1	7	14	1	2	2	2	3	0	4	2	16	4	3	0	0	0	0	0
8	3	6	7	2	3	2	3	5	2	2	16	4	3	0	0	0	0	0
8	3	6	1	2	2	3	2	4	2	0	4	1	0	1	0	1	0	0
1	5	5	13	1	3	3	2	5	5	2	16	4	3	0	0	0	0	0
8	7	12	2	1	1	3	1	1	5	2	16	4	3	0	0	0	0	0
8	7	13	17	1	4	3	3	1	4	0	15	1	0	1	1	1	0	0
2	3	0	11	2	4	3	2	5	2	0	8	0	0	1	1	1	0	0
8	6	14	18	2	3	3	3	4	1	0	4	0	0	1	1	1	1	1
8	7	0	20	1	2	1	1	5	4	0	4	4	1	0	1	0	0	0
1	11	0	14	2	3	3	2	5	5	0	8	1	0	1	1	1	0	1
8	11	10	19	2	2	1	2	4	5	0	4	0	2	0	0	0	0	0
8	3	0	22	1	5	3	1	1	2	0	1	1	1	1	1	0	0	0
8	5	0	10	2	4	3	2	4	3	0	3	1	0	1	1	1	0	1
8	3	0	19	1	6	3	1	1	2	0	14	0	0	1	1	1	0	0
1	5	2	7	1	3	3	1	2	5	0	13	1	0	1	1	1	0	1
8	1	0	25	1	3	3	1	5	5	0	11	1	0	1	0	1	0	0
1	9	0	12	2	3	3	2	5	5	0	7	1	0	1	1	1	1	1
8	7	0	12	2	1	1	2	5	5	2	16	4	3	0	0	0	0	0
1	3	0	13	2	3	3	2	2	2	0	5	1	0	1	0	0	0	0
1	6	3	12	2	5	6	3	5	1	0	1	4	1	1	1	1	0	1
8	7	7	7	2	1	3	2	5	4	0	2	3	0	1	1	0	1	1
8	7	2	11	2	1	6	3	5	4	2	16	4	3	0	0	0	0	0
1	6	0	12	2	3	3	2	5	1	0	4	2	0	1	1	1	0	1
8	6	6	5	2	1	3	2	5	1	2	16	4	3	0	0	0	0	0
1	3	14	2	2	2	3	2	5	2	0	5	1	0	1	1	0	0	0
1	3	0	18	2	3	0	2	5	2	0	6	1	0	1	1	0	0	1
1	7	0	12	2	1	2	2	5	4	0	9	2	0	1	1	0	0	1
8	1	0	22	1	3	5	1	5	5	2	16	4	3	0	0	0	0	0
8	7	0	10	2	2	3	2	4	4	0	12	1	0	1	1	0	0	1
8	3	0	14	2	3	3	2	4	2	0	3	1	0	1	1	0	0	1
1	6	9	5	1	1	3	2	2	1	0	2	1	0	1	1	0	1	1
8	3	0	10	2	2	2	2	4	2	0	1	1	0	1	1	0	0	1
1	11	0	31	1	2	3	1	5	5	0	6	1	0	1	1	1	0	1
1	3	9	2	2	1	0	2	5	2	0	7	1	0	1	1	1	0	1
8	3	14	17	2	1	3	2	5	2	0	4	0	0	0	1	0	0	1
1	7	0	10	2	3	2	2	5	4	0	6	2	0	1	1	0	1	1
8	1	2	9	1	2	5	2	1	5	0	4	1	0	1	1	1	0	0
8	3	0	13	2	2	6	2	1	2	0	1	1	0	0	0	0	0	0
8	3	0	12	2	4	1	2	4	2	2	0	4	1	0	0	0	0	0
1	3	0	16	2	3	3	2	2	2	0	6	1	0	1	1	0	0	0
8	0	0	8	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	7	0	19	1	4	1	1	5	4	2	6	4	1	0	0	0	0	0
8	6	2	7	1	3	2	2	1	1	2	8	4	0	1	1	1	0	0
1	6	14	2	1	4	1	1	2	1	0	1	0	0	1	1	0	0	0
8	5	0	12	2	2	3	2	4	3	0	5	1	0	1	1	0	1	1
8	8	0	15	2	4	3	2	1	5	0	15	1	0	1	1	1	1	1
8	5	0	20	1	3	3	1	4	3	0	1	4	0	1	0	0	0	0
8	7	2	9	1	1	1	1	1	5	0	4	4	1	0	0	0	0	0
8	7	0	27	1	4	2	1	1	4	1	6	0	1	1	1	0	0	0
1	11	2	5	2	1	1	2	5	5	0	2	1	1	1	0	0	0	0
1	5	2	7	2	1	1	2	2	3	0	15	4	1	0	0	0	0	0
8	5	0	9	2	2	5	2	4	3	0	7	4	0	1	1	0	1	1
1	7	2	11	1	1	3	2	2	5	0	8	4	0	1	1	0	0	1
1	3	9	21	1	3	3	3	0	2	2	16	4	3	0	0	0	0	0
1	7	7	2	1	1	3	1	2	5	2	16	4	3	0	0	0	0	0
8	6	12	12	2	2	3	3	5	1	0	6	1	0	1	1	0	0	0
8	7	2	10	2	3	0	3	4	4	2	1	4	1	1	1	1	0	1
8	0	11	9	2	2	6	2	3	0	0	15	1	0	1	1	1	0	1
8	3	10	17	2	2	2	3	1	2	0	8	1	0	1	1	0	0	0
8	7	0	9	2	3	3	2	4	4	0	6	1	0	1	1	1	1	1
1	6	0	11	2	2	2	2	2	1	0	4	0	0	1	1	0	0	1
1	3	10	14	2	1	3	3	5	2	0	10	4	0	1	1	1	0	1
8	5	9	18	2	1	1	2	5	3	2	16	4	3	0	0	0	0	0
1	0	0	1	2	4	6	2	5	5	0	6	1	0	1	1	0	0	0
8	3	2	6	2	3	1	2	1	2	2	3	0	1	1	0	1	0	0
8	5	0	20	1	2	3	1	4	5	0	2	1	0	0	0	0	0	0
8	3	0	20	1	3	3	1	4	2	0	16	0	1	1	0	0	0	0
8	5	2	11	2	1	4	2	5	3	2	16	4	3	0	0	0	0	0
1	7	3	16	1	2	0	2	5	4	2	16	0	1	1	1	0	0	1
1	7	0	16	2	1	2	2	2	4	2	16	4	3	0	0	0	0	0
4	9	5	5	2	2	2	3	2	5	0	4	4	0	1	1	0	0	0
8	6	0	16	2	2	3	2	4	1	0	2	0	1	1	1	0	1	1
8	5	0	16	2	2	1	2	4	3	2	1	4	0	0	0	0	0	0
8	7	0	14	2	2	2	2	4	4	0	2	4	0	1	1	0	1	1
1	5	6	1	2	3	3	2	5	3	2	16	4	3	0	0	0	0	0
8	7	0	10	2	3	2	3	4	4	0	2	1	1	1	1	0	1	1
8	0	0	7	3	1	0	3	3	0	2	16	4	3	0	0	0	0	0
8	3	9	6	2	2	3	2	4	2	0	3	0	0	1	1	0	0	1
8	6	3	7	2	1	3	3	1	1	2	2	3	1	1	1	0	0	1
1	7	2	12	1	2	3	2	0	4	2	5	3	0	0	0	0	0	0
1	3	0	13	2	3	0	2	0	2	2	2	4	1	1	0	0	0	0
1	7	2	9	1	1	1	1	2	5	0	3	4	1	1	0	0	0	0
8	7	0	12	2	1	1	2	4	4	2	16	4	3	0	0	0	0	0
8	3	0	12	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
1	3	9	10	2	4	3	3	2	2	0	15	1	0	1	1	0	0	1
8	7	5	7	2	3	3	3	4	4	2	16	4	3	0	0	0	0	0
1	7	0	12	2	3	1	2	0	5	0	3	1	0	1	1	0	0	1
1	3	2	5	2	4	2	3	0	2	0	6	4	1	1	1	1	0	1
1	5	0	22	1	1	2	1	5	3	0	15	4	0	1	1	1	0	0
1	6	2	14	2	2	1	3	2	1	2	16	4	3	0	0	0	0	0
1	7	10	16	2	3	3	2	0	4	0	3	4	0	1	1	0	0	1
8	5	10	17	2	2	3	2	5	3	2	16	4	3	0	0	0	0	0
8	6	8	1	2	1	1	2	1	1	1	3	4	1	1	0	0	0	0
8	3	6	2	2	4	7	2	5	2	0	15	4	1	0	0	0	0	0
1	11	10	19	2	1	0	2	0	5	0	15	1	0	1	1	1	0	1
1	9	10	23	1	3	3	3	2	5	0	6	4	1	1	1	0	0	0
1	3	0	11	2	2	2	2	5	2	0	3	1	0	1	1	0	0	1
8	5	9	9	2	1	1	3	1	3	0	15	4	0	0	0	0	0	0
1	3	0	16	2	3	3	2	4	2	2	16	4	3	0	0	0	0	0
8	3	2	17	2	3	2	3	1	2	0	6	2	0	1	1	0	0	1
8	0	12	10	1	4	6	2	5	5	2	16	4	3	0	0	0	0	0
1	5	0	11	2	3	2	2	0	5	0	2	2	0	1	1	1	0	1
8	5	2	9	2	2	1	2	4	3	2	16	4	3	0	0	0	0	0
8	5	2	7	2	2	2	3	5	3	0	7	2	0	1	1	0	1	1
8	0	0	9	2	1	0	2	5	0	2	1	0	0	1	1	0	0	0
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	0	16	2	3	3	2	5	2	0	6	1	0	1	1	1	0	1
1	3	12	14	2	2	3	3	0	2	0	14	1	0	1	1	1	0	1
1	1	5	1	1	1	3	1	5	5	2	16	4	3	0	0	0	0	0
1	5	0	10	2	3	2	2	5	3	0	3	2	0	1	1	0	0	0
1	3	0	13	2	4	3	2	5	2	0	9	0	1	1	1	0	0	0
1	3	0	8	2	6	2	2	0	2	2	0	4	0	0	0	0	0	0
1	7	0	31	0	3	1	0	0	5	0	2	2	0	1	1	0	1	1
8	7	0	14	2	2	3	2	5	4	0	3	0	2	1	0	0	0	0
8	3	0	16	2	4	3	2	4	2	2	16	4	3	0	0	0	0	0
1	7	10	11	2	2	0	3	0	4	2	5	4	2	1	1	0	0	0
1	3	0	11	2	2	2	2	2	2	0	6	1	0	1	1	1	0	1
1	3	0	13	2	3	1	2	5	2	2	0	0	0	0	0	0	0	0
8	9	0	31	0	3	3	0	1	5	0	15	0	0	1	0	0	1	0
1	6	0	14	2	2	3	2	0	1	0	2	1	0	1	1	1	0	1
8	2	5	2	2	2	6	2	4	5	0	11	1	0	1	1	1	0	0
1	7	0	31	1	3	3	1	2	5	0	15	1	0	1	1	0	1	1
1	1	0	23	1	4	3	1	2	5	0	4	1	0	1	0	1	0	0
1	6	14	2	2	1	1	2	5	1	0	15	1	0	1	1	0	1	1
8	1	6	21	1	1	2	3	5	5	2	1	3	0	0	0	0	0	0
1	7	5	1	2	4	2	2	4	4	2	2	4	0	1	1	0	0	0
8	0	9	9	2	2	6	3	3	0	2	16	4	3	0	0	0	0	0
1	6	0	10	2	3	2	2	2	1	0	1	4	1	0	1	0	0	0
1	3	0	11	2	4	2	2	0	2	0	10	4	0	1	1	1	0	1
8	3	7	1	2	2	3	2	4	2	0	3	4	0	1	1	0	0	1
1	11	10	31	1	3	1	3	5	5	0	8	3	0	0	0	0	0	0
8	5	12	7	2	3	1	2	5	3	2	16	4	1	1	0	0	0	0
1	11	0	22	1	3	3	1	5	5	2	16	4	3	0	0	0	0	0
8	11	4	2	2	2	3	2	4	5	2	16	4	3	0	0	0	0	0
8	3	7	10	2	5	3	2	5	2	2	16	4	3	0	0	0	0	0
1	3	0	11	2	2	3	2	5	2	0	1	1	0	1	1	1	1	1
1	0	5	8	2	1	2	3	2	0	2	16	4	3	0	0	0	0	0
8	3	2	14	2	3	4	3	1	2	0	5	0	1	1	0	0	0	0
1	5	11	1	1	7	3	1	2	5	0	5	1	0	1	1	1	0	1
8	3	7	1	2	3	3	2	4	2	0	7	1	1	0	0	0	0	0
1	7	11	29	1	2	3	2	2	5	0	1	1	0	1	1	0	0	1
8	3	0	15	2	4	3	2	1	2	0	5	1	0	1	1	0	0	1
1	2	12	10	2	1	6	2	5	5	2	1	0	1	0	0	0	0	0
1	6	6	11	2	4	2	3	2	1	2	16	4	3	0	0	0	0	0
8	2	2	10	1	2	0	2	1	5	0	2	0	1	1	1	1	0	0
1	7	10	8	2	4	2	3	5	4	0	6	3	0	1	1	0	0	1
8	3	0	19	1	3	3	1	4	2	0	13	1	0	1	1	0	0	1
8	6	0	11	2	1	3	2	4	1	2	16	4	3	0	0	0	0	0
1	11	0	16	2	3	3	3	5	5	0	15	1	0	1	1	0	0	0
1	3	0	8	2	2	3	2	0	2	2	0	4	1	0	0	0	0	0
8	6	2	4	2	1	1	2	1	1	0	10	1	0	1	1	1	0	1
8	1	0	21	1	3	3	1	5	5	0	15	0	0	1	1	1	0	1
1	7	10	31	0	3	0	3	0	5	0	12	1	0	0	0	0	0	0
1	11	2	3	2	1	1	2	2	5	2	3	4	0	0	0	0	0	0
1	3	0	12	2	2	3	2	0	2	0	1	1	0	1	1	1	1	1
8	3	0	11	2	3	3	2	5	2	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	0	2	5	0	0	4	1	0	1	1	0	0	0
1	3	0	21	1	2	3	1	2	2	0	4	1	0	1	0	0	0	0
1	6	6	10	2	3	2	2	5	1	0	5	2	0	1	0	1	0	0
8	3	0	20	1	3	3	3	4	2	2	16	4	0	0	0	0	0	0
1	0	9	9	2	1	6	3	5	0	2	16	4	3	0	0	0	0	0
1	3	0	11	2	4	3	2	0	2	0	9	1	0	1	1	1	0	0
1	11	0	13	2	2	3	2	5	5	0	2	1	0	1	0	1	0	0
1	5	11	9	1	2	3	2	2	5	0	4	2	0	1	1	0	0	1
8	5	2	12	2	2	1	3	1	3	0	3	0	0	1	1	0	0	1
8	9	0	27	1	1	0	1	5	5	2	2	3	0	0	0	0	0	0
1	9	0	18	2	1	1	2	2	5	2	16	4	3	0	0	0	0	0
1	3	1	4	2	7	2	3	5	2	0	7	2	0	0	1	0	0	0
8	7	2	3	2	4	2	3	4	4	2	16	4	3	0	0	0	0	0
1	0	9	8	2	8	6	3	5	0	2	16	4	3	0	0	0	0	0
1	3	0	21	1	4	2	1	5	2	0	15	0	0	1	1	1	0	1
8	5	14	15	1	1	2	3	5	3	2	16	4	3	0	0	0	0	0
8	5	13	18	2	2	1	3	1	3	2	2	0	0	1	1	0	0	0
8	11	14	1	2	3	1	2	4	5	2	16	4	3	0	0	0	0	0
1	9	0	25	1	2	3	1	0	5	0	3	1	1	1	1	0	0	1
8	0	0	8	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	0	13	2	4	2	2	2	5	0	15	1	0	1	1	0	0	1
1	3	0	10	2	2	2	2	0	5	0	1	2	0	1	1	1	0	1
1	6	9	2	2	3	6	2	5	1	0	6	4	1	0	0	0	0	0
8	7	3	3	2	1	3	2	4	4	2	16	4	3	0	0	0	0	0
1	11	0	28	1	2	3	1	2	5	0	6	1	0	1	1	1	1	0
8	3	0	8	2	4	2	2	4	2	0	9	0	0	1	1	0	1	1
8	3	0	19	2	4	3	2	4	2	0	10	1	0	1	1	1	0	1
1	5	0	20	1	4	3	1	5	3	0	1	1	0	1	1	0	0	0
1	1	0	16	2	1	1	2	5	5	2	16	0	2	1	1	0	0	1
1	3	0	13	2	4	3	2	5	2	0	7	2	0	1	1	1	1	1
8	7	6	14	2	4	3	3	4	4	2	16	4	3	0	0	0	0	0
1	3	0	13	2	3	3	2	0	2	0	5	1	0	1	1	0	0	0
1	5	0	9	2	2	2	2	2	3	0	5	4	2	1	0	0	0	0
1	6	6	16	1	3	3	3	5	1	0	9	4	1	1	1	0	0	0
8	3	7	5	2	1	1	2	1	2	2	16	4	2	0	0	0	0	0
1	3	0	9	2	2	3	2	5	2	0	4	1	0	1	0	0	0	0
8	5	7	13	1	3	3	2	1	3	2	1	1	0	0	0	0	0	0
1	5	10	14	2	3	3	2	5	3	0	5	0	1	1	1	1	0	1
8	8	0	31	0	1	3	2	1	5	2	1	0	0	1	1	0	1	1
8	5	1	9	2	4	3	3	5	3	0	15	0	1	1	1	0	0	0
8	1	0	13	2	1	3	2	5	5	2	16	4	3	0	0	0	0	0
1	9	2	5	2	2	3	2	2	5	0	3	0	2	1	1	0	0	0
1	3	9	14	2	2	1	3	2	2	0	16	0	0	1	1	0	0	1
1	3	0	10	2	3	3	2	2	2	2	0	0	1	0	0	0	0	0
8	5	0	19	1	1	3	1	1	3	0	3	0	0	1	1	0	1	1
1	6	0	8	2	4	2	2	4	1	0	15	1	0	1	1	1	0	1
1	7	0	5	2	3	7	2	5	5	2	16	4	3	0	0	0	0	0
8	7	0	10	2	3	3	2	4	4	2	16	3	1	0	0	0	0	0
1	6	0	13	2	2	3	2	5	1	0	4	1	0	1	1	0	0	1
1	7	0	23	1	3	3	1	0	4	0	11	1	1	1	1	0	0	1
8	0	0	9	2	4	6	2	5	0	0	14	0	0	1	1	1	0	0
1	5	10	23	1	3	3	3	0	5	0	4	1	0	1	1	1	0	1
8	3	0	13	2	3	3	2	4	2	0	7	1	0	1	1	1	1	1
8	5	7	11	1	3	3	2	5	3	2	16	4	3	0	0	0	0	0
1	6	0	20	2	1	1	2	2	5	0	15	1	0	1	1	1	0	1
1	7	9	20	1	5	3	3	0	4	0	15	1	0	1	0	1	1	0
8	3	3	4	2	2	0	3	1	2	2	1	0	1	0	0	0	0	0
1	9	10	14	2	2	2	3	0	5	2	0	4	0	0	0	0	0	0
1	0	0	9	2	2	6	2	5	0	0	3	4	0	1	0	0	0	0
8	7	2	10	0	2	2	1	1	5	0	7	1	1	1	1	0	0	1
1	6	0	22	1	4	3	1	5	1	0	8	3	0	1	1	0	0	0
1	11	2	6	2	2	1	2	2	5	2	16	4	3	0	0	0	0	0
8	5	2	8	1	3	1	2	4	5	0	2	0	0	1	1	1	1	1
1	3	0	19	1	4	3	1	5	2	0	4	1	0	0	0	0	0	0
8	3	7	4	2	5	3	2	4	2	2	16	4	3	0	0	0	0	0
1	6	0	17	2	1	1	2	2	1	2	1	0	1	1	1	0	0	1
1	3	0	20	1	4	3	1	5	2	0	9	0	0	1	1	0	0	0
8	3	0	12	2	4	3	2	4	2	2	16	4	3	0	0	0	0	0
1	5	0	31	0	4	3	0	2	5	2	16	4	3	0	0	0	0	0
8	3	0	13	2	4	3	2	4	2	0	1	1	0	1	1	0	0	1
8	6	14	6	2	3	3	2	1	1	0	4	0	0	1	1	0	0	0
8	2	14	3	2	1	6	2	5	5	2	16	4	3	0	0	0	0	0
1	9	0	11	2	3	3	2	5	5	2	0	0	0	0	0	0	0	0
1	5	14	24	1	1	3	1	2	5	2	0	4	2	0	0	0	0	0
1	3	12	8	2	2	3	3	5	2	2	6	0	0	0	0	0	0	0
1	11	12	31	0	5	7	3	2	5	0	5	4	0	1	1	1	1	1
8	7	2	5	1	1	3	2	1	4	0	5	0	1	1	1	0	0	0
8	3	13	15	1	1	3	3	5	2	2	1	0	1	1	1	0	0	1
1	6	14	2	2	3	2	3	5	1	0	6	3	2	1	1	0	0	1
1	1	0	23	1	3	3	1	2	5	0	10	1	0	1	1	1	0	1
8	5	0	32	0	4	3	0	1	5	0	5	0	0	0	0	0	0	0
1	5	8	16	2	2	1	3	5	3	2	16	4	3	0	0	0	0	0
8	6	3	2	1	3	6	2	1	1	2	1	4	0	0	0	0	0	0
0	6	2	4	2	1	1	2	5	1	2	16	4	3	0	0	0	0	0
8	2	9	9	2	4	0	2	1	5	2	16	4	3	0	0	0	0	0
1	3	2	5	2	3	3	2	5	2	0	14	0	0	1	1	1	0	0
8	5	2	11	2	1	6	3	1	3	2	16	4	1	0	0	0	0	0
1	3	0	8	2	1	2	2	5	5	2	1	4	0	0	0	0	0	0
8	6	7	28	1	2	1	1	1	5	0	2	2	0	1	1	0	0	0
8	3	7	13	2	3	4	3	4	2	2	16	4	3	0	0	0	0	0
1	11	0	30	1	3	2	1	0	5	0	1	1	0	1	1	0	0	1
8	7	0	12	2	2	3	2	4	5	0	2	1	0	1	1	0	0	1
1	3	0	13	2	1	5	2	2	2	2	2	4	0	0	0	0	0	0
1	6	5	2	1	3	2	1	0	5	0	15	4	0	1	1	0	0	1
1	3	0	20	1	2	3	1	0	2	0	7	1	0	1	0	0	0	0
1	3	0	16	2	4	3	2	2	2	0	11	1	0	0	1	0	0	0
1	7	8	31	0	3	3	3	2	5	0	3	1	0	1	1	1	0	1
8	5	0	15	2	2	3	2	4	3	0	4	0	0	1	1	0	1	1
1	7	2	14	2	1	6	2	5	4	0	3	1	0	1	1	0	0	1
8	6	0	15	2	4	3	2	4	1	0	8	1	0	1	1	1	0	1
8	6	13	12	2	2	6	2	1	1	2	16	4	3	0	0	0	0	0
1	11	7	24	1	3	3	3	5	5	2	16	4	3	0	0	0	0	0
8	5	0	9	2	1	2	2	4	3	2	16	4	3	0	0	0	0	0
1	11	14	15	2	1	3	2	5	5	0	1	1	0	1	1	0	0	1
8	6	2	10	2	2	2	3	5	1	0	8	1	0	1	1	0	1	1
8	3	2	6	2	1	1	2	5	2	0	4	3	1	0	0	0	0	0
1	5	0	15	2	3	3	2	0	3	0	9	2	0	1	1	1	1	1
8	3	0	13	2	4	2	3	4	2	2	16	4	1	0	0	0	0	0
8	3	2	4	2	3	2	3	4	2	2	16	4	3	0	0	0	0	0
8	6	13	5	2	3	3	2	4	1	0	7	1	0	1	1	0	0	1
1	7	0	8	2	4	6	2	5	4	0	6	4	0	0	1	0	0	1
1	5	8	19	2	2	3	2	5	3	0	5	1	1	1	1	1	0	1
1	0	0	7	3	2	0	3	3	0	2	6	4	0	0	0	0	0	0
8	1	0	23	1	2	2	1	1	5	0	8	1	0	1	1	1	0	1
1	11	2	4	1	1	1	2	5	5	2	1	0	0	0	0	0	0	0
8	0	10	9	2	9	6	3	3	0	2	16	4	3	0	0	0	0	0
1	5	2	5	2	1	1	2	2	3	0	9	4	1	1	1	0	0	0
8	11	14	1	2	3	3	2	4	5	2	16	4	3	0	0	0	0	0
1	6	2	3	2	5	2	2	2	1	0	15	0	0	1	1	0	0	1
8	3	0	27	1	3	5	1	1	2	0	2	1	0	0	0	0	0	0
7	7	7	10	2	2	1	3	4	4	0	15	2	0	1	1	0	0	1
1	11	9	31	1	3	5	3	2	5	2	1	1	1	1	0	0	0	0
1	11	0	27	1	2	3	1	2	5	2	16	4	3	0	0	0	0	0
8	3	14	19	2	4	3	2	4	2	0	5	1	0	1	0	1	0	0
1	7	0	15	2	3	3	2	0	4	0	5	0	0	1	1	0	0	1
1	6	2	4	1	2	1	2	5	1	2	16	4	3	0	0	0	0	0
8	7	11	6	0	3	3	1	1	5	0	15	0	0	1	1	1	0	1
8	6	5	3	2	1	2	3	4	1	0	1	0	0	1	0	0	0	0
1	9	0	11	2	1	2	2	4	5	0	9	1	0	1	1	1	1	1
1	3	9	20	1	2	2	2	5	2	0	2	1	0	1	1	1	0	1
3	3	1	14	2	5	3	3	2	2	0	8	1	0	1	1	1	0	0
1	2	12	14	1	3	0	2	5	5	0	15	1	0	0	1	0	0	0
8	3	0	15	2	2	3	2	4	2	0	1	1	0	1	1	0	1	1
8	7	0	14	2	3	3	2	1	4	0	14	1	0	1	1	0	0	1
8	0	0	9	2	2	6	2	5	0	2	3	2	0	1	0	0	0	0
1	7	2	5	2	4	3	2	2	4	0	7	0	1	1	1	0	0	1
1	5	0	10	2	3	3	2	2	3	0	12	0	0	1	1	0	0	1
8	3	0	12	2	3	3	2	1	2	0	5	1	0	1	1	1	0	1
3	9	5	23	1	5	3	2	2	5	0	15	4	0	1	1	1	0	1
1	7	0	9	2	6	3	2	5	4	2	16	4	3	0	0	0	0	0
8	3	0	11	2	3	3	2	4	2	0	2	3	1	0	0	0	0	0
1	7	13	1	1	4	3	1	2	4	0	6	0	0	0	1	0	0	1
1	3	0	20	1	5	1	1	5	2	0	9	1	0	1	1	0	1	1
1	3	5	10	1	5	3	2	5	2	0	8	1	0	1	1	0	0	1
1	0	0	8	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
8	0	0	8	2	4	6	2	3	0	0	6	0	1	1	1	1	0	0
8	6	10	14	2	5	1	2	5	1	2	15	4	1	1	1	0	0	0
8	11	4	3	2	2	1	2	4	5	2	16	4	3	0	0	0	0	0
1	3	0	12	2	5	3	2	0	2	0	6	1	1	1	1	1	0	1
8	3	0	15	2	4	3	2	4	2	0	14	1	0	1	1	1	0	1
1	5	3	11	2	4	1	3	2	3	0	9	1	0	1	1	0	0	0
8	1	0	22	1	1	3	1	1	5	2	16	4	3	0	0	0	0	0
1	3	0	11	2	3	3	2	5	2	0	15	0	0	1	1	1	0	1
8	2	5	6	2	4	0	3	4	5	2	3	3	0	1	1	0	0	1
1	3	0	17	2	4	3	2	0	2	0	9	4	0	1	1	1	0	1
8	3	0	13	2	4	3	2	5	2	0	1	0	0	1	1	0	1	0
7	3	6	16	1	6	3	3	4	2	2	16	4	3	0	0	0	0	0
8	3	0	12	2	3	2	2	5	2	0	6	1	0	1	1	1	0	1
8	7	0	10	2	3	4	2	4	4	2	16	4	3	0	0	0	0	0
8	5	6	17	1	2	3	3	4	3	0	4	3	0	1	0	1	0	0
8	3	5	13	2	3	3	3	4	2	2	16	4	3	0	0	0	0	0
1	7	0	31	1	3	3	1	0	5	2	16	4	3	0	0	0	0	0
1	6	2	8	1	1	1	2	0	1	0	10	4	0	1	1	0	0	1
8	3	2	8	2	2	3	2	5	2	2	8	1	0	0	0	0	0	0
1	5	0	13	2	2	3	2	5	3	0	2	2	0	1	1	0	0	1
1	5	10	25	1	2	3	2	2	5	0	4	1	0	1	1	0	0	1
8	5	0	20	1	4	3	1	4	3	0	8	1	0	1	1	0	1	1
8	7	1	28	1	4	3	1	1	5	0	1	3	0	1	1	0	0	0
1	6	0	21	1	4	3	1	0	1	0	15	1	0	1	1	1	0	1
8	6	5	5	2	4	3	2	4	1	2	16	4	3	0	0	0	0	0
1	7	0	20	1	2	3	1	0	4	0	3	2	0	1	1	0	0	1
1	7	9	2	2	3	3	2	0	4	0	6	1	0	1	1	1	0	1
1	9	6	1	1	3	3	1	5	5	0	5	4	0	0	0	0	0	0
8	7	0	12	2	1	1	3	4	4	2	8	0	1	1	1	0	0	1
1	7	5	1	2	3	5	2	2	4	0	15	0	0	1	1	1	0	0
8	5	2	11	2	2	3	3	4	3	0	2	0	0	1	1	0	0	1
1	3	0	10	2	3	3	2	2	2	0	1	1	0	1	1	0	0	1
1	5	0	13	2	2	3	2	2	3	0	3	1	0	1	1	0	1	0
8	11	0	20	1	5	3	1	5	5	0	2	1	1	1	1	0	1	1
1	7	2	5	2	2	1	2	2	4	0	3	4	1	1	1	0	0	0
8	3	3	14	1	1	3	3	5	2	0	11	4	0	1	0	0	0	0
1	3	0	20	1	2	3	1	5	2	2	3	2	0	1	1	0	0	1
8	6	8	5	2	2	3	2	4	1	0	2	3	0	1	1	0	0	0
1	0	0	8	2	1	6	2	5	0	0	3	4	0	1	1	0	0	1
1	3	0	15	2	2	1	2	2	2	2	16	4	3	0	0	0	0	0
8	3	8	12	1	4	5	2	1	2	0	3	0	0	0	1	1	0	0
8	3	2	9	2	2	6	3	5	2	2	16	4	3	0	0	0	0	0
8	3	0	20	1	4	3	1	4	2	0	12	0	1	1	1	1	0	1
1	3	14	11	2	4	2	2	5	2	0	9	1	0	1	0	1	0	0
8	7	12	12	2	1	1	3	4	4	2	16	4	3	0	0	0	0	0
1	1	12	18	1	1	3	3	2	5	2	2	4	0	0	0	0	0	0
8	3	0	15	2	4	3	2	4	2	0	4	1	0	1	1	0	0	0
1	0	0	1	3	1	0	3	5	0	2	16	4	3	0	0	0	0	0
8	7	5	20	1	2	3	2	4	4	0	5	2	0	1	1	0	0	1
1	3	6	4	1	1	6	2	0	2	2	5	0	1	0	0	0	0	0
8	5	6	6	1	3	3	2	4	3	0	8	2	0	1	1	0	0	1
8	6	1	3	2	2	1	2	5	1	0	5	0	1	0	0	0	0	0
1	0	11	7	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
8	3	0	12	2	2	2	2	4	2	0	8	0	0	1	1	0	0	1
1	6	2	11	1	3	1	1	2	5	0	10	2	0	1	1	0	0	0
1	5	0	15	2	3	3	3	5	3	0	7	1	0	1	1	1	0	0
1	11	2	4	2	1	1	2	2	5	2	16	4	3	0	0	0	0	0
8	3	0	14	2	1	3	2	4	2	2	16	4	3	0	0	0	0	0
8	3	0	15	2	2	3	2	4	2	0	13	0	0	1	1	1	0	1
1	7	0	31	1	4	3	1	0	5	0	5	1	0	1	1	1	1	1
8	7	5	27	1	2	3	2	1	5	0	3	1	0	1	1	0	1	1
8	3	8	1	1	2	3	1	4	2	1	3	0	1	1	0	0	0	0
8	0	0	9	2	3	6	2	3	0	0	5	0	1	1	1	0	0	0
8	7	0	9	2	3	2	2	4	4	0	11	4	0	1	1	0	0	1
1	3	0	8	2	2	3	2	5	2	0	8	1	0	1	1	1	1	1
1	0	0	9	2	2	0	2	5	0	0	15	1	1	1	1	1	0	1
8	0	0	2	3	1	0	3	3	0	2	16	4	3	0	0	0	0	0
1	7	0	20	1	3	5	1	0	4	0	7	2	0	0	1	0	0	0
1	0	14	8	2	3	6	3	3	0	2	1	4	0	0	0	0	0	0
1	7	6	2	1	6	6	1	0	5	0	4	0	0	1	1	0	0	1
1	7	0	31	0	2	3	0	5	5	0	4	1	0	1	0	0	0	0
8	3	0	12	2	4	3	2	4	2	0	6	4	0	1	1	1	0	0
1	3	5	16	1	3	0	2	0	5	0	7	1	0	1	1	0	0	1
1	7	13	15	1	4	3	1	0	5	0	7	1	0	1	1	1	1	1
3	5	5	11	2	4	3	3	5	3	2	1	1	0	1	1	1	0	0
8	5	5	12	2	3	3	2	4	3	0	2	3	0	1	1	0	0	1
1	7	0	21	1	4	3	1	2	4	0	8	0	0	1	1	0	0	1
1	5	0	8	2	3	2	2	5	3	0	5	0	0	1	1	0	0	1
1	1	12	12	1	1	3	2	0	5	2	1	3	1	0	0	0	0	0
8	6	0	16	2	1	3	2	1	1	2	1	3	0	0	0	0	0	0
1	5	14	5	1	4	6	2	5	3	2	0	1	1	0	0	0	0	0
1	3	0	19	2	3	3	2	0	2	0	1	1	0	1	1	1	0	1
8	6	9	5	1	2	0	2	1	1	2	16	4	3	0	0	0	0	0
1	7	2	7	1	3	3	2	2	4	0	10	1	0	1	1	0	0	1
8	5	5	3	2	1	2	2	5	3	2	16	4	3	0	0	0	0	0
1	7	0	16	2	3	3	2	5	4	0	6	1	0	1	1	0	0	1
8	0	14	8	2	1	0	3	3	0	2	16	4	3	0	0	0	0	0
8	3	0	22	1	1	3	1	5	5	0	10	1	0	1	1	1	0	1
8	3	0	14	2	2	3	2	4	2	0	3	3	0	1	1	0	0	1
1	5	2	12	1	3	3	2	5	5	0	12	1	0	1	1	1	0	1
1	7	0	19	2	1	3	2	5	4	0	3	3	0	1	0	0	0	0
1	7	8	9	2	7	3	3	0	4	0	15	1	1	1	1	0	0	0
1	6	0	14	2	2	3	2	5	1	0	5	2	0	1	0	1	0	0
8	6	2	9	1	2	1	2	5	1	0	4	1	0	1	1	1	0	0
1	3	0	17	2	4	3	2	4	2	0	4	1	1	1	1	0	0	0
8	11	2	6	2	1	1	2	1	5	0	5	0	1	1	0	0	0	0
1	3	5	21	1	2	3	3	0	2	2	2	4	0	1	1	0	0	0
8	5	0	17	2	3	3	2	1	3	0	5	1	0	1	1	0	0	0
1	7	2	7	2	2	1	2	2	4	2	16	4	3	0	0	0	0	0
1	7	8	14	1	4	6	2	5	5	0	9	1	0	1	1	1	0	1
1	5	6	3	2	1	3	2	5	3	0	4	1	0	1	1	1	0	0
8	3	12	7	1	1	3	2	4	2	0	1	3	0	1	1	0	0	0
1	11	11	31	1	4	7	3	2	5	2	16	4	0	0	0	1	0	0
1	7	2	5	2	1	1	2	2	4	2	0	0	2	1	1	0	0	0
8	5	5	12	2	1	3	2	4	3	0	14	0	0	1	1	1	0	0
8	3	0	15	2	4	3	2	4	2	0	7	0	0	1	1	0	0	1
8	11	2	6	2	1	1	2	1	5	2	3	0	1	0	0	0	0	0
8	0	12	1	2	2	6	2	3	0	2	16	0	1	1	0	0	0	0
8	3	0	8	2	3	2	3	1	2	0	4	1	0	1	1	0	1	0
1	0	0	10	2	1	6	2	5	0	0	15	4	0	1	1	0	0	1
1	7	2	6	2	1	3	2	2	4	0	5	1	1	0	1	0	0	1
1	7	0	18	2	6	1	2	2	4	0	15	2	0	1	1	0	0	0
8	0	0	9	2	3	6	2	3	0	0	15	1	0	1	1	1	0	1
1	3	0	18	2	5	3	2	0	2	0	13	1	0	1	1	0	0	1
8	6	9	4	1	4	5	2	1	1	0	7	1	0	1	1	1	1	0
1	6	2	6	1	2	1	2	0	1	0	4	0	1	1	1	0	0	0
1	9	0	12	2	2	6	2	2	5	0	2	4	0	1	1	0	0	1
8	0	0	8	2	1	6	2	4	5	2	16	4	3	0	0	0	0	0
1	6	8	13	1	1	3	2	0	1	0	3	1	0	1	1	0	0	1
8	6	6	5	1	8	1	2	4	1	0	11	0	0	1	1	0	1	0
8	7	0	16	2	3	3	2	4	4	0	5	1	0	1	1	0	1	1
1	3	0	14	2	4	3	2	4	2	2	16	4	3	0	0	0	0	0
1	3	0	8	2	5	3	2	5	2	0	8	2	0	1	0	0	0	0
1	3	0	17	2	3	4	2	5	2	0	0	1	0	1	1	1	0	1
1	5	0	19	2	5	3	2	2	3	0	10	1	1	1	1	0	0	0
1	3	2	11	1	5	3	2	2	2	2	16	4	3	0	0	0	0	0
1	11	2	3	2	1	6	2	2	5	2	16	4	3	0	0	0	0	0
8	7	2	8	0	3	3	1	1	5	0	3	2	0	1	1	0	0	1
8	0	9	9	2	1	0	3	3	0	2	16	4	3	0	0	0	0	0
1	3	11	10	2	3	2	2	5	2	0	6	1	0	1	1	0	0	1
1	3	0	18	2	3	3	2	5	2	0	10	1	0	1	1	1	0	0
8	3	0	14	2	4	1	2	1	2	0	15	1	0	1	1	0	0	0
1	0	12	9	2	5	0	2	2	5	0	13	1	0	1	1	1	0	0
1	11	2	3	2	1	2	3	2	5	2	3	3	0	0	0	0	0	0
8	6	0	16	2	2	1	2	4	1	2	16	4	3	0	0	0	0	0
1	7	0	25	1	2	3	1	2	4	0	5	1	0	1	1	1	0	1
1	3	0	11	2	3	3	2	0	2	0	7	0	1	1	1	0	0	1
8	7	0	12	2	1	3	2	5	4	2	16	4	3	0	0	0	0	0
8	11	9	9	1	2	2	2	1	5	1	1	0	1	1	1	1	0	1
1	7	10	30	1	2	3	3	0	5	0	7	1	0	1	1	1	0	1
8	0	12	7	2	3	6	2	4	5	2	16	4	3	0	0	0	0	0
1	3	0	12	2	1	3	2	5	5	0	7	4	0	1	1	0	0	1
1	5	2	12	1	7	3	2	0	5	0	15	1	0	1	1	1	0	1
8	5	5	1	2	2	3	2	4	3	2	16	4	3	0	0	0	0	0
1	0	0	6	3	2	0	3	3	0	2	16	4	3	0	0	0	0	0
8	5	0	20	1	3	3	1	1	3	0	10	4	0	1	1	1	1	1
8	7	0	12	2	1	1	2	4	4	2	16	4	3	0	0	0	0	0
8	3	2	4	2	1	3	2	1	2	0	1	1	1	1	1	0	0	0
1	7	2	10	2	2	1	3	2	4	2	2	0	0	0	1	0	0	0
1	0	7	3	2	2	6	3	5	0	2	2	4	1	1	1	1	0	1
8	5	0	11	2	2	3	2	4	3	2	16	4	3	0	0	0	0	0
1	5	0	18	2	3	3	2	0	3	0	15	1	0	1	1	1	0	1
8	6	2	18	2	3	0	3	1	1	2	16	4	3	0	0	0	0	0
1	6	11	3	2	3	3	2	5	1	0	4	1	0	1	0	0	0	0
1	6	9	5	1	2	3	2	0	1	0	2	1	0	1	1	1	1	1
8	11	0	31	1	4	3	1	1	5	0	12	1	0	1	1	1	1	1
8	3	3	13	1	3	3	2	1	2	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	6	2	3	0	2	5	4	0	0	1	0	0	0
8	0	12	1	2	1	6	2	5	0	2	6	4	1	1	0	1	0	0
8	3	0	8	2	4	3	2	4	5	0	14	0	0	1	1	0	0	1
1	11	2	13	1	2	3	2	2	5	1	5	1	0	1	1	0	0	1
1	9	2	19	1	3	1	2	0	5	2	5	3	0	0	0	0	0	0
8	3	0	15	2	2	3	2	5	2	0	5	0	0	1	1	1	0	1
1	3	0	20	1	2	2	1	5	2	0	8	1	0	1	1	0	1	1
1	7	9	5	0	1	6	1	2	5	0	6	3	1	1	1	0	0	0
1	1	10	24	1	2	3	2	2	5	0	3	4	0	1	1	0	0	0
8	3	0	20	1	6	3	1	4	2	0	15	1	0	1	1	1	0	1
8	3	13	14	1	4	3	3	1	2	2	16	4	3	0	0	0	0	0
8	6	0	12	2	3	3	2	4	5	0	7	1	0	1	1	0	0	1
8	0	9	8	2	2	0	3	3	0	0	1	0	1	1	0	0	0	0
8	5	0	17	2	3	3	2	4	3	0	5	1	0	1	1	1	0	1
8	0	9	8	2	2	6	3	3	0	2	3	0	0	1	1	0	0	0
8	5	0	10	2	1	2	2	1	3	1	0	0	0	1	0	0	0	0
8	9	3	9	1	2	1	2	4	5	0	11	1	0	1	0	1	0	0
8	3	6	1	2	5	3	2	1	2	0	3	1	0	1	1	1	0	0
1	3	0	20	1	4	2	1	0	2	0	9	4	0	0	1	0	0	1
1	3	8	5	2	3	2	3	0	2	0	6	4	0	0	0	0	0	0
1	9	14	20	1	3	3	2	2	5	0	1	0	0	0	0	0	0	0
8	1	7	7	2	2	6	2	1	5	2	1	4	0	0	0	0	0	0
8	3	14	20	1	5	3	2	5	2	0	15	1	0	1	1	0	0	1
8	3	0	20	1	3	3	1	4	5	0	6	2	0	1	1	1	1	1
8	1	0	24	1	2	3	1	1	5	0	9	2	0	1	1	0	0	1
1	9	9	26	1	9	4	2	5	5	0	7	4	1	0	0	0	0	0
8	0	0	14	2	3	6	2	4	5	0	6	1	0	1	0	0	1	0
8	5	0	11	2	2	3	2	4	3	0	6	2	0	1	1	0	1	1
8	0	0	10	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	0	9	8	2	2	6	3	3	0	0	10	1	0	1	1	0	0	0
1	3	5	3	2	2	3	2	0	2	2	16	4	3	0	0	0	0	0
1	3	0	20	1	2	3	1	2	2	0	2	2	0	1	1	0	1	1
8	7	0	15	2	2	3	2	4	4	0	4	1	0	1	1	0	0	1
1	7	2	7	2	4	1	2	5	4	2	16	4	3	0	0	0	0	0
8	6	5	21	1	3	1	2	1	1	0	9	0	0	1	1	0	1	1
8	6	9	7	1	1	3	2	1	1	0	15	0	0	0	1	0	0	0
8	7	13	10	2	4	3	3	5	4	0	7	2	0	1	1	1	0	0
1	6	6	1	2	4	3	2	2	1	0	4	1	0	1	0	0	0	0
1	3	0	17	2	4	3	2	0	2	0	7	1	0	1	1	0	0	1
1	6	11	17	2	2	1	2	2	1	0	12	1	0	1	0	0	1	0
1	5	0	12	2	1	1	2	5	3	1	7	4	1	1	0	1	0	1
1	5	2	8	2	2	1	2	5	3	0	2	0	0	1	1	0	0	0
1	6	2	7	2	2	1	2	2	1	0	10	0	0	0	1	0	0	0
8	0	0	8	2	3	6	2	3	0	0	3	1	0	1	1	1	0	1
8	5	13	16	2	4	3	3	5	3	0	15	1	0	1	1	1	0	1
8	0	0	9	2	1	6	2	3	0	2	2	4	3	0	0	0	0	0
1	7	0	31	1	9	7	1	5	5	2	16	4	3	0	0	0	0	0
1	3	0	8	2	3	2	2	5	5	0	3	1	0	1	1	1	0	1
8	0	14	11	2	2	6	2	4	5	0	5	1	0	1	0	0	0	0
1	5	5	3	2	3	3	2	2	3	0	15	1	0	1	1	0	0	0
1	3	2	4	1	5	2	2	2	2	0	6	0	0	1	1	1	0	1
1	3	0	16	2	3	3	2	5	2	0	15	1	0	1	1	0	0	1
1	8	2	4	1	4	6	1	0	5	0	5	1	1	1	1	1	0	1
1	0	14	9	2	1	6	3	5	0	2	2	3	1	0	0	0	0	0
8	3	0	8	2	1	2	2	4	2	0	15	1	0	1	1	0	1	1
1	6	2	22	1	3	3	2	5	1	0	7	1	1	1	1	0	0	1
8	0	9	10	2	1	0	3	3	0	2	16	4	3	0	0	0	0	0
1	5	9	15	1	4	3	2	2	5	0	8	1	0	1	1	0	0	0
1	5	0	10	2	2	2	2	5	3	0	8	4	0	1	1	1	0	0
1	5	0	10	2	1	2	2	2	3	0	2	4	1	1	1	0	1	1
8	7	8	6	2	1	3	2	4	4	0	2	0	0	1	1	0	1	1
8	3	5	12	2	3	6	2	1	2	2	0	3	1	0	0	0	0	0
1	3	6	2	2	2	3	2	2	2	0	6	1	0	1	1	0	0	0
1	3	1	3	2	3	6	2	2	2	2	16	4	3	0	0	0	0	0
8	3	0	12	2	3	3	2	5	2	0	13	1	0	1	1	0	0	1
1	3	0	11	2	4	5	2	2	2	0	7	1	0	1	1	1	1	1
8	2	12	10	2	3	0	2	4	5	0	5	1	0	0	0	0	0	0
8	11	0	15	2	3	1	3	5	5	0	3	0	1	1	1	0	0	0
1	3	12	14	2	2	1	3	5	2	2	16	4	3	0	0	0	0	0
1	3	11	7	2	4	1	2	0	2	0	15	4	1	1	1	1	1	1
1	9	2	19	1	2	3	3	5	5	0	5	3	0	1	0	0	0	0
8	0	0	9	2	3	6	2	5	0	0	8	1	1	1	1	1	0	0
8	9	0	30	0	2	3	0	1	5	0	2	0	0	1	0	0	0	0
1	5	6	8	2	1	3	3	0	3	0	5	4	1	1	1	0	0	1
8	5	11	10	2	2	3	2	4	3	2	16	4	3	0	0	0	0	0
1	2	11	8	2	1	0	2	2	5	0	9	0	0	1	1	0	0	1
1	3	2	16	1	2	3	3	5	2	2	2	3	0	1	0	0	0	0
8	3	0	15	2	3	3	2	4	2	0	5	3	0	1	0	0	0	0
8	0	9	9	2	3	0	3	3	0	0	13	1	0	0	0	0	0	0
5	5	6	17	1	1	3	3	1	3	2	16	4	3	0	0	0	0	0
1	7	6	10	1	3	3	2	2	4	0	13	1	0	1	1	1	0	0
8	7	0	31	0	4	3	0	1	5	0	4	1	0	1	1	0	0	0
8	1	2	20	1	4	3	3	5	5	2	16	4	3	0	0	0	0	0
1	0	0	9	2	1	0	2	5	0	0	8	1	0	1	1	1	0	0
8	6	11	11	2	3	3	3	4	1	0	2	1	0	1	0	0	0	0
8	0	11	8	2	1	0	2	3	0	2	16	4	3	0	0	0	0	0
8	3	5	9	2	3	3	3	5	2	0	4	3	2	1	1	0	1	1
8	3	0	16	2	1	3	2	4	2	2	16	4	3	0	0	0	0	0
1	5	1	8	1	1	2	2	2	3	0	4	0	0	1	1	0	1	0
1	6	2	4	2	3	2	2	5	1	0	4	0	0	0	0	0	0	0
1	5	0	15	2	4	7	2	5	3	2	16	4	3	0	0	0	0	0
8	7	0	20	1	2	3	1	4	4	2	3	4	2	1	1	0	1	1
1	3	9	3	1	3	6	2	5	2	0	8	1	0	1	1	1	0	1
1	6	2	3	1	1	0	2	5	1	2	16	4	3	0	0	0	0	0
1	0	0	10	2	1	6	2	5	0	2	0	4	2	0	0	0	0	0
8	3	0	9	2	2	2	2	4	2	0	2	1	1	1	1	0	0	1
8	3	6	17	1	3	3	3	1	2	0	2	1	0	1	1	1	0	1
8	3	0	15	2	2	1	2	5	2	0	3	1	1	1	1	0	0	1
1	3	2	9	1	2	1	2	2	2	0	4	4	0	1	1	0	0	0
1	5	0	17	2	8	7	2	2	3	0	6	4	1	1	1	0	1	1
1	11	0	17	2	3	3	2	2	5	0	9	1	0	1	1	0	0	1
1	11	2	3	2	1	1	2	2	5	2	16	4	3	0	0	0	0	0
1	3	0	18	2	4	3	2	2	2	0	9	1	0	1	1	0	1	1
1	6	2	7	1	2	1	1	5	5	0	13	0	0	0	1	0	0	0
1	3	0	16	2	4	1	3	0	2	0	7	4	0	1	1	0	0	1
7	6	5	8	1	1	3	2	4	1	0	3	1	0	1	1	1	1	1
8	6	6	2	1	2	2	1	4	1	0	4	0	0	1	1	0	0	1
1	3	0	9	2	5	3	2	5	2	0	15	1	1	1	1	0	0	1
8	1	0	10	2	2	2	2	5	5	2	16	4	3	0	0	0	0	0
8	7	0	11	2	2	0	2	1	4	0	6	0	0	1	1	0	0	1
8	7	2	10	2	3	3	3	1	4	0	6	0	2	1	1	0	0	0
8	5	5	9	2	2	2	2	1	3	0	1	1	1	1	0	0	0	0
8	0	9	24	1	2	3	3	1	5	0	4	1	0	1	1	0	0	0
8	11	0	9	2	2	2	2	4	5	0	10	4	0	0	0	0	0	0
8	6	0	9	2	2	2	2	4	1	0	4	4	0	1	1	0	1	1
8	11	2	13	2	3	3	2	5	5	0	6	1	0	1	1	0	0	1
8	5	2	9	1	1	3	2	5	3	2	16	4	3	0	0	0	0	0
1	3	0	14	2	3	2	2	5	2	0	5	1	0	1	1	0	0	0
1	11	0	31	0	3	7	0	2	5	2	16	4	3	0	0	0	0	0
1	6	1	2	1	4	1	2	0	1	0	8	4	0	1	1	1	0	1
1	5	0	23	1	1	3	1	0	5	0	8	1	0	1	1	0	0	1
8	3	0	8	2	5	3	2	4	2	0	11	1	0	1	1	0	0	0
8	3	0	21	1	2	3	1	1	5	0	4	1	0	1	1	0	0	0
8	2	14	14	1	4	6	3	1	5	0	15	1	0	1	1	1	0	1
1	7	0	14	2	3	3	2	5	4	0	7	1	0	1	1	1	1	1
8	7	5	11	2	1	1	3	5	4	2	16	4	3	0	0	0	0	0
1	3	0	10	2	2	2	2	5	2	0	3	1	0	1	1	1	0	1
8	6	10	20	1	2	3	2	4	1	0	2	4	0	1	1	0	0	1
1	7	0	16	2	3	3	2	5	4	0	8	2	0	1	1	1	0	1
1	5	0	13	2	3	3	2	5	3	0	5	4	1	1	1	0	0	1
8	6	0	14	2	3	3	2	5	1	0	3	1	0	1	1	1	0	0
8	7	0	20	1	2	3	1	4	4	0	2	4	1	1	1	0	1	1
8	6	5	9	2	2	3	3	4	1	2	16	4	3	0	0	0	0	0
1	1	0	23	1	3	3	1	5	5	0	3	1	0	1	1	0	1	1
1	7	9	24	0	3	6	3	2	5	0	13	0	0	1	0	0	0	0
1	3	0	12	2	4	3	2	5	2	2	16	4	3	0	0	0	0	0
1	7	0	21	1	1	3	1	5	5	0	9	4	0	1	1	0	1	1
8	0	9	9	2	4	6	3	5	0	0	15	1	0	1	0	0	0	0
1	3	0	12	2	3	3	2	5	2	0	12	1	0	1	1	1	1	1
8	6	2	6	2	2	1	3	1	1	2	16	0	0	1	0	0	0	0
1	7	0	31	0	2	3	0	2	5	0	3	0	2	1	1	0	0	0
1	5	0	24	1	5	3	2	5	5	2	16	4	3	0	0	0	0	0
1	1	0	23	1	3	3	1	2	5	0	5	1	0	1	1	1	0	1
8	5	0	13	2	4	3	2	4	5	0	7	1	0	1	1	1	1	0
8	3	0	16	2	3	3	2	4	2	2	16	4	3	0	0	0	0	0
1	5	0	24	1	2	3	1	2	5	0	3	2	0	1	1	1	0	1
8	5	4	2	2	2	3	2	4	3	2	16	4	3	0	0	0	0	0
8	8	2	4	1	1	1	2	1	5	0	0	4	0	0	0	0	0	0
8	11	7	3	2	4	1	2	1	5	0	9	1	0	1	0	1	0	0
1	7	2	5	2	1	5	3	0	4	0	15	1	0	1	1	1	0	1
8	5	2	5	1	3	6	1	5	5	0	1	1	0	1	1	0	0	1
1	7	1	14	1	1	3	2	0	5	0	5	0	1	1	1	1	0	1
1	9	0	21	1	3	3	1	2	5	0	3	1	0	1	1	1	0	1
8	0	9	8	2	2	6	3	5	0	2	16	4	3	0	0	0	0	0
8	5	1	21	1	1	3	2	5	5	2	16	4	3	0	0	0	0	0
1	9	13	15	1	4	3	2	2	5	2	16	4	3	0	0	0	0	0
8	3	0	12	2	2	3	2	5	2	0	1	1	0	0	1	0	0	0
1	7	0	10	2	1	2	2	0	4	2	16	4	3	0	0	0	0	0
1	7	5	16	1	4	5	3	5	4	0	8	2	0	1	1	1	0	1
8	7	0	13	2	4	1	2	5	5	2	2	0	0	0	0	0	0	0
8	6	0	22	1	4	3	1	1	1	0	9	0	0	1	1	1	0	1
8	6	14	1	1	6	3	2	1	1	0	15	1	0	1	1	0	1	1
1	6	2	5	2	1	1	2	5	1	2	16	4	3	0	0	0	0	0
8	3	0	8	2	1	5	2	5	2	2	16	4	3	0	0	0	0	0
1	3	0	12	2	4	3	2	5	5	0	9	1	0	1	1	1	1	1
1	6	0	22	1	3	3	3	2	1	0	7	1	0	1	1	1	1	0
1	3	0	16	2	4	3	2	2	2	0	12	1	0	1	1	0	0	0
1	6	14	17	2	2	1	3	5	1	2	16	4	3	0	0	0	0	0
1	1	6	21	1	2	3	3	5	5	0	5	1	0	1	1	0	0	0
8	3	0	11	2	4	3	2	4	2	0	14	1	0	1	1	1	0	1
1	0	0	8	2	0	6	2	5	0	0	4	3	0	1	0	0	0	0
1	7	2	12	1	2	1	1	0	5	0	10	1	0	1	1	1	1	0
8	6	13	8	2	1	3	3	4	1	0	1	0	0	1	1	1	1	1
1	0	11	8	2	2	6	3	5	0	2	16	4	0	1	0	0	0	0
8	0	2	4	2	4	6	3	3	0	2	16	4	1	0	0	1	0	0
8	3	6	1	1	3	3	2	5	2	0	4	2	0	1	1	0	0	1
1	5	0	18	2	3	3	2	5	3	0	5	1	0	1	1	1	1	1
1	7	0	22	1	3	3	1	0	5	2	16	4	3	0	0	0	0	0
8	3	0	18	2	3	5	2	4	2	0	7	4	0	1	1	0	0	1
1	5	0	9	2	2	2	2	5	3	2	2	1	0	1	1	1	0	1
8	3	9	12	2	5	3	3	5	2	2	16	4	0	1	1	0	0	0
1	0	9	9	2	1	6	3	5	0	2	16	4	3	0	0	0	0	0
8	3	5	1	2	3	6	2	1	2	0	5	2	0	1	1	1	0	1
1	11	2	10	2	3	6	2	2	5	2	16	4	3	0	0	0	0	0
8	6	9	12	1	3	3	2	5	5	0	5	1	0	1	1	0	0	1
1	11	9	7	2	1	6	2	2	5	2	16	4	3	0	0	0	0	0
1	11	2	5	1	2	3	2	2	5	2	16	4	3	0	0	0	0	0
1	6	13	20	1	2	6	3	2	1	0	6	1	0	1	1	0	0	1
1	3	0	16	2	4	2	2	0	2	0	7	2	0	1	1	1	0	1
8	3	0	12	2	3	3	2	4	2	0	2	0	0	1	1	1	1	1
8	0	0	9	2	2	0	2	3	0	0	3	1	0	1	1	0	0	0
8	0	9	9	2	2	6	3	3	0	0	5	4	0	1	0	0	0	0
8	5	2	8	2	2	6	2	1	3	2	16	4	0	0	0	0	0	0
8	7	2	5	2	1	1	2	1	4	0	7	0	1	1	0	0	0	0
1	7	2	16	1	2	2	3	5	4	0	4	3	2	1	1	0	0	0
8	3	0	8	2	3	2	2	5	2	0	5	2	0	1	1	0	1	1
8	2	2	10	1	3	6	2	1	5	2	16	4	3	0	0	0	0	0
8	6	2	4	2	2	0	2	5	1	1	1	1	0	1	1	0	0	0
1	9	10	31	0	3	1	3	2	5	0	5	0	1	1	1	1	0	0
8	7	0	14	2	2	1	2	4	4	2	16	4	3	0	0	0	0	0
8	3	2	13	2	3	1	3	5	2	0	7	0	1	1	1	1	0	1
8	5	0	21	1	1	5	1	1	3	0	5	1	0	1	1	0	0	1
1	6	0	12	2	3	3	2	5	1	1	4	0	1	1	0	0	0	0
1	6	1	9	2	1	3	3	2	1	0	7	0	1	1	1	0	0	1
1	3	0	15	2	3	6	2	5	2	0	5	3	1	1	1	0	1	1
1	9	0	13	2	3	2	2	5	5	2	16	4	3	0	0	0	0	0
1	0	0	6	3	1	6	3	5	0	2	16	4	3	0	0	0	0	0
1	3	0	9	2	4	2	2	2	2	0	5	2	0	0	0	1	0	1
1	3	2	4	1	1	2	2	2	2	2	16	4	3	0	0	0	0	0
8	3	6	1	1	2	3	1	5	2	0	4	2	0	1	1	0	1	1
8	3	0	9	2	4	3	2	5	2	0	9	1	0	1	1	1	1	1
1	0	0	9	2	9	6	2	5	0	0	2	0	1	0	0	0	0	0
1	9	0	19	2	4	3	2	0	5	0	15	4	0	1	1	1	0	1
1	0	0	8	2	8	6	2	5	0	2	1	4	0	0	0	0	0	0
1	5	3	4	2	3	3	2	0	3	0	5	3	0	1	0	0	1	0
8	3	0	30	1	3	3	1	1	2	0	15	1	0	1	1	1	1	1
1	11	0	14	2	1	1	2	5	5	2	16	4	3	0	0	0	0	0
1	3	0	10	2	3	2	2	0	2	0	5	1	0	1	0	1	0	0
1	3	0	14	2	6	3	2	0	2	0	15	1	0	1	1	1	0	1
1	3	10	10	2	4	2	3	5	2	0	2	2	0	1	1	0	0	1
1	5	3	13	1	3	3	2	5	5	0	6	0	0	1	1	1	0	0
8	5	14	20	1	3	1	2	1	3	2	16	4	3	0	0	0	0	0
1	6	5	5	2	1	3	2	2	1	2	16	4	3	0	0	0	0	0
8	3	0	19	1	2	3	2	4	2	0	4	0	0	1	1	1	0	1
8	7	0	15	2	5	6	2	4	5	0	7	1	0	1	1	0	0	0
8	5	9	8	2	4	2	3	4	3	0	10	0	0	1	1	1	1	1
8	9	14	1	2	1	0	2	4	5	2	16	4	3	0	0	0	0	0
8	6	2	7	1	1	3	1	1	5	1	3	1	0	1	1	0	0	0
1	7	2	7	2	3	1	3	0	4	2	2	4	1	1	0	0	0	0
1	7	0	15	2	5	3	2	5	4	0	1	2	0	1	1	1	1	1
8	5	2	30	1	3	1	3	5	5	0	8	1	0	0	0	0	0	0
8	3	0	9	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
1	3	0	20	1	2	3	1	5	2	0	1	1	0	1	1	0	0	0
8	6	14	20	1	3	5	2	1	1	0	15	1	0	1	1	1	0	1
1	3	0	13	2	4	3	2	5	2	0	6	1	0	1	1	1	1	1
8	5	8	7	2	2	3	2	5	3	0	12	2	0	1	1	1	0	0
1	7	0	8	2	3	3	2	5	4	0	4	0	1	1	1	1	0	1
8	9	0	9	2	2	2	2	4	5	0	4	1	0	1	1	1	0	0
8	5	2	7	1	1	3	2	5	3	0	11	4	0	1	0	0	0	0
1	7	10	18	2	3	3	2	2	4	0	16	1	0	1	1	0	0	0
8	3	6	1	1	1	2	1	5	2	0	6	2	0	1	1	0	0	1
1	3	0	17	2	2	3	2	0	2	0	5	4	0	1	1	0	1	1
4	5	5	11	2	2	3	3	4	3	2	16	4	3	0	0	0	0	0
1	0	14	9	2	1	0	2	5	5	0	8	0	0	0	1	1	0	0
8	0	0	9	2	2	6	2	3	0	2	16	3	0	1	0	1	0	0
1	6	2	10	2	1	6	3	2	1	0	1	0	1	1	1	0	0	1
8	3	0	10	2	5	2	2	4	2	0	15	1	0	1	1	1	0	1
1	5	2	6	1	3	1	2	2	5	0	3	0	1	1	1	0	0	1
8	7	0	10	2	2	4	2	1	4	0	3	3	0	1	0	0	0	0
8	9	0	31	1	2	3	1	1	5	0	1	2	0	1	1	0	0	1
1	0	4	8	2	1	6	3	5	5	2	16	3	1	0	0	0	0	0
8	3	11	12	1	1	3	3	5	2	0	4	4	0	1	1	0	0	1
8	5	0	15	2	3	3	2	4	3	0	14	2	0	1	1	1	0	1
1	6	12	7	2	4	3	3	0	1	0	7	1	0	1	1	1	0	1
8	3	4	13	2	1	3	2	4	2	0	4	0	0	0	1	0	0	1
1	3	0	12	2	3	3	2	0	2	0	15	2	0	1	1	1	0	1
0	7	2	4	2	2	2	3	5	4	2	16	4	3	0	0	0	0	0
8	6	7	7	2	2	1	2	4	1	0	4	3	0	1	1	0	0	0
8	11	2	2	2	9	7	2	4	5	2	16	4	3	0	0	0	0	0
1	7	2	13	0	2	6	2	2	5	0	3	4	0	0	1	1	0	0
1	3	0	8	2	4	2	2	5	2	0	10	1	0	1	1	0	0	0
8	3	0	11	2	3	2	2	4	2	0	5	1	0	1	1	1	1	0
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	0	14	9	2	2	6	2	5	0	0	11	0	0	0	0	0	0	0
8	3	11	9	2	3	3	2	5	2	0	5	1	0	1	1	0	1	1
8	7	0	16	2	4	1	2	1	4	0	8	2	0	1	1	0	0	1
8	6	9	19	1	3	3	3	4	1	0	4	1	0	1	1	1	1	1
8	11	2	10	1	2	3	2	5	5	0	5	0	0	1	1	0	0	1
1	5	2	9	1	2	1	2	2	3	0	6	0	1	1	1	0	1	1
8	6	0	8	2	4	3	2	4	1	0	13	0	0	1	1	1	1	1
8	7	0	9	2	2	2	2	1	4	0	15	0	0	1	1	1	1	1
8	3	0	12	2	1	3	2	4	2	0	12	0	1	1	1	0	0	0
1	3	0	17	2	4	3	2	2	2	0	7	4	0	1	1	1	0	0
8	5	5	2	1	1	1	2	1	3	2	16	4	0	0	0	0	0	0
8	3	0	8	2	2	3	2	4	2	0	3	4	0	1	1	0	0	1
1	7	2	11	1	1	3	1	5	5	0	8	3	0	1	1	0	0	1
8	0	13	7	2	2	0	3	3	0	0	3	2	0	1	1	0	0	1
8	6	3	3	2	2	3	2	4	1	2	16	4	3	0	0	0	0	0
1	6	12	5	2	2	2	3	5	1	0	4	1	0	1	1	0	0	1
1	7	2	12	1	2	7	2	2	4	2	16	4	3	0	0	0	0	0
8	7	13	20	0	3	1	2	1	5	0	2	1	0	1	1	1	0	1
8	6	6	10	2	1	3	3	1	1	2	16	4	3	0	0	0	0	0
1	0	9	9	2	1	6	3	5	0	2	16	4	3	0	0	0	0	0
1	5	0	17	2	4	3	2	2	3	2	16	4	3	0	0	0	0	0
1	7	3	7	1	3	6	1	2	5	0	14	1	0	1	1	0	1	1
1	11	12	1	2	4	6	2	2	5	2	16	4	3	0	0	0	0	0
8	7	0	20	1	4	1	2	5	4	2	1	1	1	0	0	0	0	0
8	5	0	12	2	3	0	2	1	3	2	7	3	0	1	0	0	0	0
8	6	0	9	2	2	2	2	4	1	0	8	1	0	1	1	1	0	1
1	6	0	15	2	1	1	2	2	1	2	1	3	1	0	0	1	0	0
8	5	12	21	1	3	3	3	1	5	0	8	1	0	1	1	0	0	1
1	5	2	8	1	3	1	2	0	5	0	15	2	0	1	1	1	0	0
8	7	14	8	1	2	3	1	1	5	0	2	1	0	1	1	1	0	0
1	0	0	8	2	1	6	2	5	0	2	1	2	0	1	0	0	0	0
1	6	7	20	1	4	3	3	0	1	0	6	1	0	1	1	1	1	1
8	9	4	3	2	3	3	2	4	5	2	16	4	3	0	0	0	0	0
8	6	2	6	2	1	1	2	1	1	2	16	4	3	0	0	0	0	0
8	7	0	14	2	2	1	2	1	4	2	2	3	0	0	0	0	0	0
8	3	8	20	1	4	3	2	1	2	0	5	1	0	1	1	1	1	1
1	3	11	9	2	2	4	3	5	2	0	16	0	1	1	1	1	0	0
8	0	14	8	2	1	6	3	3	0	2	16	4	3	0	0	0	0	0
1	6	2	7	2	1	6	2	5	1	0	3	2	0	1	1	1	0	1
1	5	0	25	1	2	3	1	2	5	2	2	3	0	0	0	0	0	0
1	0	9	9	2	8	6	3	5	0	0	6	3	0	1	1	0	0	0
1	7	3	13	1	2	6	1	5	5	0	2	1	1	1	1	0	0	1
1	3	13	10	2	4	6	2	0	2	2	16	4	3	0	0	0	0	0
1	6	11	7	2	1	1	3	5	1	2	16	4	3	0	0	0	0	0
8	3	0	11	2	2	3	2	5	2	0	5	2	0	1	1	0	0	0
8	3	2	9	1	2	1	2	5	2	0	1	0	0	1	1	0	0	1
8	0	0	8	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
8	5	2	6	2	1	2	2	5	3	2	16	0	0	0	1	0	0	0
1	6	0	16	2	1	1	2	0	1	0	3	1	0	1	0	1	0	0
8	3	0	14	2	4	3	2	5	2	2	16	4	3	0	0	0	0	0
1	11	3	5	2	1	1	2	0	5	2	16	4	3	0	0	0	0	0
8	0	0	10	2	1	0	2	3	0	2	16	4	3	0	0	0	0	0
8	6	0	12	2	1	3	2	4	1	2	16	4	3	0	0	0	0	0
8	7	3	9	2	3	2	3	4	4	0	0	0	1	0	0	0	0	0
1	6	6	2	1	1	3	2	5	1	0	11	0	1	1	1	1	1	1
1	5	10	20	1	4	1	2	5	3	2	5	1	1	1	1	0	0	0
1	3	3	1	1	1	0	1	0	2	0	2	0	1	0	0	0	0	0
8	11	0	12	2	1	1	2	4	5	2	16	4	3	0	0	0	0	0
1	3	12	1	2	1	6	2	2	2	0	12	1	0	1	1	1	1	1
8	5	2	8	2	1	3	3	4	3	2	0	1	1	0	0	0	0	0
8	0	0	9	2	2	6	2	5	0	2	16	4	3	0	0	0	0	0
8	5	0	11	2	1	3	2	4	5	2	16	4	3	0	0	0	0	0
1	5	0	12	2	2	3	2	0	3	0	2	3	1	0	0	0	0	0
1	3	7	1	2	5	3	2	5	2	0	12	1	0	1	1	1	1	1
8	3	0	18	2	2	7	2	4	2	0	4	4	0	1	0	0	0	0
1	9	13	7	1	3	1	2	5	5	0	7	1	0	0	1	0	0	0
1	6	8	6	1	3	3	1	5	5	2	16	4	3	0	0	0	0	0
8	7	3	25	1	2	3	2	1	4	2	16	4	3	0	0	0	0	0
1	7	0	17	2	1	2	2	5	4	0	15	0	0	1	1	1	0	1
1	6	2	6	2	2	1	2	2	1	0	15	1	0	1	1	0	0	1
8	2	12	12	2	2	0	3	3	5	0	4	4	0	1	0	0	0	0
8	7	6	1	2	3	3	2	5	4	0	4	2	0	0	0	0	0	0
1	5	5	2	1	9	7	1	0	5	2	16	4	3	0	0	0	0	0
8	5	0	18	2	2	3	2	1	3	0	3	4	0	1	1	0	1	1
8	0	0	9	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
1	7	0	28	1	4	3	1	0	4	0	11	1	0	1	1	0	0	0
8	0	0	8	2	2	6	2	3	0	2	16	4	0	0	0	0	0	0
8	3	0	17	2	3	3	2	5	2	0	14	4	0	1	0	0	0	0
1	0	0	10	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
8	0	0	8	2	3	6	2	3	0	1	9	1	0	1	1	0	0	0
8	3	8	1	2	5	3	2	4	2	0	15	1	0	1	1	0	0	1
8	5	0	16	2	2	3	2	4	3	2	0	4	0	0	0	0	0	0
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	7	2	6	2	2	1	3	5	4	2	16	4	3	0	0	0	0	0
8	5	0	21	1	3	3	1	1	3	0	15	1	0	1	1	1	1	1
1	5	0	24	1	2	3	1	2	5	2	3	3	0	0	0	0	0	0
8	5	0	15	2	1	3	2	4	3	2	1	0	2	1	1	0	0	1
1	9	0	23	1	3	3	1	0	5	0	6	1	0	1	1	1	1	1
8	5	0	9	2	2	3	2	4	3	2	16	4	3	0	0	0	0	0
8	3	0	10	2	2	2	2	4	5	0	1	2	0	1	1	0	1	1
8	3	0	13	2	3	3	2	4	2	0	4	1	0	1	1	1	0	1
8	3	1	16	2	2	3	2	5	2	2	16	4	3	0	0	0	0	0
8	5	3	6	1	4	2	2	1	3	1	1	0	1	0	0	0	0	0
8	6	0	20	1	3	3	1	1	1	0	9	1	0	1	1	0	1	0
8	11	2	5	2	1	3	2	1	5	2	16	4	3	0	0	0	0	0
8	0	0	10	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
8	3	0	13	2	3	3	2	4	2	0	7	1	0	1	1	0	1	1
1	7	10	20	1	1	0	3	0	5	0	2	1	0	1	1	0	0	1
1	6	2	8	1	1	2	1	2	5	0	10	0	0	1	1	0	0	0
8	0	0	10	2	3	6	2	3	0	2	3	2	0	0	0	0	0	0
1	5	2	6	1	3	3	2	5	3	0	5	1	0	1	1	1	0	1
8	7	9	10	2	2	0	3	1	4	2	16	4	3	0	0	0	0	0
1	7	13	28	1	4	3	3	2	5	0	12	1	0	1	1	1	1	1
8	7	5	9	2	2	1	3	5	4	0	4	4	2	1	1	0	0	0
1	5	0	9	2	4	2	2	2	3	0	15	1	0	1	1	0	0	0
8	7	2	4	2	2	1	2	4	4	0	3	3	0	1	0	0	0	0
1	6	0	10	2	3	2	2	5	1	0	8	1	0	1	1	1	0	1
1	5	2	5	1	3	1	1	2	5	2	6	4	0	0	0	0	0	0
8	3	10	19	1	5	3	2	5	2	0	11	4	1	1	1	1	0	0
8	7	2	9	1	1	1	1	1	5	2	16	3	0	0	1	0	0	0
8	5	6	11	2	3	2	3	1	3	0	4	4	1	1	0	1	1	0
1	7	2	10	1	2	1	2	2	4	0	3	0	0	1	1	0	0	0
8	3	0	15	2	5	3	2	4	2	0	15	1	0	1	1	1	1	1
1	11	3	9	1	3	1	2	5	5	0	15	1	0	1	1	0	0	1
8	3	0	17	2	6	3	2	4	2	0	15	1	0	1	1	1	0	1
1	3	0	18	2	2	2	2	5	2	0	6	1	0	1	1	0	0	1
1	11	3	3	1	1	1	2	2	5	0	15	0	1	1	1	0	0	0
8	3	0	12	2	3	1	2	4	2	0	3	0	0	1	0	0	0	0
8	5	0	17	2	2	3	2	5	3	2	16	4	3	0	0	0	0	0
8	3	0	21	1	3	3	1	4	2	2	16	4	3	0	0	0	0	0
8	3	0	14	2	5	3	2	4	2	0	15	4	0	1	1	1	0	1
8	3	5	17	2	4	3	3	1	2	0	10	0	0	1	1	1	0	1
8	5	6	2	1	1	2	2	5	3	0	9	1	1	1	1	0	1	1
1	11	0	11	2	3	2	3	0	5	0	2	0	0	1	0	0	0	0
8	3	0	20	1	4	3	1	1	2	0	7	1	0	1	1	1	0	1
8	8	0	8	2	2	2	2	4	5	2	16	4	3	0	0	0	0	0
1	0	14	5	2	1	6	3	3	0	2	0	4	0	0	0	0	0	0
1	3	0	17	2	2	4	2	0	2	0	4	1	0	1	1	0	0	0
1	3	14	5	2	2	0	2	2	2	0	3	3	0	1	1	0	0	1
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	0	9	9	2	1	6	3	3	0	2	16	4	3	0	0	0	0	0
8	3	0	14	2	2	3	2	4	2	0	4	4	1	1	1	0	0	1
8	11	0	27	1	3	3	1	1	5	0	1	1	0	1	1	0	0	1
1	3	9	3	1	2	3	2	5	2	0	1	1	0	1	0	0	0	0
1	7	9	13	2	2	2	3	5	4	0	4	1	0	1	0	0	0	0
1	3	0	13	2	3	3	2	0	2	0	15	4	0	1	1	0	0	1
8	3	0	10	2	2	2	2	4	2	0	11	4	0	0	0	0	0	0
1	3	0	23	1	2	3	1	5	2	0	7	4	0	1	0	0	0	0
1	9	2	5	1	1	1	2	2	5	2	16	4	3	0	0	0	0	0
1	6	10	30	1	5	2	3	5	1	0	15	1	0	1	1	1	0	0
8	0	0	9	2	2	0	2	5	0	2	5	0	0	1	0	0	0	0
1	1	0	24	1	4	3	1	2	5	0	4	1	0	1	1	1	0	1
8	3	9	3	2	2	3	2	4	2	0	4	0	0	1	1	0	0	1
1	3	0	20	1	1	2	1	0	2	2	16	4	3	0	0	0	0	0
8	3	0	19	1	4	3	1	1	2	0	3	2	0	0	0	0	0	0
1	3	12	14	1	3	3	2	2	2	0	8	1	0	1	1	1	0	1
8	0	9	8	2	2	0	3	5	0	2	16	4	1	0	0	0	0	0
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	0	16	2	3	3	2	4	2	0	3	1	0	1	1	0	0	1
8	5	0	9	2	4	3	2	4	3	0	5	1	0	1	1	0	0	1
8	6	2	16	1	2	3	3	1	1	0	7	1	0	0	0	0	0	0
1	0	0	9	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
8	3	0	13	2	1	3	2	5	2	2	1	3	0	1	0	0	0	0
1	3	0	14	2	3	3	2	2	2	2	2	4	0	1	1	0	0	1
8	3	13	18	2	2	2	3	4	2	0	6	2	0	1	1	0	0	1
1	7	6	1	2	4	1	2	5	4	0	6	4	0	1	1	1	0	1
8	0	12	9	2	3	6	3	3	0	2	16	4	3	0	0	0	0	0
8	3	6	1	2	4	3	2	4	2	0	16	0	0	1	0	0	0	0
1	11	10	20	1	3	1	3	2	5	2	16	4	3	0	0	0	0	0
8	7	2	9	0	1	2	1	1	5	2	3	4	0	0	0	0	0	0
1	6	2	6	1	1	6	2	0	1	0	15	0	1	1	1	0	0	1
8	3	0	9	2	1	3	2	4	2	2	3	4	1	1	1	1	0	1
1	7	0	20	1	1	3	1	5	4	0	5	1	0	1	1	1	0	1
1	9	2	8	1	2	1	1	0	5	0	3	0	1	1	0	0	0	0
8	3	0	17	2	3	3	2	4	2	0	6	1	0	1	1	0	0	1
8	7	0	8	2	4	2	2	5	4	0	9	1	0	1	1	1	0	1
1	7	0	9	2	3	3	2	5	4	0	11	1	0	1	1	1	0	1
1	5	2	5	1	1	1	1	5	5	2	16	4	3	0	0	0	0	0
1	3	2	8	2	2	1	3	5	2	2	16	4	3	0	0	0	0	0
1	9	9	31	1	1	2	3	2	5	0	1	2	0	1	1	1	0	0
8	3	5	4	1	1	3	2	4	2	2	2	3	0	0	0	0	0	0
1	7	5	16	2	3	3	2	0	4	0	6	4	0	1	1	0	0	1
1	1	0	30	0	3	3	0	5	5	0	15	1	0	1	1	1	1	1
8	3	0	15	2	5	3	2	4	2	0	15	1	0	1	1	1	0	1
1	7	6	7	1	3	6	1	5	5	0	5	1	0	1	1	1	0	1
8	7	0	14	2	4	3	2	1	4	0	15	2	0	1	1	0	0	1
1	9	2	4	2	4	1	2	5	5	0	2	4	0	0	1	0	0	0
1	7	0	16	2	2	3	2	0	5	0	1	1	0	1	1	0	0	1
8	5	0	15	2	2	3	2	4	3	0	4	1	0	1	1	0	0	1
8	2	4	4	1	2	1	2	5	5	2	16	4	3	0	0	0	0	0
8	3	5	6	2	3	3	2	4	2	0	13	1	0	1	1	1	1	1
1	7	14	18	2	6	3	2	2	4	2	16	4	3	0	0	0	0	0
1	3	0	28	1	2	1	1	2	2	2	1	4	0	1	1	0	0	0
8	3	0	18	1	2	3	1	4	5	0	3	1	0	1	1	0	0	0
1	3	0	12	2	3	3	2	0	2	0	4	1	0	1	1	0	0	1
8	3	11	8	1	2	3	2	4	2	0	1	4	0	1	1	0	0	0
1	11	12	13	2	2	3	2	2	5	2	16	4	3	0	0	0	0	0
8	7	0	14	2	1	2	2	1	4	0	5	1	0	1	1	0	0	1
1	0	0	9	2	2	6	2	3	0	0	15	1	0	1	1	0	0	0
8	7	0	13	2	2	2	2	5	5	2	2	3	0	1	1	0	1	1
8	7	0	20	1	4	2	1	4	4	2	16	4	3	0	0	0	0	0
8	0	9	10	2	2	5	3	3	0	2	16	4	3	0	0	0	0	0
8	6	0	19	2	3	3	2	4	5	0	13	0	0	1	1	1	0	1
8	3	8	2	1	3	3	2	5	2	0	4	1	0	1	1	0	0	1
1	7	3	4	2	2	3	2	5	4	0	4	4	0	1	0	0	1	0
8	7	2	8	2	1	3	3	1	4	2	16	4	3	0	0	0	0	0
8	2	2	10	1	2	0	2	5	5	0	3	2	0	1	1	0	0	0
8	0	14	19	1	2	2	2	4	5	0	2	4	0	1	1	0	0	1
8	7	2	8	2	1	1	2	4	4	0	8	1	0	1	1	1	0	1
1	3	0	11	2	4	3	2	0	2	0	10	1	0	1	1	0	0	1
8	7	13	11	2	3	3	3	5	4	0	15	1	0	1	1	1	1	1
1	5	14	1	2	2	1	2	0	3	0	3	1	1	1	1	0	0	0
8	3	0	12	2	1	1	2	4	2	2	16	4	3	0	0	0	0	0
8	5	10	19	2	3	3	2	4	3	0	6	1	0	1	1	0	0	1
8	7	10	22	1	2	3	3	2	4	0	10	4	0	1	0	1	0	0
1	7	0	20	1	3	1	2	0	4	2	16	4	3	0	0	0	0	0
1	8	0	12	2	4	3	2	0	5	0	11	1	0	1	1	0	0	1
8	3	9	8	1	2	3	2	5	2	2	16	4	0	1	1	0	1	1
1	0	5	9	2	1	0	2	3	0	2	16	4	3	0	0	0	0	0
8	7	6	2	2	2	3	2	4	4	0	2	1	0	1	0	1	0	0
8	11	2	14	2	2	3	3	5	5	2	16	0	0	0	1	0	0	0
8	5	0	15	2	4	3	2	1	3	2	1	1	0	0	0	0	0	0
8	0	0	9	2	2	6	2	1	5	2	16	4	3	0	0	0	0	0
1	7	0	20	1	1	1	1	5	4	0	15	1	0	1	1	0	0	1
8	6	0	20	1	3	3	1	1	1	0	6	4	0	1	0	0	0	0
1	5	0	11	2	2	2	2	2	3	0	5	1	0	1	1	1	1	1
8	3	2	13	1	7	1	2	1	2	0	15	1	1	1	1	0	0	1
1	7	2	31	1	2	3	2	0	5	0	3	4	0	1	1	0	0	1
8	0	14	3	2	3	6	3	3	0	0	4	0	0	1	0	1	0	0
1	6	11	6	2	1	1	2	0	1	2	16	4	3	0	0	0	0	0
0	5	0	8	2	5	2	2	5	5	0	11	1	0	1	1	0	0	1
8	0	5	10	2	2	6	3	3	0	2	1	0	0	1	1	0	0	1
1	5	1	12	1	6	3	2	2	5	0	15	1	0	1	1	1	1	1
8	13	0	14	2	1	1	2	4	5	0	6	0	0	1	1	1	1	1
1	5	6	2	2	3	2	2	2	3	0	4	0	0	1	1	1	1	1
1	11	0	25	1	3	3	1	5	5	0	7	3	0	1	1	0	0	1
1	7	12	26	1	1	3	1	5	5	0	14	1	0	1	1	1	1	1
1	5	0	12	2	4	3	2	5	3	0	7	0	0	1	1	1	0	0
1	3	0	10	2	4	3	2	5	2	0	7	0	0	1	1	0	1	0
8	3	2	9	1	3	3	2	4	2	0	7	1	0	1	1	0	0	1
8	0	0	9	2	2	6	3	3	0	0	15	1	0	1	1	1	0	1
1	11	14	2	2	2	3	2	0	5	2	16	4	3	0	0	0	0	0
1	1	0	16	1	3	3	1	5	5	2	0	1	0	0	1	0	0	0
8	7	0	8	2	3	2	2	4	4	0	4	1	1	1	1	0	0	1
8	2	2	6	2	1	0	2	5	5	2	16	4	0	0	0	0	0	0
1	7	2	17	1	4	6	2	2	5	0	8	4	1	1	1	0	0	1
1	3	0	27	1	5	3	1	0	2	0	8	4	1	1	0	0	0	0
1	5	2	16	2	2	0	3	5	3	2	3	2	0	1	0	1	0	0
1	7	11	17	2	4	3	3	5	4	0	9	1	0	1	1	1	1	1
1	7	0	18	2	2	3	2	5	5	2	2	2	0	1	1	0	0	1
8	3	0	12	2	2	3	2	5	2	0	2	1	0	1	1	0	0	0
1	11	10	11	2	3	1	3	5	5	0	10	1	0	1	1	1	1	1
1	3	14	20	1	2	3	3	5	2	0	2	1	0	1	1	1	1	1
1	5	2	7	1	4	1	1	2	5	2	16	4	3	0	0	0	0	0
1	1	0	24	1	3	3	1	2	5	0	5	1	0	1	1	0	0	1
1	7	0	25	1	2	1	1	2	4	0	3	1	0	1	1	1	0	0
8	0	0	8	2	1	6	2	3	0	0	6	4	0	0	0	0	0	0
1	5	0	22	1	2	3	1	2	5	0	15	0	0	1	1	1	1	0
1	3	0	24	1	4	3	1	5	5	0	6	1	0	1	1	1	0	1
8	5	0	9	2	2	2	2	4	5	0	2	2	0	1	0	1	0	0
8	7	10	29	1	4	1	2	1	5	0	9	0	0	0	0	0	0	0
8	3	9	16	2	4	3	2	5	2	0	5	0	0	1	1	0	0	1
1	13	0	21	1	2	1	1	5	5	0	1	4	0	1	1	1	0	1
8	7	0	19	1	5	3	1	4	4	0	5	2	0	1	1	0	0	1
8	5	0	12	2	1	3	2	5	5	0	1	4	0	1	1	0	0	1
8	1	5	18	1	3	3	3	1	5	2	16	4	3	0	0	0	0	0
8	7	8	3	1	1	5	2	1	4	2	16	4	3	0	0	0	0	0
1	0	11	7	2	4	6	3	5	0	0	7	1	1	1	1	0	0	1
8	0	0	9	2	4	6	2	3	0	0	8	4	0	1	0	0	0	0
8	7	12	2	0	3	3	1	1	5	2	16	4	3	0	0	0	0	0
8	1	2	15	1	4	3	2	1	5	2	16	4	0	1	1	0	0	0
1	6	0	21	1	2	3	1	0	1	0	3	4	0	1	1	1	0	1
1	11	14	2	2	6	2	2	5	5	0	15	2	0	1	1	0	0	1
1	7	0	26	1	2	3	1	0	4	0	4	0	0	1	1	1	0	1
8	6	0	10	2	1	2	2	5	1	2	3	4	1	0	0	0	0	0
1	1	0	22	1	2	3	1	5	5	0	3	2	0	1	1	0	0	1
8	3	0	12	2	3	3	2	4	2	2	16	4	3	0	0	0	0	0
1	11	3	4	2	3	1	2	2	5	0	6	1	1	1	1	0	0	1
8	7	6	16	1	3	3	2	1	5	2	16	4	3	0	0	0	0	0
1	6	7	4	1	3	3	1	5	5	2	16	4	3	0	0	0	0	0
8	0	9	9	2	2	6	3	3	0	2	16	4	3	0	0	0	0	0
8	3	0	21	1	3	3	3	4	2	2	3	1	0	0	0	0	0	0
8	0	0	8	2	1	6	2	3	0	2	1	3	1	0	0	0	0	0
8	7	0	11	2	1	1	2	4	4	2	16	4	3	0	0	0	0	0
1	0	9	9	2	2	6	3	5	0	2	16	4	3	0	0	0	0	0
1	3	0	21	1	3	3	1	0	2	0	15	1	0	1	1	1	1	1
1	7	14	31	1	3	3	1	2	5	2	16	4	3	0	0	0	0	0
1	7	0	13	2	2	3	2	2	4	0	4	4	1	1	1	0	1	1
8	11	0	14	2	3	1	2	5	5	2	6	2	0	0	0	0	0	0
8	3	0	16	2	3	3	2	1	2	0	15	1	0	1	0	0	0	0
1	5	5	14	1	2	3	2	5	3	0	5	2	0	1	1	1	0	1
8	3	0	15	2	2	3	2	4	2	0	4	1	0	1	1	1	0	1
8	6	0	13	2	2	3	2	5	1	0	4	4	0	1	0	1	0	0
8	7	2	9	2	2	1	3	5	4	2	16	0	0	1	1	0	0	1
1	11	14	12	1	3	3	2	5	5	0	6	1	0	1	1	1	1	1
1	3	6	2	2	2	3	2	0	2	0	8	1	0	1	1	0	0	1
8	6	0	20	1	2	3	1	4	1	1	16	0	1	1	0	0	0	0
8	3	0	17	2	3	3	2	4	2	0	5	1	0	1	0	1	0	0
1	3	0	16	2	4	3	2	5	2	0	4	2	0	1	1	0	0	0
8	3	9	11	2	2	1	3	1	2	2	16	4	0	1	1	0	0	0
1	11	2	5	2	2	1	2	2	5	2	5	0	0	1	0	0	0	0
8	5	7	14	2	2	3	3	4	3	2	16	4	3	0	0	0	0	0
1	5	9	17	2	1	2	3	2	3	0	2	0	1	1	1	0	0	0
1	5	0	9	2	3	3	2	5	3	0	5	0	0	0	0	0	0	0
8	3	0	13	2	5	3	2	1	2	0	5	4	1	1	1	1	1	1
8	5	5	14	2	2	3	3	1	3	0	3	1	0	0	0	0	0	0
8	6	0	17	2	3	3	2	4	1	1	4	4	0	1	1	0	0	0
1	5	0	25	1	2	1	1	5	5	2	16	4	3	0	0	0	0	0
8	3	0	9	2	2	2	2	4	2	0	0	4	0	0	0	0	0	0
1	5	0	13	2	3	3	2	0	5	0	12	1	0	1	1	1	0	1
8	5	6	6	2	5	3	2	5	3	0	12	0	1	1	1	0	0	0
8	6	0	24	1	1	3	1	1	1	0	1	0	0	1	1	0	0	1
8	3	0	8	2	4	3	2	4	2	0	12	1	1	1	1	0	0	1
1	7	0	16	2	5	5	2	5	4	0	11	1	0	1	1	0	0	0
8	6	0	18	2	3	3	2	5	1	0	9	1	1	0	1	0	0	0
8	11	13	26	1	3	3	3	1	5	2	16	4	3	0	0	0	0	0
8	0	0	8	2	2	6	2	3	0	0	14	0	0	1	1	1	0	1
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	7	0	11	2	4	2	2	5	5	0	8	0	0	1	1	0	1	1
8	3	0	8	2	4	0	2	5	2	2	5	3	0	0	0	0	0	0
1	3	11	5	2	2	6	2	0	2	2	1	4	1	0	0	0	0	0
8	3	7	11	2	1	1	3	5	2	0	15	1	0	1	1	0	1	1
8	11	4	12	2	2	3	3	4	5	0	6	4	0	1	1	0	0	1
1	3	6	12	2	2	3	2	2	2	0	4	2	0	1	1	0	0	0
1	5	0	21	1	4	3	1	0	3	0	15	1	0	1	0	1	0	0
8	3	0	10	2	3	3	2	5	2	0	6	4	0	1	1	1	0	0
8	7	0	19	1	5	1	1	1	4	0	15	1	0	0	1	0	0	0
1	5	0	15	2	2	6	2	2	3	0	3	1	0	1	1	0	0	1
8	5	0	20	1	5	3	1	4	3	0	15	0	0	1	1	1	1	1
1	3	7	14	1	4	3	3	0	2	0	8	2	0	1	0	0	1	0
8	3	14	13	2	4	3	2	5	2	0	8	1	0	1	1	0	0	1
8	6	7	1	2	1	1	2	5	1	0	15	4	0	1	1	1	0	0
1	0	14	9	2	1	6	3	5	5	0	3	2	0	1	1	1	0	1
8	6	0	17	2	1	3	2	4	1	0	3	1	2	1	1	0	0	0
1	11	14	3	1	4	1	2	0	5	2	15	4	1	0	0	0	0	0
8	8	0	8	2	3	1	2	4	5	0	6	4	1	1	0	0	0	0
1	3	0	16	2	3	3	2	2	2	0	5	1	0	1	1	1	0	0
1	0	0	8	2	2	6	2	5	0	0	9	4	0	1	1	0	0	1
1	7	0	12	2	1	3	3	0	4	2	16	4	0	1	1	0	0	0
1	9	0	13	2	3	3	2	2	5	2	16	4	3	0	0	0	0	0
8	6	0	11	2	3	2	2	4	5	0	5	1	1	1	1	1	1	1
1	11	2	4	1	2	3	2	2	5	2	16	4	3	0	0	0	0	0
8	11	0	12	2	2	3	2	4	5	2	16	4	3	0	0	0	0	0
8	6	6	1	2	5	6	2	5	1	0	15	1	0	1	1	0	0	1
1	0	0	9	2	8	6	2	5	0	2	16	4	3	0	0	0	0	0
1	5	0	23	1	3	3	1	0	5	2	16	4	1	1	0	1	0	0
1	0	0	9	2	9	6	2	5	0	0	15	1	0	1	1	0	0	1
8	3	0	9	2	2	3	2	4	2	0	2	1	0	1	1	1	1	1
1	5	0	24	1	3	3	1	5	5	0	9	1	0	1	1	0	0	1
1	3	0	8	2	1	2	2	0	2	0	9	1	0	1	1	0	0	1
8	0	1	8	1	2	1	1	5	5	0	7	1	0	1	1	0	0	1
1	0	0	9	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
8	7	0	8	2	9	7	2	5	4	2	16	4	3	0	0	0	0	0
8	5	14	25	1	3	3	1	1	5	0	11	1	0	1	1	1	0	1
8	0	0	8	2	3	6	2	3	0	0	8	1	0	1	1	1	0	1
1	5	0	23	1	1	2	1	5	5	2	16	4	3	0	0	0	0	0
1	5	0	20	1	2	3	1	0	3	0	6	1	0	1	1	1	1	1
8	3	0	9	2	4	3	2	4	2	0	11	2	0	0	0	0	0	0
1	3	3	6	2	1	1	2	2	2	2	16	4	3	0	0	0	0	0
1	0	0	9	2	8	6	2	5	0	2	1	4	0	0	0	0	0	0
8	3	2	11	1	2	1	2	1	5	2	16	4	3	0	0	0	0	0
8	3	0	14	2	3	3	2	1	2	0	6	1	0	1	1	1	0	1
1	9	11	30	1	4	2	3	2	5	0	15	0	0	1	1	0	1	1
8	0	12	8	2	2	0	2	3	0	2	16	4	3	0	0	0	0	0
1	7	14	3	2	1	1	2	2	4	2	6	0	0	1	1	0	0	0
1	7	2	5	2	1	1	2	0	4	2	2	4	3	0	0	0	0	0
8	6	13	11	2	1	1	2	1	1	0	15	0	0	1	1	0	1	1
1	6	0	20	1	4	3	1	5	1	0	4	1	1	1	1	1	0	1
1	8	2	4	1	2	1	1	0	5	2	2	3	0	0	0	0	0	0
8	0	0	8	2	3	0	2	3	0	0	15	1	0	1	1	1	0	0
1	3	5	5	2	2	3	2	5	2	0	4	2	0	1	1	1	0	0
8	3	0	13	2	3	6	2	5	2	0	6	1	0	1	1	1	1	1
8	3	0	19	1	4	3	1	4	2	2	4	3	0	1	1	1	0	0
8	0	9	9	2	1	0	3	3	0	2	16	4	3	0	0	0	0	0
8	5	10	26	1	4	5	2	5	5	0	5	1	0	1	1	0	1	1
0	3	6	1	2	5	6	2	5	2	0	15	1	0	1	1	1	0	0
1	6	3	17	1	4	3	2	0	1	0	3	1	0	1	1	1	0	1
8	3	0	11	2	4	3	2	4	2	0	1	1	0	1	1	0	1	1
8	3	0	10	2	4	6	2	5	5	2	16	4	3	0	0	0	0	0
8	3	2	4	1	5	3	2	1	2	0	15	1	0	1	1	0	0	1
8	7	11	7	2	3	2	3	1	4	2	16	4	0	0	0	0	0	0
1	2	2	10	2	3	0	2	0	5	0	6	1	1	1	0	0	0	0
8	7	5	9	2	2	3	2	4	4	2	16	4	3	0	0	0	0	0
1	6	14	14	2	6	3	3	2	1	0	15	1	0	1	1	1	0	1
8	7	0	21	1	2	3	1	5	4	0	9	1	0	1	1	1	0	1
1	9	2	18	1	2	2	3	2	5	2	16	4	3	0	0	0	0	0
1	3	0	13	2	1	3	2	0	2	0	2	4	0	1	1	0	1	1
8	3	0	18	2	2	3	2	4	2	0	5	4	2	1	1	1	1	1
8	0	0	9	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	7	0	31	1	2	2	1	0	5	2	3	3	0	0	1	0	0	0
8	6	6	1	2	3	3	2	4	1	0	7	2	0	1	1	0	1	1
8	3	0	10	2	3	3	2	4	2	0	7	1	0	1	1	1	0	1
1	7	0	11	2	2	1	2	0	5	2	16	4	1	1	1	0	0	0
1	6	2	13	2	4	6	3	0	1	2	16	4	3	0	0	0	0	0
1	7	2	31	1	3	4	1	0	5	0	6	4	0	0	0	0	0	0
1	3	0	8	2	1	2	2	5	2	0	2	3	2	1	1	0	0	1
1	5	0	20	1	4	3	1	2	5	0	6	1	0	1	1	1	0	1
8	5	0	10	2	2	2	2	4	3	0	1	1	1	1	1	0	0	1
8	11	11	8	2	1	3	3	4	5	2	16	4	3	0	0	0	0	0
8	3	5	17	1	1	6	3	5	5	2	16	4	1	0	0	0	0	0
1	3	9	6	1	3	3	2	0	2	2	2	3	0	0	0	0	0	0
1	7	0	9	2	1	2	2	0	5	2	16	4	3	0	0	0	0	0
1	7	0	14	2	2	3	2	5	4	0	4	1	0	1	1	1	0	1
8	5	0	18	2	3	3	2	4	3	2	7	4	0	1	1	0	0	1
8	3	0	16	2	4	3	2	4	2	0	5	1	0	1	1	1	0	1
8	0	0	9	2	1	6	2	5	0	2	1	1	0	0	0	0	0	0
8	9	0	20	1	2	3	1	4	5	2	1	1	1	1	1	0	0	1
8	0	9	9	2	3	6	3	3	0	0	6	0	0	1	1	0	1	1
1	3	6	2	2	4	3	2	5	2	0	12	1	0	1	1	0	0	1
1	11	0	8	2	1	2	2	5	5	2	16	4	3	0	0	0	0	0
1	11	5	10	2	3	3	3	5	5	0	7	2	0	1	1	1	0	0
8	5	0	18	2	4	3	2	1	3	0	7	1	0	1	1	1	0	0
1	1	0	22	1	5	3	1	5	5	0	12	0	1	0	1	0	0	1
8	6	14	3	2	8	3	2	5	1	0	6	0	0	1	1	1	0	1
1	5	0	10	2	2	2	2	2	3	0	1	1	0	1	1	0	0	0
1	5	2	12	2	1	3	2	5	3	0	9	3	2	1	1	0	0	1
8	5	0	26	1	6	3	1	1	5	0	15	1	0	1	1	1	1	1
1	0	0	18	2	2	6	2	0	5	0	6	4	0	1	1	0	0	0
1	7	2	4	2	2	1	2	2	4	2	16	4	3	0	0	0	0	0
1	5	0	24	1	4	3	1	0	5	0	14	4	0	1	1	0	0	1
1	6	13	3	2	4	3	2	0	1	0	9	1	0	1	1	0	1	1
8	5	1	7	2	4	1	2	5	3	0	9	0	0	1	0	0	0	0
1	7	0	13	2	3	3	2	2	4	0	7	0	0	1	0	0	0	0
1	6	0	22	1	3	3	1	2	1	0	4	0	0	1	0	1	0	0
1	1	6	1	1	3	3	1	5	5	0	4	0	0	1	1	1	0	1
1	3	10	16	2	4	3	2	5	2	0	10	1	0	1	1	0	0	1
1	0	0	9	2	9	6	2	5	0	0	3	1	0	1	1	1	0	1
8	2	2	10	2	3	0	2	5	5	2	16	4	3	0	0	0	0	0
8	0	0	3	3	2	0	3	3	0	2	1	0	0	1	0	0	0	0
3	7	2	9	0	3	3	1	2	5	2	16	3	0	1	0	0	0	0
8	3	0	13	2	1	1	2	4	5	0	2	0	0	1	0	0	0	0
8	11	14	20	1	3	3	3	5	5	0	4	0	0	1	1	1	1	1
8	7	0	13	2	2	3	2	1	4	0	4	3	1	1	1	1	0	1
1	11	11	31	1	3	3	3	2	5	0	5	2	0	1	1	1	0	1
8	3	0	9	2	2	3	2	4	2	2	16	4	3	0	0	0	0	0
1	3	0	20	2	2	3	2	2	2	0	5	2	0	1	1	0	0	0
1	11	2	4	2	5	1	2	2	5	2	16	4	3	0	0	0	0	0
8	6	10	12	2	1	1	3	4	1	0	5	3	0	1	1	0	0	0
1	7	2	11	1	2	1	1	0	5	2	5	4	1	1	1	0	0	0
8	3	0	8	2	2	3	2	4	2	0	7	0	0	1	1	0	0	1
8	11	7	13	2	4	3	3	4	5	2	16	4	3	0	0	0	0	0
8	5	0	10	2	3	3	2	4	3	0	8	0	0	1	1	0	0	1
8	0	11	3	2	1	6	3	3	0	2	1	3	0	0	0	0	0	0
8	9	0	16	2	3	1	2	4	5	0	13	3	0	1	1	0	0	1
1	7	0	26	1	3	3	1	5	4	0	11	1	0	1	1	0	0	0
8	3	3	2	2	4	6	2	1	2	0	15	2	0	1	1	0	0	0
8	3	0	14	2	2	3	2	4	2	2	1	0	0	1	0	0	0	0
1	3	0	10	2	4	3	2	5	2	0	2	1	0	1	1	1	1	0
1	7	9	19	2	3	0	2	2	4	0	12	1	0	1	1	0	0	0
8	5	0	13	2	3	3	2	4	3	0	15	1	0	1	1	1	0	1
1	1	0	29	1	1	3	1	0	5	0	15	4	0	1	1	0	0	1
8	5	2	6	1	4	5	1	1	5	2	6	4	0	1	1	0	0	0
1	3	7	13	1	2	3	2	5	2	1	2	1	0	1	1	0	0	0
8	6	2	5	2	1	1	2	1	1	1	16	1	1	1	1	0	0	0
8	2	12	11	2	2	0	3	4	5	0	9	4	2	1	0	0	0	0
8	3	1	4	2	2	1	2	5	2	2	16	4	3	0	0	0	0	0
8	3	0	19	2	2	4	2	4	2	0	2	1	0	1	1	0	1	1
8	7	0	17	2	3	3	2	1	4	0	7	0	0	1	1	1	0	0
8	0	11	8	2	3	0	2	3	0	2	16	4	3	0	0	0	0	0
1	7	9	18	0	2	2	2	0	5	0	3	0	0	1	1	1	1	1
8	11	2	4	2	1	3	2	4	5	2	16	4	3	0	0	0	0	0
8	5	0	9	2	5	2	2	1	5	0	7	1	0	1	1	0	0	1
1	7	0	19	1	4	3	2	2	5	0	5	1	0	1	1	1	0	0
1	5	6	3	2	4	2	3	5	3	0	8	1	0	1	1	0	0	0
8	3	0	14	2	3	3	2	4	2	0	14	2	0	1	1	0	0	1
8	5	13	9	2	2	3	3	5	3	0	1	0	0	1	1	0	0	0
1	11	2	5	2	5	5	2	2	5	0	15	4	1	0	1	0	0	1
8	0	0	8	2	1	0	2	3	0	0	1	3	1	1	1	0	0	0
8	3	12	11	2	4	3	2	4	2	2	16	4	3	0	0	0	0	0
8	6	4	6	2	7	3	2	4	1	2	16	4	3	0	0	0	0	0
1	6	9	3	1	1	1	2	2	1	2	16	4	3	0	0	0	0	0
1	3	7	10	2	2	3	2	5	2	0	1	4	2	0	0	0	0	0
1	6	11	8	2	2	2	3	4	1	0	5	3	1	1	1	1	0	1
8	3	0	12	2	1	2	2	4	2	2	16	4	3	0	0	0	0	0
8	3	0	8	2	1	2	2	4	2	2	16	4	3	0	0	0	0	0
8	3	8	2	2	6	3	3	4	2	0	5	1	0	1	1	1	0	0
1	7	10	14	2	1	1	3	2	5	0	9	0	1	1	1	0	1	1
1	3	0	21	1	1	1	1	0	2	2	3	0	0	1	0	0	0	0
8	7	14	17	2	2	3	2	4	4	0	1	2	0	1	1	0	0	1
8	7	6	2	1	2	2	1	1	5	0	7	1	0	1	0	1	0	0
1	3	0	10	2	5	3	2	0	2	2	3	4	0	1	0	1	0	0
1	5	0	9	2	2	5	2	5	3	2	16	4	1	0	0	0	0	0
8	3	0	10	2	2	2	2	1	2	0	1	1	0	0	0	0	0	0
1	11	2	12	2	1	1	3	0	5	2	16	0	1	1	1	0	0	1
8	7	0	18	2	2	3	2	4	4	0	1	1	0	1	1	0	1	1
1	11	11	17	2	2	1	3	2	5	2	7	1	0	0	0	0	0	0
1	3	0	20	1	2	3	1	5	2	0	8	1	0	1	1	0	0	0
8	0	0	9	2	3	6	2	3	0	2	16	3	0	0	0	0	0	0
8	6	0	15	2	5	3	2	1	1	2	16	4	3	0	0	0	0	0
1	2	12	5	2	4	0	2	2	5	0	5	1	0	1	0	1	0	0
8	0	0	10	2	3	0	2	3	0	0	3	0	1	1	1	0	0	1
1	5	0	26	1	2	3	1	0	5	0	3	1	0	1	1	0	0	1
8	0	0	9	2	3	6	2	3	0	2	16	4	1	1	0	1	0	0
1	5	3	5	2	3	1	2	0	3	2	16	4	3	0	0	0	0	0
8	0	14	9	2	1	6	3	3	0	2	16	4	3	0	0	0	0	0
8	0	0	9	2	2	6	2	3	0	2	16	4	0	0	0	0	0	0
1	3	0	11	2	1	3	2	0	2	0	3	3	1	1	1	0	0	1
8	3	0	13	2	1	3	2	5	2	2	16	4	3	0	0	0	0	0
1	5	3	6	1	1	3	2	5	3	0	1	4	2	0	1	0	0	0
8	6	0	20	1	4	2	1	1	1	2	16	4	3	0	0	0	0	0
1	3	7	2	2	1	3	2	5	2	2	16	4	3	0	0	0	0	0
1	0	0	9	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
1	5	0	13	2	3	3	2	0	3	0	6	0	0	1	1	1	0	1
1	6	0	17	2	1	3	2	0	1	0	1	2	0	1	1	1	1	1
8	3	0	20	1	2	3	1	4	2	2	16	4	3	0	0	0	0	0
8	5	6	5	2	3	3	2	1	3	0	5	1	0	1	1	1	1	0
1	3	0	11	2	3	1	2	0	2	2	2	4	1	0	0	0	0	0
1	7	2	6	1	4	1	2	5	4	2	5	4	1	0	0	0	0	0
7	3	5	8	2	2	3	2	4	2	0	1	4	0	0	0	0	0	0
8	0	0	9	2	1	6	2	5	0	0	4	0	0	0	0	0	0	0
8	3	12	3	2	3	3	2	4	2	0	3	4	2	1	1	0	0	1
8	2	3	1	2	3	0	2	5	5	0	3	0	0	1	1	0	0	0
1	1	0	23	1	6	3	1	2	5	0	15	1	0	1	1	0	0	1
8	5	11	2	2	1	1	2	5	3	2	1	4	1	0	0	0	0	0
1	11	0	11	2	2	5	2	0	5	2	16	4	3	0	0	0	0	0
1	6	0	17	2	2	3	2	0	1	2	16	0	1	0	0	0	0	0
8	3	0	31	1	2	7	1	1	2	2	16	4	3	0	0	0	0	0
8	7	9	12	2	1	1	3	4	4	2	16	4	3	0	0	0	0	0
8	3	0	9	2	4	3	2	1	2	2	1	0	0	0	0	0	0	0
1	6	5	9	2	2	1	2	0	1	0	5	1	0	1	1	1	0	0
1	11	5	5	2	1	1	2	2	5	0	5	4	1	1	1	0	0	1
1	5	0	20	1	2	3	1	0	3	0	1	4	0	1	0	0	0	0
1	5	10	26	1	2	3	3	2	5	0	4	1	0	1	1	1	0	0
1	11	0	10	2	1	2	3	0	5	0	1	0	0	1	0	1	1	0
8	5	2	7	2	2	1	2	4	3	0	2	4	0	1	0	0	0	0
8	3	0	17	2	1	3	2	4	2	2	1	4	2	0	0	0	0	0
1	9	2	7	1	6	1	2	5	5	0	2	1	0	0	0	0	0	0
8	0	0	8	2	0	6	2	5	0	2	16	4	3	0	0	0	0	0
8	0	0	10	2	1	6	2	5	0	0	2	0	1	1	1	0	0	1
1	3	0	14	2	7	1	2	2	2	0	15	1	1	0	0	0	0	0
1	6	2	7	1	1	1	2	5	1	0	6	4	1	1	1	0	0	1
1	9	0	20	2	1	3	2	5	5	0	3	0	0	1	1	1	0	1
1	5	2	10	2	1	2	2	5	3	0	8	1	0	1	1	0	0	1
1	3	0	16	2	2	3	2	2	2	0	3	1	0	1	0	0	0	0
1	11	0	14	2	3	3	2	2	5	0	12	1	0	1	1	1	1	1
1	1	0	22	1	2	3	1	5	5	0	2	4	0	1	1	0	0	0
8	7	2	9	1	1	1	1	1	5	0	5	1	0	1	1	1	0	0
1	5	0	8	2	1	2	2	5	3	2	1	0	0	1	1	1	0	1
8	6	0	9	2	1	2	2	1	1	0	2	1	0	1	1	1	0	1
1	5	5	1	2	5	6	2	2	3	0	7	1	0	1	1	0	0	1
1	3	0	12	2	3	3	2	2	2	0	4	1	0	1	1	0	1	1
1	5	2	9	2	1	2	3	5	3	0	5	0	1	1	1	1	0	1
1	9	0	9	2	3	2	2	0	5	0	3	1	0	1	1	0	0	1
8	7	2	19	0	2	3	2	1	5	0	7	1	0	1	1	0	0	1
1	3	0	12	2	2	3	2	0	2	0	3	1	0	1	1	0	0	1
8	0	0	9	2	0	0	2	3	0	2	16	4	3	0	0	0	0	0
1	11	0	10	2	3	2	2	5	5	0	5	4	0	1	1	0	0	1
8	6	2	6	2	3	0	2	5	1	0	3	1	0	1	1	0	0	0
1	11	2	15	0	2	1	2	2	5	0	16	4	0	0	0	0	0	0
8	11	2	2	2	1	3	2	4	5	2	16	4	2	1	1	0	0	1
8	5	7	8	2	3	3	2	4	3	0	2	2	0	1	1	1	0	0
1	5	8	26	1	2	3	3	5	3	2	5	0	0	1	0	0	0	0
8	3	0	19	2	3	3	2	4	2	0	5	1	0	1	0	1	0	0
1	3	13	17	1	3	3	3	2	2	0	4	1	0	1	1	0	1	1
8	3	0	12	2	2	3	2	4	2	0	7	1	0	1	0	0	0	0
8	5	0	18	2	3	3	3	4	3	0	6	1	0	1	1	0	0	1
1	3	0	13	2	4	3	2	0	2	0	15	1	0	1	1	1	0	1
8	6	11	4	2	5	1	2	5	1	2	16	4	3	0	0	0	0	0
8	8	0	8	2	2	2	2	4	5	0	3	0	0	1	1	0	0	0
8	3	0	19	1	3	3	1	5	2	0	10	1	0	1	1	1	0	0
8	3	0	12	2	5	3	2	4	5	0	15	1	0	1	1	0	0	0
1	0	0	8	2	2	0	2	3	0	2	16	4	3	0	0	0	0	0
1	2	4	4	2	3	0	2	2	5	2	16	4	3	0	0	0	0	0
8	3	0	11	2	1	1	2	5	2	0	9	1	0	1	0	1	0	0
1	7	2	4	2	2	1	2	0	4	2	2	0	0	0	1	0	0	1
8	3	2	5	2	3	0	2	5	2	2	0	4	0	0	0	0	0	0
1	9	2	23	1	2	6	3	0	5	0	8	1	0	1	0	0	0	0
8	3	2	9	1	1	1	2	1	2	2	16	4	3	0	0	0	0	0
8	3	2	4	2	2	5	2	4	2	2	16	4	3	0	0	0	0	0
1	11	2	5	2	4	1	2	5	5	0	7	2	0	1	1	0	0	1
1	3	0	21	1	3	2	1	0	2	2	1	1	0	1	1	0	0	0
1	11	0	15	2	1	2	2	5	5	0	3	3	0	0	1	0	0	0
8	3	0	10	2	1	2	2	5	2	2	4	1	1	0	0	0	0	0
8	7	5	8	2	1	1	3	5	4	2	3	3	1	0	0	0	0	0
8	3	0	20	1	3	3	1	5	2	0	3	1	0	1	1	0	0	1
8	3	0	15	2	1	3	2	1	2	0	7	1	0	1	1	1	0	1
1	5	0	9	2	4	2	2	2	3	0	5	4	1	1	1	0	0	0
1	3	14	2	2	4	3	2	2	2	0	6	0	0	1	1	0	0	0
8	3	0	9	2	4	3	2	5	2	0	1	3	0	0	0	0	0	0
8	3	5	10	2	3	3	2	1	2	0	1	4	0	1	1	0	0	1
8	3	0	14	2	3	3	2	4	5	0	5	4	1	1	1	1	0	1
1	6	2	8	1	2	1	2	5	1	0	4	2	0	1	1	0	0	1
8	0	0	8	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
8	5	9	8	2	2	3	2	4	3	0	7	1	0	1	1	1	0	1
1	3	0	11	2	3	3	2	0	2	0	4	0	1	1	1	0	1	1
8	2	2	6	2	2	0	2	5	5	2	16	4	3	0	0	0	0	0
8	5	9	20	1	2	1	3	5	3	2	16	4	3	0	0	0	0	0
8	9	2	11	1	1	3	2	1	5	2	16	4	3	0	0	0	0	0
1	0	9	8	2	8	6	3	5	0	2	16	4	3	0	0	0	0	0
1	3	0	12	2	5	2	2	0	2	0	15	1	0	1	1	1	0	1
8	3	0	14	2	5	1	2	4	2	0	4	4	2	1	1	0	0	0
8	0	0	9	2	4	6	2	3	0	0	8	3	1	1	1	0	0	0
1	7	0	25	1	4	3	1	2	5	0	15	1	0	1	1	1	1	1
8	7	2	25	1	4	3	3	1	5	0	15	1	0	1	1	0	0	0
8	3	0	13	2	3	3	2	4	2	0	2	4	2	1	1	1	0	0
8	9	2	7	2	2	2	3	1	5	2	16	4	3	0	0	0	0	0
1	5	0	31	0	1	2	0	0	3	2	16	4	3	0	0	0	0	0
1	3	6	12	1	4	1	2	5	2	0	10	0	1	1	1	0	0	1
8	3	14	5	1	3	1	2	1	2	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	6	2	3	0	2	3	3	0	0	0	0	0	0
8	7	0	15	2	2	3	2	4	4	0	1	4	0	1	1	0	1	1
8	6	1	1	2	3	3	2	5	1	2	3	3	0	1	1	0	0	0
3	5	4	3	1	2	3	1	2	5	0	1	3	0	1	0	0	0	0
4	5	9	13	2	1	3	3	1	3	2	4	4	0	0	0	0	0	0
1	3	6	1	2	5	3	2	2	2	0	1	1	0	1	1	0	0	0
8	3	9	9	2	2	3	3	4	2	0	6	0	1	1	1	1	1	1
1	0	12	3	2	8	5	3	5	0	0	6	3	0	1	1	0	0	0
8	6	2	8	1	3	0	2	4	1	2	4	4	0	1	0	0	0	0
1	6	3	6	2	2	3	3	0	1	0	6	1	0	1	1	1	1	1
8	7	0	14	2	4	3	2	4	4	0	14	3	2	0	0	0	0	0
8	6	10	20	1	3	3	2	4	1	0	13	1	0	1	1	0	0	1
1	7	2	10	1	1	1	1	2	5	0	5	0	1	1	1	0	0	0
8	3	12	3	1	3	1	2	5	2	0	2	2	0	1	1	0	0	0
1	0	9	20	1	1	0	2	5	5	0	7	4	0	1	1	0	0	1
1	5	0	26	1	3	3	1	0	5	2	16	4	3	0	0	0	0	0
8	0	9	3	2	2	6	2	3	0	0	5	4	1	1	1	0	0	0
8	2	12	3	2	2	0	2	3	5	2	16	4	3	0	0	0	0	0
8	3	0	9	2	5	3	2	5	2	0	15	4	0	0	0	0	0	0
8	3	0	8	2	2	3	2	4	2	2	0	0	1	0	0	0	0	0
1	3	0	8	2	2	2	2	2	2	0	5	1	0	1	1	0	0	1
8	9	5	3	2	1	3	2	1	5	0	15	1	0	1	1	0	1	1
1	6	10	13	2	6	3	3	2	1	0	8	0	1	1	1	0	0	1
1	13	0	30	1	3	3	1	5	5	0	7	1	0	1	1	0	0	0
8	3	0	20	1	3	3	1	4	2	0	10	1	0	1	1	0	0	1
8	0	0	9	2	3	6	2	3	0	0	5	4	1	1	0	1	0	0
8	5	0	14	2	1	5	2	5	3	2	16	4	3	0	0	0	0	0
1	5	2	14	1	2	3	3	0	3	0	0	4	0	0	0	0	0	0
8	6	2	8	2	7	2	3	5	1	0	15	4	1	1	1	0	0	0
1	6	10	19	1	2	3	3	0	1	2	16	4	3	0	0	0	0	0
8	3	0	8	2	1	3	2	4	2	0	1	0	0	1	1	0	1	1
8	6	9	3	1	1	3	2	1	1	2	16	4	3	0	0	0	0	0
1	8	14	29	1	3	3	2	2	5	0	6	4	0	1	1	1	0	1
8	6	8	1	1	1	1	1	1	1	0	15	2	0	1	0	0	0	0
8	7	0	11	2	3	2	2	4	4	0	13	4	0	1	1	0	0	1
1	9	11	1	2	2	2	2	5	5	0	10	2	0	1	1	0	0	0
8	5	5	19	1	1	3	3	5	5	0	14	4	0	0	0	0	0	0
1	3	5	5	2	4	3	3	5	2	0	6	3	0	0	0	1	0	0
8	3	2	4	1	2	3	2	5	2	0	2	0	0	1	1	0	0	1
8	3	0	19	1	4	3	1	5	2	0	9	2	0	1	1	0	1	1
8	3	13	12	2	2	3	2	1	2	0	15	1	0	1	0	0	0	0
8	5	6	3	1	3	2	1	1	5	0	7	4	0	1	0	0	0	0
8	0	0	8	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	5	2	16	2	1	1	3	5	3	2	16	4	3	0	0	0	0	0
8	6	2	4	2	1	1	2	4	1	0	7	1	0	1	1	1	1	1
1	3	0	10	2	6	3	2	2	2	0	15	1	0	1	1	0	0	1
1	9	12	12	1	4	2	2	2	5	0	10	4	0	1	1	1	1	1
8	3	0	8	2	4	2	2	4	2	0	9	3	2	1	1	0	0	1
8	0	0	9	2	2	6	2	5	0	2	1	3	0	0	0	0	0	0
1	7	12	15	1	2	3	2	0	5	0	6	4	0	1	1	0	0	1
8	3	8	1	2	1	3	2	5	5	0	8	0	0	1	1	1	1	1
8	6	2	9	2	1	1	3	1	1	2	3	4	1	0	0	0	0	0
1	5	0	10	2	2	3	2	0	3	0	6	1	0	1	1	0	0	1
1	3	5	12	2	2	3	2	2	2	0	6	1	0	1	0	0	0	0
8	11	2	7	2	2	3	3	4	5	0	4	1	0	1	1	0	0	1
1	6	2	4	2	3	1	2	2	1	0	5	1	0	1	1	0	0	1
1	9	0	30	1	4	3	1	5	5	2	16	4	3	0	0	0	0	0
1	7	6	7	1	3	3	1	5	5	0	6	1	0	1	0	1	0	0
1	5	2	8	2	2	2	3	0	3	0	4	4	0	0	1	0	0	1
1	5	14	4	2	1	5	2	5	3	2	16	4	3	0	0	0	0	0
1	6	6	2	2	4	3	2	5	1	0	8	2	0	1	1	0	0	1
8	3	14	18	2	4	3	2	4	2	0	11	1	0	1	1	1	1	1
8	6	0	13	2	2	3	2	4	1	0	1	4	1	1	0	0	0	0
8	3	9	14	1	2	5	2	5	2	2	16	4	3	0	0	0	0	0
8	7	0	23	1	2	3	1	1	5	2	1	1	1	1	1	0	0	1
8	0	9	8	2	1	6	3	3	0	0	4	2	0	1	1	1	1	1
8	7	0	11	2	1	1	2	4	4	2	2	3	1	0	0	1	0	0
1	5	2	4	2	4	2	2	5	3	0	5	3	1	1	0	0	0	0
8	3	3	4	1	1	6	2	1	2	2	16	4	3	0	0	0	0	0
8	3	2	11	2	2	6	3	1	2	2	3	0	0	1	0	0	0	0
8	3	0	20	2	2	4	2	5	2	0	5	0	0	1	1	0	1	1
1	6	0	11	2	1	1	2	5	1	0	7	1	0	1	1	1	0	0
1	3	0	12	2	1	1	2	0	2	0	2	1	0	1	1	1	0	0
1	7	13	20	1	4	3	2	2	5	0	8	1	0	1	1	1	0	1
1	6	10	13	2	2	3	3	2	1	0	5	4	0	1	1	1	0	1
1	7	2	9	1	2	2	2	2	4	0	15	1	0	1	1	1	0	0
1	5	5	9	2	4	3	3	0	3	0	9	1	0	1	1	1	0	1
1	3	2	8	1	1	3	2	5	2	2	2	4	0	0	0	0	0	0
8	3	2	8	2	1	1	3	4	2	0	7	2	0	0	1	0	0	1
8	3	0	13	2	3	3	2	4	2	0	2	1	0	1	1	0	0	1
8	3	0	22	1	3	3	1	1	2	0	15	0	0	1	1	1	1	1
1	11	14	17	2	3	3	3	0	5	0	3	1	0	1	1	1	0	1
8	3	0	8	2	4	2	2	4	2	0	5	2	0	1	1	0	0	0
8	3	0	11	2	4	3	2	1	2	0	2	1	0	1	1	1	1	1
1	1	0	23	1	2	2	1	5	5	1	12	0	0	1	1	0	0	0
1	3	0	14	2	3	3	2	2	2	2	0	1	0	1	0	0	0	0
8	0	0	9	2	1	6	2	3	0	0	12	1	0	1	0	0	0	0
3	6	2	8	2	5	3	3	2	1	0	15	4	0	1	1	0	0	1
8	10	0	10	2	3	2	2	5	5	0	5	1	0	1	1	0	0	1
1	7	12	1	1	4	5	1	5	5	0	13	4	0	1	1	1	0	1
1	0	0	9	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
1	3	0	8	2	5	7	2	5	2	2	16	4	0	1	0	0	0	0
8	7	0	19	1	5	3	1	4	4	0	15	1	0	1	1	0	0	1
1	5	0	22	1	5	3	1	2	5	0	15	1	0	1	1	0	0	1
8	3	6	2	2	2	1	2	5	2	1	2	1	1	0	0	0	0	0
1	5	2	6	2	1	1	3	2	3	2	16	4	3	0	0	0	0	0
1	7	6	8	2	3	3	3	2	4	0	5	1	0	1	1	1	1	1
8	7	0	14	2	2	3	2	4	4	2	2	0	0	1	0	0	0	0
1	7	0	29	1	2	3	1	2	5	2	16	4	3	0	0	0	0	0
8	7	2	4	2	1	1	2	4	4	2	4	4	1	1	1	0	0	1
8	9	0	21	1	2	3	1	4	5	0	2	4	0	0	0	0	0	0
8	5	0	25	1	2	3	1	1	5	0	1	3	1	0	0	0	0	0
1	11	8	12	2	3	3	3	0	5	0	5	2	0	1	1	0	0	1
1	7	2	7	0	2	6	1	5	5	2	16	4	3	0	0	0	0	0
1	3	0	11	2	4	3	2	0	2	0	7	4	0	1	1	0	0	0
8	3	0	20	1	3	3	1	4	5	0	6	1	0	1	1	0	0	1
8	9	0	20	1	3	2	1	1	5	0	3	1	0	1	1	1	0	1
8	0	9	7	2	5	6	3	3	0	2	16	4	3	0	0	0	0	0
1	7	10	18	2	3	1	3	5	5	0	5	0	0	1	1	1	0	1
8	7	10	15	2	2	0	3	1	5	0	12	1	1	0	0	0	0	0
1	3	0	20	2	3	0	2	5	2	0	3	1	0	1	1	1	0	1
1	5	2	15	1	2	3	2	2	5	2	16	4	3	0	0	0	0	0
8	9	0	11	2	2	4	2	1	5	2	16	4	3	0	0	0	0	0
8	6	0	11	2	2	3	2	5	1	2	0	3	0	0	0	0	0	0
1	5	2	6	1	3	6	1	5	5	2	3	2	0	0	0	0	0	0
1	11	2	4	2	1	6	2	2	5	0	3	0	1	0	1	0	0	1
1	0	9	8	2	0	6	3	5	0	2	16	4	3	0	0	0	0	0
8	5	6	1	1	3	3	2	4	3	0	15	1	0	1	1	0	0	1
1	11	0	18	2	4	1	2	0	5	0	7	4	0	1	0	1	0	0
3	3	6	5	2	1	3	3	4	2	2	16	4	3	0	0	0	0	0
1	5	8	15	2	3	3	3	0	3	2	16	4	3	0	0	0	0	0
1	7	10	14	2	1	2	3	2	5	2	16	4	3	0	0	0	0	0
8	11	0	11	2	4	3	2	4	5	0	15	1	1	0	0	0	0	0
8	5	0	10	2	3	2	2	4	3	0	2	1	0	1	1	0	1	1
8	3	2	12	2	4	3	2	4	2	0	7	2	0	1	1	0	1	1
1	6	10	21	1	2	3	3	2	1	0	4	1	0	1	1	0	0	1
1	6	1	3	2	6	1	2	0	1	2	15	1	0	1	1	1	0	1
1	0	0	8	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
8	11	0	14	2	2	1	2	4	5	2	16	4	3	0	0	0	0	0
1	0	0	10	2	2	0	2	5	0	2	16	4	3	0	0	0	0	0
8	6	0	13	2	4	3	2	1	1	0	5	0	0	1	1	0	0	0
8	7	0	14	2	3	3	2	4	4	2	16	4	3	0	0	0	0	0
1	9	14	2	2	3	2	3	2	5	2	16	4	3	0	0	0	0	0
1	5	2	22	1	2	2	3	2	5	0	3	2	0	1	1	1	0	0
8	6	13	15	1	5	3	3	5	1	2	16	4	3	0	0	0	0	0
3	9	2	25	1	3	3	3	2	5	2	16	4	3	0	0	0	0	0
8	3	7	8	2	1	3	3	4	2	0	3	4	1	1	0	0	0	0
1	7	10	16	2	2	1	2	2	5	0	1	2	0	1	1	0	0	1
1	7	2	13	1	2	6	1	0	5	0	6	4	0	1	1	1	0	0
8	3	14	1	2	4	1	2	4	2	0	7	4	0	0	1	0	0	0
1	0	0	9	2	3	0	2	5	0	2	16	4	3	0	0	0	0	0
8	3	0	8	2	4	3	2	4	2	0	8	1	0	1	1	1	0	1
8	0	0	10	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	0	13	2	1	3	2	0	2	2	16	4	3	0	0	0	0	0
1	3	2	17	2	2	3	3	2	2	0	5	1	0	1	1	1	0	0
1	7	3	17	2	4	3	3	2	4	0	6	1	0	1	0	0	0	0
8	7	0	12	2	3	3	2	5	4	0	15	1	0	1	1	1	1	1
1	0	0	8	2	1	0	2	5	0	2	16	4	3	0	0	0	0	0
8	0	14	5	2	4	6	3	1	5	0	2	1	0	1	1	1	0	0
8	0	0	8	2	9	6	2	3	0	0	7	1	0	1	1	0	0	0
8	5	0	9	2	3	2	2	4	3	0	15	1	0	1	1	1	0	1
8	3	14	1	2	3	3	2	4	2	0	5	1	0	1	1	0	0	1
1	0	0	9	2	1	6	2	5	0	0	15	3	0	1	1	0	0	1
8	6	2	15	2	1	3	2	4	1	2	16	4	3	0	0	0	0	0
8	3	6	1	1	5	3	1	5	5	0	10	4	2	1	1	0	1	1
1	11	10	31	1	3	3	2	5	5	0	4	0	1	1	0	1	0	0
1	9	7	1	2	1	0	2	2	5	2	16	4	3	0	0	0	0	0
8	5	0	19	2	2	3	2	4	3	0	3	1	1	1	1	0	0	1
8	5	2	15	1	2	3	2	4	3	0	15	2	0	1	1	0	0	1
1	5	0	12	2	3	3	2	0	3	2	16	4	3	0	0	0	0	0
8	7	2	15	2	3	3	3	5	4	0	5	1	0	1	1	1	1	1
8	6	2	11	2	1	3	2	5	1	0	2	0	0	1	1	1	0	1
1	7	3	3	2	5	1	2	5	4	2	16	4	3	0	0	0	0	0
8	7	0	31	0	3	3	0	1	5	2	16	4	3	0	0	0	0	0
1	3	0	23	1	3	3	1	0	2	0	2	4	1	1	1	1	0	1
8	6	2	8	2	1	1	3	4	1	2	16	4	1	0	0	0	0	0
1	5	2	21	1	3	3	2	2	5	0	5	1	0	1	1	0	0	1
1	7	2	12	0	1	2	1	2	5	0	5	0	0	1	1	0	0	1
1	3	0	11	2	5	3	2	0	2	0	2	1	0	1	1	1	1	1
1	7	0	9	2	5	2	2	0	4	0	10	0	0	1	1	0	1	0
8	3	0	20	1	4	3	1	1	2	0	2	0	0	1	1	0	0	1
8	3	0	12	2	1	1	2	0	2	2	16	0	1	1	0	0	0	0
8	7	10	18	2	2	3	2	5	4	0	15	1	0	1	1	1	0	1
8	3	0	20	1	3	3	1	4	2	0	3	4	0	1	1	0	0	1
1	5	0	21	1	2	3	1	5	3	2	3	4	1	1	1	0	0	0
8	7	0	31	1	3	2	1	1	5	0	14	2	0	1	1	1	0	0
8	3	0	14	2	5	3	2	1	2	0	15	1	0	1	1	0	0	0
1	5	0	25	1	2	3	1	0	5	2	0	0	0	1	1	0	0	0
8	5	0	8	2	5	4	2	4	3	0	3	1	1	0	0	0	0	0
1	5	2	19	1	5	6	2	2	3	0	15	1	0	1	1	0	0	0
8	3	9	11	2	3	3	3	4	2	2	16	4	3	0	0	0	0	0
8	3	9	6	1	4	3	2	4	2	0	8	0	0	1	1	1	1	1
1	5	0	23	1	3	3	1	5	5	0	7	1	0	1	1	0	0	1
8	7	0	15	2	2	3	2	5	4	2	16	4	3	0	0	0	0	0
1	0	2	17	1	3	1	2	0	5	0	14	1	1	1	1	0	0	1
8	5	9	8	1	1	2	1	1	5	2	16	4	3	0	0	0	0	0
1	1	2	6	1	1	1	2	5	5	2	2	4	0	0	1	0	0	0
1	9	0	11	2	5	2	2	0	5	0	11	1	0	1	1	1	1	0
8	3	6	14	2	1	3	2	4	2	0	1	1	0	1	1	0	1	1
1	3	7	1	2	5	6	2	5	2	0	15	1	0	1	1	1	0	1
8	6	5	1	2	1	1	2	4	1	2	16	0	0	1	1	0	0	1
1	3	2	5	2	1	1	3	5	2	2	16	4	3	0	0	0	0	0
8	3	0	8	2	5	3	2	5	2	0	10	0	1	1	1	1	0	1
8	3	0	13	2	3	3	2	4	2	0	7	1	0	1	1	1	0	1
8	0	0	9	2	1	6	2	3	0	2	4	4	0	0	0	0	0	0
8	5	2	7	1	2	1	1	1	5	2	4	0	0	0	0	0	0	0
1	3	7	1	2	5	3	2	5	2	0	6	1	1	1	1	1	0	0
8	5	9	2	2	4	3	2	1	3	0	6	1	0	1	1	0	0	1
8	3	0	19	1	3	6	1	1	2	0	2	4	0	1	1	1	0	0
8	6	2	6	2	2	3	2	5	1	0	5	0	0	1	0	0	0	0
8	0	5	1	2	3	6	2	3	0	0	4	1	0	1	1	0	0	1
1	3	0	12	2	3	3	2	5	2	2	16	4	3	0	0	0	0	0
1	3	0	13	2	2	2	2	5	2	0	3	1	0	1	1	0	0	1
1	3	0	15	2	1	3	2	0	2	2	16	4	0	1	1	0	0	1
1	7	2	3	2	1	1	2	2	4	2	16	4	3	0	0	0	0	0
1	6	2	12	2	3	1	3	0	1	0	2	1	0	1	1	0	0	1
8	3	0	13	2	7	3	2	5	2	2	16	4	1	0	0	0	0	0
1	6	1	4	2	3	1	2	5	1	0	5	4	0	1	0	0	0	0
1	9	2	6	1	2	3	1	2	5	2	16	4	3	0	0	0	0	0
1	7	0	13	2	1	3	2	0	4	0	2	3	0	1	1	0	0	1
8	9	4	2	2	2	1	2	4	5	2	16	4	3	0	0	0	0	0
1	3	6	3	2	4	3	2	5	2	0	8	1	1	1	1	1	0	0
8	3	0	10	2	2	2	2	4	2	0	2	0	0	1	0	0	0	0
8	7	2	8	1	1	1	2	4	4	2	16	4	3	0	0	0	0	0
1	6	0	12	2	3	3	3	2	1	0	4	0	0	1	1	1	0	1
8	0	9	8	2	1	6	3	3	0	0	3	4	1	1	0	0	0	0
1	0	0	8	2	3	0	2	5	0	0	4	0	1	1	1	0	0	1
1	7	14	2	1	3	1	1	0	5	0	7	1	0	1	1	1	0	1
1	3	0	12	2	3	3	2	2	2	0	5	2	0	1	1	0	0	0
8	7	0	16	2	2	3	2	4	4	2	16	4	3	0	0	0	0	0
1	5	6	1	1	2	3	1	0	5	0	4	4	0	1	1	0	1	0
1	7	1	16	1	3	3	2	2	5	0	15	0	0	0	1	0	0	0
8	0	0	9	2	4	6	2	3	0	0	14	4	0	1	1	1	1	1
8	3	0	12	2	1	3	2	4	2	2	1	1	1	0	0	0	0	0
1	7	0	16	2	3	3	2	5	5	2	16	4	3	0	0	0	0	0
8	11	2	10	2	3	1	3	1	5	2	16	3	0	1	1	0	0	0
1	7	7	20	1	3	3	2	5	5	0	11	1	0	1	1	1	1	1
1	7	13	29	1	1	3	3	2	5	0	12	1	0	1	1	1	0	0
1	3	0	19	2	4	3	2	0	2	2	16	4	3	0	0	0	0	0
8	7	0	17	2	3	3	2	1	4	0	3	1	0	1	0	1	0	0
1	3	0	11	2	3	3	2	0	2	0	12	1	0	1	1	1	1	1
1	9	2	4	2	2	1	2	2	5	0	8	1	0	1	1	0	0	0
8	3	0	12	2	3	3	2	1	2	0	6	1	0	1	1	1	1	1
1	7	0	8	2	3	6	2	5	4	0	6	1	0	1	1	1	0	1
8	7	14	0	1	2	3	2	1	5	0	5	0	0	1	1	1	0	1
1	7	0	14	2	4	3	2	5	4	0	10	1	0	1	1	1	0	1
8	5	0	20	1	1	1	1	1	3	0	3	0	0	1	0	1	0	0
1	9	0	26	1	2	3	1	2	5	0	3	2	0	1	1	1	1	0
1	6	2	9	1	1	1	1	2	5	2	4	4	1	0	1	0	0	0
1	6	9	7	2	1	3	2	5	1	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	0	2	3	0	0	2	0	0	1	1	0	0	0
8	7	0	10	2	2	3	2	4	4	0	5	0	0	1	1	0	0	1
1	9	0	17	2	2	3	2	0	5	0	8	0	0	1	1	1	0	1
8	5	13	15	1	3	3	2	1	5	2	16	4	3	0	0	0	0	0
8	3	0	12	2	5	3	2	4	2	0	8	1	0	1	1	0	0	1
1	11	5	3	2	2	6	2	2	5	2	16	4	0	0	0	0	0	0
1	11	6	7	2	3	3	3	0	5	2	16	4	3	0	0	0	0	0
8	7	0	13	2	2	3	2	4	4	2	16	4	3	0	0	0	0	0
1	6	0	25	1	3	3	1	2	1	0	6	1	0	1	1	0	0	0
8	6	3	19	1	2	3	2	4	1	0	5	4	1	1	0	0	0	0
1	1	0	26	1	4	3	1	5	5	0	9	1	0	1	1	1	0	1
8	9	2	6	2	1	1	3	5	5	2	16	4	3	0	0	0	0	0
1	3	2	6	2	2	1	2	5	2	0	5	3	0	1	1	0	0	0
8	7	0	15	2	2	3	2	4	4	0	3	0	0	1	0	1	0	0
1	7	0	13	2	4	3	2	2	5	0	12	1	0	1	1	1	1	1
1	3	0	25	1	1	3	1	0	2	2	16	0	1	1	1	0	0	1
8	6	3	3	2	1	2	2	1	1	0	8	1	0	1	1	1	0	1
8	6	3	4	2	2	1	2	4	1	2	16	0	0	1	1	0	0	0
8	6	2	10	2	1	1	2	5	1	2	16	4	3	0	0	0	0	0
1	0	9	9	2	8	6	3	5	0	2	16	4	3	0	0	0	0	0
0	5	2	7	2	1	1	3	1	3	0	7	1	0	1	1	1	0	1
1	0	0	8	2	2	6	2	5	0	2	0	4	0	0	0	0	0	0
8	0	0	8	2	9	6	2	5	0	0	15	0	0	1	1	1	0	0
8	6	5	11	1	2	1	2	4	1	2	16	4	3	0	0	0	0	0
8	3	0	11	2	2	0	2	1	2	0	3	0	0	1	1	1	0	0
1	3	0	17	2	2	4	2	0	2	0	4	0	0	1	1	0	1	1
1	5	0	11	2	1	3	2	5	3	0	12	2	0	1	1	1	0	1
1	5	3	1	1	1	2	1	0	5	2	16	4	3	0	0	0	0	0
1	5	13	14	1	3	3	2	0	5	0	15	1	1	1	1	1	0	1
8	6	2	5	2	3	3	2	5	1	0	5	1	1	1	1	1	0	1
8	3	0	20	1	5	3	1	4	2	0	9	1	0	1	1	1	0	1
8	5	0	10	2	2	3	3	4	3	0	1	1	0	1	1	0	1	1
8	0	12	9	2	3	6	3	3	0	2	16	4	0	0	0	0	0	0
1	9	9	30	1	4	7	2	5	5	2	2	4	3	0	0	0	0	0
1	11	5	2	2	1	1	2	0	5	0	2	1	1	1	0	0	0	0
1	7	0	11	2	3	3	2	2	5	0	7	1	0	1	1	1	1	1
1	7	0	13	2	3	3	2	5	4	2	3	0	0	0	1	0	0	1
1	2	9	8	2	2	0	3	5	5	2	2	3	1	0	0	0	0	0
1	7	0	13	2	5	3	2	0	5	0	2	0	0	1	1	0	0	1
1	7	0	20	1	3	3	1	2	4	0	4	1	0	1	1	0	1	0
1	3	0	8	2	2	2	2	5	2	0	2	4	0	1	1	0	0	1
8	2	5	12	2	2	0	3	5	5	0	2	2	0	1	1	0	0	0
1	3	5	1	2	1	0	2	5	5	2	0	4	0	0	0	0	0	0
1	9	0	31	1	9	4	1	5	5	0	9	1	0	1	1	1	1	1
8	2	2	8	2	2	0	2	1	5	0	15	0	0	1	1	1	0	1
8	3	0	14	2	4	3	2	4	2	2	16	4	3	0	0	0	0	0
8	3	0	13	2	4	3	2	4	2	0	15	1	0	1	1	0	0	1
8	7	0	13	2	1	0	2	4	4	0	15	4	0	1	1	1	0	1
8	3	0	13	2	4	2	2	1	2	0	8	1	0	1	1	1	0	1
8	3	5	6	2	3	3	2	4	2	2	16	4	3	0	0	0	0	0
1	8	0	11	2	1	2	2	5	5	1	0	4	0	1	0	0	0	0
8	2	9	7	2	3	6	2	4	5	2	16	3	0	1	1	0	0	0
1	3	3	4	1	1	3	2	5	2	0	1	4	0	0	0	0	0	0
1	3	0	11	2	2	3	2	0	2	0	15	1	0	1	0	1	0	0
1	7	5	6	2	4	2	3	5	4	0	8	0	0	1	1	0	0	0
1	3	6	4	1	4	3	2	2	2	0	14	4	0	1	1	0	0	1
8	0	14	8	2	4	6	3	4	5	1	2	0	0	1	0	0	0	0
1	6	2	4	2	1	1	2	5	1	0	7	1	0	1	1	1	0	1
1	11	0	14	2	5	3	2	0	5	0	2	4	3	0	0	0	0	0
1	3	0	11	2	4	2	2	0	2	0	10	1	0	1	1	0	0	1
1	3	9	5	2	1	1	3	5	2	2	16	4	3	0	0	0	0	0
8	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	9	5	8	1	3	3	2	5	5	0	4	0	0	1	1	1	0	1
1	11	10	26	1	2	3	3	2	5	0	4	0	0	1	0	0	0	0
8	5	0	11	2	3	3	2	4	3	0	3	1	0	0	0	0	0	0
8	0	0	9	2	1	0	2	3	0	2	16	4	3	0	0	0	0	0
1	7	7	26	1	2	1	1	0	5	0	6	1	0	1	1	0	0	1
1	7	9	17	0	2	3	2	5	5	2	16	4	0	0	0	1	0	0
8	0	0	8	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
8	0	0	10	2	3	7	2	3	0	2	16	4	3	0	0	0	0	0
1	5	10	29	1	1	2	2	5	5	2	16	4	3	0	0	0	0	0
8	6	3	1	2	1	2	2	4	1	2	16	4	3	0	0	0	0	0
8	0	0	10	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
8	3	0	11	2	4	3	2	4	2	0	6	0	0	1	1	1	0	1
1	6	2	7	2	1	1	2	5	1	2	16	4	3	0	0	0	0	0
1	1	0	22	1	3	3	1	5	5	2	16	4	3	0	0	0	0	0
8	5	0	14	2	3	3	2	1	3	0	4	1	0	1	1	0	0	1
8	0	0	6	3	4	6	3	3	0	2	1	4	0	0	0	0	0	0
1	3	0	8	2	4	3	2	5	2	0	9	1	0	1	1	1	0	0
3	3	5	10	2	3	5	3	1	2	2	16	4	3	0	0	0	0	0
1	7	12	1	1	1	3	1	0	5	2	3	3	2	1	1	1	0	0
8	7	2	6	1	2	1	2	4	4	2	16	4	3	0	0	0	0	0
8	3	0	12	2	2	3	2	4	2	0	1	1	0	1	1	0	0	1
8	0	0	8	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	0	16	2	2	2	2	5	2	0	2	3	0	1	1	0	1	1
8	0	14	9	2	3	6	3	3	0	2	16	4	3	0	0	0	0	0
1	6	6	8	1	2	3	1	5	5	0	1	3	1	0	0	0	0	0
8	5	1	3	2	1	2	2	4	3	2	16	4	3	0	0	0	0	0
8	11	2	7	2	2	7	2	5	5	2	16	4	3	0	0	0	0	0
8	8	0	13	2	1	1	2	5	5	0	4	1	0	1	1	1	0	1
8	5	11	16	2	1	2	2	4	3	2	7	0	0	1	1	0	0	1
1	3	0	14	2	4	3	2	5	5	0	9	1	0	1	1	1	0	1
8	1	4	10	1	2	1	2	1	5	2	16	4	3	0	0	0	0	0
1	3	9	2	1	3	6	2	5	2	0	5	1	0	1	1	1	0	1
1	0	0	10	2	2	0	2	5	0	0	14	1	0	1	1	1	0	1
1	0	0	8	2	5	0	2	5	0	0	2	3	1	1	1	0	0	0
8	0	0	8	2	4	6	2	5	0	2	16	4	0	0	0	0	0	0
8	3	6	19	1	5	3	2	1	2	2	16	4	3	0	0	0	0	0
1	5	0	18	2	4	3	2	2	3	0	7	1	0	1	1	1	0	0
1	11	0	31	1	3	3	1	5	5	2	16	4	3	0	0	0	0	0
8	7	1	8	2	6	3	2	1	4	2	16	4	3	0	0	0	0	0
1	11	2	5	2	4	3	2	5	5	0	15	1	0	1	1	0	0	0
1	7	0	24	1	2	3	1	0	4	2	16	4	3	0	0	0	0	0
8	0	0	10	2	2	6	2	5	0	0	4	2	0	1	1	0	0	1
1	0	0	9	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
8	3	5	11	2	4	3	3	4	2	0	8	1	0	1	1	1	0	1
1	7	0	15	2	4	2	2	5	4	0	6	0	0	1	1	0	1	1
8	3	14	2	2	1	0	2	1	2	0	1	1	1	1	0	0	0	0
8	3	0	12	2	3	3	3	4	2	0	3	2	0	1	1	1	0	0
1	3	0	20	1	3	3	1	0	2	0	6	0	0	1	0	0	0	0
1	1	0	23	1	3	7	2	0	5	0	11	1	0	1	1	1	0	1
1	7	12	16	2	2	3	3	5	4	0	5	0	0	1	1	1	1	1
1	5	2	30	1	2	3	2	0	5	0	1	4	1	0	0	0	0	0
8	6	3	7	2	1	1	2	5	1	0	7	4	0	1	1	0	0	1
8	9	2	4	1	2	1	1	1	5	2	16	4	3	0	0	0	0	0
8	6	11	11	2	4	3	3	1	1	0	3	1	0	1	0	0	0	0
8	3	12	13	2	4	3	3	4	2	0	15	1	0	1	1	1	0	1
1	7	0	8	2	4	3	2	5	4	0	15	0	0	1	1	1	0	1
8	7	0	10	2	4	3	2	5	4	0	15	1	0	1	1	1	0	1
1	5	0	25	1	4	3	1	0	5	0	2	1	0	1	1	0	0	1
8	7	0	22	1	1	3	1	5	4	2	16	4	3	0	0	0	0	0
8	7	2	28	1	2	3	1	1	5	2	16	4	0	1	1	0	0	0
8	5	2	12	1	1	1	2	1	5	0	5	3	0	1	1	0	0	0
1	3	0	8	2	2	2	2	0	2	0	1	1	0	1	1	1	0	1
8	3	0	12	2	2	3	2	5	2	2	16	4	3	0	0	0	0	0
1	3	0	10	2	3	7	2	0	2	2	16	4	3	0	0	0	0	0
1	6	10	13	2	2	1	3	2	1	0	3	0	0	0	0	0	0	0
1	7	0	25	1	2	3	1	0	4	2	1	2	0	1	1	0	0	1
1	11	2	4	2	1	1	2	2	5	2	16	4	3	0	0	0	0	0
1	3	0	14	2	2	3	2	0	2	2	16	4	3	0	0	0	0	0
1	8	10	23	1	4	3	3	2	5	2	16	4	3	0	0	0	0	0
8	3	0	17	2	1	1	2	4	2	2	16	4	3	0	0	0	0	0
1	3	9	5	1	2	3	2	0	2	2	16	4	3	0	0	0	0	0
8	3	6	13	2	2	3	3	4	2	2	16	4	3	0	0	0	0	0
8	6	9	14	2	4	3	2	4	1	2	16	4	3	0	0	0	0	0
8	9	0	14	2	5	3	2	5	5	0	11	1	0	1	1	0	0	1
1	3	5	19	2	5	3	3	0	2	0	10	1	0	1	1	1	1	1
8	3	0	8	2	2	2	2	4	2	0	5	4	0	1	1	0	0	1
8	5	0	20	2	4	3	2	4	3	2	16	4	3	0	0	0	0	0
1	11	2	14	2	1	1	3	2	5	0	11	0	0	1	1	0	0	1
8	7	0	18	2	2	3	2	4	4	2	16	4	3	0	0	0	0	0
1	7	3	6	2	1	3	3	2	4	0	15	4	0	1	1	0	0	0
1	5	9	16	1	4	1	2	2	5	0	15	0	1	1	1	0	0	1
8	3	0	19	2	3	3	2	5	2	1	1	1	0	0	0	0	0	0
8	3	0	11	2	2	2	2	4	2	0	4	1	0	1	1	0	0	0
1	5	10	26	1	2	3	3	2	5	2	0	1	0	0	0	0	0	0
8	3	0	12	2	5	3	2	5	2	0	7	4	0	1	1	1	0	1
1	6	2	2	1	4	2	2	5	1	0	8	0	0	1	1	0	0	1
8	3	5	2	2	2	2	2	0	2	2	16	4	3	0	0	0	0	0
1	7	5	4	1	6	1	2	5	4	0	15	0	0	1	1	0	0	1
8	11	4	3	2	3	3	2	4	5	2	16	4	3	0	0	0	0	0
8	0	9	8	2	2	6	3	3	0	2	1	4	1	1	1	0	0	0
8	11	0	16	2	3	2	2	1	5	2	16	4	3	0	0	0	0	0
1	11	0	13	2	2	3	2	2	5	0	8	0	0	1	1	0	0	0
8	5	6	2	2	3	3	2	5	3	0	3	1	0	1	1	1	0	0
1	6	5	5	1	1	3	2	2	1	0	5	4	0	1	1	0	0	1
8	2	2	16	1	3	6	3	1	5	2	16	4	3	0	0	0	0	0
8	7	2	13	1	4	3	1	1	5	0	3	1	0	1	1	1	0	0
1	5	0	22	1	2	3	1	2	5	2	16	4	3	0	0	0	0	0
1	1	0	24	1	3	3	1	2	5	0	15	2	0	1	0	0	0	0
1	2	3	1	2	1	6	2	2	5	0	12	1	0	1	1	1	0	0
8	7	0	10	2	2	2	2	4	4	0	5	2	0	1	1	0	0	0
8	3	0	22	1	4	3	1	5	5	0	8	1	0	1	1	1	0	1
8	5	0	24	1	2	3	1	1	5	0	9	1	0	1	1	1	0	0
8	3	0	13	2	4	3	2	4	2	2	16	4	3	0	0	0	0	0
1	3	13	9	2	2	2	3	2	2	0	2	0	0	1	1	1	1	0
8	3	12	19	2	4	3	3	4	2	0	13	1	0	1	1	1	0	1
1	5	2	10	2	2	2	3	2	3	0	5	1	0	1	1	1	1	1
8	5	0	17	2	4	3	2	4	3	0	5	2	0	0	0	0	0	0
1	7	0	18	2	4	3	2	5	4	0	9	1	0	1	1	0	0	0
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
7	6	5	5	2	1	2	2	1	1	2	16	4	1	1	1	0	0	1
1	6	0	20	1	1	3	1	2	1	0	11	0	0	1	1	1	0	1
1	3	2	7	1	1	1	2	5	5	0	8	1	0	1	1	1	0	1
8	3	0	13	2	5	3	2	1	2	0	1	1	0	1	0	0	0	0
1	3	4	4	2	2	2	3	2	2	0	6	1	1	1	1	0	0	0
1	7	0	11	2	2	3	2	0	4	0	3	1	0	1	1	0	0	1
1	5	4	21	1	3	3	3	2	5	0	8	4	1	1	1	1	0	1
8	3	14	5	2	3	7	3	4	2	2	16	4	3	0	0	0	0	0
1	3	0	10	2	4	3	2	2	2	0	7	2	0	1	1	0	0	1
1	2	2	10	1	3	0	2	2	5	2	16	4	3	0	0	0	0	0
8	10	0	11	2	4	3	2	1	5	2	16	4	3	0	0	0	0	0
1	5	5	21	1	4	0	3	0	5	2	16	4	3	0	0	0	0	0
8	5	5	20	1	4	3	2	1	5	0	15	2	0	1	1	1	1	1
1	7	2	5	2	1	1	2	0	4	0	4	0	0	1	0	0	0	0
1	7	13	16	2	1	3	3	4	4	2	16	4	3	0	0	0	0	0
1	3	8	6	2	1	2	2	5	2	0	1	0	0	1	1	0	0	1
1	3	0	15	2	4	6	2	2	2	2	16	4	3	0	0	0	0	0
1	3	6	1	2	3	1	2	5	2	0	7	1	1	1	1	0	0	1
8	7	2	11	1	1	3	1	5	5	2	16	0	0	1	1	0	0	1
1	2	2	11	2	2	0	3	5	5	0	1	4	0	1	1	0	0	0
8	3	0	9	2	2	2	2	5	2	2	16	4	1	0	0	0	0	0
1	3	0	10	2	3	2	2	5	2	0	5	0	0	1	1	1	0	0
8	3	0	14	2	4	1	2	5	2	0	9	1	0	1	1	0	0	0
1	6	5	6	1	3	3	1	5	5	2	16	4	3	0	0	0	0	0
8	5	14	1	2	1	1	2	4	3	0	4	1	0	1	1	0	1	0
8	5	0	16	2	1	3	2	1	3	0	13	0	0	1	1	1	1	1
1	5	0	12	2	2	3	2	5	3	2	16	4	3	0	0	0	0	0
1	6	0	19	2	1	3	2	5	1	0	3	0	1	1	1	0	0	1
1	2	2	10	2	1	0	2	0	5	2	2	4	1	0	0	0	0	0
8	11	9	2	2	4	3	2	1	5	0	15	2	0	0	0	0	0	0
1	3	0	7	2	3	2	2	0	2	0	4	4	0	1	1	1	0	1
1	5	0	20	1	4	1	1	5	3	2	16	4	3	0	0	0	0	0
1	7	0	26	1	4	3	1	5	4	0	15	1	0	1	1	1	1	1
8	3	2	17	2	2	3	3	4	2	0	10	1	0	1	1	0	0	1
8	3	6	1	1	1	3	1	4	2	0	3	0	0	1	1	0	0	1
1	7	0	19	1	4	1	1	4	4	0	5	2	0	1	1	0	0	1
8	6	2	5	2	3	1	2	4	1	2	16	4	3	0	0	0	0	0
1	3	0	20	1	4	3	1	5	2	0	10	4	1	1	1	1	0	1
8	1	0	22	1	4	3	1	5	5	0	15	1	0	1	1	1	1	1
1	1	0	23	1	3	6	1	5	5	0	12	1	0	1	1	0	0	1
1	3	0	15	2	2	3	2	0	2	2	0	4	0	0	0	0	0	0
1	5	9	17	1	2	3	2	5	5	0	2	1	0	1	1	1	0	1
8	7	0	15	2	4	3	2	4	4	0	7	1	0	1	0	0	0	0
1	2	14	2	2	1	0	2	2	5	2	1	3	0	0	0	0	0	0
8	3	6	1	2	1	1	2	5	2	0	16	0	2	1	0	0	0	0
1	3	0	20	1	3	6	1	2	2	0	12	0	0	1	1	0	0	1
8	3	2	5	2	3	3	3	5	2	2	16	4	3	0	0	0	0	0
8	3	0	16	2	1	3	2	4	2	2	16	4	3	0	0	0	0	0
1	7	12	31	0	2	5	1	2	5	2	16	4	3	0	0	0	0	0
8	3	0	13	2	2	3	2	4	2	2	0	1	0	0	0	0	0	0
8	7	0	15	2	2	3	2	4	5	2	16	4	3	0	0	0	0	0
8	0	0	9	2	2	6	2	5	0	0	6	1	0	1	1	0	0	1
8	3	0	17	2	4	3	2	5	2	0	7	3	0	1	0	0	0	0
8	7	0	19	2	1	7	2	4	4	2	16	4	3	0	0	0	0	0
1	7	2	13	1	2	3	2	2	5	0	1	2	0	1	1	0	0	1
8	3	2	18	2	6	5	3	5	2	0	15	4	0	1	0	1	0	0
8	9	12	1	2	1	4	2	5	5	2	16	4	3	0	0	0	0	0
1	3	0	17	2	2	1	2	5	2	2	0	4	0	0	0	0	0	0
1	6	2	5	2	5	3	2	2	1	2	16	4	3	0	0	0	0	0
1	0	0	8	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
8	0	0	10	2	2	6	2	3	0	0	2	1	0	1	0	0	0	0
1	0	0	9	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
8	3	0	10	2	2	2	2	4	2	2	2	3	0	0	0	0	0	0
1	5	0	25	1	1	3	1	2	5	0	10	1	0	1	1	0	1	0
1	3	10	12	2	7	3	3	2	2	0	6	1	0	1	1	1	0	1
8	3	0	15	2	2	3	2	5	5	2	3	3	1	1	0	1	0	0
8	6	3	2	2	1	1	2	5	1	0	3	4	0	1	1	0	0	1
1	5	10	27	1	3	3	2	2	5	0	11	0	0	1	1	0	0	1
8	3	0	13	2	2	3	2	4	2	0	2	4	2	0	0	0	0	0
1	3	3	11	1	1	1	2	2	2	0	15	0	1	1	1	0	0	1
8	5	6	14	1	2	3	2	5	5	0	2	4	1	1	0	0	0	0
8	9	2	7	1	3	1	1	1	5	2	16	4	1	0	0	0	0	0
1	1	0	22	1	2	6	1	0	5	2	16	4	3	0	0	0	0	0
8	3	11	18	2	2	3	2	4	2	0	4	1	0	1	1	1	0	1
8	5	0	11	2	5	2	2	5	3	0	5	4	1	1	1	1	0	0
8	3	5	2	2	1	6	3	5	2	2	16	4	3	0	0	0	0	0
8	6	9	12	2	3	3	3	4	1	2	16	4	3	0	0	0	0	0
1	3	0	20	2	3	2	2	5	2	0	9	1	0	1	1	0	1	1
8	11	14	1	2	2	2	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	12	2	3	3	2	4	2	0	7	1	0	1	1	0	0	0
1	5	0	14	2	3	3	2	5	3	0	3	0	0	1	1	0	0	1
1	7	0	22	1	2	1	1	0	4	0	15	0	1	1	1	0	0	1
8	13	0	20	1	2	2	1	1	5	2	1	0	0	1	1	0	0	0
1	5	0	12	2	4	3	2	2	5	0	14	1	0	1	1	0	1	1
1	5	0	14	2	2	3	2	2	3	0	4	1	0	1	1	1	1	1
1	7	12	8	1	1	2	1	5	5	2	16	4	0	0	0	0	0	0
8	5	0	12	2	2	3	2	4	3	2	16	4	3	0	0	0	0	0
1	3	0	20	2	4	3	2	2	2	0	9	0	0	1	1	0	0	1
8	3	0	20	2	1	3	2	4	2	0	1	4	0	1	1	0	0	0
8	0	0	21	1	1	3	1	5	5	2	16	4	3	0	0	0	0	0
8	3	0	12	2	5	3	2	4	2	0	12	1	0	1	1	1	0	0
8	5	11	16	1	2	0	2	1	3	2	6	1	1	1	0	0	0	1
1	7	0	8	2	3	2	2	5	4	0	1	0	0	1	1	1	0	0
1	7	2	11	1	1	3	2	5	4	2	16	4	3	0	0	0	0	0
1	0	9	7	2	2	6	3	3	0	0	6	2	0	0	1	0	0	0
8	5	2	8	1	2	1	2	1	5	2	2	3	0	0	0	0	0	0
8	3	0	8	2	5	2	2	4	2	0	6	1	1	1	1	0	0	1
1	6	2	6	2	1	1	3	5	1	1	16	1	1	1	0	0	0	0
1	7	0	23	1	2	3	1	5	4	0	16	0	1	1	0	0	0	0
1	11	9	3	2	2	1	2	5	5	0	4	3	0	0	0	0	0	0
1	3	5	9	2	1	3	2	5	2	0	1	1	0	0	0	0	0	0
8	0	14	9	2	2	6	2	3	0	0	4	4	0	1	1	0	0	1
1	9	0	17	2	2	1	2	2	5	2	16	4	3	0	0	0	0	0
8	3	0	20	1	3	3	1	1	2	0	7	0	1	1	1	1	0	1
8	7	6	9	2	1	1	3	5	4	2	6	2	0	0	0	0	0	0
4	6	11	3	2	4	0	2	5	1	2	16	4	3	0	0	0	0	0
1	3	14	12	2	3	3	2	2	2	0	7	0	1	1	1	0	0	0
1	5	0	11	2	2	2	2	5	3	0	1	1	0	1	1	0	0	1
1	5	0	14	2	1	2	3	2	3	0	4	4	0	1	1	1	0	1
1	5	0	18	2	3	3	2	0	5	0	4	1	0	1	1	1	0	1
8	11	0	19	2	3	3	2	4	5	0	2	4	0	0	0	0	0	0
1	3	8	7	1	1	3	2	0	2	2	1	3	0	1	1	0	1	1
8	0	0	7	2	1	0	2	3	0	0	5	1	0	1	1	1	0	0
8	7	0	20	1	3	3	1	4	4	0	5	1	0	1	1	1	0	1
8	1	5	6	2	2	0	2	1	5	0	6	1	0	1	1	1	0	1
1	5	0	12	2	2	3	2	0	3	0	3	1	0	1	1	1	1	1
8	3	0	12	2	4	3	2	4	2	2	16	4	3	0	0	0	0	0
8	3	0	10	2	3	2	2	4	2	0	11	2	0	0	0	0	0	0
8	6	2	7	2	1	1	2	1	1	0	7	0	0	1	0	0	0	0
8	5	0	11	2	2	3	2	4	3	2	16	4	3	0	0	0	0	0
8	3	0	20	1	4	3	1	4	2	0	6	0	0	1	1	0	0	1
8	3	0	17	2	1	1	2	5	2	2	1	3	0	0	0	0	0	0
8	6	0	20	1	5	3	1	5	1	0	5	4	1	1	1	0	0	1
8	1	2	19	1	5	3	3	1	5	0	0	0	1	1	1	0	0	0
1	1	0	23	1	1	3	1	5	5	0	4	1	0	1	1	1	1	1
8	11	5	2	2	5	1	2	4	5	0	5	4	0	1	1	0	0	1
1	0	0	9	2	3	6	2	5	0	0	2	1	0	1	1	0	0	1
1	6	2	10	2	2	1	3	5	1	0	7	3	0	0	0	0	0	1
1	6	2	5	2	3	0	2	2	1	0	7	1	0	0	1	0	0	0
8	6	2	17	1	2	1	3	1	1	2	16	4	3	0	0	0	0	0
8	3	5	8	1	6	3	2	4	2	0	15	1	0	1	1	0	0	1
8	3	6	4	1	3	3	2	4	2	0	4	2	0	1	1	0	0	1
1	3	0	19	2	3	3	2	2	2	2	0	1	0	0	1	0	0	0
8	7	0	13	2	4	3	2	4	5	0	5	1	0	1	1	0	0	1
8	9	0	13	2	4	1	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	19	1	2	3	1	4	2	0	8	4	0	1	0	0	0	0
1	0	9	17	1	3	1	2	2	5	0	10	0	1	1	1	1	0	1
8	7	9	15	2	3	3	2	1	4	0	8	1	0	1	1	0	0	1
8	6	2	6	2	2	1	2	5	1	2	16	4	3	0	0	0	0	0
8	7	6	10	2	2	1	2	4	4	0	2	1	0	1	1	1	1	0
8	3	2	7	2	3	3	2	1	2	0	4	1	0	1	1	1	0	1
8	3	1	9	1	2	3	2	4	2	0	5	1	1	1	1	1	0	1
8	0	11	9	2	3	0	3	3	0	2	16	4	0	0	0	0	0	0
1	3	0	12	2	3	1	2	2	2	2	16	4	3	0	0	0	0	0
8	1	2	19	1	4	3	2	5	5	0	15	1	0	1	1	1	0	0
1	5	3	5	2	1	1	2	5	3	1	15	1	0	1	1	0	0	1
1	0	9	9	2	7	6	3	5	0	2	1	3	0	0	0	0	0	0
8	6	2	4	2	1	1	2	5	1	2	16	4	3	0	0	0	0	0
1	7	2	11	1	2	2	1	2	5	0	3	0	0	0	1	1	0	0
1	3	9	7	2	1	2	3	0	2	0	3	2	0	1	1	0	0	0
8	6	2	13	2	3	1	3	5	1	2	3	3	0	0	0	0	0	0
8	3	13	12	1	5	7	2	1	2	2	16	4	3	0	0	0	0	0
8	3	0	15	2	4	1	2	4	2	2	16	4	3	0	0	0	0	0
8	3	1	10	2	3	1	2	5	2	0	7	4	0	1	0	0	0	0
8	2	6	2	1	5	6	1	5	5	2	4	2	0	1	1	0	0	1
8	0	0	9	2	3	0	2	3	0	2	2	0	1	0	0	0	0	0
1	3	7	4	2	1	1	2	2	2	2	16	4	3	0	0	0	0	0
1	9	0	24	1	4	2	1	0	5	0	15	0	0	1	1	1	0	1
1	11	0	9	2	1	2	2	0	5	0	3	4	0	1	1	1	1	1
8	6	2	16	2	3	1	2	5	1	0	6	0	1	1	1	1	0	1
1	7	2	9	2	1	1	3	2	4	2	1	4	0	0	0	0	0	0
8	7	0	13	2	1	3	2	1	4	0	1	0	1	1	1	0	1	1
8	7	0	11	2	3	2	2	1	4	0	9	0	0	1	1	1	0	1
1	7	0	9	2	3	2	2	5	4	0	5	0	0	1	1	1	0	1
8	6	8	3	2	5	2	3	4	1	0	8	1	0	1	1	0	1	1
1	5	0	10	2	2	3	2	0	3	0	4	4	0	1	1	1	0	0
1	5	0	11	2	4	3	2	2	3	0	15	1	0	1	1	0	1	1
8	3	9	16	1	3	3	3	5	2	0	7	2	0	1	1	0	0	1
8	5	3	3	1	2	1	2	1	3	2	16	4	3	0	0	0	0	0
8	8	0	15	2	4	3	2	4	5	0	14	0	0	1	1	1	1	1
8	7	0	20	1	2	3	1	4	4	0	5	1	0	1	1	1	1	1
8	3	0	11	2	5	3	2	4	2	0	15	1	0	1	1	1	0	1
1	5	0	25	1	4	3	1	0	5	0	9	1	0	1	0	1	0	0
8	5	0	11	2	2	0	2	1	3	2	8	1	0	1	1	0	0	1
8	7	0	14	2	2	1	2	4	4	0	6	1	0	0	0	0	0	0
8	3	6	1	2	2	0	2	4	2	0	4	4	0	1	0	1	0	0
8	5	0	15	2	3	3	2	5	3	0	7	2	0	1	1	0	0	1
1	6	11	18	2	2	3	2	5	1	2	16	4	3	0	0	0	0	0
1	5	0	15	2	1	3	2	5	3	0	3	1	0	1	1	0	0	1
1	3	9	17	1	1	3	3	0	2	1	16	0	1	0	1	0	0	0
1	3	9	10	2	3	3	2	5	2	0	6	1	0	1	1	0	0	0
8	7	12	8	2	3	3	3	4	4	2	16	4	3	0	0	0	0	0
8	3	0	20	1	2	3	1	4	2	0	6	1	0	1	1	0	1	1
1	0	5	4	1	2	6	1	5	5	0	2	2	0	1	1	0	0	1
8	3	6	1	2	1	1	2	5	2	2	6	2	0	1	1	1	0	0
1	7	6	13	2	2	1	3	2	4	0	8	0	0	1	0	1	0	0
8	3	8	16	2	1	2	3	1	2	2	16	4	3	0	0	0	0	0
8	11	14	17	2	3	3	2	5	5	0	1	1	0	1	1	0	0	0
1	6	2	14	1	1	6	3	2	1	0	6	3	1	1	1	0	0	1
1	0	0	9	2	1	6	2	3	0	0	3	3	0	1	0	0	0	0
8	11	5	16	2	3	3	3	4	5	0	3	1	0	1	1	1	0	1
3	3	7	8	2	3	1	3	2	2	0	4	0	1	1	1	0	0	1
1	3	0	20	1	4	2	1	5	2	0	6	0	0	1	1	1	1	1
1	6	7	8	1	3	3	2	2	1	0	3	1	0	1	1	0	1	0
1	7	0	29	1	0	7	1	2	5	0	8	1	0	1	1	1	1	1
8	3	12	9	2	4	2	3	4	2	0	12	1	0	1	1	1	0	1
1	6	5	9	2	3	4	2	5	1	2	1	4	0	0	0	0	0	0
8	0	0	21	1	3	6	1	4	5	0	3	1	0	1	1	1	0	1
1	5	2	12	1	4	3	2	0	5	0	5	1	0	1	1	1	1	1
8	6	0	14	2	2	1	2	1	1	2	1	1	1	0	0	0	0	0
8	7	8	19	1	1	3	3	4	4	2	16	4	3	0	0	0	0	0
1	5	0	14	2	2	3	2	0	3	0	5	3	0	0	0	0	0	0
8	3	0	10	2	4	2	2	4	2	0	13	1	0	1	1	1	1	1
1	3	0	21	1	2	3	1	2	2	0	4	1	0	1	1	0	0	0
1	11	0	17	2	1	3	2	2	5	0	7	4	0	1	0	1	0	0
8	3	0	19	1	3	3	1	4	2	0	13	1	0	1	1	0	0	1
1	11	2	16	1	1	1	3	2	5	2	16	4	1	0	0	0	0	0
1	5	5	9	2	1	1	3	5	3	0	2	4	0	0	1	0	0	0
1	7	0	9	2	3	3	2	5	4	2	16	4	0	0	0	0	0	0
8	7	9	2	1	1	1	2	4	4	2	2	3	0	0	0	0	0	0
8	7	0	20	2	3	3	2	4	4	2	16	4	1	0	0	0	0	0
8	3	0	10	2	4	2	2	4	2	0	15	1	0	1	1	0	0	1
1	7	0	31	1	3	2	1	2	5	0	3	1	0	1	1	1	0	1
1	3	0	14	2	5	3	2	5	2	0	14	1	0	1	1	1	0	1
1	7	14	4	2	2	3	2	0	4	0	2	1	0	1	1	0	0	1
1	6	10	11	2	2	3	3	5	1	0	2	1	0	1	1	0	0	1
1	0	0	8	2	1	0	2	5	0	2	16	3	0	1	1	0	0	1
1	3	0	14	2	4	3	2	2	5	2	16	4	3	0	0	0	0	0
1	5	0	9	2	1	2	2	2	3	2	16	4	3	0	0	0	0	0
8	10	0	9	2	2	0	2	4	5	2	16	4	3	0	0	0	0	0
1	9	2	8	2	1	1	3	2	5	0	14	1	0	1	1	1	0	0
1	1	11	23	1	4	3	2	5	5	0	0	1	1	1	0	0	0	0
1	0	0	9	2	1	0	2	5	0	2	1	0	0	1	0	0	0	0
8	3	2	14	2	3	3	3	4	2	0	5	0	1	1	1	1	0	1
3	3	0	16	2	3	3	2	2	2	0	3	2	0	1	0	1	0	0
1	5	0	8	2	2	3	2	0	3	2	3	4	1	1	1	0	1	1
8	11	13	3	2	5	3	2	5	5	0	1	4	0	0	0	0	0	0
1	11	10	25	1	5	3	2	2	5	0	3	2	0	1	1	1	0	1
8	11	0	11	2	1	3	2	4	5	2	1	0	0	0	0	0	0	0
1	3	5	22	1	2	3	3	2	2	2	0	0	1	0	0	0	0	0
8	3	6	2	1	5	3	2	5	2	0	15	1	0	1	1	1	0	0
1	6	10	12	2	1	3	3	2	1	0	12	1	0	1	1	0	0	1
8	7	11	7	2	2	3	2	4	4	0	0	0	0	0	0	1	0	0
1	6	0	17	2	1	2	2	2	1	0	8	1	0	1	1	1	0	1
8	6	1	15	2	2	2	3	4	1	0	11	1	0	1	0	1	0	0
1	7	6	7	1	4	1	2	2	4	0	8	1	0	1	0	0	0	0
1	5	0	12	2	3	3	2	2	3	0	2	0	0	1	1	0	0	0
1	0	0	9	2	1	6	2	5	0	2	2	4	0	1	0	1	0	0
8	3	0	9	2	3	3	2	4	2	0	1	0	1	1	1	0	1	0
1	3	14	7	2	6	3	2	0	2	0	15	2	0	1	1	0	0	0
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	0	0	10	2	2	6	2	3	0	2	0	4	0	0	0	0	0	0
1	0	0	9	2	3	6	2	5	0	0	6	1	1	1	1	0	0	1
1	9	11	31	0	2	3	2	2	5	2	3	0	1	1	1	0	0	1
1	5	4	22	1	3	3	2	2	5	2	0	4	0	0	0	0	0	0
8	6	3	15	2	2	3	3	4	1	2	16	4	3	0	0	0	0	0
8	6	6	1	2	4	3	2	4	1	0	8	4	1	1	1	0	0	1
1	7	0	14	2	4	3	2	5	4	0	7	1	0	1	1	0	0	1
8	5	0	9	2	4	3	2	5	5	0	12	1	0	1	1	1	0	0
8	0	0	8	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	0	0	9	2	1	6	2	5	0	0	7	4	1	1	1	0	0	0
8	5	0	13	2	3	3	2	1	3	0	2	1	0	1	1	1	1	0
8	3	0	11	2	2	3	2	4	2	0	6	0	0	1	1	0	0	0
1	5	0	11	2	2	3	2	5	5	0	3	1	0	1	0	1	0	0
1	9	12	31	0	3	3	3	0	5	0	8	1	0	1	1	1	0	1
1	0	0	9	2	2	6	2	5	0	2	7	4	1	0	0	0	0	0
8	9	12	14	2	2	3	3	4	5	0	15	1	0	1	1	1	0	1
1	3	0	15	2	2	3	2	5	2	0	2	1	0	1	1	0	0	1
1	3	0	12	2	1	1	2	2	2	0	5	1	1	1	0	0	0	0
1	5	14	27	1	2	3	2	0	5	0	3	0	0	1	1	1	0	1
8	5	5	8	1	1	3	2	4	3	2	1	4	2	0	0	0	0	0
1	2	12	2	2	1	0	2	2	5	2	16	4	3	0	0	0	0	0
7	7	6	9	2	1	3	3	1	4	2	16	4	3	0	0	0	0	0
8	6	2	11	2	2	1	2	5	1	2	4	0	1	0	0	0	0	0
1	3	8	14	1	4	3	3	5	2	0	10	1	0	1	1	0	0	0
8	0	0	8	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
8	5	0	11	2	1	1	2	4	3	2	16	4	3	0	0	0	0	0
1	5	3	3	1	4	3	1	5	5	0	6	0	0	1	1	0	0	1
1	3	0	11	2	3	2	2	2	2	0	3	2	0	1	1	1	1	1
8	3	0	15	2	1	1	3	4	2	0	2	3	2	1	0	0	0	0
8	3	11	13	2	1	1	3	5	2	0	5	0	0	1	0	0	0	0
1	0	0	9	2	8	6	2	5	0	2	16	4	3	0	0	0	0	0
8	6	1	1	2	2	3	2	1	1	2	16	4	3	0	0	0	0	0
1	0	0	9	2	8	6	2	5	0	0	7	4	0	1	1	0	0	1
8	9	0	12	2	1	1	2	4	5	0	4	1	0	1	1	1	0	1
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	0	13	2	1	3	2	5	2	0	2	4	0	1	1	0	0	1
8	3	0	20	1	4	3	1	4	5	2	16	4	3	0	0	0	0	0
1	5	0	8	2	2	3	2	5	3	0	1	1	0	1	1	0	1	1
8	6	6	1	1	4	3	2	5	1	0	6	1	0	1	1	1	1	1
8	0	0	9	2	2	6	2	3	0	0	6	0	1	1	1	0	0	0
8	0	9	11	1	3	0	1	5	5	2	2	0	0	0	0	0	0	0
8	3	0	15	2	5	3	2	4	2	2	4	4	0	0	0	0	0	0
1	9	9	6	2	2	3	2	5	5	0	4	4	1	1	0	0	0	0
1	5	2	4	1	1	2	1	5	5	2	1	4	0	0	0	0	0	0
8	3	0	11	2	3	1	2	1	2	0	2	4	1	1	1	0	0	1
1	5	0	9	2	4	2	2	2	3	0	5	1	0	1	1	1	0	1
8	5	0	11	2	3	1	2	5	3	0	5	4	2	1	0	0	0	0
1	7	14	4	1	1	3	2	2	4	0	4	4	0	1	1	0	0	1
8	0	0	8	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	11	2	3	2	2	1	2	2	5	2	16	4	3	0	0	0	0	0
8	3	6	1	2	5	3	2	1	2	2	0	0	1	1	0	1	0	0
1	3	6	1	2	4	3	2	5	2	0	5	1	0	1	0	0	1	0
1	7	2	16	1	3	6	2	5	5	0	11	1	0	0	0	0	0	0
1	3	0	10	2	5	3	2	0	2	0	4	1	0	1	1	1	0	0
8	11	5	6	2	1	0	3	5	5	2	16	4	3	0	0	0	0	0
1	3	3	11	1	1	3	2	5	2	2	1	3	0	1	1	0	0	0
1	5	12	23	1	4	3	2	2	3	0	15	1	0	1	1	1	0	0
1	6	5	3	2	1	1	2	2	1	0	10	0	0	1	1	0	0	0
1	5	0	10	2	2	3	2	0	3	0	3	1	0	1	1	0	0	0
1	11	2	5	2	4	1	2	2	5	0	15	1	0	1	1	0	0	0
8	3	0	8	2	4	2	2	4	2	0	8	1	0	1	1	1	0	1
8	3	5	7	1	2	6	2	5	2	0	7	4	2	0	0	0	0	0
1	7	3	12	1	1	1	2	2	5	0	5	0	1	1	1	0	0	1
8	5	6	15	2	4	3	3	4	3	2	16	4	3	0	0	0	0	0
8	7	2	5	2	2	3	2	1	4	0	2	4	0	1	0	0	0	0
1	7	2	4	2	2	1	2	2	4	0	3	2	0	1	1	0	0	0
1	7	0	8	2	4	2	2	2	5	0	3	1	0	1	1	1	1	1
1	6	0	17	2	4	0	2	0	1	0	15	1	0	1	1	1	0	1
8	5	6	11	2	1	3	3	4	3	2	16	4	3	0	0	0	0	0
1	3	6	7	2	3	3	3	0	2	2	16	4	3	0	0	0	0	0
1	5	0	19	2	4	3	2	2	3	0	14	1	0	1	1	0	1	1
8	0	0	10	2	4	6	2	3	0	0	5	3	0	1	1	0	0	1
1	5	2	4	2	2	3	2	2	3	0	4	1	1	1	1	0	0	1
1	3	0	15	2	3	3	2	5	2	0	11	1	0	1	1	1	0	1
1	3	0	11	2	2	2	2	2	2	0	9	1	0	0	1	0	0	0
8	5	1	5	1	1	1	2	4	3	2	16	4	3	0	0	0	0	0
8	3	5	14	2	3	3	3	4	2	0	15	1	0	1	1	1	0	1
1	7	0	9	2	1	2	3	4	5	2	16	4	3	0	0	0	0	0
8	6	6	5	2	4	3	2	4	1	2	16	4	3	0	0	0	0	0
1	1	11	23	1	3	3	2	5	5	0	2	1	0	1	1	0	0	1
8	3	14	10	2	3	2	3	1	2	0	7	1	0	1	0	0	0	0
8	11	0	21	1	3	3	1	1	5	0	2	4	1	1	1	0	0	1
8	3	0	12	2	2	3	2	4	5	2	1	1	1	1	1	0	0	1
1	3	5	7	2	2	3	2	2	2	0	1	2	0	1	1	1	0	0
8	5	2	6	2	6	3	2	4	3	0	5	1	0	1	1	1	0	0
8	3	0	12	2	4	3	3	1	2	0	10	1	0	1	1	1	0	0
8	3	14	3	2	5	6	2	5	2	0	15	1	0	1	1	1	0	0
1	6	2	4	2	4	1	2	5	1	2	16	4	1	0	0	0	0	0
8	3	0	17	2	3	3	2	4	2	0	7	1	0	1	1	0	0	1
1	6	8	12	2	3	3	3	5	1	0	5	3	0	1	1	0	0	1
1	7	0	15	2	2	3	2	5	4	0	2	1	0	1	1	0	1	1
1	6	10	20	1	3	3	3	5	1	2	0	4	0	0	1	0	0	0
8	0	0	9	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
1	3	2	7	3	1	2	3	2	2	0	12	4	1	1	1	0	0	0
8	3	9	17	2	1	3	3	4	2	0	6	0	0	1	1	1	1	1
8	5	0	21	1	3	1	1	1	3	0	15	1	0	1	1	0	0	1
8	3	0	13	2	3	3	2	1	2	1	5	0	1	1	0	1	0	0
1	3	0	9	2	3	2	2	5	2	0	5	0	0	1	1	1	0	1
8	3	11	11	2	2	3	3	1	2	0	2	4	0	1	1	1	0	1
1	5	0	10	2	3	2	2	0	3	0	3	1	1	1	1	0	0	1
1	7	5	19	2	2	3	3	2	4	0	15	1	0	1	1	1	0	1
8	5	0	19	1	3	3	1	1	3	0	12	0	0	1	1	0	0	0
1	0	0	8	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
1	5	8	17	1	2	2	2	2	5	0	1	3	0	1	1	0	0	1
1	5	0	8	2	3	3	2	4	3	0	12	2	0	1	1	0	0	1
8	3	5	8	2	2	3	3	4	2	2	1	4	0	0	0	0	0	0
1	7	2	6	2	2	1	2	5	4	0	5	0	0	1	1	0	0	0
3	9	0	18	2	2	1	2	2	5	0	3	0	0	1	1	0	0	1
8	3	0	13	2	1	1	2	4	2	2	16	4	3	0	0	0	0	0
1	6	0	14	2	2	6	2	2	1	0	1	2	0	1	1	0	0	0
1	5	9	11	2	3	3	3	0	3	0	3	1	0	1	1	1	1	1
8	3	0	15	2	4	3	2	4	2	0	4	1	0	1	1	1	1	1
1	5	0	15	2	5	3	2	0	3	0	15	1	0	1	0	0	1	0
1	3	0	11	2	3	3	2	4	2	0	13	0	0	1	1	0	0	1
1	3	8	2	2	4	3	2	2	2	0	15	1	0	1	1	0	0	1
1	1	14	29	1	2	3	2	5	5	0	3	1	0	1	1	0	0	0
1	11	2	5	2	1	1	3	2	5	0	5	1	0	1	1	0	0	0
8	5	2	5	2	3	1	2	5	3	0	6	2	0	1	1	1	0	0
1	3	11	3	2	1	1	2	5	2	2	6	0	1	0	0	0	0	0
8	3	0	8	2	2	2	2	4	2	0	2	1	1	1	1	0	1	1
1	3	5	4	2	1	3	2	2	2	2	16	4	3	0	0	0	0	0
8	7	6	31	1	2	1	3	0	5	2	16	4	3	0	0	0	0	0
8	3	0	16	2	3	3	2	4	5	0	4	1	0	1	1	0	0	1
1	6	3	10	2	1	1	3	2	1	0	1	1	0	0	0	0	0	0
1	5	0	10	2	4	2	2	2	5	0	2	4	1	1	1	1	1	1
1	9	5	2	2	2	3	2	2	5	0	4	1	0	1	1	0	0	0
3	7	6	11	2	2	1	3	4	4	2	16	4	3	0	0	0	0	0
1	6	0	13	2	1	5	2	2	1	2	16	4	3	0	0	0	0	0
1	3	0	21	1	4	3	1	2	5	0	5	1	0	1	0	1	0	0
8	0	9	7	2	1	0	3	3	0	2	3	3	0	0	0	0	0	0
1	6	0	9	2	3	2	2	5	1	0	4	2	0	1	1	0	0	1
8	9	0	17	2	3	5	2	4	5	0	6	1	0	0	1	0	0	0
1	2	1	8	2	1	0	2	2	5	2	16	4	3	0	0	0	0	0
1	3	5	13	1	2	3	2	2	2	0	5	2	0	1	1	1	0	0
1	3	0	14	2	2	3	2	0	2	0	2	1	0	1	1	0	1	1
8	3	3	2	1	3	6	2	1	2	0	6	1	0	1	1	0	0	1
8	5	2	9	1	2	3	2	0	5	0	3	3	0	1	1	0	1	1
8	0	0	10	2	9	6	2	3	0	2	16	4	0	0	0	0	0	0
1	3	0	8	2	2	2	2	5	2	0	4	3	1	1	1	1	0	1
1	7	5	8	2	4	3	3	5	4	2	16	4	3	0	0	0	0	0
1	3	1	3	1	3	3	2	5	2	2	7	4	1	0	0	0	0	0
8	3	13	3	2	1	1	2	5	2	2	16	4	3	0	0	0	0	0
1	7	2	12	2	2	0	3	2	4	0	3	2	0	1	1	0	0	0
1	5	0	15	2	1	2	2	2	3	0	10	0	0	1	1	0	1	0
8	6	2	3	2	1	3	2	4	1	2	16	4	3	0	0	0	0	0
1	7	2	10	1	1	0	1	2	5	2	1	4	1	0	0	0	0	0
1	3	0	19	2	4	3	2	0	2	0	5	1	0	1	1	0	1	1
1	6	0	20	1	3	2	1	0	1	2	16	4	3	0	0	0	0	0
1	6	12	6	2	2	3	2	2	1	0	2	1	0	1	1	0	0	0
8	7	2	19	1	2	3	2	5	5	0	6	1	0	1	1	1	0	1
8	3	0	9	2	3	2	2	4	2	2	3	3	0	1	1	0	1	1
1	7	14	31	0	3	2	3	2	5	2	16	4	3	0	0	0	0	0
1	0	0	9	2	3	6	2	3	0	0	10	1	0	1	1	1	1	1
1	3	0	21	1	3	1	1	5	2	0	6	2	0	1	1	0	0	0
1	7	5	5	2	1	2	3	5	4	0	4	0	0	1	1	1	0	1
1	11	0	9	2	1	0	2	0	5	2	16	4	3	0	0	0	0	0
8	0	0	8	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
8	6	9	12	2	1	0	3	1	1	0	4	0	2	0	1	0	0	1
1	11	3	4	1	2	3	2	0	5	0	1	0	0	1	1	1	1	1
8	7	0	13	2	4	3	2	5	4	0	9	1	0	1	1	1	0	1
8	0	9	8	2	2	6	3	3	0	0	3	4	0	1	1	0	0	0
1	9	12	15	1	3	3	2	2	5	2	16	4	3	0	0	0	0	0
1	7	14	9	2	4	2	3	2	4	0	15	1	0	0	0	0	0	0
1	3	0	9	2	3	3	2	4	5	0	5	1	0	1	1	0	0	1
1	5	2	10	1	5	1	1	2	5	0	10	0	1	0	1	0	0	1
1	0	0	9	2	3	6	2	5	0	2	1	4	0	0	0	0	0	0
8	1	5	4	1	1	3	1	5	5	1	16	0	1	0	0	1	0	0
1	3	0	14	2	4	3	2	0	2	0	15	0	0	1	1	0	0	0
1	6	2	10	1	4	3	2	5	1	2	1	3	0	1	0	0	0	0
8	5	0	15	2	4	3	2	1	3	0	9	2	0	1	0	1	0	0
8	3	5	1	2	4	0	2	4	2	0	4	1	0	1	1	0	0	1
8	7	11	11	2	1	3	3	4	4	0	6	4	0	1	1	0	1	1
1	6	0	11	2	2	2	2	4	1	0	10	1	0	1	1	1	0	1
8	3	9	17	1	4	3	3	4	2	0	9	2	0	1	1	0	1	1
1	7	0	21	1	2	3	1	0	5	0	4	4	0	1	0	1	0	0
1	3	9	1	2	2	3	2	0	2	0	9	1	0	1	1	0	0	1
1	3	2	10	2	2	1	2	2	2	2	16	4	3	0	0	0	0	0
1	6	0	21	1	4	1	1	5	1	2	16	4	3	0	0	0	0	0
8	5	0	15	2	1	3	2	4	3	0	3	2	0	1	1	0	0	1
1	11	10	30	0	2	4	2	2	5	2	16	4	3	0	0	0	0	0
1	3	2	12	2	4	3	2	0	2	0	13	1	0	1	1	1	1	1
3	2	9	1	1	2	0	1	2	5	2	16	4	3	0	0	0	0	0
1	3	0	9	2	4	2	2	5	5	0	6	1	0	1	1	1	1	1
1	0	0	8	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
1	7	2	10	0	4	6	1	0	5	0	15	1	0	1	1	1	0	1
1	1	0	25	1	5	3	1	5	5	0	11	0	0	1	1	0	0	0
8	7	0	13	2	2	3	2	4	4	0	16	0	1	1	1	0	0	1
8	6	2	5	2	1	0	2	1	1	2	16	4	3	0	0	0	0	0
8	3	9	19	1	2	3	3	4	2	0	2	1	0	1	1	1	0	1
1	2	2	12	1	2	0	2	5	5	2	16	4	3	0	0	0	0	0
1	11	2	15	2	1	3	3	2	5	0	3	1	1	1	0	0	0	0
8	9	14	13	1	1	7	2	5	5	2	16	4	3	0	0	0	0	0
8	3	3	14	1	2	1	3	5	2	2	16	4	3	0	0	0	0	0
1	1	0	23	1	2	3	1	0	5	0	8	1	0	1	1	1	0	1
1	6	14	9	1	4	3	2	0	1	2	16	4	3	0	0	0	0	0
1	7	14	9	2	1	3	2	5	4	0	3	1	1	1	1	0	0	1
8	6	2	6	1	2	3	1	5	5	0	2	1	0	1	0	0	0	0
1	5	0	16	2	3	3	2	5	3	0	1	1	1	1	1	0	0	1
8	9	2	5	2	1	4	2	5	5	1	1	4	0	1	1	0	0	0
1	7	0	13	2	2	2	2	5	4	2	16	3	0	1	0	0	0	0
8	3	0	16	2	2	3	2	4	2	2	2	4	1	0	0	0	0	0
1	6	13	11	2	2	3	3	0	1	2	16	4	3	0	0	0	0	0
1	0	0	8	2	1	0	2	5	0	0	15	1	0	1	1	0	0	0
8	6	10	19	2	1	3	2	4	1	2	0	4	0	0	0	0	0	0
1	0	0	9	2	3	6	2	5	0	1	5	0	0	1	1	1	0	0
1	5	0	12	2	3	3	3	0	3	0	11	1	0	1	1	1	0	0
1	1	0	22	1	2	3	1	5	5	0	4	1	0	1	1	1	0	1
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	0	0	9	2	1	6	2	5	0	0	3	4	0	1	1	0	0	1
8	5	7	5	2	1	3	2	5	3	0	13	1	0	1	1	1	0	1
8	3	0	21	1	5	1	1	4	2	0	5	1	0	1	1	1	1	0
1	3	0	10	2	6	2	2	0	2	0	3	1	0	1	1	0	0	1
8	7	0	16	2	2	1	2	4	4	0	14	1	0	1	1	1	1	1
8	3	7	8	2	2	3	2	4	2	0	5	1	1	1	0	0	0	0
8	5	2	13	2	1	2	2	5	3	2	3	0	1	0	1	0	0	0
1	6	5	8	2	1	1	3	2	1	0	6	2	0	1	1	0	1	0
1	0	9	8	2	1	6	3	5	0	2	16	4	3	0	0	0	0	0
8	3	9	10	1	1	3	2	5	2	0	6	0	1	1	1	0	0	1
1	7	2	13	1	3	6	3	0	4	2	5	3	2	0	0	0	0	0
1	6	2	11	1	1	1	1	2	5	0	15	1	0	1	1	0	0	0
8	11	14	1	2	3	0	2	1	5	2	16	4	3	0	0	0	0	0
8	1	0	22	1	3	4	3	5	5	2	16	4	3	0	0	0	0	0
1	8	13	5	1	2	3	2	2	5	0	0	1	1	1	1	1	0	0
1	3	9	7	1	1	3	2	2	2	2	16	4	3	0	0	0	0	0
1	1	0	23	1	3	1	1	2	5	0	9	1	0	1	1	1	0	1
1	13	0	9	2	2	2	2	5	5	0	15	2	0	1	0	1	0	0
1	3	0	10	2	3	5	2	2	2	0	3	2	0	1	1	1	0	1
8	0	0	8	2	2	6	2	5	0	0	15	4	0	0	0	0	0	0
1	3	0	18	1	2	3	1	0	2	0	1	1	0	1	1	1	1	1
1	3	11	13	1	4	3	2	5	2	0	12	1	0	1	1	1	0	1
1	5	0	20	1	6	3	1	5	3	2	16	4	3	0	0	0	0	0
8	6	2	3	2	1	1	2	4	1	2	16	4	3	0	0	0	0	0
8	3	14	11	2	3	3	3	4	2	0	10	0	1	1	1	0	0	1
8	0	11	9	2	9	6	3	3	0	2	16	4	3	0	0	0	0	0
8	9	12	31	1	4	3	3	1	5	0	8	1	0	1	1	1	0	1
8	5	0	10	2	2	2	2	4	3	2	16	4	3	0	0	0	0	0
8	3	0	10	2	2	4	2	4	2	2	16	4	3	0	0	0	0	0
1	11	12	13	2	3	1	3	2	5	0	11	4	1	1	1	0	0	1
8	3	0	8	2	6	2	2	5	2	0	4	0	1	1	1	1	0	0
1	3	2	3	2	1	0	3	5	5	2	16	4	3	0	0	0	0	0
8	7	0	20	1	2	3	1	4	4	0	2	2	0	1	1	0	0	1
8	7	9	22	1	2	3	3	1	4	2	2	4	1	1	1	0	0	1
1	7	0	31	1	2	3	1	5	5	0	4	1	0	1	1	0	0	1
1	5	2	4	2	2	1	2	0	3	0	4	4	0	1	0	0	0	0
1	3	0	11	2	5	3	2	0	2	0	4	0	0	1	1	1	1	1
8	0	0	9	2	1	0	2	3	0	2	16	4	3	0	0	0	0	0
8	7	0	16	2	2	3	2	5	4	1	2	1	1	0	0	0	0	0
8	6	0	15	2	4	3	2	4	1	0	9	1	0	1	1	1	0	1
1	9	0	12	2	1	3	2	5	5	0	3	3	1	0	0	0	0	0
1	7	6	15	2	2	3	3	0	4	0	3	4	0	1	1	0	0	1
1	5	6	2	1	2	6	1	5	5	0	3	0	0	0	1	0	0	1
1	5	0	20	1	2	1	1	2	3	0	8	0	1	1	1	0	1	1
8	0	0	9	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	0	21	1	2	3	2	5	2	0	5	1	0	1	1	1	0	1
8	0	0	9	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
1	7	3	11	1	1	3	2	2	4	2	3	4	1	0	0	0	0	0
1	6	0	21	1	4	2	1	5	1	0	15	1	0	1	1	0	1	1
8	0	0	10	2	1	6	2	3	0	2	16	0	0	0	1	0	0	1
1	5	2	7	1	1	3	2	0	3	0	15	0	0	1	1	1	0	0
1	3	0	9	2	5	2	2	2	2	0	12	1	1	1	1	1	1	1
8	5	0	14	2	2	1	2	5	3	0	4	1	1	1	1	0	0	1
1	6	10	12	2	4	6	3	2	1	0	15	1	0	1	1	0	0	1
8	3	13	11	2	2	3	3	4	2	0	3	2	0	1	1	1	1	1
1	5	0	19	1	1	1	1	5	3	0	7	4	0	1	1	1	0	1
1	3	2	11	2	1	3	3	5	2	2	16	4	3	0	0	0	0	0
8	7	9	13	2	2	3	3	1	4	2	16	4	3	0	0	0	0	0
8	2	12	8	2	2	0	2	5	5	2	16	4	3	0	0	0	0	0
1	3	0	13	2	4	6	2	5	2	0	9	1	0	1	1	1	1	0
1	5	0	24	1	3	3	1	0	5	0	3	1	0	1	1	0	0	1
1	7	10	30	1	3	1	2	2	5	0	5	4	1	1	1	1	0	1
8	7	0	11	2	5	3	2	5	5	0	12	1	0	1	1	1	0	1
1	6	4	5	1	2	3	1	5	5	0	3	4	1	1	1	0	1	1
1	7	2	6	1	2	1	2	2	4	2	2	3	0	1	0	0	0	0
1	5	5	8	2	2	3	2	5	3	0	15	1	0	1	1	0	0	1
1	6	0	11	2	2	2	2	2	1	0	8	1	0	1	1	0	0	0
1	3	0	16	2	5	3	2	5	2	0	11	1	0	1	1	1	0	1
1	0	3	1	2	1	6	2	5	0	2	1	4	0	0	0	0	0	0
8	3	9	15	1	1	3	3	4	2	2	0	4	1	0	0	0	0	0
1	3	0	13	2	4	1	2	0	2	2	16	4	3	0	0	0	0	0
1	3	10	20	1	2	3	3	0	2	0	5	2	0	1	1	0	0	0
1	1	0	22	1	3	3	1	5	5	2	16	4	3	0	0	0	0	0
8	0	9	7	2	9	6	3	3	0	2	16	4	1	0	0	1	0	0
1	7	3	3	1	1	3	2	5	4	2	1	0	2	1	1	0	0	1
8	7	0	11	2	1	2	2	4	4	2	0	4	1	0	0	0	0	0
1	5	14	1	1	5	3	1	5	3	0	14	1	0	1	1	0	0	0
1	7	0	10	2	3	1	2	5	4	0	6	2	0	1	1	1	0	0
1	5	0	9	2	6	3	2	5	3	0	13	1	0	1	1	1	0	1
1	0	1	18	1	2	3	1	2	5	0	4	1	0	1	1	0	0	1
1	11	0	10	2	2	3	2	0	5	2	1	1	1	0	0	0	0	0
8	5	0	12	2	1	3	2	4	3	0	13	0	0	1	1	1	1	1
1	9	14	2	2	5	2	2	0	5	2	16	4	3	0	0	0	0	0
8	3	5	4	2	4	3	2	4	5	0	15	4	0	1	1	1	0	1
1	7	14	19	2	5	3	2	5	4	0	15	1	0	1	1	1	0	1
8	6	2	16	2	1	3	3	4	1	0	7	4	1	1	1	1	0	1
1	3	0	12	2	1	3	2	5	2	0	15	4	1	1	0	1	0	0
8	5	2	9	2	1	1	3	3	3	2	16	4	3	0	0	0	0	0
1	11	4	1	2	1	1	2	5	5	0	15	0	0	1	1	1	0	1
8	9	2	11	2	1	1	3	4	5	2	4	4	2	1	1	0	0	0
8	8	0	20	1	3	3	1	4	5	2	16	4	3	0	0	0	0	0
1	7	0	16	2	3	2	2	5	5	0	6	2	0	1	1	0	0	1
8	0	0	10	2	1	0	2	3	0	2	16	4	3	0	0	0	0	0
1	3	0	8	2	5	3	2	0	2	0	15	1	0	1	1	1	0	1
1	5	5	14	1	4	3	2	5	5	0	2	1	0	1	1	1	0	1
1	9	2	15	1	2	3	2	2	5	0	7	0	0	1	1	0	0	1
1	7	0	20	1	2	3	1	5	4	0	4	3	0	1	1	0	1	1
8	6	6	8	1	2	3	2	5	1	0	3	1	1	1	1	0	1	1
1	9	0	18	2	4	6	2	2	5	0	11	1	1	1	1	0	0	1
8	7	9	8	2	7	2	3	5	4	2	16	4	3	0	0	0	0	0
8	6	6	7	1	2	3	2	1	1	0	5	0	0	1	1	1	0	1
1	0	9	9	2	1	6	3	3	0	0	15	1	0	0	0	0	0	0
8	3	9	7	1	4	3	2	4	2	0	2	1	0	1	1	1	0	0
1	3	9	10	2	4	3	2	2	2	0	5	1	0	1	0	1	0	0
8	6	2	6	1	1	3	2	1	1	2	1	4	3	0	0	0	0	0
1	7	11	17	1	0	0	2	2	5	0	10	4	1	1	1	1	0	0
8	6	0	19	1	3	3	1	4	1	0	5	1	0	1	1	0	0	1
1	3	11	4	2	6	3	2	2	2	0	15	0	0	0	1	0	0	0
1	3	0	15	2	1	2	2	2	2	0	3	2	0	1	1	0	0	0
8	3	0	17	2	3	1	2	4	2	0	12	1	0	1	1	1	1	1
1	3	10	12	2	5	3	3	0	2	0	15	0	0	1	1	1	0	1
1	3	9	14	1	3	3	3	5	2	0	8	1	0	1	1	1	1	1
1	0	0	8	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
1	7	2	8	1	1	3	2	2	4	0	3	2	0	1	1	0	0	0
1	3	0	20	1	2	1	1	5	2	2	16	3	0	1	0	0	0	0
1	3	14	12	2	2	5	2	2	2	0	3	1	0	1	1	0	0	0
8	7	11	13	1	2	3	2	1	5	2	16	4	3	0	0	0	0	0
1	5	0	9	2	1	2	2	5	3	2	16	4	3	0	0	0	0	0
8	0	9	10	2	2	6	3	5	0	2	16	4	3	0	0	0	0	0
8	2	12	7	2	2	0	2	4	5	2	16	4	3	0	0	0	0	0
1	3	3	19	1	3	3	3	2	2	2	16	4	3	0	0	0	0	0
1	3	0	15	2	3	3	2	5	2	0	6	1	0	1	1	0	0	1
8	0	11	9	2	1	6	3	3	0	0	9	1	0	1	1	1	0	0
1	5	0	27	1	3	3	1	0	5	2	3	0	0	1	0	1	0	0
8	3	0	19	1	4	3	1	4	2	0	13	1	0	1	1	1	0	1
1	9	0	30	1	2	3	1	5	5	0	13	1	0	1	1	0	0	1
8	6	9	3	2	4	3	2	1	1	0	7	1	0	1	1	1	0	0
1	9	13	22	1	2	3	2	0	5	0	2	1	0	1	1	0	0	1
8	5	0	19	1	1	1	1	5	3	0	10	0	0	1	1	0	0	1
1	7	2	3	1	1	3	2	2	4	2	5	0	1	1	1	0	0	1
1	5	2	6	1	2	1	2	2	3	0	6	1	0	1	1	0	0	0
8	5	0	11	2	3	3	2	4	3	0	12	1	0	1	1	1	0	1
8	3	3	2	1	1	1	2	5	2	2	16	4	3	0	0	0	0	0
1	6	2	14	2	1	3	3	5	1	2	1	4	0	0	0	0	0	0
8	7	2	4	2	1	1	2	4	4	2	16	0	2	1	1	0	0	0
8	5	13	2	2	1	2	2	1	3	0	10	0	1	0	1	0	0	1
1	6	0	12	2	1	1	2	5	1	0	11	0	0	1	1	1	0	0
8	6	2	20	1	2	1	3	5	1	0	4	0	1	1	1	0	0	1
8	5	0	16	2	2	3	2	5	3	0	6	0	0	1	1	0	0	1
8	3	0	13	2	4	3	2	1	2	0	9	0	0	1	1	1	1	1
8	5	5	7	2	1	1	3	5	3	0	9	2	0	1	1	0	1	1
8	3	13	9	2	2	3	3	4	2	2	16	4	3	0	0	0	0	0
8	11	2	4	2	3	1	2	5	5	0	5	0	0	0	0	0	0	0
8	5	3	12	2	2	1	3	5	3	2	1	1	1	0	0	0	0	0
1	0	5	5	2	3	6	2	5	5	2	16	4	3	0	0	0	0	0
8	6	0	9	2	2	2	2	4	1	0	4	0	0	1	1	1	0	1
1	5	9	18	2	3	3	2	5	3	0	7	1	0	1	1	1	0	1
8	6	0	11	2	2	3	2	4	1	2	16	4	3	0	0	0	0	0
8	3	0	10	2	3	2	2	4	2	0	6	2	0	1	0	0	1	0
1	7	14	8	1	3	3	1	0	5	0	6	3	2	1	0	0	0	0
8	9	14	2	2	5	1	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	15	2	1	3	2	4	2	2	16	4	3	0	0	0	0	0
8	6	0	17	2	2	3	2	4	1	0	4	1	0	1	1	0	0	0
1	5	0	11	2	4	3	2	5	3	0	8	1	0	1	0	0	0	0
8	5	2	8	1	1	1	2	5	3	2	2	0	0	1	1	0	0	1
1	5	2	5	2	1	3	2	5	3	2	2	3	0	0	0	0	0	0
8	6	14	3	2	1	6	2	4	1	2	16	4	3	0	0	0	0	0
1	7	10	20	1	3	3	2	2	4	0	1	1	1	1	1	0	0	1
8	6	0	20	1	2	3	1	4	1	2	0	1	0	0	0	0	0	0
8	5	0	15	2	2	3	2	1	3	1	3	0	1	1	1	0	0	1
8	5	0	11	2	2	3	2	4	3	2	0	4	0	0	0	0	0	0
1	9	2	5	1	1	1	2	2	5	2	2	3	1	0	1	0	0	0
1	3	0	13	2	2	3	2	0	2	0	5	0	0	1	1	0	0	0
1	7	0	21	1	1	1	1	5	4	0	1	1	1	0	0	0	0	0
8	7	3	6	2	4	2	3	4	4	0	9	1	0	1	1	1	1	1
1	0	0	11	2	1	6	2	5	5	2	16	4	3	0	0	0	0	0
8	5	0	21	1	3	1	1	4	3	0	15	0	0	1	1	1	0	1
8	6	2	15	2	3	1	2	5	1	2	16	4	3	0	0	0	0	0
1	3	5	13	2	2	3	3	0	2	0	3	2	0	1	1	1	1	1
8	6	13	20	1	4	3	3	1	1	0	9	1	0	1	1	1	0	0
8	3	11	2	2	4	2	2	4	2	2	11	4	1	1	0	0	0	1
8	5	0	16	2	5	5	2	1	3	0	9	1	0	1	1	1	0	1
8	7	0	20	1	4	1	1	4	5	0	12	1	0	1	1	1	1	1
8	7	2	7	1	1	2	1	1	5	2	3	0	1	0	0	0	0	0
8	9	2	17	2	2	1	3	4	5	0	2	1	0	1	1	1	0	1
8	3	0	12	2	4	1	2	5	2	0	11	1	0	1	1	0	0	1
1	3	0	11	2	5	3	2	0	2	2	16	4	3	0	0	0	0	0
1	6	0	19	2	2	3	2	2	1	0	3	3	0	1	1	0	0	1
1	9	6	1	2	3	1	2	2	5	0	4	1	0	1	1	0	0	0
8	6	0	15	2	4	3	2	1	1	0	5	2	0	1	1	1	1	1
8	6	2	5	1	3	1	2	1	1	2	5	3	0	0	0	0	0	0
8	0	0	10	2	2	6	2	5	0	2	16	4	3	0	0	0	0	0
8	3	0	10	2	5	2	2	4	2	0	2	1	1	1	1	0	1	1
8	0	9	8	2	2	6	3	3	0	2	16	4	3	0	0	0	0	0
8	3	0	20	1	6	3	1	4	2	0	15	4	0	1	1	1	0	1
8	9	2	5	2	1	1	2	4	5	2	16	4	3	0	0	0	0	0
8	5	0	11	2	3	2	2	4	3	0	6	1	0	1	1	1	1	1
8	3	3	4	1	3	3	1	1	5	0	13	0	0	1	1	0	0	1
1	3	5	2	2	2	2	2	0	2	2	16	4	3	0	0	0	0	0
1	3	0	14	2	4	3	2	5	5	1	3	1	0	0	0	0	0	0
8	3	0	18	2	3	3	2	4	2	0	4	4	1	1	1	0	0	1
8	7	0	18	2	3	3	2	4	4	0	3	4	0	1	1	1	1	1
8	3	0	20	1	3	3	1	4	2	0	4	1	0	1	1	1	1	1
1	7	0	9	2	3	2	2	5	4	1	1	0	1	1	1	0	0	1
1	7	0	31	0	2	3	0	2	5	0	6	2	0	1	1	1	0	1
8	5	0	20	1	4	3	1	5	3	0	1	1	0	1	0	0	0	0
8	3	0	20	1	1	5	1	1	2	0	13	0	0	1	1	1	0	1
1	11	0	20	1	3	3	1	0	5	0	5	0	0	0	0	0	0	0
1	5	14	4	2	2	0	2	2	3	2	16	4	3	0	0	0	0	0
1	5	0	16	2	3	1	2	0	3	2	16	4	3	0	0	0	0	0
1	7	0	11	2	1	3	2	0	4	0	14	4	0	1	1	0	1	1
8	3	5	11	2	3	3	2	4	2	0	5	1	0	1	1	0	0	1
1	7	0	15	2	1	3	2	5	4	2	16	4	3	0	0	0	0	0
8	7	0	16	2	2	1	2	5	4	2	1	3	1	0	0	0	0	0
1	6	2	5	2	3	1	2	0	1	0	7	3	1	1	0	0	0	0
1	5	3	2	2	2	1	2	5	3	2	3	4	1	1	1	0	1	1
8	5	0	20	1	3	3	1	4	3	2	16	4	3	0	0	0	0	0
1	3	3	20	1	2	3	3	0	2	0	2	4	0	1	1	1	1	1
1	7	6	7	2	4	2	3	0	4	0	10	1	0	1	1	1	1	1
8	7	2	6	2	2	1	2	4	4	2	16	4	3	0	0	0	0	0
8	3	6	17	2	3	3	2	4	2	0	15	0	0	1	1	1	0	1
8	7	0	18	2	3	3	2	5	4	2	1	1	0	0	1	0	0	0
8	3	0	19	1	5	1	1	1	2	0	4	0	0	1	1	1	1	1
8	3	9	10	2	1	2	3	4	2	2	16	4	3	0	0	0	0	0
1	7	6	3	2	4	3	2	5	4	0	10	1	0	1	1	0	0	1
1	3	0	18	2	4	2	2	5	2	0	8	1	0	1	1	1	0	1
8	0	0	8	2	0	6	3	3	0	2	16	4	2	1	1	0	1	1
1	3	0	13	2	6	3	2	2	2	0	8	1	0	1	1	0	0	1
1	9	2	5	1	2	1	1	0	5	0	4	4	0	0	0	0	0	0
8	7	12	13	2	2	3	3	4	4	2	16	4	3	0	0	0	0	0
8	3	0	23	1	3	3	1	5	2	0	7	1	0	1	1	1	1	1
8	3	0	13	2	2	3	2	4	2	0	9	1	0	1	1	0	1	1
1	3	0	12	2	3	3	2	5	2	0	7	0	1	1	1	1	0	1
1	3	0	13	2	2	3	2	2	2	2	16	4	3	0	0	0	0	0
8	7	2	4	2	1	6	2	1	4	0	7	4	1	1	0	0	0	0
8	7	0	20	1	3	3	1	1	4	0	11	1	0	1	1	0	0	1
8	3	0	20	1	5	3	1	5	2	0	11	2	0	1	1	0	0	1
1	0	0	8	2	3	0	2	5	0	2	3	1	1	1	0	0	0	0
8	7	0	17	2	3	1	2	4	5	0	5	4	0	1	1	0	1	1
8	3	1	1	2	1	1	2	4	2	0	11	0	0	1	1	0	0	0
8	6	9	16	1	3	3	2	1	1	0	5	2	0	1	1	0	1	0
8	11	4	3	2	4	2	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	9	2	4	3	2	1	2	0	6	1	0	1	1	1	0	1
1	3	0	10	2	5	3	2	5	2	0	3	1	0	1	1	0	0	0
8	5	8	19	1	3	3	3	5	3	0	15	1	0	1	1	1	1	1
8	6	9	4	2	1	1	2	4	1	0	5	2	0	1	1	0	1	1
1	6	2	5	2	2	3	2	5	1	0	6	0	0	1	1	0	0	0
8	3	0	14	2	4	3	2	4	2	0	7	1	0	1	1	0	0	1
8	0	0	10	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	1	2	7	1	5	3	2	5	5	0	14	4	1	1	1	1	0	1
1	7	14	22	1	6	7	3	2	5	2	16	4	3	0	0	0	0	0
8	3	7	8	2	2	3	3	5	2	2	1	4	0	0	0	0	0	0
1	7	0	31	0	4	3	0	0	5	0	15	4	0	1	1	0	0	1
1	3	9	8	2	3	2	3	5	2	0	8	1	0	1	1	1	0	1
8	3	6	18	2	4	3	2	1	2	0	2	1	0	1	1	1	0	1
8	7	2	12	2	3	1	3	4	4	0	2	3	0	0	0	0	0	0
8	3	4	13	1	1	2	2	1	2	2	16	4	3	0	0	0	0	0
8	7	0	9	2	1	0	2	1	4	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	6	2	5	0	0	4	1	0	0	0	0	0	0
1	6	2	8	2	2	1	2	2	1	0	3	0	0	1	1	0	0	1
1	7	11	19	1	1	2	2	2	4	0	15	1	0	1	1	1	0	0
8	0	9	8	2	1	6	3	3	0	2	16	4	3	0	0	0	0	0
8	7	0	10	2	3	2	2	4	4	0	8	1	0	1	1	1	0	1
1	0	11	9	2	2	6	3	5	0	0	6	1	0	1	1	1	1	1
8	3	0	19	2	1	1	2	4	2	2	16	4	3	0	0	0	0	0
1	11	2	6	2	2	2	3	5	5	2	1	4	0	0	0	0	0	0
8	11	2	6	2	1	1	3	4	5	2	16	4	3	0	0	0	0	0
8	3	8	1	2	3	2	2	4	2	2	16	4	3	0	0	0	0	0
1	5	0	13	2	2	3	2	5	3	0	4	3	2	1	1	0	0	1
8	6	9	11	2	1	1	2	4	1	2	16	4	3	0	0	0	0	0
1	3	12	13	2	4	3	3	0	2	0	15	0	0	1	1	1	1	0
8	3	2	11	2	5	3	3	1	2	0	4	1	1	0	1	0	0	0
1	3	0	16	2	3	3	2	2	2	0	11	1	0	1	1	0	0	1
8	5	0	15	2	1	1	2	4	3	2	16	4	3	0	0	0	0	0
1	5	0	8	2	5	2	2	5	5	0	14	1	0	1	1	1	0	1
1	3	0	12	2	1	2	2	5	2	0	2	0	1	1	1	0	1	1
8	5	0	11	2	1	2	2	4	3	0	1	4	0	1	1	0	0	0
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	5	2	9	2	3	1	3	0	3	0	13	1	0	1	1	0	0	1
8	0	0	8	2	2	6	2	3	0	2	1	2	0	0	0	0	0	0
1	6	0	20	1	1	1	1	5	1	2	2	4	3	0	0	0	0	0
8	3	7	15	2	3	3	3	1	2	0	7	0	0	1	0	0	0	0
1	7	0	18	2	2	3	2	5	4	0	6	1	0	1	1	1	0	1
1	6	0	13	2	2	3	2	0	1	0	6	1	0	1	1	0	0	1
1	11	2	5	2	2	2	2	2	5	0	7	2	0	1	1	0	0	1
1	7	0	13	2	2	1	2	5	4	2	1	3	0	0	0	0	0	0
1	7	0	9	2	1	1	2	0	4	2	0	0	0	0	0	0	0	0
3	2	5	4	2	2	0	2	0	5	2	16	4	3	0	0	0	0	0
1	7	2	7	1	2	0	2	0	4	2	16	4	3	0	0	0	0	0
8	0	0	8	2	2	6	2	5	0	2	16	4	3	0	0	0	0	0
1	3	0	21	1	1	1	1	0	2	0	2	1	0	1	0	1	1	0
8	5	0	8	2	6	3	2	4	3	0	12	1	0	1	1	0	0	1
8	5	2	5	1	2	2	2	5	5	0	5	1	0	1	1	0	0	0
1	9	2	7	1	1	2	1	0	5	2	5	0	0	1	1	0	0	1
1	7	2	10	1	2	1	1	2	5	2	16	4	3	0	0	0	0	0
1	1	0	23	1	5	3	1	5	5	0	10	1	0	1	1	1	0	1
8	7	5	7	1	3	3	2	5	5	0	2	1	0	1	1	0	0	1
1	7	2	3	1	1	2	1	0	4	2	16	0	1	1	1	0	0	1
8	5	5	14	2	2	2	3	1	3	0	1	4	0	0	0	0	0	0
8	7	2	24	1	7	3	2	1	5	2	16	4	3	0	0	0	0	0
8	3	12	13	1	2	3	2	4	2	0	1	1	0	1	0	0	0	0
1	5	0	19	1	2	3	1	0	3	0	3	1	0	1	1	1	0	1
8	3	11	10	1	4	3	2	1	2	0	8	1	1	1	1	0	0	1
1	7	0	17	2	4	0	2	0	4	0	8	1	0	1	1	1	0	0
8	3	5	8	2	2	3	3	4	2	0	2	1	0	1	0	0	0	0
8	0	12	9	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	0	8	2	4	3	2	5	2	0	8	1	0	1	1	1	0	0
1	5	0	13	2	4	3	2	0	3	0	1	1	0	1	1	0	0	1
8	5	2	7	1	4	1	2	1	5	0	6	0	0	0	0	0	0	0
1	3	0	15	2	3	1	2	5	2	0	15	0	0	1	1	1	0	0
8	3	0	13	2	5	3	2	5	2	0	9	1	2	1	1	0	0	1
1	6	12	14	2	1	1	2	2	1	2	16	4	3	0	0	0	0	0
8	5	0	11	2	2	3	2	4	3	0	2	0	0	1	1	1	0	1
8	11	3	23	1	6	2	3	1	5	0	7	1	0	1	1	1	0	1
1	9	6	6	1	2	1	1	2	5	2	16	4	3	0	0	0	0	0
8	3	14	3	1	1	3	1	5	2	0	10	1	0	1	1	1	1	1
8	7	11	14	2	7	1	2	4	4	2	16	4	3	0	0	0	0	0
8	11	2	23	1	1	1	2	1	5	2	16	4	3	0	0	0	0	0
1	0	14	8	2	2	6	3	5	0	2	2	4	0	0	0	0	0	0
1	3	8	6	1	2	3	2	5	2	0	3	4	0	1	1	0	0	1
1	7	0	26	1	3	3	1	0	5	0	9	4	0	1	1	1	0	1
8	3	0	12	2	5	3	2	5	2	0	12	4	1	1	1	1	0	1
8	5	0	11	2	3	2	2	5	3	0	4	0	1	1	1	0	1	1
8	5	0	10	2	3	2	2	4	3	2	6	0	1	1	1	0	0	0
8	3	1	15	1	7	3	2	5	2	0	8	0	0	1	1	1	1	1
1	9	13	22	1	2	3	3	5	5	2	16	4	3	0	0	0	0	0
1	3	0	10	2	4	7	2	5	2	0	15	2	0	1	1	1	1	1
1	3	10	18	2	3	1	2	2	2	0	15	1	0	1	1	1	0	0
8	6	2	10	2	1	1	3	4	1	0	10	0	1	1	1	0	0	1
1	7	0	11	2	6	3	2	0	4	2	16	4	3	0	0	0	0	0
1	5	5	6	2	1	2	3	5	3	2	16	4	3	0	0	0	0	0
8	6	3	2	2	3	3	2	4	1	0	6	1	0	1	1	1	0	1
1	3	0	26	1	2	3	1	5	2	0	4	4	0	1	1	1	0	1
1	5	0	26	1	3	3	1	2	5	2	2	1	1	1	1	0	0	0
8	3	0	20	1	1	3	1	5	2	0	15	1	0	1	1	0	0	0
8	0	0	10	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	6	15	2	4	3	2	5	2	0	9	1	0	1	1	1	0	0
1	6	2	9	1	2	3	1	2	5	2	0	4	1	0	0	0	0	0
8	7	0	13	2	3	3	2	4	4	0	5	1	0	1	1	1	0	1
1	3	5	7	2	2	3	2	2	2	0	15	4	0	1	0	0	1	0
8	0	0	8	2	9	6	2	3	0	2	16	4	0	1	0	1	0	0
8	3	0	22	1	1	1	1	1	2	0	15	0	0	1	1	0	0	0
6	5	6	2	2	1	3	2	4	3	2	16	4	3	0	0	0	0	0
8	7	2	8	1	1	1	2	5	4	2	16	4	3	0	0	0	0	0
8	11	11	2	2	2	3	2	5	5	0	3	0	0	1	1	0	0	1
8	3	0	16	2	4	3	2	4	2	0	4	1	0	1	1	1	0	1
8	6	2	8	2	2	1	3	4	1	0	9	4	0	1	0	0	0	0
1	5	0	12	2	2	2	2	0	3	0	3	4	1	1	1	1	0	1
1	6	5	7	2	3	3	2	5	1	0	6	0	0	1	1	1	0	0
8	6	2	3	2	1	1	2	1	1	2	16	4	1	0	0	0	0	0
8	2	7	3	1	2	0	1	5	5	2	16	4	3	0	0	0	0	0
1	0	9	6	2	2	0	3	2	5	0	7	1	0	0	0	0	0	0
8	0	9	8	2	4	6	3	3	0	2	16	4	3	0	0	0	0	0
8	3	0	20	1	1	4	1	5	5	2	16	4	3	0	0	0	0	0
8	5	14	14	2	3	3	3	4	3	0	5	0	2	1	1	0	0	1
1	5	0	27	1	3	3	1	2	5	0	15	1	0	1	1	1	0	1
8	6	0	16	2	3	3	2	4	1	0	3	1	0	1	1	0	1	1
8	5	5	7	2	1	2	2	1	3	2	16	4	3	0	0	0	0	0
1	3	0	16	2	5	3	2	2	2	0	4	4	1	1	1	1	0	1
8	3	0	17	2	2	3	2	4	2	0	2	4	0	1	1	0	0	1
1	3	0	12	2	3	3	2	2	2	0	13	0	1	1	1	0	1	1
1	1	14	28	1	2	3	3	2	5	0	1	1	0	1	1	0	1	1
8	0	2	4	2	1	6	3	3	0	2	16	4	3	0	0	0	0	0
1	7	12	30	0	2	0	2	0	5	0	4	1	0	1	1	1	1	1
8	6	0	12	2	1	2	2	5	5	0	5	4	0	1	1	1	0	1
8	5	10	24	1	3	1	3	1	5	2	2	3	1	0	0	0	0	0
8	5	3	3	2	1	2	2	5	3	2	16	4	3	0	0	0	0	0
8	7	2	9	1	1	1	1	1	5	2	16	4	3	0	0	0	0	0
8	3	14	10	2	1	1	3	4	2	2	1	0	1	1	1	0	0	1
8	3	0	10	2	4	2	2	5	2	0	7	0	1	1	0	0	0	0
8	5	5	14	1	2	3	3	4	3	0	3	1	0	1	1	0	0	1
8	5	5	2	2	2	1	2	4	3	2	16	4	3	0	0	0	0	0
1	0	0	8	2	2	6	2	3	0	2	16	4	2	0	0	0	0	0
8	7	0	9	2	3	2	2	4	4	0	6	1	0	1	1	0	0	1
1	6	10	15	2	3	2	3	2	1	2	16	4	3	0	0	0	0	0
8	3	0	19	1	4	3	1	4	2	0	7	1	0	1	1	1	1	1
8	1	0	23	1	2	3	1	1	5	0	8	0	0	1	0	1	1	0
8	3	13	5	2	1	1	3	4	5	0	11	2	0	1	1	0	0	1
1	7	7	25	0	5	3	3	2	5	0	13	1	0	1	1	1	1	1
8	3	0	12	2	5	3	2	4	2	0	10	1	0	1	1	0	0	1
8	3	0	12	2	2	3	2	1	2	2	2	1	0	0	0	0	0	0
1	5	2	9	1	3	3	2	5	5	2	1	3	0	1	0	0	0	0
1	7	0	14	2	1	3	2	5	4	0	8	1	0	1	1	1	0	1
8	5	0	23	1	4	3	1	1	5	0	15	1	0	1	1	1	0	1
8	2	5	9	2	1	0	3	5	5	0	10	0	0	1	1	1	0	0
8	9	0	18	2	3	3	2	5	5	0	4	1	0	1	1	1	0	1
8	1	0	23	1	2	3	1	1	5	0	4	1	1	0	0	0	0	0
1	3	6	4	1	2	6	2	5	2	0	2	1	0	1	1	0	0	1
8	5	0	11	2	4	3	2	5	3	0	9	4	0	1	1	0	0	1
1	3	3	17	1	3	3	2	5	2	0	5	1	0	1	1	1	0	1
8	5	6	22	1	1	3	3	5	5	2	5	0	1	1	1	1	0	0
1	7	2	7	2	2	1	2	0	4	2	0	4	1	0	0	0	0	0
8	0	11	10	2	2	6	3	5	0	0	4	1	1	1	0	0	0	0
1	7	2	10	0	3	7	1	2	5	0	15	1	0	1	1	0	0	1
8	3	0	10	2	2	3	2	5	2	0	5	1	0	1	1	1	0	1
8	6	3	3	1	2	2	1	1	5	0	6	1	0	1	1	1	0	0
8	6	0	17	2	2	3	2	4	1	0	4	4	0	0	0	0	0	0
1	7	0	13	2	2	3	2	2	4	0	15	1	0	1	1	0	0	1
1	6	0	12	2	1	0	2	5	1	2	16	4	3	0	0	0	0	0
1	6	2	9	1	4	3	2	5	1	0	10	1	0	1	1	0	0	1
8	5	14	16	2	2	6	3	5	5	0	1	1	0	1	1	0	1	0
8	3	0	17	2	1	1	2	1	2	0	5	0	1	1	0	0	0	0
1	2	2	5	2	1	0	2	5	5	0	5	0	1	0	0	0	0	0
8	3	2	25	1	4	0	2	5	5	2	1	3	1	0	0	0	0	0
8	3	6	10	1	5	3	2	4	2	0	11	1	0	1	1	0	1	0
8	0	9	8	2	1	6	3	3	0	0	2	2	0	1	1	0	0	0
1	7	0	10	2	4	2	2	5	4	2	16	4	3	0	0	0	0	0
1	5	0	8	2	3	2	2	0	3	0	13	1	0	1	1	1	1	1
1	6	0	20	1	3	6	1	5	1	0	3	1	0	1	1	0	1	1
1	0	0	10	2	1	6	2	5	0	0	15	1	0	1	1	0	0	0
8	5	5	12	2	2	3	2	4	3	0	7	0	2	1	1	0	0	1
1	6	2	8	2	1	3	2	2	1	0	8	2	0	1	1	1	0	0
8	0	0	8	2	1	0	2	3	0	0	3	4	0	1	1	0	0	1
1	7	9	7	1	5	3	1	2	5	0	6	0	1	1	1	0	0	1
1	5	0	23	1	2	3	1	2	5	0	4	1	0	1	1	1	0	1
1	3	0	14	2	3	3	2	0	5	0	7	4	0	1	1	1	0	1
1	6	5	10	2	2	1	3	2	1	2	16	0	1	1	0	0	0	0
1	6	0	14	2	2	3	2	2	5	0	5	1	0	1	1	0	0	1
8	7	9	1	2	5	3	2	5	5	2	16	4	3	0	0	0	0	0
1	7	0	15	2	3	2	2	0	5	0	15	0	0	1	1	1	0	1
8	7	0	13	2	4	3	2	1	4	0	9	2	0	1	1	0	0	1
1	3	5	14	1	4	3	3	0	2	0	0	1	0	1	1	0	1	0
8	5	0	16	2	2	3	2	4	3	0	2	1	0	1	1	1	1	1
1	3	2	6	1	1	6	2	0	2	0	8	1	0	1	1	0	0	1
6	11	11	9	2	2	7	2	5	5	2	16	4	3	0	0	0	0	0
1	7	0	31	1	2	2	1	2	5	0	15	1	0	1	1	1	0	1
8	3	0	11	2	2	3	2	4	2	0	4	4	0	1	1	0	1	1
8	3	2	6	1	3	1	2	5	5	2	9	1	0	0	0	0	0	0
1	3	0	12	2	1	3	2	0	2	2	16	4	3	0	0	0	0	0
8	6	2	17	2	2	3	2	4	1	0	15	4	0	1	1	1	0	1
1	1	0	23	1	6	3	1	0	5	0	3	4	0	1	1	1	0	1
0	3	9	1	2	2	7	2	4	2	2	16	4	3	0	0	0	0	0
8	7	0	19	2	5	1	2	5	4	0	12	0	0	1	1	0	1	0
1	12	4	8	1	2	0	1	5	5	2	3	3	1	0	0	0	0	0
8	5	0	11	2	3	3	2	4	3	0	4	4	1	1	1	0	1	1
8	0	9	8	2	1	6	3	3	0	2	16	4	3	0	0	0	0	0
1	1	14	23	1	2	3	3	2	5	0	1	1	1	1	1	0	0	1
8	6	2	4	2	6	1	2	5	1	2	16	4	3	0	0	0	0	0
8	3	12	1	1	1	1	2	4	2	2	2	0	1	1	1	1	0	1
8	6	0	20	1	2	3	1	5	1	0	1	1	0	1	1	0	1	1
1	3	0	12	2	4	3	2	2	2	0	10	1	0	1	1	1	0	1
1	6	2	6	1	1	1	1	2	5	2	7	4	1	0	0	0	0	0
1	0	0	8	2	2	0	2	5	0	2	16	4	3	0	0	0	0	0
8	5	0	16	2	4	3	2	1	3	0	2	0	1	1	1	0	0	0
1	11	0	20	1	3	1	1	5	5	0	5	0	0	1	0	0	0	0
1	6	9	15	2	1	3	3	2	1	0	5	1	0	1	1	1	0	1
3	7	11	5	2	1	1	2	2	4	2	16	4	3	0	0	0	0	0
1	7	0	31	1	3	3	3	0	5	0	2	1	0	1	1	1	0	1
8	7	0	21	1	3	3	2	4	4	0	7	3	1	1	1	1	1	1
8	6	0	12	2	1	3	2	4	1	0	6	0	0	1	0	0	0	0
8	0	0	9	2	3	6	2	5	0	0	7	2	0	1	1	0	0	0
1	6	11	8	2	2	3	3	2	1	2	16	4	3	0	0	0	0	0
1	3	0	18	2	4	3	2	5	2	0	8	1	0	1	1	0	0	1
1	3	0	12	2	4	3	2	0	2	0	8	2	0	1	1	1	0	1
1	3	6	13	2	3	3	3	2	2	0	7	2	0	1	1	0	0	0
8	0	14	5	1	3	6	2	5	5	0	4	1	0	1	1	0	1	0
1	5	0	10	2	3	2	2	2	3	0	3	1	0	1	1	0	1	1
1	6	11	14	2	3	1	3	0	1	2	16	4	3	0	0	0	0	0
1	0	12	8	2	2	0	3	5	0	0	4	1	0	1	1	1	0	1
1	5	0	23	1	3	2	1	5	5	2	16	4	3	0	0	0	0	0
8	3	10	20	1	6	3	2	5	2	0	5	1	0	1	1	0	0	1
8	3	0	16	2	3	3	2	4	2	0	3	1	0	1	0	0	0	0
8	3	5	22	1	3	3	2	5	2	0	8	1	0	1	1	1	0	1
8	0	0	9	2	2	6	2	3	0	0	16	0	2	1	1	0	0	1
8	3	6	4	1	3	3	2	4	2	0	9	1	0	1	1	1	0	1
1	6	10	14	2	1	3	3	5	1	0	6	4	0	1	1	1	0	0
8	6	8	9	2	4	2	2	4	1	0	15	1	0	1	1	1	0	1
1	5	0	26	1	3	6	1	0	5	2	16	4	3	0	0	0	0	0
8	3	0	21	1	5	3	1	1	2	2	16	4	3	0	0	0	0	0
8	7	12	5	2	2	3	2	5	4	0	3	3	0	0	0	0	0	0
1	7	2	13	2	1	0	3	2	4	2	4	0	0	1	1	0	0	0
1	0	9	8	2	2	0	3	2	5	0	4	0	0	1	0	0	0	0
1	5	0	19	1	1	3	1	2	3	0	6	0	0	1	1	1	0	1
1	3	10	18	2	1	3	2	2	2	0	11	1	0	1	1	1	1	1
1	3	7	1	2	6	2	2	5	2	0	3	1	0	0	0	0	0	0
1	3	0	15	2	5	3	2	5	2	0	13	0	0	1	1	1	1	1
8	3	14	4	2	2	3	2	5	2	0	1	1	1	1	1	0	0	0
8	3	0	20	1	4	0	3	1	2	0	10	0	1	1	1	0	0	1
8	3	0	13	2	3	1	2	5	2	0	3	1	0	1	1	1	0	1
1	1	10	23	1	5	3	2	5	5	0	10	1	0	1	1	0	0	1
1	3	5	1	2	6	2	2	0	2	1	1	0	0	1	0	0	0	0
8	3	0	12	2	2	3	2	4	2	0	2	1	0	1	1	0	0	1
8	2	12	4	2	5	0	2	5	5	0	15	1	1	0	1	0	0	0
1	9	0	18	2	3	3	2	0	5	0	7	1	0	1	1	1	1	1
1	5	0	20	1	1	0	1	5	3	0	3	4	0	1	1	0	0	1
1	6	10	21	1	2	3	2	5	1	0	6	1	0	1	1	0	1	1
8	6	0	12	2	2	1	2	4	1	0	1	3	0	1	1	0	0	1
8	7	0	9	2	2	3	2	4	4	0	4	1	0	1	1	0	1	1
8	3	13	9	1	2	6	2	1	2	0	1	0	0	1	1	0	0	0
8	0	9	7	2	2	0	3	3	0	2	1	4	1	0	0	0	0	0
1	8	9	2	1	1	3	1	5	5	0	3	0	1	0	0	0	0	0
8	2	3	8	2	1	0	2	5	5	2	16	4	3	0	0	0	0	0
3	3	5	14	1	4	3	3	5	2	0	8	0	0	1	1	0	0	1
1	7	0	21	1	4	3	1	0	4	0	9	1	0	1	1	1	1	1
8	9	0	12	2	3	3	2	4	5	0	4	1	0	1	1	0	0	1
8	9	0	26	1	2	3	1	1	5	1	1	0	0	1	0	0	0	0
1	0	0	9	2	1	6	2	5	0	0	7	0	1	1	1	0	0	0
1	7	5	15	1	3	3	3	0	4	2	16	4	3	0	0	0	0	0
8	6	2	4	2	1	1	3	5	1	0	8	0	0	1	1	0	0	1
1	5	0	14	2	2	1	2	2	3	1	4	1	1	1	0	1	0	0
8	8	0	11	2	3	3	2	1	5	0	7	1	0	1	1	1	0	1
1	3	3	8	2	4	3	3	5	2	2	16	4	3	0	0	0	0	0
8	6	0	13	2	3	3	2	1	1	0	5	2	0	1	1	1	1	0
1	3	0	14	2	4	3	2	5	2	0	3	1	0	1	1	0	1	1
8	3	9	4	2	1	1	2	4	2	0	8	1	0	1	1	1	0	1
1	7	5	20	1	1	1	3	2	4	2	16	4	3	0	0	0	0	0
1	11	3	5	2	1	3	2	5	5	0	1	3	1	1	1	0	0	1
8	7	6	3	2	3	2	2	4	4	0	7	1	0	1	1	1	1	1
8	6	5	4	2	2	2	3	4	1	0	6	4	0	1	1	1	0	1
8	3	5	11	2	1	3	3	4	2	0	3	1	1	1	1	0	1	1
1	5	9	9	2	3	2	3	5	3	0	7	1	0	1	1	0	1	1
8	5	0	19	1	5	3	1	4	3	0	5	1	1	1	1	0	1	1
5	0	5	1	2	3	6	3	3	0	2	16	4	3	0	0	0	0	0
1	5	6	23	1	2	3	3	2	5	2	16	0	0	1	0	0	1	0
8	7	2	6	1	2	3	1	1	5	2	2	0	1	1	1	0	0	0
8	3	0	20	1	2	3	1	4	2	2	1	0	0	1	1	0	1	1
1	7	0	31	1	2	3	1	2	5	0	2	2	0	1	1	1	1	1
1	7	0	11	2	4	3	2	2	5	0	14	4	0	1	1	1	1	0
8	3	14	6	2	1	2	3	5	2	1	1	3	0	0	0	1	0	0
8	3	7	17	2	3	3	3	4	2	0	6	0	1	1	1	0	0	0
1	7	0	31	0	2	0	0	2	5	0	4	1	0	1	1	1	0	1
8	6	0	22	1	2	0	1	1	1	0	8	0	0	1	1	0	0	1
8	3	0	12	2	5	3	2	4	2	0	15	1	0	1	1	1	1	1
1	5	0	23	1	4	2	1	5	5	2	11	1	0	1	0	0	0	0
1	7	4	12	0	4	3	2	5	5	0	15	1	0	1	1	1	1	1
8	7	1	18	1	2	3	2	1	5	1	2	1	0	1	1	0	0	0
4	3	6	5	2	1	2	2	0	2	2	0	4	1	0	0	0	0	0
8	6	2	13	2	2	1	3	4	1	2	16	4	3	0	0	0	0	0
8	5	0	19	2	4	3	2	4	3	0	8	1	0	1	1	0	0	1
8	7	2	12	1	3	3	1	5	5	0	5	1	0	0	0	0	0	0
8	3	0	19	1	7	3	1	4	2	2	16	4	3	0	0	0	0	0
8	7	5	11	2	3	2	3	1	4	0	3	0	0	1	1	0	0	0
8	7	0	20	2	2	3	2	4	4	0	2	1	0	1	1	1	0	1
1	11	0	10	2	3	2	2	2	5	2	1	4	1	0	0	0	0	0
1	5	0	31	1	4	3	1	2	5	0	11	2	0	1	1	0	0	1
1	6	0	12	2	3	4	2	0	1	2	16	4	3	0	0	0	0	0
8	3	3	7	2	1	3	3	4	2	0	6	4	0	1	1	1	1	1
8	3	5	4	1	1	2	2	1	2	2	1	2	0	0	0	0	0	0
8	5	0	20	1	3	3	1	4	3	0	8	1	0	1	1	1	1	1
1	11	2	4	2	1	1	2	0	5	0	7	3	1	1	1	0	0	1
8	3	0	12	2	1	3	2	4	2	0	8	1	0	1	1	0	1	1
8	3	0	16	2	3	1	2	4	2	0	4	2	0	1	1	0	0	1
8	0	9	1	2	3	0	2	3	0	2	16	4	3	0	0	0	0	0
1	1	0	22	1	3	1	1	2	5	0	7	1	0	1	1	0	0	0
8	3	7	2	2	4	3	3	4	2	0	13	1	0	1	1	0	0	0
8	5	6	2	2	2	3	2	1	3	0	3	1	0	1	1	0	0	0
8	3	7	18	1	2	3	3	5	2	0	7	4	1	0	1	0	0	0
8	11	0	8	2	4	3	2	4	5	2	16	4	3	0	0	0	0	0
1	0	0	9	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
8	3	9	3	2	5	1	2	5	2	0	14	1	0	1	1	0	0	0
8	7	0	22	1	2	1	1	1	4	2	1	0	1	1	1	0	0	0
8	3	0	22	1	5	3	1	5	2	0	1	1	0	1	0	0	0	0
1	3	10	21	1	2	0	3	2	2	0	10	4	0	1	0	0	0	0
8	11	0	17	2	3	3	2	4	5	2	16	4	3	0	0	0	0	0
1	5	10	30	1	2	3	2	5	5	0	15	1	0	1	1	1	0	1
8	0	9	8	2	4	6	3	3	0	2	2	4	1	0	0	0	0	0
1	7	14	15	2	3	3	2	2	4	0	8	4	0	1	1	0	0	0
8	5	9	11	2	3	3	3	4	3	0	3	1	0	1	1	1	0	1
1	5	10	13	2	1	5	3	2	3	0	2	4	0	1	1	0	0	0
1	11	10	27	1	3	3	3	2	5	2	16	4	3	0	0	0	0	0
8	3	0	19	1	1	1	1	4	2	0	4	3	0	1	1	0	1	1
8	5	0	12	2	5	3	2	4	3	0	11	1	0	1	1	1	0	1
8	5	0	8	2	3	3	2	4	3	0	5	1	0	1	1	0	0	1
8	7	6	17	2	2	3	3	5	4	2	1	4	0	0	0	0	0	0
1	5	10	13	2	4	1	3	2	3	0	9	2	0	1	1	1	0	0
8	0	0	8	2	3	6	2	3	0	0	7	0	0	1	0	1	0	0
1	3	0	13	2	1	3	2	5	2	2	16	0	0	0	0	0	0	0
1	7	0	8	2	2	2	2	0	4	2	0	0	0	0	0	0	0	0
1	9	0	20	1	2	3	2	2	5	0	5	1	0	1	0	1	0	0
8	3	6	2	1	3	6	1	1	2	0	7	4	0	1	0	0	0	0
8	7	2	7	2	3	1	2	1	4	0	11	3	0	0	0	0	0	0
8	6	0	12	2	5	3	2	4	1	0	15	1	0	1	1	1	1	1
8	11	2	7	2	1	1	2	4	5	2	16	0	2	1	0	0	0	0
8	6	0	17	2	3	3	2	4	1	2	1	0	1	0	0	0	0	0
1	6	0	20	1	2	3	1	2	5	2	16	4	1	0	0	1	0	0
1	7	0	29	1	3	3	1	2	5	0	7	1	0	1	1	1	0	1
1	3	11	15	1	2	3	3	5	2	0	2	2	0	1	1	0	0	1
1	7	2	5	2	2	6	2	5	4	0	2	3	0	1	1	1	0	0
1	7	0	27	1	4	3	1	2	5	0	15	1	0	1	1	0	1	1
1	6	2	9	2	1	3	2	2	1	0	15	1	0	1	1	0	0	0
8	7	2	7	1	1	2	1	1	5	2	16	4	3	0	0	0	0	0
1	7	0	13	2	2	2	2	5	4	0	15	2	0	1	1	1	0	0
8	11	7	18	1	3	3	2	5	5	2	16	4	3	0	0	0	0	0
1	3	0	21	1	2	3	1	5	2	0	6	1	0	1	1	0	0	1
8	6	0	12	2	2	2	2	4	1	2	16	4	3	0	0	0	0	0
1	0	5	10	2	2	2	3	0	0	0	3	1	0	1	1	0	0	0
8	5	0	17	2	1	2	2	4	3	2	16	4	3	0	0	0	0	0
1	3	0	15	2	4	2	2	5	2	0	8	4	0	1	1	1	0	1
8	11	0	15	2	2	2	2	5	5	2	16	4	3	0	0	0	0	0
1	5	2	21	1	3	3	3	0	5	2	16	4	0	1	1	1	1	1
8	0	0	9	2	9	6	2	3	0	0	15	1	0	1	1	0	0	1
8	5	5	1	1	3	3	1	4	3	0	6	4	0	1	1	1	1	1
1	6	14	1	2	3	1	2	2	1	0	5	1	0	1	1	1	0	0
1	6	5	25	1	4	3	2	5	1	0	9	0	0	1	1	0	0	0
8	0	2	10	1	1	1	1	5	5	0	15	1	0	1	1	1	0	1
8	7	13	31	0	3	3	2	1	5	0	1	3	0	0	0	0	0	0
1	0	12	5	2	2	0	3	5	0	0	9	1	0	1	1	1	1	1
1	3	0	19	2	4	3	2	2	2	2	16	4	3	0	0	0	0	0
8	0	0	9	2	2	6	2	3	0	0	15	2	0	1	1	1	0	0
8	5	2	5	1	5	1	1	1	5	0	15	1	0	0	1	0	0	0
1	7	2	5	1	2	1	2	2	4	2	16	4	3	0	0	0	0	0
1	1	0	23	1	4	3	1	2	5	0	15	0	0	1	0	0	1	0
1	5	0	20	1	3	3	1	2	3	0	1	4	1	1	0	0	0	0
1	3	6	18	2	3	3	3	5	2	0	6	1	0	1	1	0	1	0
8	0	0	9	2	1	0	2	5	0	2	16	4	3	0	0	0	0	0
1	5	4	2	1	2	3	1	2	5	2	16	4	3	0	0	0	0	0
1	6	11	17	2	3	3	3	0	1	0	4	1	0	0	1	0	0	0
1	7	5	6	1	9	7	1	0	5	2	16	4	3	0	0	0	0	0
8	0	2	3	2	1	6	3	5	0	2	16	4	3	0	0	0	0	0
1	3	4	11	2	5	3	2	5	2	0	9	2	0	1	0	0	1	0
1	3	0	18	2	1	2	2	0	2	0	2	0	1	1	1	0	0	1
8	2	12	6	2	1	0	2	1	5	0	5	4	1	1	1	1	1	0
8	3	4	10	2	1	3	3	4	2	0	2	1	1	1	1	0	0	1
8	3	7	1	2	5	3	2	4	2	0	15	1	0	1	1	1	0	1
8	0	9	7	2	2	6	3	3	0	0	3	0	0	1	0	0	0	0
1	7	0	20	1	4	6	1	2	4	0	0	1	0	0	0	0	0	0
8	5	0	16	2	2	3	2	4	3	0	8	4	0	1	1	1	1	1
1	0	0	9	2	2	6	2	5	0	0	11	4	0	1	1	0	0	1
8	7	5	7	2	1	3	2	1	4	2	16	4	3	0	0	0	0	0
8	3	2	19	1	1	3	2	5	2	2	16	0	1	1	1	0	0	1
1	2	4	9	2	2	0	3	2	5	2	1	0	2	0	1	0	0	0
1	6	12	11	1	4	6	2	0	1	0	15	1	1	1	1	1	0	0
1	1	11	4	1	5	2	1	5	5	0	9	1	1	1	1	0	0	1
1	11	14	1	2	5	3	2	5	5	2	16	4	3	0	0	0	0	0
8	7	0	20	1	2	3	1	4	4	0	7	4	0	1	1	1	0	1
8	0	0	8	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	0	20	1	2	3	1	5	2	0	4	1	0	1	1	0	0	0
1	11	3	5	2	2	1	2	2	5	2	16	0	0	0	0	0	0	0
1	3	0	11	2	2	2	2	0	2	0	5	4	3	0	0	0	0	0
1	3	2	9	1	1	0	1	0	5	0	15	1	0	1	1	0	0	0
8	0	0	9	2	1	6	3	3	0	2	16	4	3	0	0	0	0	0
8	6	14	26	1	4	3	2	1	5	0	4	1	0	1	1	1	0	1
8	3	0	8	2	6	2	2	4	2	0	11	1	1	1	1	1	0	1
8	1	0	24	1	5	3	1	1	5	2	1	1	0	1	1	0	0	0
8	6	11	3	2	1	1	2	1	1	2	16	4	3	0	0	0	0	0
1	7	0	12	2	2	3	2	5	4	2	9	0	1	1	1	1	1	1
1	3	1	8	2	3	3	3	5	2	0	4	1	0	1	0	0	0	0
8	5	0	19	1	5	3	1	4	3	0	7	1	0	1	1	1	1	1
1	3	0	10	2	1	2	2	5	2	0	3	1	0	1	1	0	0	1
1	5	0	16	2	2	3	2	0	3	2	16	4	0	0	0	0	0	0
8	7	6	4	1	4	3	2	4	4	0	14	1	0	1	1	0	0	1
8	0	2	3	2	2	6	3	3	0	2	1	4	0	0	1	0	0	1
1	7	14	30	1	2	2	3	2	5	0	2	3	0	0	0	1	0	0
8	3	14	5	2	2	2	3	4	2	1	2	1	0	1	0	0	0	0
1	0	0	9	2	3	6	2	5	0	0	8	0	0	0	1	0	0	0
8	3	3	13	2	1	3	3	1	2	1	2	4	0	1	0	0	0	0
8	3	0	8	2	2	3	2	4	2	0	3	1	0	1	1	0	0	1
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	6	18	1	4	3	2	4	2	0	1	3	0	0	1	0	0	0
1	3	7	5	2	5	6	3	5	2	0	3	1	0	1	1	0	0	1
1	3	9	2	1	6	3	2	5	2	2	16	4	3	0	0	0	0	0
8	5	3	10	1	2	1	2	1	5	0	5	2	0	1	1	0	0	1
8	6	6	5	2	1	3	2	4	1	0	12	1	0	1	1	0	1	1
1	0	11	10	2	2	6	3	5	0	0	6	2	0	1	1	0	0	0
8	0	14	9	2	2	6	3	3	0	2	11	0	1	0	0	0	0	0
8	6	2	12	2	4	3	2	4	1	2	16	4	3	0	0	0	0	0
1	3	10	21	1	5	6	3	2	5	0	15	0	0	1	1	0	0	0
1	5	0	13	2	2	3	2	5	3	0	1	2	0	1	1	1	1	1
8	7	2	11	0	4	1	1	5	5	0	5	1	0	1	1	1	0	0
8	3	0	11	2	3	3	2	1	2	0	7	1	0	1	1	0	0	0
1	0	0	9	2	1	0	2	5	0	2	2	4	0	0	0	0	0	0
1	7	2	5	2	2	1	2	5	4	2	16	4	3	0	0	0	0	0
1	1	0	22	1	3	3	1	5	5	0	6	1	0	1	1	0	0	1
8	5	2	8	2	1	2	3	1	3	2	16	4	3	0	0	0	0	0
1	7	2	5	1	2	1	2	2	4	0	5	4	1	1	1	0	0	1
1	3	0	26	1	3	3	1	5	2	0	5	2	0	1	1	0	0	1
1	3	0	13	2	4	3	2	5	2	0	6	1	0	1	0	0	0	0
1	7	6	12	1	2	3	2	2	5	0	2	0	0	1	1	0	0	0
8	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	10	20	1	3	3	2	5	2	0	8	1	0	1	1	1	0	0
1	3	5	13	1	3	3	3	2	2	0	3	0	1	1	1	0	0	1
8	7	2	15	2	2	1	3	5	4	0	5	0	0	1	1	0	1	0
8	5	0	20	1	2	1	1	5	3	0	14	1	0	1	1	1	0	1
8	0	9	9	2	1	6	3	5	0	2	16	4	3	0	0	0	0	0
1	7	0	31	1	3	3	1	2	5	0	6	0	0	1	1	0	1	1
8	3	0	16	2	3	3	2	4	2	0	13	1	0	0	1	0	0	0
8	7	0	10	2	2	5	2	4	4	0	15	0	0	1	1	1	0	1
8	11	2	5	2	1	1	2	1	5	2	1	4	1	0	0	0	0	0
1	5	1	8	2	2	1	3	2	3	0	7	0	1	1	1	0	0	0
1	0	0	9	2	1	6	2	5	0	0	4	1	1	1	1	1	0	0
8	3	0	20	1	3	3	1	1	2	0	4	1	0	1	1	0	0	1
1	7	11	9	2	1	2	3	0	4	0	15	4	0	1	1	1	0	1
8	5	2	6	2	2	1	2	4	3	0	3	4	0	1	1	0	0	1
8	9	10	16	2	3	2	3	4	5	2	16	4	3	0	0	0	0	0
8	0	0	9	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	6	12	2	4	0	3	5	2	2	16	4	3	0	0	0	0	0
1	3	0	12	2	2	3	2	5	2	0	4	1	0	1	1	1	1	1
8	7	14	1	2	2	3	2	4	4	2	16	4	3	0	0	0	0	0
1	3	13	2	2	1	2	2	0	2	0	4	0	0	1	1	0	0	0
1	11	2	6	2	1	6	2	5	5	0	2	3	1	1	0	0	0	0
8	0	0	8	2	1	6	2	5	0	2	1	3	0	0	0	0	0	0
1	0	0	9	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
8	6	11	20	2	4	3	2	4	1	0	3	0	0	1	1	1	0	1
1	3	0	18	2	3	3	2	0	2	0	2	1	0	1	0	1	0	0
1	3	0	12	2	5	3	2	5	2	0	6	0	0	1	1	1	0	0
8	3	0	12	2	2	3	2	4	2	0	5	1	0	1	1	1	1	1
8	7	14	20	1	2	5	2	1	5	0	6	0	0	1	1	0	0	1
8	3	6	2	2	2	3	2	4	2	0	1	1	0	1	1	0	0	1
8	6	9	2	2	1	3	2	5	1	0	5	4	1	1	1	0	0	0
8	0	0	9	2	1	6	2	3	0	0	1	4	2	1	1	0	0	1
8	7	0	18	2	3	3	2	4	4	2	16	4	3	0	0	0	0	0
8	3	0	11	2	2	2	2	5	2	2	2	1	0	1	0	0	0	0
1	3	0	17	2	5	3	2	5	2	0	11	1	0	1	1	0	0	1
1	7	12	1	0	3	6	1	2	5	2	2	1	1	1	0	1	0	0
1	3	0	20	1	3	3	1	2	2	0	5	1	0	1	1	0	0	1
1	7	0	14	2	3	3	2	0	5	0	5	1	0	1	1	1	1	1
8	5	6	11	1	3	1	2	5	3	0	5	0	0	1	1	0	0	1
1	3	10	26	1	2	3	2	0	2	0	2	1	0	1	0	0	0	0
8	5	0	19	2	2	4	2	4	3	0	4	4	0	1	0	0	1	0
1	0	0	9	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
8	6	0	15	2	1	3	2	4	1	0	3	4	2	1	1	0	1	1
3	0	11	10	2	1	6	2	5	0	0	15	0	1	1	0	0	0	0
8	6	0	17	2	1	3	2	1	1	0	2	3	0	1	1	0	1	0
8	5	2	4	2	1	1	2	5	3	2	16	4	3	0	0	0	0	0
8	6	2	7	2	2	3	2	4	1	2	16	4	3	0	0	0	0	0
1	3	0	27	1	5	3	1	5	2	0	15	1	0	1	0	1	0	0
1	3	13	28	1	1	3	3	5	2	0	3	0	0	1	1	0	1	1
8	5	0	20	1	1	3	1	4	3	2	16	4	3	0	0	0	0	0
1	7	0	15	2	4	3	2	2	4	2	16	4	3	0	0	0	0	0
1	8	0	15	2	3	3	3	5	5	2	16	4	3	0	0	0	0	0
8	13	0	17	2	4	3	2	4	5	2	16	4	3	0	0	0	0	0
1	2	14	7	2	2	0	3	2	5	0	8	0	1	0	0	0	0	0
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	7	2	9	1	1	1	1	1	5	2	1	4	3	0	0	0	0	0
1	1	0	24	1	4	3	1	0	5	0	12	1	0	1	1	1	0	1
8	0	9	1	2	2	0	2	5	0	2	0	3	0	0	0	0	0	0
1	5	0	16	2	3	3	2	0	3	0	6	1	0	1	1	1	1	1
8	0	0	9	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
8	3	2	5	3	1	0	3	4	5	2	1	3	1	1	0	0	0	0
1	7	0	22	1	2	3	1	0	4	0	1	1	0	1	1	0	0	1
8	0	0	9	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
8	5	2	4	2	2	1	3	4	3	0	1	1	1	1	0	0	0	0
1	3	0	9	2	4	6	2	2	5	0	3	1	0	1	1	1	0	0
8	5	3	8	2	1	2	3	4	3	2	16	4	3	0	0	0	0	0
1	6	0	13	2	4	1	2	0	1	0	15	1	0	0	0	0	0	0
8	5	14	4	2	4	3	2	5	3	0	9	1	0	1	1	0	0	1
8	0	0	9	2	1	6	2	3	0	0	4	1	0	1	1	0	0	1
1	5	0	15	2	4	0	3	5	5	0	5	0	0	1	1	1	0	0
1	3	0	19	2	2	1	2	5	2	2	16	4	3	0	0	0	0	0
8	3	0	17	2	1	1	2	4	2	2	16	4	3	0	0	0	0	0
8	3	0	18	1	2	3	1	4	5	0	3	1	0	1	1	1	1	1
8	7	3	6	0	3	5	1	5	5	0	2	3	1	1	1	0	0	1
8	3	0	11	2	2	3	2	4	2	0	8	4	0	1	1	0	1	1
8	5	6	1	1	3	3	1	4	3	2	16	4	3	0	0	0	0	0
1	6	0	19	2	2	2	2	2	1	2	1	1	0	0	0	0	0	0
8	3	9	8	1	1	1	2	5	2	0	7	1	0	1	1	1	0	1
8	3	9	16	2	1	1	2	4	2	0	7	2	0	1	1	1	0	1
8	5	9	5	1	0	1	2	5	3	2	16	4	3	0	0	0	0	0
1	7	0	11	2	2	1	2	5	4	0	5	0	0	1	1	0	0	0
1	3	0	11	2	4	3	2	2	5	0	13	1	0	1	1	0	0	1
1	11	2	10	1	3	1	2	2	5	0	5	0	1	1	0	0	0	0
8	0	0	8	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0
1	5	11	10	1	1	1	2	2	5	0	11	2	0	1	1	0	0	1
1	7	0	21	1	2	1	1	2	5	2	16	4	0	0	0	0	0	0
8	0	0	10	2	2	6	2	5	0	0	4	4	0	1	1	0	0	1
1	3	0	8	2	6	3	2	5	2	0	14	1	0	1	1	1	0	1
8	3	0	20	1	2	1	1	1	2	0	3	4	1	0	0	0	0	0
1	7	3	17	1	2	1	3	2	4	0	4	0	0	1	0	0	0	0
8	6	11	12	2	5	2	3	4	1	0	9	1	0	1	1	1	0	1
1	6	2	4	1	1	1	2	2	1	0	8	1	0	1	1	0	0	0
8	5	7	3	2	5	3	2	1	3	2	16	4	3	0	0	0	0	0
1	0	0	8	2	2	6	2	5	0	0	9	1	0	1	0	0	0	0
1	7	10	30	0	4	3	3	2	5	0	7	0	1	1	1	1	0	0
8	3	0	19	2	5	4	2	4	2	2	16	4	3	0	0	0	0	0
8	3	14	9	2	5	2	3	5	2	0	13	1	0	1	1	1	0	1
8	3	0	17	2	2	3	2	1	2	0	7	1	0	1	1	0	0	0
1	3	6	2	1	2	6	2	5	2	0	3	3	1	1	0	0	1	0
8	5	2	12	2	1	1	2	5	3	0	8	0	0	1	1	1	0	1
1	3	0	7	2	3	2	2	4	2	2	16	4	3	0	0	0	0	0
8	3	0	9	2	1	2	2	4	2	0	2	4	0	0	0	0	0	0
1	7	0	11	2	2	1	2	5	4	0	3	0	0	1	0	0	0	0
1	5	0	9	2	2	2	2	0	3	0	6	1	0	1	1	0	0	1
4	3	7	15	2	2	3	3	4	2	0	6	4	2	0	0	0	0	0
1	5	3	8	2	4	2	3	0	3	0	15	0	1	0	0	1	0	0
1	7	0	10	2	2	2	2	0	4	2	3	3	0	0	0	0	0	0
8	7	10	25	1	2	1	2	5	5	2	7	0	0	1	1	1	1	1
1	11	13	16	1	3	3	2	5	5	0	15	1	0	1	1	1	0	1
8	5	0	21	1	1	3	1	1	5	0	3	3	0	1	1	0	0	1
1	3	11	18	1	2	3	3	5	2	0	5	1	0	1	1	1	1	1
1	6	3	6	2	1	1	2	0	1	2	16	0	1	0	1	0	0	0
8	3	0	8	2	3	3	2	4	2	0	13	1	0	1	1	1	1	1
1	7	9	9	1	2	3	1	5	5	2	16	3	0	1	0	1	0	0
8	7	0	20	1	2	3	1	4	4	0	11	4	1	1	1	0	0	1
1	3	0	9	2	3	2	2	5	2	2	4	1	0	1	1	0	0	1
1	7	2	12	1	3	6	2	2	5	0	15	1	1	0	0	0	0	0
1	5	1	19	1	4	3	3	2	3	0	8	1	0	1	1	0	0	0
8	3	0	11	2	3	3	2	4	2	2	16	4	3	0	0	0	0	0
8	3	9	8	2	0	7	3	5	2	2	16	4	3	0	0	0	0	0
1	7	0	21	1	3	2	1	2	5	0	15	1	0	1	1	1	0	1
8	3	5	3	1	1	6	2	5	2	1	2	0	0	1	0	0	0	0
8	0	0	10	2	1	6	2	5	0	0	1	3	0	1	0	0	0	0
1	3	9	7	2	1	2	3	4	2	2	16	4	0	1	1	0	0	1
1	3	2	4	2	1	1	2	2	2	0	9	1	1	1	1	1	0	0
8	7	10	17	2	3	3	2	4	4	0	7	2	0	1	1	1	1	0
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	11	4	3	2	1	2	3	2	5	2	16	4	3	0	0	0	0	0
1	2	12	5	2	3	0	2	5	5	0	4	0	0	0	0	0	0	0
1	5	2	15	1	3	3	3	2	5	0	7	0	1	1	1	0	0	0
1	7	4	18	2	5	2	3	5	4	0	11	1	0	1	1	1	0	1
1	7	0	31	1	3	5	1	5	5	0	6	1	0	1	0	0	0	0
1	11	0	32	0	5	3	0	5	5	0	13	0	0	1	1	1	1	1
8	6	2	18	1	4	3	3	4	1	2	16	4	3	0	0	0	0	0
1	5	0	12	2	3	3	2	5	3	0	4	1	0	1	1	1	1	1
1	7	0	31	1	9	7	1	5	5	2	16	4	3	0	0	0	0	0
1	3	0	8	2	2	3	2	5	2	0	15	2	0	1	1	0	0	0
1	3	6	1	2	4	2	3	0	2	0	6	2	0	1	1	0	0	1
8	7	0	25	1	3	3	1	1	4	0	2	1	0	1	0	0	0	0
8	3	3	5	2	3	3	2	5	2	0	7	1	0	1	1	1	0	0
3	7	5	7	1	2	6	1	5	5	2	16	4	1	0	0	1	0	0
8	11	0	21	1	2	6	1	4	5	2	16	4	3	0	0	0	0	0
1	0	12	25	1	4	3	3	2	5	0	7	0	1	1	1	1	1	1
1	3	0	12	2	9	6	2	2	2	2	16	4	3	0	0	0	0	0
1	3	0	21	1	4	3	1	2	2	0	7	1	0	1	1	1	0	0
8	3	0	18	2	2	0	2	1	2	2	16	4	3	0	0	0	0	0
8	3	2	6	2	3	1	2	4	2	2	16	4	3	0	0	0	0	0
8	3	0	20	1	2	1	1	4	2	2	3	4	1	0	0	0	0	0
1	7	2	16	2	2	1	3	2	4	2	16	4	3	0	0	0	0	0
8	3	0	8	2	1	1	2	4	2	2	1	0	1	1	1	0	0	1
1	1	0	23	1	4	0	1	0	5	0	5	1	0	1	1	0	0	0
1	7	2	9	1	3	2	1	0	5	0	3	0	1	0	0	1	0	0
1	5	0	12	2	6	3	2	5	3	0	8	1	0	1	1	0	0	0
8	3	0	20	1	4	3	1	1	2	0	3	4	0	1	1	1	1	1
1	5	6	1	1	1	3	1	5	5	0	3	4	1	1	1	0	0	0
8	3	0	9	2	2	3	2	4	2	0	1	4	0	1	1	0	1	1
8	9	0	15	2	1	3	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	22	1	2	3	1	5	2	2	16	4	3	0	0	0	0	0
1	6	0	16	2	1	1	2	5	5	0	10	4	0	1	1	0	0	1
1	7	14	9	1	4	3	1	5	5	0	2	0	0	1	1	0	0	1
8	0	9	8	2	3	6	3	3	0	0	15	2	0	1	1	0	0	1
1	3	2	13	1	2	3	2	5	2	0	6	4	0	1	1	0	1	1
8	0	0	24	1	2	6	1	5	5	0	3	4	2	0	0	0	0	0
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	8	2	9	1	4	2	2	1	5	0	5	1	0	1	0	1	0	0
8	5	2	4	2	1	1	2	4	3	0	9	1	0	0	0	0	0	0
8	3	2	11	2	1	1	3	5	2	0	1	0	0	1	1	0	0	1
1	11	0	31	1	2	3	1	5	5	0	1	2	0	1	1	0	1	1
8	11	0	20	1	3	3	1	1	5	0	9	1	0	1	1	1	0	1
8	3	0	19	2	1	1	2	4	2	2	16	4	3	0	0	0	0	0
1	11	0	17	2	3	0	2	0	5	2	0	4	1	0	0	0	0	0
8	3	0	18	2	2	3	2	5	2	0	6	0	0	1	1	0	0	1
1	7	2	19	2	2	1	2	2	4	2	16	4	3	0	0	0	0	0
1	9	3	2	2	1	1	2	5	5	2	1	3	0	0	0	0	0	0
1	6	0	13	2	3	6	2	5	1	0	15	1	0	1	1	1	0	1
1	2	2	9	2	2	0	2	5	5	2	1	4	0	0	0	0	0	0
8	5	9	7	2	1	1	2	4	3	2	3	4	0	1	1	0	0	1
8	11	2	16	2	2	4	3	5	5	2	16	4	3	0	0	0	0	0
1	7	2	15	0	1	6	2	5	5	2	16	4	3	0	0	0	0	0
1	5	0	10	2	2	2	2	2	3	0	1	4	0	1	1	1	0	1
1	9	10	27	1	4	7	3	2	5	2	16	4	3	0	0	0	0	0
1	7	0	31	1	1	6	1	2	5	2	0	4	1	0	0	0	0	0
8	3	0	17	2	2	3	2	4	2	2	1	3	1	0	0	0	0	0
8	0	9	9	2	1	6	3	3	0	2	16	4	3	0	0	0	0	0
1	0	13	1	2	3	6	2	3	0	0	2	1	0	1	1	0	0	0
8	7	2	11	2	1	1	3	4	4	2	16	4	3	0	0	0	0	0
7	6	6	7	2	1	2	3	4	1	0	9	4	0	1	1	1	1	1
1	7	14	8	2	2	2	3	5	4	0	7	1	0	1	1	0	1	1
8	3	0	20	1	4	3	2	4	2	0	13	0	0	1	1	0	0	1
1	0	12	7	2	1	0	3	5	5	2	3	0	0	0	0	0	0	0
8	0	14	7	2	1	0	3	3	0	0	15	4	0	1	1	0	0	0
1	6	3	6	2	4	3	3	0	1	0	5	1	0	1	1	1	0	1
1	3	12	28	0	3	3	2	2	2	2	16	4	3	0	0	0	0	0
1	9	9	14	2	1	0	2	4	5	2	16	4	3	0	0	0	0	0
8	3	9	16	2	3	2	3	1	2	0	15	1	0	1	1	1	0	0
8	0	9	8	2	1	0	3	3	0	2	16	4	3	0	0	0	0	0
1	5	2	8	2	2	3	2	5	3	0	14	0	0	1	1	0	0	0
1	7	0	18	2	3	3	2	5	4	0	4	1	0	1	1	0	0	0
1	3	0	13	2	4	3	2	0	2	0	15	2	0	1	1	0	0	0
1	5	0	16	2	2	3	2	0	3	0	2	0	1	1	1	0	0	1
8	3	0	11	2	2	2	2	4	2	0	15	2	0	1	1	0	1	1
1	5	9	1	2	4	2	2	0	3	2	5	2	0	1	1	0	0	1
1	11	9	8	2	2	3	2	0	5	2	16	4	3	0	0	0	0	0
8	0	0	8	2	2	6	2	3	0	0	4	3	2	1	1	0	0	0
8	3	0	14	2	1	3	2	4	2	0	1	0	1	1	1	0	1	1
8	6	2	7	1	1	1	2	5	1	2	16	4	3	0	0	0	0	0
8	7	0	8	2	2	2	2	4	4	0	15	0	0	1	1	0	0	1
8	7	0	18	2	4	3	2	5	4	0	14	1	0	1	1	1	1	1
8	3	0	9	2	5	2	2	4	2	0	15	1	0	1	1	1	0	1
8	3	0	15	2	2	3	2	4	2	1	2	0	0	1	0	0	0	0
8	11	10	20	2	2	4	2	4	5	0	9	1	0	1	1	1	0	1
1	7	2	6	2	2	3	2	0	4	0	4	4	0	1	1	0	0	1
8	3	0	9	2	6	3	2	4	2	0	15	1	0	1	1	1	0	1
8	6	2	11	2	4	2	2	1	1	0	2	0	1	0	0	0	0	0
8	3	0	9	2	2	5	2	4	2	2	16	4	3	0	0	0	0	0
8	0	9	10	2	1	0	3	3	0	0	15	0	0	0	1	1	0	1
1	3	13	12	2	2	3	3	2	2	2	16	3	1	1	1	0	0	1
8	7	0	16	2	3	3	2	4	4	0	7	1	0	1	1	1	0	1
8	0	13	10	2	1	0	3	3	0	2	16	0	0	1	1	0	0	1
8	6	6	12	2	2	3	3	4	1	2	0	4	0	0	0	0	0	0
8	3	0	12	2	1	3	2	4	2	0	3	0	0	1	1	0	1	1
8	3	6	10	2	6	3	2	5	2	0	14	1	1	1	1	0	0	1
8	0	12	5	2	1	6	3	3	0	2	1	3	2	0	0	0	0	0
1	6	2	6	2	2	6	2	5	1	2	16	4	3	0	0	0	0	0
1	5	0	11	2	2	2	2	2	3	2	16	4	3	0	0	0	0	0
1	5	0	21	1	4	3	2	0	5	2	16	4	3	0	0	0	0	0
8	5	0	10	2	2	3	2	4	3	0	4	1	0	1	0	1	0	0
1	11	14	5	2	3	2	2	2	5	2	16	4	3	0	0	0	0	0
1	5	9	8	1	2	3	2	0	5	0	13	1	0	1	0	0	0	0
1	11	0	15	2	5	3	2	5	5	0	12	3	0	1	1	0	0	1
1	6	2	7	1	2	3	2	5	1	0	6	1	0	1	1	0	0	0
1	5	2	7	1	2	1	1	2	5	2	16	4	3	0	0	0	0	0
8	5	9	7	1	1	3	2	5	5	2	16	4	1	1	0	0	0	0
8	5	2	12	2	3	1	3	1	3	0	5	1	0	1	1	0	0	0
8	3	9	9	2	6	3	2	1	2	1	5	1	1	1	1	1	0	1
1	7	2	13	1	6	6	2	2	5	2	16	4	3	0	0	0	0	0
8	7	2	21	0	2	1	1	1	5	2	16	4	3	0	0	0	0	0
8	0	0	8	2	2	6	2	3	0	1	4	4	0	1	1	0	0	0
1	3	0	19	2	4	3	2	0	2	0	12	1	0	1	1	1	0	1
8	6	13	5	2	2	1	2	1	1	0	11	2	0	1	1	1	0	1
8	3	14	5	2	4	2	2	4	2	2	16	4	3	0	0	0	0	0
1	6	0	16	2	1	1	2	0	1	2	16	4	3	0	0	0	0	0
8	5	6	12	2	3	3	3	4	3	2	1	3	0	1	0	1	0	0
8	3	0	20	1	5	3	1	4	2	0	15	4	0	1	1	1	1	1
1	7	4	29	1	2	4	2	5	5	0	4	4	0	0	1	0	0	0
1	6	2	14	2	5	3	3	2	1	0	15	4	0	1	1	0	0	1
8	5	0	20	1	2	0	1	1	3	2	16	3	0	0	0	0	0	0
1	3	0	8	2	2	2	2	5	2	0	5	0	0	1	1	1	0	1
1	11	0	11	2	4	3	2	0	5	2	16	4	0	1	1	1	0	1
1	3	0	9	2	4	2	2	2	2	2	16	4	3	0	0	0	0	0
1	6	3	10	2	1	2	3	2	1	0	5	3	0	1	1	0	1	0
1	7	14	28	1	4	3	3	2	5	0	15	4	1	1	0	0	0	0
1	5	0	9	2	4	2	2	2	5	1	4	0	0	1	0	0	0	0
8	8	14	12	2	1	3	3	4	5	2	16	4	3	0	0	0	0	0
1	7	2	5	1	1	1	2	5	4	2	1	4	0	1	0	0	0	0
8	7	0	18	2	1	3	2	4	4	1	2	0	0	1	0	0	0	0
1	0	0	9	2	9	6	2	5	0	2	16	4	3	0	0	0	0	0
8	11	6	7	2	4	4	2	4	5	0	8	1	0	1	1	1	1	1
8	7	6	1	1	3	3	1	5	4	0	10	1	0	1	1	1	0	1
8	11	6	13	2	2	3	3	1	5	0	2	2	0	1	1	0	1	1
1	3	0	12	2	1	3	2	5	2	0	1	4	0	1	1	0	0	0
8	3	9	13	1	4	3	3	4	2	0	9	0	0	1	0	1	1	0
1	3	0	20	1	2	3	1	0	2	0	10	0	0	1	1	1	0	1
1	9	2	5	1	1	2	2	2	5	2	16	4	3	0	0	0	0	0
1	0	0	9	2	1	6	2	5	0	0	11	4	1	1	1	0	0	0
1	3	6	4	1	3	3	1	5	2	0	1	1	0	1	0	0	0	0
1	11	2	11	1	2	1	2	2	5	2	2	0	1	1	1	0	0	0
1	6	6	27	1	4	3	3	2	1	0	13	1	0	1	0	1	0	0
1	7	0	15	2	6	1	2	5	4	2	16	4	1	1	1	0	0	0
1	5	2	10	1	2	3	2	2	5	0	1	4	0	1	1	0	0	1
8	7	0	12	2	3	3	2	4	4	0	4	1	0	1	1	0	0	1
1	7	10	31	1	3	3	3	0	5	0	6	0	0	1	0	1	0	0
8	7	2	25	1	2	1	3	5	4	2	16	4	3	0	0	0	0	0
8	6	2	8	2	4	1	2	5	1	0	9	2	0	1	1	0	0	0
8	9	14	18	2	2	3	2	4	5	2	0	1	1	0	0	0	0	0
1	3	0	20	1	4	3	1	5	2	0	14	1	0	1	1	0	0	1
8	0	0	8	2	2	6	2	5	0	2	1	2	0	1	1	0	0	0
1	3	0	13	2	1	2	2	5	2	0	1	0	0	1	1	0	0	1
8	3	7	18	1	1	3	3	5	2	2	16	4	3	0	0	0	0	0
1	5	0	12	2	4	0	2	5	5	2	5	2	0	1	0	1	0	0
1	5	1	7	2	4	3	2	0	3	0	15	1	0	1	1	1	0	1
1	11	0	23	1	3	3	1	2	5	2	16	4	3	0	0	0	0	0
1	3	0	12	2	4	3	2	2	2	0	15	1	0	1	1	1	0	1
1	11	0	26	1	3	3	1	2	5	2	16	4	3	0	0	0	0	0
1	7	10	12	2	4	3	3	5	5	0	7	1	0	1	1	1	0	1
1	0	5	3	2	3	6	3	5	0	2	1	4	0	0	0	0	0	0
1	3	0	8	2	4	6	2	5	2	0	8	4	0	1	1	1	0	0
8	5	0	8	2	4	2	2	1	3	0	1	1	0	1	1	0	0	0
1	1	9	24	1	3	3	3	0	5	0	6	1	0	1	1	1	1	1
8	6	0	13	2	2	3	3	4	1	0	1	3	0	1	1	0	0	0
1	5	2	21	1	4	2	3	2	5	2	16	4	3	0	0	0	0	0
1	8	0	20	1	2	2	1	0	5	0	8	1	0	1	1	1	1	1
8	3	0	10	2	1	2	2	4	5	0	1	1	0	1	1	0	1	1
1	11	0	15	2	1	1	2	5	5	0	2	0	1	1	1	1	0	1
1	0	0	8	2	1	6	2	5	0	0	13	0	0	1	1	0	0	1
8	6	6	2	1	3	3	1	1	1	0	7	0	0	1	1	0	0	1
1	5	0	24	1	3	3	1	0	5	2	16	4	3	0	0	0	0	0
8	3	6	10	2	2	3	3	4	2	2	16	4	3	0	0	0	0	0
8	3	10	17	2	2	3	2	4	2	0	1	1	1	0	0	0	0	0
8	11	0	16	2	2	3	2	4	5	2	16	4	3	0	0	0	0	0
1	6	13	5	2	3	0	3	5	1	2	6	4	0	0	0	0	0	0
1	3	0	9	2	4	2	2	5	2	0	15	1	0	0	0	1	0	0
8	7	2	5	2	2	1	2	1	4	2	16	4	3	0	0	0	0	0
8	9	13	11	1	2	1	2	1	5	0	3	1	0	1	0	0	0	0
1	3	0	14	2	5	3	2	5	2	2	16	4	1	0	0	0	0	0
1	2	2	18	1	3	0	2	5	5	2	16	4	3	0	0	0	0	0
8	7	7	10	2	2	2	3	4	4	2	16	4	3	0	0	0	0	0
8	3	9	5	2	7	3	2	5	2	0	8	4	0	0	0	0	0	0
1	7	0	9	2	2	2	2	5	4	2	16	4	3	0	0	0	0	0
8	6	11	10	2	1	2	3	4	1	0	3	0	0	1	1	1	1	1
1	9	2	4	2	1	3	2	2	5	2	2	4	0	0	0	0	0	0
1	3	2	5	1	1	1	2	5	2	2	16	4	3	0	0	0	0	0
1	7	2	7	2	4	3	3	2	4	2	16	4	3	0	0	0	0	0
1	1	14	23	1	4	3	1	5	5	0	8	1	0	1	1	0	1	1
8	7	0	31	1	4	3	1	1	5	2	0	1	0	0	0	0	0	0
1	6	2	6	1	4	3	2	2	1	2	16	4	0	0	0	0	0	0
8	2	8	2	2	3	0	2	1	5	2	6	0	1	0	0	0	0	0
8	3	0	9	2	4	3	3	4	2	0	15	1	0	1	1	1	1	1
8	0	0	8	2	2	0	2	5	0	2	16	4	3	0	0	0	0	0
1	5	0	23	1	4	3	1	5	5	0	8	1	0	1	1	0	0	1
1	3	0	23	1	2	0	1	5	5	2	16	4	3	0	0	0	0	0
8	11	0	15	2	3	1	2	4	5	0	15	1	0	1	0	0	0	0
1	0	9	8	2	2	6	3	3	0	2	2	3	0	1	1	0	0	1
8	5	3	9	2	1	2	3	4	3	2	16	4	3	0	0	0	0	0
8	5	0	12	2	3	3	2	4	3	0	3	1	0	1	1	0	0	1
8	0	0	9	2	1	6	2	5	0	2	16	4	3	0	0	0	0	0
1	7	14	30	1	3	3	2	0	5	0	8	2	0	1	1	0	0	1
1	0	14	1	2	2	6	2	3	0	0	7	4	0	0	0	0	0	0
4	7	0	9	2	4	2	2	5	4	0	4	1	0	1	1	0	1	1
8	8	14	12	2	2	3	2	5	5	2	2	0	0	1	1	0	0	1
8	0	0	9	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
1	7	9	30	1	3	3	2	0	5	0	8	1	0	1	1	0	1	1
1	11	0	19	2	1	1	2	5	5	0	15	1	0	1	1	1	0	0
1	11	2	9	2	1	1	2	5	5	0	6	3	1	1	1	0	1	0
1	5	14	12	1	9	7	2	5	5	2	16	4	3	0	0	0	0	0
1	7	14	20	1	4	1	3	2	5	0	15	0	0	1	1	0	0	1
8	0	12	7	2	2	6	3	3	0	0	8	0	0	1	1	1	0	1
1	0	0	8	2	1	0	2	5	0	2	15	4	0	0	1	0	0	0
8	3	5	15	1	1	1	3	4	2	2	16	4	0	0	0	0	0	0
8	5	2	8	1	2	0	2	5	5	2	2	0	0	1	1	0	0	1
1	3	11	8	2	2	3	3	0	2	0	3	1	0	1	1	0	1	1
1	7	6	15	1	2	2	1	5	5	2	5	3	0	1	0	0	0	0
1	7	0	15	2	4	3	2	2	4	0	6	1	0	1	1	0	0	1
8	0	0	2	3	2	0	3	3	0	0	3	0	0	1	1	0	0	1
1	9	0	26	1	2	3	1	5	5	2	16	4	1	0	0	0	0	0
8	7	10	18	2	2	1	3	4	4	2	16	4	3	0	0	0	0	0
8	6	14	2	2	2	3	2	4	1	2	16	4	3	0	0	0	0	0
8	0	12	10	2	1	6	3	5	0	2	16	4	3	0	0	0	0	0
8	7	5	3	2	4	2	3	1	4	0	15	0	1	1	1	1	1	0
8	3	12	1	1	5	3	2	5	2	0	2	1	0	1	1	0	1	1
1	5	5	7	2	3	3	2	0	3	0	2	2	0	1	1	1	0	0
1	5	3	14	1	3	3	2	5	5	0	15	1	0	1	0	1	0	0
1	7	0	16	2	3	3	2	0	5	0	3	4	0	1	1	0	0	1
8	6	2	4	1	1	1	2	1	1	2	16	4	3	0	0	0	0	0
1	3	0	20	1	5	3	1	2	2	0	15	1	0	1	1	0	0	1
1	3	10	15	2	1	1	2	5	2	2	1	3	1	0	0	0	0	0
1	7	0	31	1	3	1	1	2	5	0	4	1	0	1	1	0	0	0
1	6	0	12	2	2	3	2	5	1	2	16	4	3	0	0	0	0	0
8	7	0	8	2	4	3	2	4	4	0	13	1	0	1	1	0	0	1
1	5	0	12	2	4	3	2	2	3	0	5	0	1	1	1	0	0	0
1	3	14	12	2	4	3	3	5	2	0	8	4	1	1	1	1	0	1
1	0	14	8	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0
1	7	0	21	1	2	3	1	0	4	0	5	1	0	1	1	1	0	0
1	7	2	13	0	5	1	2	2	5	0	15	0	0	1	1	0	0	0
1	3	5	11	2	4	3	3	2	2	0	11	1	0	1	1	0	0	0
8	0	9	8	2	1	0	3	3	0	2	16	4	0	0	0	0	0	0
8	0	9	7	2	1	6	3	3	0	2	16	4	3	0	0	0	0	0
1	2	2	6	2	1	6	2	2	5	2	15	1	1	1	0	0	0	0
8	5	6	13	2	3	3	3	4	3	0	2	4	0	0	0	0	0	0
1	7	0	24	1	3	2	2	0	4	0	11	1	0	1	1	1	1	1
1	0	0	9	2	2	0	2	5	0	2	1	4	0	0	0	0	0	0
1	2	5	7	2	4	6	3	5	5	0	13	4	0	0	0	0	0	0
8	5	9	15	2	5	3	2	4	3	0	1	3	0	1	1	0	0	1
1	1	6	23	1	3	7	3	5	5	2	16	4	3	0	0	0	0	0
8	6	2	8	2	1	1	2	4	1	0	15	0	0	1	0	0	0	0
3	9	5	5	1	2	3	1	5	5	0	4	3	0	1	1	0	0	1
8	1	0	17	2	5	3	2	5	5	0	4	1	0	1	0	1	0	0
1	3	0	9	2	3	2	2	5	2	0	1	4	0	1	1	0	1	0
1	5	2	8	1	1	1	2	2	5	0	15	0	0	1	1	0	1	1
8	3	9	21	1	2	3	2	4	2	2	1	1	1	0	0	0	0	0
1	3	0	13	2	3	3	2	5	2	0	15	1	0	1	1	0	0	1
1	3	0	30	1	3	3	1	5	2	0	3	1	0	1	1	0	1	1
8	3	0	9	2	1	3	2	4	2	2	16	4	3	0	0	0	0	0
2	3	5	7	2	4	6	3	1	2	0	5	1	0	1	1	0	0	1
8	6	0	18	2	5	3	2	4	1	0	10	1	0	1	1	1	1	1
1	7	0	13	2	1	1	2	2	5	0	4	3	0	1	1	0	0	0
1	3	0	11	2	6	2	2	0	2	0	15	3	0	1	0	0	0	0
8	0	0	10	2	2	6	2	5	0	0	8	0	1	1	0	0	0	0
8	7	2	10	2	2	1	3	4	4	0	4	4	0	1	0	0	0	0
1	3	0	17	2	1	6	2	5	2	0	13	1	1	1	1	0	0	1
8	0	5	4	2	4	6	3	3	0	2	16	4	0	0	0	0	0	0
1	0	9	6	2	2	6	3	5	0	2	3	1	0	0	0	0	0	0
1	7	0	28	1	2	3	1	2	5	0	6	1	0	1	1	0	0	0
1	6	0	19	2	3	3	2	0	1	0	11	1	0	1	1	0	0	1
1	2	9	4	2	1	0	2	5	5	2	16	4	2	0	0	0	0	0
1	3	0	7	2	3	2	2	4	2	0	3	1	1	0	0	0	0	0
8	5	0	14	2	3	3	2	4	3	0	6	1	0	1	1	0	0	1
1	6	6	2	2	3	1	2	0	1	2	16	4	3	0	0	0	0	0
8	6	6	7	1	2	1	2	4	1	0	7	2	0	1	1	0	1	1
8	0	0	10	2	3	6	2	3	0	0	5	3	0	1	0	0	0	0
1	5	3	10	1	2	3	1	0	5	2	2	0	1	0	0	0	0	0
8	3	0	13	2	3	3	2	4	2	0	4	4	0	1	0	0	0	0
1	2	3	16	1	2	3	3	2	5	0	7	1	0	1	0	1	0	0
8	3	9	5	1	4	2	2	4	2	1	2	0	0	1	1	0	0	1
2	6	6	7	2	2	5	2	1	1	0	4	1	1	1	1	0	0	1
1	9	10	19	2	3	0	3	2	5	0	2	1	0	1	1	1	0	1
1	3	5	7	2	1	1	3	2	2	2	1	4	0	1	1	0	0	0
8	2	2	5	2	1	0	2	5	5	2	16	4	3	0	0	0	0	0
1	3	0	22	1	3	3	1	2	2	0	16	1	1	0	0	1	0	0
8	3	2	5	2	1	1	2	4	2	2	16	4	3	0	0	0	0	0
1	1	0	23	1	2	3	1	2	5	0	5	1	0	1	1	1	0	1
1	3	2	7	2	1	3	3	2	2	2	1	0	1	1	1	0	0	1
8	1	2	13	1	4	3	2	5	5	0	15	1	0	0	1	0	0	0
8	3	0	16	2	2	3	2	5	2	0	3	0	0	1	1	0	0	1
8	0	0	9	2	4	6	2	3	0	0	15	1	0	1	1	1	0	1
1	7	0	18	2	2	2	2	0	4	2	16	4	3	0	0	0	0	0
1	5	0	26	1	4	3	1	2	5	0	2	1	0	0	1	0	0	0
8	3	14	18	1	2	2	3	4	2	0	9	4	0	1	1	0	1	1
1	7	0	12	2	2	3	2	2	4	2	1	4	0	1	1	0	0	1
1	7	0	28	1	3	3	1	0	5	0	5	1	0	1	1	1	0	1
1	6	2	8	2	2	3	3	2	1	0	1	1	1	0	0	0	0	0
1	7	7	16	1	4	3	3	2	4	0	4	0	0	1	1	1	0	1
8	7	0	31	1	2	1	2	5	5	0	3	1	0	1	1	1	0	1
8	3	2	11	1	3	1	2	5	2	0	6	0	1	0	1	0	0	0
1	3	0	12	2	1	1	2	5	2	0	15	1	0	1	1	1	0	1
1	5	0	21	1	3	3	1	5	3	2	16	4	3	0	0	0	0	0
8	6	0	18	2	2	0	2	1	1	2	16	4	3	0	0	0	0	0
1	7	0	22	1	2	3	1	0	4	2	16	4	3	0	0	0	0	0
1	7	9	25	0	2	3	3	0	5	0	3	1	0	1	0	1	0	0
1	1	0	23	1	4	3	1	5	5	0	8	1	0	1	1	1	0	1
1	7	0	20	1	4	3	1	0	4	0	15	1	0	1	1	1	0	1
1	3	9	16	2	3	3	3	5	2	2	3	4	0	0	0	0	0	0
1	7	0	17	2	2	2	2	5	5	0	1	1	0	1	0	0	0	0
8	0	0	10	2	2	6	2	5	5	2	16	4	3	0	0	0	0	0
8	0	0	9	2	1	6	2	3	0	2	16	4	3	0	0	0	0	0
1	5	11	24	1	1	3	2	5	5	2	0	0	0	0	0	0	0	0
1	3	0	11	2	3	3	2	2	2	0	10	1	0	1	1	1	1	1
8	3	9	20	1	2	3	2	4	2	0	3	1	0	1	1	1	1	1
1	7	14	1	2	1	1	2	2	4	0	14	1	0	1	1	0	0	1
3	7	2	6	2	1	1	2	2	4	2	16	0	0	1	1	0	0	0
8	3	0	17	2	3	3	2	4	2	2	16	3	0	0	0	0	0	0
8	5	0	11	2	2	1	2	4	3	2	16	4	3	0	0	0	0	0
8	0	0	8	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	7	8	2	4	2	3	1	2	0	10	1	0	1	1	0	0	0
1	1	6	8	1	6	3	2	5	5	0	15	0	0	1	1	1	0	1
8	3	0	18	2	5	4	2	4	2	0	13	1	0	1	1	1	0	1
8	3	0	19	2	2	3	2	5	2	0	1	1	0	0	1	0	0	0
0	3	6	19	1	2	3	3	2	2	2	16	4	3	0	0	0	0	0
8	3	0	16	2	5	3	2	1	2	0	7	1	0	1	1	1	0	1
8	0	0	8	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0
8	3	11	11	2	4	4	2	5	2	0	8	0	0	1	1	1	0	1
8	3	0	18	2	2	3	2	4	2	2	2	3	0	0	0	0	0	0
1	5	0	9	2	2	2	2	5	3	0	1	1	0	1	1	1	0	0
1	7	10	31	1	8	7	1	5	5	2	16	4	3	0	0	0	0	0
8	11	2	5	2	2	3	2	4	5	2	16	4	3	0	0	0	0	0
8	3	0	7	2	3	2	3	5	2	0	6	1	0	1	1	0	0	0
8	0	5	9	2	4	6	2	3	0	0	6	2	0	1	1	0	0	1
1	7	0	27	1	1	3	1	0	5	0	4	1	0	1	0	0	0	0
1	3	10	18	2	3	6	3	2	2	0	3	1	0	1	1	1	0	1
8	3	9	15	2	1	1	3	4	2	0	10	0	0	1	0	0	1	0
8	5	0	17	2	3	3	2	4	3	0	2	1	0	0	1	0	0	0
1	7	0	14	2	2	0	2	2	4	2	1	4	0	0	0	0	0	0
1	3	5	7	2	1	3	2	5	2	2	16	4	3	0	0	0	0	0
1	7	14	13	2	4	1	2	2	5	2	16	4	3	0	0	0	0	0
8	5	2	6	1	2	1	2	1	5	2	16	4	3	0	0	0	0	0
1	3	0	10	2	3	2	2	5	2	0	8	1	0	1	1	1	0	0
8	0	12	19	1	9	0	2	4	5	2	16	4	3	0	0	0	0	0
1	7	10	13	2	4	6	3	2	5	0	8	1	1	1	1	1	0	1
8	3	14	10	2	2	1	3	4	2	2	16	4	3	0	0	0	0	0
8	7	12	9	2	1	0	3	5	4	2	16	4	0	0	0	0	0	0
8	2	14	2	2	4	0	2	4	5	0	3	1	0	1	1	1	1	1
8	3	0	9	2	3	3	2	4	2	0	4	3	0	1	1	0	0	1
8	3	0	18	2	3	3	2	4	2	0	11	4	3	0	0	0	0	0
8	3	0	20	1	3	3	1	5	2	0	14	1	1	1	1	1	0	1
1	2	2	7	1	2	0	2	2	5	0	9	4	0	1	1	0	0	0
8	6	2	13	2	2	3	3	1	1	2	16	4	3	0	0	0	0	0
1	3	0	15	2	1	3	2	5	2	2	16	4	3	0	0	0	0	0
1	7	0	31	1	2	3	1	2	5	0	3	1	0	0	1	0	0	0
8	7	2	16	1	9	4	1	1	5	2	16	4	3	0	0	0	0	0
1	7	3	13	1	2	3	2	5	5	0	2	2	0	1	1	1	1	1
8	0	0	9	2	1	6	2	3	0	0	6	1	0	1	1	1	0	1
8	6	0	11	2	3	3	2	4	1	0	5	3	2	1	1	0	1	1
1	5	2	7	1	2	2	2	0	5	2	2	0	0	0	0	0	0	0
8	5	0	8	2	6	3	2	4	3	0	8	0	0	1	1	1	0	1
8	1	0	12	2	3	3	2	1	5	0	5	1	1	1	1	1	1	1
8	3	0	14	2	1	3	2	1	2	0	15	1	0	1	1	1	0	1
8	7	11	12	2	1	1	2	5	4	2	16	4	3	0	0	0	0	0
8	6	5	7	2	1	1	2	1	1	0	10	0	1	1	1	0	0	1
8	9	0	14	2	8	4	2	4	5	2	16	4	3	0	0	0	0	0
8	3	2	7	2	1	6	2	5	2	2	16	4	0	0	0	0	0	0
8	3	5	17	2	2	3	2	4	2	0	3	1	0	1	1	1	0	0
1	7	7	7	2	3	1	3	2	4	0	3	4	0	1	1	0	0	0
8	0	0	8	2	2	6	2	5	0	2	2	4	0	0	0	0	0	0
8	6	0	10	2	3	2	2	1	1	0	2	1	1	0	0	0	0	0
8	0	0	9	2	2	0	2	3	0	0	9	0	1	1	1	0	0	1
8	3	0	16	2	6	3	2	4	2	0	15	4	1	1	1	0	1	1
1	5	0	8	2	3	3	2	5	3	0	1	0	0	1	1	0	0	1
1	0	9	8	2	1	6	3	5	0	0	15	1	0	1	1	1	0	1
1	1	3	11	1	2	1	2	2	5	2	16	4	3	0	0	0	0	0
8	3	6	5	2	4	3	3	5	2	0	8	1	0	1	1	0	0	1
1	8	0	23	1	2	3	1	2	5	2	16	4	3	0	0	0	0	0
1	3	0	8	2	3	6	2	5	2	0	5	0	0	1	1	0	0	0
8	3	0	12	2	2	1	2	4	2	2	16	4	3	0	0	0	0	0
8	7	0	31	0	2	2	1	2	5	2	16	4	3	0	0	0	0	0
1	1	10	23	1	3	3	2	0	5	0	7	1	0	1	1	1	1	0
1	6	0	10	2	3	2	2	5	1	2	16	4	3	0	0	0	0	0
1	9	0	13	2	1	1	2	2	5	2	16	4	3	0	0	0	0	0
8	9	2	11	1	2	3	2	1	5	0	5	4	0	1	1	0	0	1
1	0	14	10	2	2	6	3	5	0	0	9	1	0	1	1	1	0	1
1	6	6	13	2	1	2	3	0	1	0	8	4	0	1	1	0	1	1
8	3	0	19	1	1	3	1	1	2	2	16	4	3	0	0	0	0	0
1	7	9	19	1	4	3	2	5	4	0	8	1	0	1	1	1	0	1
1	3	14	6	2	3	3	2	5	2	0	7	1	0	1	1	1	0	1
1	7	0	13	2	2	3	2	0	5	0	7	0	1	1	1	1	0	1
8	3	0	20	1	3	3	1	4	2	0	7	2	0	1	1	0	1	1
8	3	0	9	2	5	3	2	4	2	0	6	0	0	1	1	0	0	1
8	0	9	9	2	2	0	3	3	0	2	16	4	0	1	0	1	0	0
8	3	9	6	2	1	2	2	5	2	2	16	4	3	0	0	0	0	0
8	7	14	31	1	3	1	1	1	5	0	11	1	0	1	1	1	0	0
8	0	0	9	2	7	6	2	3	0	2	16	4	3	0	0	0	0	0
1	3	10	20	1	3	3	3	5	2	0	15	1	0	1	1	0	0	0
1	5	0	16	2	2	2	2	5	3	0	3	1	0	1	1	1	1	1
8	3	13	19	1	2	3	2	1	2	2	0	1	0	0	0	0	0	0
8	3	0	13	2	3	1	2	1	2	0	12	1	0	1	0	0	1	0
8	7	2	15	1	1	5	3	1	4	2	16	4	3	0	0	0	0	0
1	3	0	20	1	3	3	1	0	2	0	10	1	0	1	1	1	0	1
8	6	0	17	2	4	2	3	1	1	2	16	4	3	0	0	0	0	0
1	0	14	8	2	1	6	2	5	0	0	14	1	0	1	1	0	0	1
8	1	0	23	1	1	3	1	5	5	0	16	4	2	1	0	0	0	0
8	3	14	1	2	2	2	2	4	2	0	7	4	1	1	1	1	0	0
8	5	13	12	2	2	3	3	4	3	0	5	1	1	1	1	0	0	1
1	11	0	11	2	3	2	2	5	5	0	6	1	0	1	1	1	0	0
8	3	6	10	2	4	2	2	1	2	0	15	0	0	1	1	0	0	0
8	0	0	10	2	4	6	2	3	0	0	5	1	0	0	1	0	0	1
8	5	2	5	1	1	1	1	1	5	2	4	0	2	1	1	0	0	1
7	0	9	9	2	1	6	3	3	0	0	15	1	0	1	1	1	0	1
1	7	0	14	2	4	3	2	0	4	2	16	4	3	0	0	0	0	0
8	11	5	10	1	4	1	2	4	5	0	7	1	1	1	1	1	1	1
1	7	0	15	2	4	3	2	0	5	0	1	0	1	1	1	1	1	1
1	3	12	5	2	7	3	3	5	2	0	4	1	0	1	1	0	0	1
8	3	0	16	2	2	2	2	1	2	2	0	1	0	0	0	0	0	0
1	3	5	3	2	2	3	2	5	2	0	1	0	0	1	1	0	0	1
8	5	9	5	2	2	2	3	4	3	2	16	4	3	0	0	0	0	0
1	3	2	7	2	1	1	2	2	2	0	14	4	0	1	0	0	0	0
8	5	2	18	2	2	1	2	4	3	0	3	4	1	1	1	0	0	1
8	3	2	9	2	3	0	3	1	2	0	15	1	0	1	1	1	0	0
8	3	2	5	2	2	5	3	4	2	2	3	4	2	1	1	0	0	0
8	0	9	10	2	3	6	3	3	0	0	5	4	0	1	0	0	0	0
1	1	0	24	1	5	3	1	0	5	0	15	1	0	1	1	1	0	1
1	5	3	4	1	3	6	2	5	3	2	16	4	3	0	0	0	0	0
8	3	0	11	2	1	3	2	4	2	0	12	4	0	1	0	1	0	0
1	9	2	6	1	2	1	2	2	5	2	1	4	0	0	0	0	0	0
1	3	3	6	2	4	3	3	5	2	0	6	2	0	1	1	1	0	1
1	6	2	6	1	1	1	1	5	5	2	16	4	3	0	0	0	0	0
1	5	0	22	1	2	3	1	2	3	2	16	4	3	0	0	0	0	0
8	3	0	14	2	5	3	2	4	2	0	1	1	0	1	1	0	0	0
1	6	14	14	2	1	3	2	2	1	2	16	4	3	0	0	0	0	0
1	0	12	7	2	3	0	3	2	5	2	16	4	3	0	0	0	0	0
8	3	6	7	2	3	3	3	5	2	0	9	1	0	1	1	1	0	1
1	11	3	10	1	1	1	2	0	5	0	9	1	0	1	1	1	0	1
1	3	0	16	2	3	3	2	0	2	2	16	4	3	0	0	0	0	0
1	5	0	8	2	3	2	2	2	3	0	6	0	1	1	1	1	0	1
1	7	0	31	1	4	3	1	5	5	0	6	1	0	1	1	1	0	1
1	0	0	9	2	1	6	2	5	0	0	6	0	0	1	1	1	0	1
1	9	0	25	1	2	3	1	5	5	0	4	2	0	1	1	0	1	1
8	3	0	15	2	3	3	2	5	2	0	6	2	0	0	0	0	0	0
8	3	2	19	2	4	1	3	4	5	2	16	4	3	0	0	0	0	0
1	3	0	16	2	3	3	2	0	2	0	6	0	1	1	1	0	0	1
1	5	0	10	2	4	2	2	5	3	0	6	1	0	1	1	1	0	0
8	5	14	12	2	2	3	3	4	3	0	3	4	0	1	1	0	1	1
8	7	0	9	2	2	3	2	4	4	0	2	1	0	1	1	0	0	0
8	7	2	6	2	2	3	2	5	4	0	2	1	0	1	1	1	0	0
8	3	9	8	2	1	1	3	4	2	0	4	4	0	1	1	1	0	1
8	9	0	17	2	4	3	2	4	5	0	5	1	0	1	1	1	0	0
1	0	9	9	2	2	0	3	5	0	0	15	0	0	1	1	0	0	1
8	3	0	17	2	4	3	2	4	2	0	3	2	0	1	1	1	1	1
1	8	0	24	1	5	3	1	5	5	0	13	2	0	1	1	1	0	1
8	6	2	10	1	2	1	2	5	1	2	1	3	0	1	1	1	0	0
1	3	14	2	2	2	3	2	5	2	0	3	4	0	1	1	0	0	1
8	3	0	11	2	2	3	2	4	2	0	2	0	0	1	1	0	1	1
8	0	0	8	2	3	6	2	5	0	0	4	1	0	1	0	1	0	0
8	0	0	9	2	2	0	2	3	0	0	4	2	0	1	1	0	0	0
1	7	2	16	1	2	6	2	5	5	2	6	0	0	1	0	0	0	0
1	9	0	30	1	6	3	1	0	5	0	12	1	0	1	1	1	0	1
8	3	0	19	1	3	3	1	4	2	0	2	1	0	1	1	0	1	1
1	5	2	11	2	3	1	3	5	3	0	3	2	0	1	1	0	0	0
1	5	0	17	2	2	1	2	0	3	0	3	0	0	1	1	0	0	0
1	7	0	14	2	1	2	2	4	4	2	1	0	0	1	1	0	1	1
1	11	5	21	1	4	3	3	2	5	0	1	4	0	1	1	0	1	1
8	0	0	8	2	2	6	2	3	0	2	2	3	2	1	1	0	0	0
1	3	0	18	2	3	1	2	0	2	0	5	1	1	1	1	0	0	1
1	7	0	11	2	2	6	2	5	4	2	2	1	1	1	1	0	0	1
8	7	0	11	2	5	3	2	5	4	0	12	1	0	1	1	0	1	1
8	9	0	19	2	2	1	2	1	5	0	8	3	0	1	0	0	0	0
1	3	0	15	2	4	3	2	2	2	0	15	1	0	1	1	0	0	0
8	5	9	5	2	1	3	2	4	3	2	16	4	3	0	0	0	0	0
8	5	14	1	2	2	2	2	5	3	0	15	0	0	1	1	1	0	0
8	3	0	9	2	4	7	2	4	2	2	16	4	3	0	0	0	0	0
8	7	3	16	2	1	3	3	4	4	0	2	3	0	1	1	0	0	1
1	3	5	6	1	2	6	2	2	2	0	12	1	0	1	1	1	1	1
1	7	2	8	2	3	2	2	2	4	2	16	4	3	0	0	0	0	0
8	0	0	10	2	2	6	2	5	0	0	13	1	0	0	1	0	0	0
8	3	0	19	1	3	3	1	5	2	0	6	1	0	1	1	0	1	1
8	0	0	8	2	3	6	2	3	0	0	4	0	1	1	0	0	0	0
1	7	0	10	2	2	3	2	5	4	0	6	1	0	1	1	1	0	0
1	7	0	8	2	3	2	2	0	4	0	7	0	0	1	1	0	0	1
1	3	9	21	1	4	0	3	2	2	0	1	0	1	1	1	0	0	1
1	11	0	19	2	2	3	2	5	5	0	3	1	0	1	0	1	0	0
8	6	2	16	2	1	1	3	1	1	0	7	0	1	0	0	0	0	0
1	3	1	2	2	2	0	2	5	2	0	8	4	0	1	1	0	0	1
1	7	3	31	1	2	3	2	0	5	0	2	3	0	0	0	0	0	0
1	11	7	26	1	1	3	3	2	5	2	16	4	3	0	0	0	0	0
1	0	0	8	2	0	6	2	5	0	0	9	3	0	1	0	1	0	0
8	5	0	20	2	2	3	3	4	3	2	0	4	0	0	0	0	0	0
8	3	6	7	2	3	1	2	5	2	1	1	1	0	1	0	0	0	0
8	3	2	17	2	4	3	2	4	2	0	15	1	0	1	1	1	0	1

8	3	0	23	1	4	3	1	5	5	0	9	1	0	1	1	1	0	0	0
8	7	0	13	2	2	3	2	4	4	0	4	4	0	1	1	0	0	1	0
8	7	0	15	2	3	3	2	1	4	2	16	4	3	0	0	0	0	0	0
1	2	0	9	2	3	6	2	2	5	2	16	4	3	0	0	0	0	0	0
1	9	0	15	2	2	3	2	0	5	2	0	4	0	1	1	0	0	0	0
8	3	0	8	2	3	3	2	4	2	0	5	2	0	1	1	0	1	1	0
8	5	2	13	2	1	1	3	1	3	0	3	1	0	1	0	0	0	0	0
1	11	11	13	2	3	3	2	2	5	0	7	2	0	1	1	1	1	1	0
8	9	11	9	2	3	3	2	4	5	0	5	0	1	1	1	0	0	1	0
1	11	0	11	2	2	0	2	0	5	2	5	4	0	1	0	1	0	0	0
8	0	5	2	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0	0
4	9	5	13	2	2	1	3	0	5	2	16	4	3	0	0	0	0	0	0
6	3	5	8	2	2	3	2	0	2	2	16	4	3	0	0	0	0	0	0
1	6	2	11	1	4	1	2	5	1	2	4	4	0	1	0	0	0	0	0
8	6	2	23	1	2	2	3	1	1	2	2	4	2	1	1	0	0	0	0
8	5	0	17	2	3	3	2	4	3	0	12	1	0	1	1	0	0	1	0
1	1	14	13	1	3	3	2	5	5	2	16	4	3	0	0	0	0	0	0
1	2	5	7	1	2	0	2	2	5	0	6	1	0	1	1	0	0	1	0
1	0	0	9	2	1	6	2	5	0	0	2	0	1	1	1	1	0	1	0
1	3	8	21	1	2	6	3	5	2	0	5	2	0	1	1	0	0	0	0
8	7	5	5	2	3	3	2	4	4	0	10	4	0	1	0	1	0	0	0
8	3	9	1	2	3	3	2	4	2	2	16	4	3	0	0	0	0	0	0
1	1	0	23	1	2	3	1	0	5	2	3	0	0	1	1	0	0	1	0
1	6	0	16	2	3	3	2	2	1	0	11	1	0	1	1	0	0	1	0
8	5	4	11	2	1	3	3	4	3	0	2	3	0	1	1	0	0	1	0
8	5	14	20	1	3	3	3	5	3	0	7	1	0	1	1	1	0	0	0
8	6	0	17	2	2	3	2	4	1	0	4	1	1	1	1	0	0	0	0
8	5	0	11	2	3	0	2	4	3	0	3	0	0	1	1	0	1	1	0
8	5	0	19	1	2	3	1	4	3	0	5	1	0	1	1	0	1	1	0
1	3	13	1	2	3	3	2	5	2	0	6	1	0	1	0	1	1	0	0
8	3	0	26	1	8	4	1	1	5	2	3	4	0	0	0	0	0	0	0
8	5	2	13	1	1	1	2	1	3	2	16	4	3	0	0	0	0	0	0
1	5	0	24	1	3	3	1	5	5	0	7	1	0	1	1	0	0	1	0
8	5	2	6	1	1	1	1	1	5	1	4	0	1	1	1	0	0	0	0
8	3	5	14	1	3	3	2	5	2	2	16	4	3	0	0	0	0	0	0
1	5	2	8	1	3	3	2	5	3	0	2	4	1	1	1	1	0	0	0
8	0	9	8	2	9	6	3	3	0	0	6	4	0	1	1	1	0	1	0
1	3	0	18	2	4	1	2	5	2	0	8	1	0	1	0	0	0	0	0
1	7	6	7	1	2	3	2	5	4	0	2	4	0	1	1	0	0	1	0
1	5	0	10	2	1	2	2	2	3	2	16	4	3	0	0	0	0	0	0
8	3	0	12	2	2	3	2	4	2	0	3	1	0	1	1	0	0	1	0
8	5	3	5	2	2	1	2	4	3	0	1	1	1	1	1	0	0	1	0
1	3	0	13	2	3	3	2	0	2	0	5	1	0	1	1	0	0	0	0
1	3	10	14	2	3	3	2	5	2	0	10	1	0	1	1	0	0	1	0
8	3	6	16	2	2	3	3	5	2	0	2	3	0	1	1	1	0	0	0
8	5	2	5	1	4	6	1	1	5	0	16	0	1	0	0	0	0	0	0
8	5	9	8	2	3	3	2	5	3	0	4	2	0	1	1	0	0	1	0
1	3	0	20	1	3	3	1	5	2	0	8	1	0	1	1	0	0	1	0
1	3	10	13	2	1	3	3	5	2	0	11	0	2	1	1	0	1	1	0
1	0	0	9	2	3	0	2	5	0	2	3	4	0	0	0	0	0	0	0
1	6	3	7	2	4	1	2	2	1	0	7	3	0	1	0	0	0	0	0
1	3	0	14	2	2	3	2	0	2	0	5	3	2	1	1	1	0	0	0
8	5	0	20	1	2	3	1	5	3	2	1	1	1	1	1	0	0	0	0
8	3	0	12	2	3	3	2	4	2	0	6	1	0	1	1	0	1	1	0
8	3	0	12	2	3	3	2	5	2	0	4	2	0	1	1	0	0	1	0
1	6	2	9	2	2	3	3	2	1	2	16	4	3	0	0	0	0	0	0
8	0	0	9	2	3	6	2	3	0	2	16	4	3	0	0	0	0	0	0
1	7	0	16	2	2	3	2	5	4	0	0	0	0	1	0	0	0	0	0
8	3	12	9	1	2	3	2	5	2	2	16	4	3	0	0	0	0	0	0
1	3	0	20	1	3	2	1	5	2	0	3	1	0	1	1	1	0	1	0
1	7	0	31	1	3	3	1	5	5	0	6	1	0	1	1	0	1	1	0
1	8	14	13	2	3	3	2	5	5	2	16	4	3	0	0	0	0	0	0
1	7	0	20	2	3	3	2	2	5	2	16	4	3	0	0	0	0	0	0
8	5	6	3	1	2	3	1	1	5	2	16	4	3	0	0	0	0	0	0
1	5	2	9	1	4	3	2	5	3	0	3	4	1	1	1	0	1	0	0
8	6	2	10	2	2	1	3	4	1	2	16	4	3	0	0	0	0	0	0
1	5	0	19	2	2	1	2	5	3	0	6	0	0	1	0	1	0	0	0
1	3	0	17	2	3	3	2	5	2	2	0	4	0	0	0	0	0	0	0
8	0	0	9	2	2	6	2	3	0	2	16	4	3	0	0	0	0	0	0
8	9	0	17	2	2	3	2	4	5	0	9	1	0	1	1	1	0	0	0
1	7	8	18	1	2	2	3	2	4	0	1	0	0	0	1	0	0	1	0
1	7	0	13	2	3	3	2	0	5	0	5	1	0	1	1	1	0	1	0
1	8	4	10	1	4	7	2	5	5	2	16	4	3	0	0	0	0	0	0
8	7	0	12	2	2	5	2	1	4	0	5	0	0	1	0	0	0	0	0
1	0	0	8	2	2	6	2	5	0	2	16	4	3	0	0	0	0	0	0
8	6	5	2	2	2	1	2	1	1	0	3	0	0	0	1	0	0	1	0
1	5	0	17	2	4	3	2	0	3	0	10	4	0	1	1	0	0	1	0
1	3	0	18	2	1	2	2	2	2	0	5	1	0	1	1	0	0	0	0
8	5	0	16	2	2	3	2	4	3	0	2	1	1	0	0	0	0	0	0
1	7	3	18	0	3	3	2	5	5	0	13	4	0	1	1	1	0	1	0
1	5	0	18	2	2	6	2	2	3	0	2	1	1	0	1	0	0	0	0
1	7	14	1	1	8	6	1	5	5	2	16	4	3	0	0	0	0	0	0
8	3	0	8	2	3	3	2	5	2	0	5	4	0	1	1	0	1	1	0
1	8	9	7	1	2	3	2	2	5	0	11	2	0	1	0	1	1	0	0
1	11	2	7	1	3	1	2	2	5	0	5	1	0	1	0	0	0	0	0
8	3	1	12	1	6	3	2	1	2	0	7	1	0	1	1	1	0	0	0
1	7	2	10	1	5	6	1	2	5	2	16	4	3	0	0	0	0	0	0
8	7	0	23	1	4	3	1	1	4	0	15	1	0	1	1	1	1	1	0
8	5	2	8	2	1	1	3	5	3	2	16	4	3	0	0	0	0	0	0
1	5	2	9	2	4	3	2	2	3	0	11	1	0	1	1	1	1	1	0
1	3	0	12	2	1	1	2	2	2	0	6	0	2	1	1	0	0	0	0
8	6	0	13	2	2	3	2	4	1	2	16	4	3	0	0	0	0	0	0
8	3	0	18	2	1	3	2	5	5	0	3	0	1	1	1	1	1	0	0
1	3	0	12	2	1	1	2	5	2	2	16	4	3	0	0	0	0	0	0
1	3	14	21	1	3	3	1	0	2	0	7	1	0	1	1	1	0	1	0
1	8	0	15	2	4	6	2	2	5	0	7	1	0	1	1	0	0	1	0
1	5	10	20	1	2	3	3	5	3	0	3	0	0	1	1	1	0	0	0
8	7	0	9	2	2	5	2	4	4	2	16	4	3	0	0	0	0	0	0
0	3	0	8	2	2	1	2	5	2	0	7	4	0	1	1	0	0	1	0
1	0	14	8	2	4	6	3	5	0	0	9	1	0	1	0	1	0	0	0
8	3	0	7	2	1	0	2	4	5	0	15	1	0	1	1	1	0	1	0
8	9	2	8	2	3	2	3	4	5	0	6	0	0	1	1	1	0	1	0
1	3	0	11	2	5	3	2	0	2	0	11	4	0	1	1	0	1	1	0
8	7	2	13	1	5	3	2	1	5	2	16	4	3	0	0	0	0	0	0
1	3	0	20	1	2	2	1	0	2	0	10	1	0	1	1	1	0	1	0
1	3	13	6	2	2	2	3	5	2	2	16	4	3	0	0	0	0	0	0
8	3	0	20	1	6	3	1	5	2	0	15	1	0	1	1	0	0	0	0
8	0	5	1	2	3	6	2	3	0	1	0	3	1	0	1	0	0	0	0
8	7	2	13	1	2	3	3	4	4	2	9	3	0	0	0	0	0	0	0
8	5	0	17	2	1	3	2	4	3	2	16	4	3	0	0	0	0	0	0
3	7	2	3	2	1	1	2	5	4	2	16	4	3	0	0	0	0	0	0
1	5	2	11	1	1	3	2	2	5	0	5	0	0	1	1	0	0	1	0
8	5	14	25	1	3	3	3	1	5	0	7	1	0	1	1	0	0	0	0
1	7	7	18	2	3	1	3	2	4	2	16	4	3	0	0	0	0	0	0
8	0	11	7	2	9	6	2	3	0	2	16	4	3	0	0	0	0	0	0
8	3	0	15	2	4	3	2	5	2	0	7	1	0	1	1	0	1	1	0
8	6	6	2	2	3	1	2	4	1	2	16	4	3	0	0	0	0	0	0
8	3	0	8	2	3	3	2	4	2	0	8	4	0	0	0	0	0	0	0
1	7	14	14	2	3	3	3	0	5	2	1	1	0	0	0	1	0	0	0
1	7	7	14	2	3	4	3	2	4	2	16	4	3	0	0	0	0	0	0
8	3	0	16	2	4	3	2	4	2	0	6	1	0	1	1	1	1	1	0
8	5	2	20	1	3	3	3	1	5	2	16	4	3	0	0	0	0	0	0
1	6	0	10	2	3	3	2	2	1	0	3	1	0	1	1	0	1	1	0
1	7	2	4	2	2	2	3	2	4	0	3	1	0	1	0	1	0	0	0
8	5	0	15	2	2	3	2	4	3	0	4	4	0	1	1	1	1	1	0
1	3	0	13	2	1	1	2	5	2	2	16	4	3	0	0	0	0	0	0
8	6	2	4	2	1	1	2	4	1	2	16	4	3	0	0	0	0	0	0
1	7	14	2	1	6	3	1	2	5	0	11	1	0	1	1	1	0	1	0
1	5	0	11	2	4	2	2	5	3	2	16	4	3	0	0	0	0	0	0
1	6	9	2	1	1	3	1	5	1	0	4	0	0	1	1	0	0	1	0
1	2	2	10	1	2	0	2	2	5	0	15	2	0	0	1	0	0	0	0
8	0	0	8	2	3	6	2	5	0	2	16	4	3	0	0	0	0	0	0
1	11	0	8	2	2	3	2	5	5	0	1	1	0	1	0	1	0	0	0
1	0	0	13	2	1	1	2	0	5	2	16	4	3	0	0	0	0	0	0
8	6	2	7	2	3	1	2	5	1	2	2	1	1	0	0	0	0	0	0
8	6	6	13	2	4	1	3	4	1	0	15	1	0	1	1	1	1	1	0
1	0	0	9	2	1	6	2	5	0	2	16	0	0	0	1	0	0	1	0
1	3	0	17	2	3	3	2	0	2	0	6	1	0	1	1	1	1	0	0
8	6	2	7	2	3	1	2	1	1	0	15	4	1	1	1	0	0	1	0
1	3	10	21	1	3	1	2	2	2	0	8	1	0	0	0	0	0	0	0
1	3	0	11	2	5	1	2	2	2	0	3	1	0	1	0	1	0	0	0
1	9	0	13	2	2	3	2	2	5	0	5	1	0	1	1	0	0	1	0
1	11	3	24	0	3	3	2	2	5	0	7	1	0	1	1	0	0	0	0
8	0	9	9	2	1	0	3	3	0	2	16	4	3	0	0	0	0	0	0
3	3	5	7	1	1	3	2	2	2	1	2	1	0	1	1	0	1	1	0
1	3	0	8	2	4	2	2	5	2	0	10	1	0	1	1	0	1	1	0
1	5	0	15	2	2	3	2	5	3	0	5	4	0	1	1	1	0	1	0
1	6	0	20	2	2	3	2	0	1	0	8	1	0	1	1	1	0	1	0
1	1	11	15	1	1	1	2	5	5	0	16	0	0	1	0	0	0	0	0
1	7	2	8	1	2	5	1	2	5	2	1	4	2	0	0	0	0	0	0
0	0
1	0
2	0
3	0
4	0
5	0
6	0
7	0
8	0
9	0
10	0
11	0
12	0
13	0
14	0
15	0
16	0
17	0
18	1
19	1
20	1
21	1
22	1
23	1
24	1
25	1
26	0
27	1
28	1
29	0
30	1
31	1
32	1
33	1
34	1
35	1
36	1
37	1
38	1
39	1
40	1
41	1
42	1
43	1
44	1
45	1
46	1
47	1
48	1
49	1
50	1
51	1
52	1
53	1
54	1
55	0
56	0
57	1
58	0
59	0
60	0
61	1
62	0
63	1
64	1
65	0
66	0
67	0
68	1
69	1
70	1
71	0
72	1
73	1
74	1
75	1
76	1
77	1
78	1
79	1
80	1
81	1
82	1
83	1
84	1
85	0
86	0
87	1
88	1
89	1
90	0
91	0
92	0
93	0
94	0
95	0
96	0
97	1
98	0
99	1
100	0
101	1
102	1
103	0
104	1
105	0
106	0
107	1
108	1
109	1
110	1
111	1
112	1
113	1
114	1
115	1
116	1
117	1
118	0
119	1
120	1
121	1
122	1
123	0
124	0
125	0
126	0
127	0
128	1
129	0
130	1
131	0
132	0
133	0
134	1
135	1
136	0
137	0
138	0
139	1
140	1
141	1
142	1
143	1
144	1
145	1
146	1
147	1
148	1
149	1
150	1
151	1
152	0
153	1
154	0
155	0
156	0
157	0
158	0
159	1
160	0
161	1
162	0
163	0
164	1
165	0
166	1
167	0
168	0
169	0
170	1
171	1
172	1
173	1
174	1
175	1
176	1
177	1
178	1
179	1
180	1
181	1
182	1
183	1
184	0
185	0
186	1
187	1
188	1
189	0
190	1
191	0
192	0
193	0
194	1
195	0
196	0
197	0
198	0
199	0
200	0
201	1
202	0
203	0
204	1
205	1
206	0
207	1
208	1
209	1
210	1
211	1
212	1
213	1
214	0
215	1
216	1
217	1
218	1
219	1
220	1
221	1
222	0
223	0
224	0
225	0
226	0
227	0
228	1
229	1
230	0
231	1
232	1
233	1
234	0
235	0
236	0
237	0
238	1
239	0
240	1
241	1
242	1
243	1
244	1
245	1
246	1
247	1
248	1
249	1
250	1
251	1
252	1
253	1
254	1
255	1
256	1
257	0
258	1
259	1
260	1
261	0
262	1
263	0
264	1
265	0
266	1
267	1
268	1
269	0
270	1
271	1
272	1
273	1
274	1
275	1
276	1
277	1
278	1
279	1
280	0
281	1
282	1
283	1
284	1
285	1
286	1
287	1
288	1
289	1
290	0
291	0
292	1
293	0
294	0
295	1
296	1
297	1
298	1
299	1
300	1
301	1
302	1
303	1
304	1
305	1
306	1
307	1
308	1
309	1
310	1
311	1
312	1
313	1
314	1
315	1
316	1
317	1
318	1
319	1
320	1
321	0
322	1
323	0
324	1
325	1
326	1
327	0
328	0
329	0
330	1
331	1
332	1
333	1
334	1
335	1
336	1
337	1
338	1
339	1
340	1
341	1
342	1
343	1
344	1
345	1
346	1
347	1
348	1
349	1
350	1
351	0
352	1
353	1
354	1
355	1
356	1
357	1
358	1
359	0
360	1
361	1
362	1
363	1
364	1
365	0
366	1
367	1
368	1
369	1
370	1
371	1
372	1
373	1
374	1
375	1
376	1
377	1
378	1
379	1
380	1
381	1
382	1
383	1
384	1
385	1
386	1
387	0
388	1
389	1
390	0
391	1
392	1
393	1
394	1
395	1
396	1
397	1
398	0
399	1
400	1
401	1
402	1
403	1
404	1
405	1
406	1
407	1
408	1
409	1
410	1
411	1
412	1
413	1
414	1
415	1
416	1
417	1
418	1
419	1
420	1
421	1
422	1
423	1
424	1
425	1
426	0
427	1
428	1
429	1
430	1
431	1
432	1
433	1
434	1
435	1
436	1
437	1
438	1
439	1
440	1
441	1
442	1
443	1
444	1
445	1
446	1
447	1
448	1
449	1
450	1
451	1
452	1
453	1
454	1
455	1
456	1
457	0
458	0
459	1
460	1
461	1
462	1
463	1
464	1
465	1
466	0
467	0
468	1
469	1
470	1
471	1
472	1
473	1
474	1
475	1
476	1
477	1
478	1
479	1
480	1
481	1
482	1
483	1
484	1
485	1
486	1
487	1
488	1
489	1
490	1
491	1
492	1
493	1
494	1
495	0
496	1
497	1
498	1
499	1
500	1
501	1
502	1
503	1
504	1
505	1
506	1
507	1
508	1
509	1
510	1
511	1
512	1
513	1
514	1
515	1
516	1
517	1
518	0
519	1
520	1
521	0
522	0
523	0
524	0
525	0
526	0
527	1
528	0
529	0
530	0
531	1
532	0
533	0
534	1
535	1
536	1
537	1
538	1
539	0
540	1
541	1
542	1
543	1
544	1
545	1
546	0
547	0
548	1
549	1
550	0
551	1
552	1
553	1
554	1
555	1
556	1
557	1
558	1
559	1
560	1
561	1
562	1
563	1
564	1
565	1
566	1
567	1
568	1
569	1
570	1
571	1
572	1
573	1
574	1
575	1
576	1
577	1
578	1
579	1
580	1
581	1
582	1
583	1
584	1
585	1
586	1
587	1
588	1
589	1
590	1
591	1
592	1
593	1
594	1
595	1
596	1
597	1
598	1
599	1
600	1
601	1
602	1
603	1
604	1
605	1
606	1
607	1
608	1
609	1
610	1
611	1
612	1
613	1
614	1
615	1
616	1
617	1
618	1
619	1
620	1
621	1
622	1
623	1
624	1
625	1
626	1
627	1
628	1
629	1
630	1
631	1
632	1
633	1
634	1
635	1
636	1
637	1
638	1
639	1
640	1
641	1
642	1
643	1
644	1
645	1
646	1
647	1
648	1
649	1
650	1
651	1
652	1
653	1
654	1
655	1
656	1
657	1
658	1
659	1
660	1
661	1
662	1
663	1
664	1
665	1
666	1
667	1
668	1
669	1
670	1
671	1
672	1
673	1
674	1
675	1
676	1
677	1
678	1
679	1
680	1
681	1
682	1
683	1
684	1
685	1
686	1
687	1
688	1
689	1
690	1
691	1
692	1
693	1
694	1
695	1
696	1
697	1
698	1
699	1
700	1
701	1
702	1
703	1
704	1
705	1
706	1
707	1
708	1
709	1
710	1
711	1
712	1
713	1
714	1
715	1
716	1
717	1
718	1
719	1
720	1
721	1
722	1
723	1
724	1
725	1
726	1
727	1
728	1
729	1
730	1
731	1
732	1
733	1
734	1
735	1
736	1
737	1
738	1
739	1
740	1
741	1
742	1
743	1
744	1
745	1
746	1
747	1
748	1
749	1
750	1
751	1
752	1
753	1
754	1
755	1
756	1
757	1
758	1
759	1
760	1
761	1
762	1
763	1
764	1
765	1
766	1
767	1
768	1
769	1
770	1
771	1
772	1
773	1
774	1
775	1
776	1
777	1
778	1
779	1
780	1
781	1
782	0
783	1
784	1
785	1
786	1
787	1
788	1
789	1
790	1
791	1
792	1
793	1
794	1
795	1
796	1
797	1
798	1
799	1
800	1
801	1
802	1
803	1
804	1
805	1
806	1
807	1
808	1
809	1
810	1
811	1
812	1
813	1
814	1
815	1
816	1
817	1
818	1
819	1
820	1
821	1
822	1
823	1
824	1
825	1
826	1
827	1
828	1
829	1
830	1
831	1
832	1
833	1
834	1
835	1
836	1
837	1
838	1
839	1
840	1
841	1
842	1
843	1
844	1
845	1
846	1
847	1
848	1
849	1
850	1
851	1
852	1
853	1
854	1
855	1
856	1
857	1
858	1
859	1
860	1
861	1
862	1
863	1
864	1
865	1
866	1
867	1
868	1
869	1
870	1
871	1
872	1
873	1
874	1
875	1
876	1
877	1
878	1
879	1
880	1
881	1
882	1
883	1
884	1
885	1
886	1
887	1
888	1
889	1
890	1
891	1
892	1
893	1
894	1
895	1
896	1
897	1
898	1
899	1
900	1
901	1
902	1
903	1
904	1
905	1
906	1
907	1
908	1
909	1
910	1
911	1
912	1
913	1
914	1
915	1
916	1
917	1
918	1
919	1
920	1
921	1
922	1
923	1
924	1
925	1
926	1
927	0
928	0
929	1
930	1
931	1
932	1
933	1
934	1
935	1
936	1
937	1
938	1
939	1
940	1
941	1
942	1
943	1
944	1
945	1
946	1
947	1
948	1
949	1
950	1
951	1
952	1
953	1
954	1
955	1
956	0
957	1
958	1
959	1
960	1
961	1
962	1
963	1
964	1
965	1
966	1
967	1
968	1
969	1
970	1
971	1
972	1
973	1
974	1
975	1
976	1
977	1
978	1
979	1
980	1
981	1
982	1
983	1
984	1
985	1
986	1
987	1
988	1
989	1
990	1
991	1
992	1
993	1
994	1
995	1
996	1
997	1
998	1
999	1
1000	1
1001	1
1002	1
1003	1
1004	1
1005	1
1006	1
1007	1
1008	1
1009	1
1010	1
1011	1
1012	1
1013	1
1014	1
1015	1
1016	1
1017	1
1018	1
1019	1
1020	1
1021	1
1022	1
1023	1
1024	1
1025	1
1026	1
1027	1
1028	1
1029	1
1030	1
1031	1
1032	1
1033	1
1034	1
1035	1
1036	1
1037	1
1038	1
1039	1
1040	1
1041	1
1042	1
1043	1
1044	1
1045	1
1046	1
1047	1
1048	1
1049	1
1050	1
1051	1
1052	1
1053	1
1054	1
1055	1
1056	1
1057	1
1058	1
1059	1
1060	1
1061	1
1062	1
1063	1
1064	1
1065	1
1066	1
1067	1
1068	1
1069	1
1070	1
1071	1
1072	1
1073	1
1074	1
1075	1
1076	1
1077	1
1078	1
1079	1
1080	1
1081	1
1082	1
1083	1
1084	1
1085	1
1086	1
1087	1
1088	1
1089	1
1090	1
1091	1
1092	1
1093	1
1094	1
1095	1
1096	1
1097	1
1098	1
1099	1
1100	1
1101	1
1102	1
1103	1
1104	1
1105	1
1106	1
1107	0
1108	1
1109	1
1110	1
1111	1
1112	1
1113	1
1114	1
1115	1
1116	1
1117	1
1118	1
1119	1
1120	1
1121	1
1122	1
1123	1
1124	1
1125	1
1126	1
1127	1
1128	1
1129	1
1130	1
1131	1
1132	1
1133	1
1134	1
1135	1
1136	1
1137	1
1138	1
1139	1
1140	1
1141	1
1142	1
1143	1
1144	1
1145	1
1146	1
1147	1
1148	1
1149	1
1150	1
1151	1
1152	1
1153	1
1154	1
1155	1
1156	1
1157	1
1158	1
1159	1
1160	1
1161	1
1162	1
1163	1
1164	1
1165	1
1166	1
1167	1
1168	1
1169	1
1170	1
1171	1
1172	1
1173	1
1174	1
1175	1
1176	1
1177	1
1178	1
1179	1
1180	1
1181	1
1182	1
1183	1
1184	1
1185	1
1186	1
1187	1
1188	1
1189	1
1190	1
1191	1
1192	1
1193	1
1194	1
1195	1
1196	1
1197	1
1198	1
1199	1
1200	1
1201	1
1202	1
1203	1
1204	1
1205	1
1206	1
1207	1
1208	1
1209	1
1210	1
1211	1
1212	1
1213	1
1214	1
1215	1
1216	1
1217	1
1218	1
1219	1
1220	1
1221	1
1222	1
1223	0
1224	1
1225	1
1226	1
1227	1
1228	1
1229	1
1230	1
1231	1
1232	1
1233	1
1234	1
1235	1
1236	1
1237	1
1238	1
1239	1
1240	1
1241	1
1242	1
1243	1
1244	1
1245	1
1246	1
1247	1
1248	1
1249	1
1250	1
1251	1
1252	1
1253	1
1254	1
1255	1
1256	1
1257	1
1258	1
1259	1
1260	1
1261	1
1262	1
1263	1
1264	1
1265	1
1266	1
1267	0
1268	1
1269	1
1270	1
1271	1
1272	1
1273	1
1274	1
1275	1
1276	1
1277	1
1278	1
1279	1
1280	1
1281	1
1282	1
1283	0
1284	1
1285	1
1286	1
1287	1
1288	1
1289	1
1290	1
1291	1
1292	1
1293	1
1294	1
1295	1
1296	1
1297	1
1298	1
1299	1
1300	1
1301	1
1302	1
1303	1
1304	1
1305	1
1306	1
1307	1
1308	1
1309	1
1310	1
1311	1
1312	1
1313	1
1314	1
1315	1
1316	1
1317	1
1318	1
1319	1
1320	1
1321	1
1322	1
1323	1
1324	1
1325	1
1326	1
1327	1
1328	1
1329	1
1330	1
1331	1
1332	1
1333	1
1334	1
1335	1
1336	1
1337	1
1338	1
1339	1
1340	1
1341	1
1342	1
1343	1
1344	0
1345	1
1346	1
1347	1
1348	1
1349	1
1350	1
1351	1
1352	1
1353	1
1354	1
1355	1
1356	1
1357	1
1358	1
1359	1
1360	1
1361	1
1362	1
1363	1
1364	1
1365	1
1366	1
1367	0
1368	1
1369	1
1370	1
1371	1
1372	1
1373	1
1374	1
1375	1
1376	1
1377	1
1378	1
1379	1
1380	1
1381	1
1382	1
1383	1
1384	1
1385	1
1386	1
1387	1
1388	1
1389	1
1390	1
1391	1
1392	1
1393	1
1394	1
1395	1
1396	1
1397	1
1398	1
1399	1
1400	1
1401	1
1402	1
1403	1
1404	1
1405	1
1406	1
1407	1
1408	1
1409	1
1410	1
1411	1
1412	1
1413	1
1414	1
1415	1
1416	1
1417	1
1418	1
1419	1
1420	1
1421	1
1422	1
1423	1
1424	1
1425	1
1426	1
1427	1
1428	1
1429	1
1430	1
1431	1
1432	0
1433	1
1434	1
1435	1
1436	1
1437	1
1438	1
1439	1
1440	1
1441	1
1442	1
1443	1
1444	1
1445	1
1446	1
1447	1
1448	1
1449	1
1450	1
1451	1
1452	1
1453	1
1454	1
1455	1
1456	1
1457	1
1458	1
1459	1
1460	1
1461	1
1462	1
1463	1
1464	1
1465	1
1466	1
1467	1
1468	1
1469	1
1470	1
1471	1
1472	1
1473	1
1474	1
1475	1
1476	1
1477	1
1478	1
1479	1
1480	1
1481	1
1482	1
1483	1
1484	1
1485	1
1486	1
1487	1
1488	1
1489	1
1490	1
1491	1
1492	1
1493	1
1494	1
1495	1
1496	1
1497	1
1498	1
1499	1
1500	1
1501	1
1502	1
1503	1
1504	1
1505	1
1506	1
1507	1
1508	1
1509	1
1510	1
1511	1
1512	1
1513	1
1514	1
1515	1
1516	1
1517	1
1518	1
1519	1
1520	1
1521	1
1522	1
1523	1
1524	1
1525	1
1526	1
1527	1
1528	1
1529	1
1530	1
1531	1
1532	1
1533	1
1534	1
1535	1
1536	1
1537	1
1538	1
1539	1
1540	1
1541	1
1542	0
1543	1
1544	1
1545	1
1546	1
1547	1
1548	1
1549	1
1550	1
1551	1
1552	1
1553	1
1554	1
1555	1
1556	1
1557	1
1558	1
1559	1
1560	1
1561	1
1562	1
1563	1
1564	1
1565	1
1566	1
1567	1
1568	1
1569	1
1570	1
1571	1
1572	1
1573	1
1574	1
1575	1
1576	1
1577	1
1578	1
1579	1
1580	1
1581	1
1582	1
1583	1
1584	1
1585	1
1586	1
1587	1
1588	1
1589	1
1590	1
1591	1
1592	1
1593	1
1594	1
1595	1
1596	1
1597	1
1598	1
1599	1
1600	1
1601	1
1602	1
1603	1
1604	1
1605	1
1606	1
1607	1
1608	1
1609	1
1610	1
1611	1
1612	1
1613	1
1614	1
1615	1
1616	1
1617	1
1618	1
1619	1
1620	1
1621	1
1622	1
1623	1
1624	1
1625	1
1626	1
1627	1
1628	1
1629	1
1630	1
1631	1
1632	1
1633	1
1634	1
1635	1
1636	1
1637	1
1638	1
1639	1
1640	1
1641	1
1642	1
1643	1
1644	1
1645	1
1646	1
1647	1
1648	1
1649	1
1650	1
1651	1
1652	1
1653	1
1654	1
1655	1
1656	1
1657	1
1658	1
1659	1
1660	1
1661	1
1662	1
1663	1
1664	1
1665	1
1666	1
1667	1
1668	1
1669	1
1670	1
1671	1
1672	1
1673	1
1674	1
1675	1
1676	1
1677	1
1678	1
1679	1
1680	1
1681	1
1682	1
1683	1
1684	1
1685	1
1686	1
1687	1
1688	1
1689	1
1690	1
1691	1
1692	1
1693	1
1694	1
1695	1
1696	1
1697	1
1698	1
1699	1
1700	1
1701	1
1702	1
1703	1
1704	1
1705	1
1706	1
1707	1
1708	1
1709	1
1710	1
1711	1
1712	1
1713	1
1714	1
1715	1
1716	1
1717	1
1718	1
1719	1
1720	1
1721	1
1722	1
1723	1
1724	1
1725	1
1726	1
1727	1
1728	1
1729	1
1730	1
1731	1
1732	1
1733	1
1734	1
1735	1
1736	1
1737	1
1738	1
1739	1
1740	1
1741	1
1742	1
1743	1
1744	1
1745	1
1746	1
1747	1
1748	1
1749	1
1750	1
1751	0
1752	1
1753	1
1754	1
1755	1
1756	1
1757	1
1758	1
1759	1
1760	1
1761	1
1762	1
1763	1
1764	1
1765	1
1766	1
1767	1
1768	1
1769	1
1770	1
1771	1
1772	1
1773	1
1774	1
1775	1
1776	1
1777	1
1778	1
1779	1
1780	1
1781	1
1782	1
1783	1
1784	1
1785	1
1786	1
1787	1
1788	1
1789	1
1790	1
1791	1
1792	1
1793	1
1794	1
1795	1
1796	1
1797	1
1798	1
1799	1
1800	1
1801	1
1802	1
1803	1
1804	1
1805	1
1806	1
1807	1
1808	1
1809	1
1810	1
1811	1
1812	1
1813	1
1814	1
1815	1
1816	1
1817	1
1818	1
1819	1
1820	1
1821	1
1822	1
1823	1
1824	1
1825	1
1826	1
1827	1
1828	1
1829	1
1830	1
1831	1
1832	1
1833	1
1834	1
1835	1
1836	1
1837	1
1838	1
1839	1
1840	1
1841	1
1842	1
1843	1
1844	1
1845	1
1846	1
1847	1
1848	1
1849	1
1850	1
1851	1
1852	1
1853	1
1854	1
1855	1
1856	1
1857	1
1858	1
1859	1
1860	1
1861	1
1862	1
1863	1
1864	1
1865	1
1866	1
1867	1
1868	1
1869	1
1870	1
1871	1
1872	1
1873	1
1874	1
1875	1
1876	1
1877	1
1878	1
1879	1
1880	1
1881	1
1882	1
1883	1
1884	1
1885	1
1886	1
1887	1
1888	1
1889	1
1890	1
1891	1
1892	1
1893	1
1894	1
1895	1
1896	1
1897	1
1898	1
1899	1
1900	1
1901	1
1902	1
1903	1
1904	1
1905	1
1906	1
1907	1
1908	1
1909	1
1910	1
1911	1
1912	1
1913	1
1914	1
1915	1
1916	1
1917	1
1918	1
1919	1
1920	1
1921	1
1922	1
1923	1
1924	1
1925	1
1926	1
1927	1
1928	1
1929	1
1930	1
1931	1
1932	1
1933	1
1934	1
1935	1
1936	1
1937	1
1938	1
1939	1
1940	1
1941	1
1942	1
1943	1
1944	1
1945	1
1946	1
1947	1
1948	1
1949	1
1950	1
1951	1
1952	1
1953	1
1954	1
1955	1
1956	1
1957	1
1958	1
1959	1
1960	1
1961	1
1962	1
1963	1
1964	1
1965	1
1966	1
1967	1
1968	1
1969	1
1970	1
1971	1
1972	1
1973	1
1974	1
1975	1
1976	1
1977	1
1978	1
1979	1
1980	1
1981	1
1982	1
1983	1
1984	1
1985	1
1986	1
1987	1
1988	1
1989	1
1990	1
1991	1
1992	1
1993	1
1994	1
1995	1
1996	1
1997	1
1998	1
1999	1
2000	1
2001	1
2002	1
2003	1
2004	1
2005	1
2006	1
2007	1
2008	1
2009	1
2010	1
2011	1
2012	1
2013	1
2014	1
2015	1
2016	1
2017	1
2018	1
2019	1
2020	1
2021	1
2022	1
2023	1
2024	1
2025	1
2026	1
2027	1
2028	1
2029	1
2030	1
2031	1
2032	1
2033	1
2034	1
2035	1
2036	1
2037	1
2038	1
2039	1
2040	1
2041	1
2042	1
2043	1
2044	1
2045	1
2046	1
2047	1
2048	1
2049	1
2050	1
2051	1
2052	0
2053	0
2054	1
2055	1
2056	1
2057	1
2058	1
2059	1
2060	1
2061	1
2062	1
2063	1
2064	1
2065	1
2066	1
2067	1
2068	1
2069	1
2070	1
2071	1
2072	1
2073	1
2074	1
2075	1
2076	1
2077	1
2078	1
2079	1
2080	1
2081	1
2082	1
2083	1
2084	1
2085	1
2086	1
2087	1
2088	1
2089	1
2090	1
2091	1
2092	1
2093	1
2094	1
2095	1
2096	1
2097	1
2098	1
2099	1
2100	1
2101	1
2102	1
2103	1
2104	1
2105	1
2106	1
2107	1
2108	1
2109	1
2110	1
2111	1
2112	1
2113	1
2114	1
2115	1
2116	1
2117	1
2118	1
2119	1
2120	1
2121	1
2122	1
2123	1
2124	1
2125	1
2126	1
2127	1
2128	1
2129	1
2130	1
2131	1
2132	1
2133	1
2134	1
2135	1
2136	1
2137	1
2138	1
2139	1
2140	1
2141	1
2142	1
2143	1
2144	1
2145	1
2146	1
2147	1
2148	1
2149	1
2150	1
2151	1
2152	1
2153	1
2154	1
2155	1
2156	1
2157	1
2158	1
2159	1
2160	1
2161	1
2162	1
2163	1
2164	1
2165	1
2166	1
2167	0
2168	1
2169	1
2170	1
2171	1
2172	1
2173	1
2174	1
2175	1
2176	1
2177	1
2178	1
2179	1
2180	1
2181	1
2182	1
2183	1
2184	1
2185	1
2186	1
2187	1
2188	1
2189	1
2190	1
2191	1
2192	1
2193	1
2194	1
2195	1
2196	1
2197	1
2198	1
2199	1
2200	1
2201	1
2202	1
2203	1
2204	1
2205	1
2206	1
2207	1
2208	1
2209	1
2210	1
2211	1
2212	1
2213	1
2214	1
2215	1
2216	1
2217	1
2218	1
2219	1
2220	1
2221	1
2222	1
2223	1
2224	1
2225	1
2226	1
-1	-1

  �C ��C  $B����  5D  )C  �C���� �pD  �C  �C����  C @0D  �C���� @pD �ND ��C���� �kD �xD ��C���� ��C  �C �ND����  C �aD  D���� ��C  �C  �C���� �_D �3D  3D���� �@D �5D ��C����  dD @YD �D����  B ��C �&D���� �JD �/D �_D���� @(D ��C  �C���� �1D  
D ��C  -D���� ��C @	D  �C����  pB  C   D����  C @<D �3D����  yD ��C ��C���� @	D @D �+D���� @mD �XD ��C���� ��C  CC  AC����  �B  �C  �C���� ��C  �C  �C���� @/D  �B �7D���� �DD �'D  �C���� ��C ��C ��C����  dD  TB  9C���� @lD  PD  JD���� ��C  =D ��C���� �uD �!D �D����  DC  C ��C����  !C  C  ^C����  -C  �C �D���� �$D  0B  �C����  }C ��C  �C����  �C  C  �A����  :C @"D @:D����  �A  �C ��C���� �DD  �A  (C���� ��C �oD @bD���� ��C @D  JC����  ND  �C  �C����  �C �ZD  �C����  �B @PD  C���� �D ��C ��C����  �C @AD  1C����  @C  'D �>D����  �C  �B @vD���� �HD �D  UC���� @D �D  �B����  �A  gD �D����  5C  tB  sD���� ��C  �C  @@����  �A @5D @D����  ^C  C  �C����  aD  �B  �C���� ��C  �C  ;C����  *C  �C �sD���� �1D @PD  kC���� �&D  `D  >D����  C �	D ��C����  D �-D �-D����  D  �B  �A����  �C �$D �-D���� �)D  �C ��C����  �C �TD @D����  �B  �C  �A����  �B  HD  C����   A �gD  pB���� ��C  *C @=D����  ,B  cC  D����  �B  $C �=D���� @
D  WC  �B����  B  �C �KD����  �B  �C  TC���� �1D  �B �nD����  �C �AD  �C���� @D �mD �KD����  �B �)D  �B����  ED  �B �`D����  oD  C @tD����   A  �B @,D���� �4D �ED @kD����  8C  �A ��C����  �B  HC �BD����  �B �sD @]D����  C �XD ��C���� @ND  (C ��C���� �"D @D  )D���� @-D ��C �D����  �B  �B �+D����  D �&D  yC���� ��C  XD �)D����  LD �ND ��C���� �&D �5D  D����  qD @*D  �B����  �A �+D  �C����  �B @4D �%D���� �aD ��C ��C���� @kD  �B @ZD���� ��C �>D @:D����  �C �ID �D���� �bD �D  �C����  @A ��C ��C����  �B �D  C����  D  �B @JD����  yD ��C  �C���� ��C  XB  C���� ��C  xC  1D����  �B  C  2D���� �4D �oD ��C����  PB �xD  0D����  nC ��C ��C���� �XD �eD ��C����  �C  �C �$D���� @D  �C  DC����  C  �A  C���� @gD  B  �C���� �D  �C �!D���� �OD  �B �ID����  �C  D  �C����  �C  �C �GD���� ��C  D �\D���� �
D �cD �BD���� ��C ��C  6C����  �C �ZD  ?D����  �C  C �aD���� @MD  sD  �C���� @D �D  �B����  CC @D @ED���� ��C �mD �D����  @A ��C �<D����  �C ��C  (D���� �qD �>D  dD����  cD ��C  �C����  �C �=D  _C���� ��C  �C  LC���� �ID @lD �oD���� ��C  /D  &C����  D �DD  -D���� ��C �sD  D����  ^C ��C  �A���� �D  �C ��C���� �uD �4D  mD����  \C  BD  �A����  VD  6C  �B����  �C @gD  �C���� @rD @)D  �C���� @)D �D � D���� �wD  �B  'D����  OD �CD  DC���� �YD  �C �5D���� �/D  D @.D���� @D  �C  �A����  nC  9C  �C���� ��C ��C  mC����   C �TD ��C����  C �"D  �B���� @YD  �C  @C���� ��C ��C @*D���� �D @FD @2D���� �OD  sD  �C����  1C  �C @aD���� �/D �@D @[D���� �D ��C  �C����  �A  mC �MD����  )C �D  3C����  �C  (B ��C���� �gD  hB  6C����  D �'D ��C����  UC  �C �"D����  �C �CD  WD����  �C �D �9D���� @RD  �C   C���� �XD ��C �3D���� @\D �jD @XD���� �D �;D ��C���� @%D  �C �D���� �ID  =D  |B���� ��C ��C  �C���� �D  �B ��C����  �C @PD  �C����  hD  @B  �C����  RD ��C  �C���� �JD �7D �?D����  _D @QD �D����  �C  �C �uD���� �FD  tC ��C���� �!D  |C  D����  PC �D  �C����  sC ��C  �C���� @RD  pA @"D����  KD  �B  =C���� �TD �D  "D���� �^D @D  �C���� �wD �UD  HC����  CD �eD @.D����  D �hD �D���� �`D ��C  �C���� �D ��C @D����  |C  AC  �C���� �PD  D  0A���� @DD ��C  RD���� @ID  D @D����  RC  �C �ED���� �tD �D @	D����  �B ��C �rD���� ��C  D  AC���� �ED  �C �ND����  C �D �D����  C ��C  �C����  �B  gD ��C���� �qD   A  �C���� ��C ��C  PC����  DC �D  �C���� �D �4D  �B����  `B @>D  �A���� �*D  #C �^D����  �C  iD  3D���� @KD �nD @9D����  �C  �C �WD���� �D  bD �D����  FC  *D  C����  D ��C   B����  �@ @D  pC���� �\D  qC �'D���� �=D �D  yC����  �B @D  �B���� �)D  eD  �C����  hB �D  2C����  �@ �;D @D���� �xD  �B @6D���� @<D  )D �)D���� @
D @D  C���� �6D  C  �B����  DB  SD  tC���� ��C  �B  �B����  �C @AD  
D���� ��C �:D �6D����  �C  �C @YD���� �)D  �C  �B���� �D  ,B  TB���� �PD �D  �B����  pC  �A �TD���� �"D  �C �cD����  D ��C  D����  \C  2C  [D���� �GD  �C �D���� @VD �D  3C���� �aD �%D �\D���� �@D  �C  �C���� @RD ��C  �A���� @eD  ^D �+D���� �.D  _D @3D����  �B @;D ��C���� �D  iD ��C����  C  @@  �B���� @D �D  �A����  ,C �D  C���� ��C ��C  XD����  �C @*D  �C����  �B  1C �D���� � D ��C  lC����  B @D  RD����  �C �D @_D����  KC �CD @xD���� �%D  �C ��C����  C @yD ��C���� @3D �^D ��C���� �
C �tD  �C���� ��C �D  /D����  D  MC  �B���� �D ��C  OD����  �B  >D  �C���� �D ��C �iD����  D ��C �
D���� �=D  <D  >D����  �B  C ��C����  B ��C �5D����  	D ��C �=D����  AC  FD ��C����  �B  	C @iD���� �-D  SC �,D����  �C �D  'D���� �[D  xC @wD���� �hD  C �!D���� �D �ZD @'D���� �D  �C ��C���� �6D ��C  �B����  �C @WD  C����  LD �qD ��C���� �XD  0D ��C����  _C  �C @nD���� �&D  FD  +D���� �hD ��C  @B����  �B �KD �JD����  D �9D ��C����  CC  �C �D���� @D  C  �C���� @^D @JD �VD���� �LD   B ��C����  �A �sD  �C����  �B ��C  �B���� �D  RD  �C���� @ D ��C  )D����  �B �"D �vD����  �B  �B  ;C���� �+D  zC  �C����   C ��C �<D���� �WD ��C  �C����  ZD @
C �jD  �C���� �%D  �C @4D����  5D �D @)D���� @pD  /D @uD����  �C   @ �WD����  �A  +D  �B���� @xD ��C  	C���� ��C  lD  �C���� � D  hB @MD����  �B @)D  'D���� �^D  $B @VD���� �dD �D  �C���� @RD @hD  �B����  D �D �2D����  /D �=D @oD����  qC  �B  �C����  cC  �C @OD����  �C  `C �GD���� @]D  C  �C���� �WD  *C  B���� @cD  �B @D����  �B �fD �[D����  AC  �A  �C���� @?D  �C  �B���� @)D ��C �?D���� �ED  0D  DD���� @D �^D �PD����  >C �7D  �B����  �B �yD �D����  �C @D  �A����  ~C  �C @D���� �gD  �B @'D����  �C  �B ��C����  D  �C  �B����  �C  �C �LD����  D ��C  vD����  �C  D  JD����  oC �DD �KD���� �ND  �A  �B���� ��C  wD �
D����   @ � D �GD���� �lD  �B ��C���� @PD ��C  `B���� @D @>D  1D���� ��C  ID �+D����  �C �RD  C���� ��C  D  !C����  �B  ;C �D���� �iD �D  |B���� @>D  $D ��C���� �oD ��C  @A����  �C �8D  {C����  �@ ��C  �B����  �B �mD  D���� ��C �oD ��C����  �C  PA �ED����  �B  pB �UD����  )D  �C  �C����  �C ��C �D����  D ��C �"D���� ��C @>D  D����  �B @6D ��C���� @nD ��C  _D���� �D �D  �C����  �C ��C  �C���� ��C  �C ��C���� �D ��C  �C���� ��C  �C  B���� �lD �#D  +C����  eC �D � D���� �]D �?D ��C����  �C  D  =D���� �D �&D �8D���� @
D   C ��C���� ��C  �B @0D���� @D �+D  �C����  4B ��C  (B����  �C  �C  �C���� @PD  �C  C���� �D �eD  >D����  BD ��C �8D����  �C  �C  �B���� ��C  �@  TB���� �RD �4D  XC����  �C ��C  �@���� �D @FD �lD����  �C  ,B @3D����  �B �D  �C����  +C  D  �C����  C  qD  FD����  &D @-D �XD����  TC     �AD����  �C �	D  �B���� @[D �kD ��C���� ��C ��C @ED����  C  �C  �C����  �C �D @D����  �C  ^D �lD����  �B  �C  C����  �C  �B  �C����  �C @uD @QD���� �D  �C �D���� ��C  �B  PD����  �C ��C �wD���� ��C ��C  �C����  �B  3C  �C���� �D �dD ��C���� �XD  jD ��C���� @cD ��C  �A���� ��C @uD @sD���� �\D  LD @D����  �C ��C ��C���� @*D  C  fC���� �XD  �C �
D    ���� ��C  �C  �C���� �iD   D �D���� �PD �vD   B���� @?D �D  xC����  �C  �C  GD���� �vD �6D  PB����  xC @D  �C����  �C  *C  D���� ��C ��C  6C����  0B  �C  �B����  �@  �B ��C����  �B  �A  HB���� �D @HD �8D����  �C ��C �RD���� �$D �eD �-D���� ��C  D ��C���� @D �UD  �B���� �&D @D �tD���� @QD @D �)D����  �C  �B  �C����  �C  ZD �4D����  mD  C ��C����  �C  EC ��C���� @xD ��C  cD����  JC @CD @&D����  �C  OD �D����  �A @qD @0D����  }C �2D @@D����  �C  +D @ID���� @PD  RD  C����  rD  aD ��C���� @]D @D �xD����   B ��C  �C����  ZD �cD  �C����  �C @aD �uD���� ��C  WD �D���� ��C  	C �D���� �xD  iC  @D���� �kD �D  +C����  B �[D  0A���� �D @D   D����  �C  jC  !C����  ?C �bD �gD����  cC  <C ��C����  `C  �C �3D����  `C ��C  �B���� �XD �D  �B����  C  �@ �D����  D @D �+D���� �=D �TD  mC���� @)D  oD �D����  �C  (B ��C����  C  aC �JD���� �D  �A �D����  �C  �@  ID���� �.D ��C @ D����  �A ��C ��C���� ��C �D  �C����  �B ��C  �C����  ,B �SD  �B����  �C �cD �D���� �SD  �C ��C���� @ZD �wD �YD����  �C ��C ��C����  �C  �C  `D���� �!D @ D  qC����  ND  UC @D���� ��C �-D @hD����  �B @xD �D����  �B   D  ;C���� �D  �B  $B���� �bD      WD���� ��C  -D @D���� ��C @3D  �B���� �D @_D �kD����  �A @gD  0A���� �TD  ;D �tD���� �#D  �C  _D����  B ��C �D���� �ND  �C �D����  nC  bD  tB���� ��C �]D �D����  6D  `B ��C����  �A �;D ��C���� @3D  &C �wD����  ;C  �B �SD���� �XD  dD ��C���� @`D  �C ��C���� �kD  �C  IC����  @D  @D  �B����  �C  �C @ D���� �eD  tC  4D����  D  [C @D���� ��C  vD  .C���� �-D  `A  C���� �TD   B ��C���� ��C �KD @D����  �C  [C �"D���� �eD �*D @eD���� �D  |C  D����  	C  xC ��C����  (C  �C ��C���� @xD  C  �A���� @'D ��C  �C����  `A @[D @_D���� �kD  �C  �B���� ��C �JD ��C���� �wD �GD  _D���� ��C �^D ��C���� �AD @	D  �B����  �B  -C  �C���� @PD @XD  ZC����  |C �cD  nC����  C  �C ��C����  gC  uC  �B���� ��C  �B  fD����  �C ��C  �C���� �uD �!D  C���� �4D  �B  nC����  =C �0D �bD���� ��C ��C  C���� �	D  �B �fD���� �D  QD  �B���� @KD  �C  �C����  SD  \D �"D����  uC @9D �9D���� @0D @mD �D����  cC @DD � D����  �C  LC  D���� �
D  cC @eD����  �C  �C  �B���� ��C �tD �.D����  �C �=D ��C����  C  �C �AD���� �mD  �C  �B���� @D  �B  �B���� ��C  QD  (D���� �'D  tD @
D���� �;D  #D  �B����  �? �;D ��C����  �B @-D ��C����  �C  3D  eD���� �QD  �B ��C����  D �jD  �C���� �aD �ND  `B����  �A  D �=D���� @BD  �B �D���� �lD �tD  D����  uC  �C @	D����  �C  xB   C����  �B @)D �3D����  gC @DD  <B���� �nD  +D  �C����  oD @1D @@D���� ��C @lD ��C���� ��C  WD  �A���� @)D  >C ��C���� �]D  �@  DB����  C �(D  �C���� ��C  C ��C����  }C  �C �
D���� ��C �<D    ����   A  WC  �C���� �GD  kD  �C����  �C �0D @yD���� ��C  mC  �C���� @D  FD ��C���� �uD �*D  D����  	C  C ��C����  XD  D  B����  rD @VD �*D����  1D  rD �1D���� ��C  7D @ D���� @/D ��C  +D����  �C �D @YD����  �B   D  -D����  MD �JD  @A���� �D �%D �PD���� �AD @)D @`D���� @D @
D  C����  D  �C �[D���� ��C  �B �7D����  D  vD  2D���� �MD  �C  FD����  �C @dD  \B����   C  �B ��C���� ��C  TB  �B���� ��C �;D �MD���� �hD  �@ @eD����  lC ��C  EC����  �C �D ��C���� ��C @vD �`D���� @5D  D  �B����  OD �UD  yD����  �C  �C  �C����  B �9D �FD����  TB �?D  �C����  C ��C @FD���� �UD  gD @D����  NC  oC ��C���� ��C  �C  QC����  tB  B  /C����  PA �D �!D����  `D  2D �5D����  C @RD @cD���� �dD  B  �B���� �&D �D  �C����  B  8D  �C���� @mD @_D  �C���� @BD  SD �rD���� �D  HC ��C����  �B ��C ��C���� @6D  �@  'D����  aC @D   A����  3D  uD  D���� �:D  �C  �C����  LC  jD  �C����  �C �D  /D����  zC   B ��C���� �?D ��C  �C���� @SD �'D  
C����  `C �DD  \B���� ��C ��C  ^C���� �PD  ID  "D����  ZD ��C  �C����  0C �sD  'D����  qC @D  	C����  pA  �A @:D���� �SD  |C �sD���� �qD  D  oD����  pC  C @D����  [D  �C  �C����  �A  MC  �A���� ��C @ D ��C���� ��C ��C  ?C����  �A  YC  /D����  �B  D �kD����  �C  C ��C����  8B ��C �&D����  8B @ED  D����  C  @@  C����  C  �C @aD����  �C @"D  8C����  �A  �C ��C���� �FD  �C �rD����  D ��C @FD����  RC  `D  VD���� �	D  -D  BD����  UD �FD  �C����  B ��C  �C����  vD  >D ��C����  HB �D @6D����  jD �ID  �B���� @D �ED  D����  �B  �B  6C����  �B ��C  �C���� �uD  �C �D����  B  AC @D���� �_D @D �<D����  'C �D  �C���� �\D ��C  �C���� @PD  .D @XD����  =C  D  �C���� @LD  7C �QD���� �)D @<D  C����  |C  D @D����  C @WD  C���� @KD �pD  �C����  �C �
D �bD���� ��C  SC @D���� �D �qD  �C���� �D �.D �kD���� ��C �D  C���� �>D  �C  C����  �B �lD  �C���� �lD  %C  �C����  (B ��C  D����  D  BC  �C����  HC �2D  �C����  �C  �B  NC���� �D @PD  .D���� @!D ��C  �B����  �C  �C  /C����  wD  �B �eD���� @D  D ��C����  #D ��C  EC����  �B �$D  �B���� �yD @yD �VD����  �C  �B �`D����  dD  vD  B���� ��C @nD �dD���� �
D  8D����  |C �hD �nD����  �C ��C  bD����  �B �<D �YD���� ��C @/D  0C����  �C  gC  �B���� � D  �C �sD���� �wD @$D �jD���� ��C @uD  �C����  �B �D  rC����  [C  tB �D���� @PD �wD @CD����  �C �D  (D���� @4D  hD  �C���� @lD ��C �`D���� �+D  ^C  �C����  SD @D �FD����  pC �D �*D���� �4D  �C ��C����  (C @#D ��C����  �C  C  C����  $D   D �VD����  C  IC  �C���� �/D  �C  �C����  �C  �A  +D���� ��C  zC  �C����  CC �TD ��C����  �B  D  �C����  _D �GD  7C����  uC  DD  TD���� ��C  �C  ;C���� �YD �VD  �C����  PD  ,C  �A����  �B �*D ��C����  D  tD  pA���� ��C  B  �C����  C @yD  MC���� @eD �(D @@D����  �A  �A ��C���� ��C �7D  �C����  �B  �C ��C���� ��C �DD @BD���� ��C �D @DD���� @2D  �B �tD���� �\D �7D �iD����  "D  �B  :C���� @D  <D  �B����  D �uD  �C����  lC �eD  ,D����  �C  �B  FD����  �C  �C  FC���� �D  tD  0A����  uC  JC  �B���� �D  
D���� �oD �D ��C���� @JD �]D ��C����  �A �+D @XD���� ��C ��C ��C����  �C  �B  �A���� �6D  �C @YD���� ��C  �B �D����  �C  �C �RD���� @D ��C  �A����  dB �rD �4D����  B  .C  �B���� ��C �$D  7C����  gC �uD �4D����  �C  hC ��C����  �C  �C �D���� �VD �/D ��C���� @D @D �LD����  $D  �A  �B����  �B  �C @2D����  �C ��C  6C���� �vD  �B  �C����  �C �uD  �B���� �:D �2D  �C����  C ��C  �C����  FD ��C  #C����  \D �D  gC����  mD   C �3D����  �B  �B  4D����  JD �D �?D����  C @ND  �B����  B  \C �D����  C  �C  �C����  �C  C  C����  �C  PC �;D����  D @D  �C����  �B �-D  tD���� ��C  �C  =C����  SD �/D ��C���� @D �D �D����  :C @D �ND����  )C �ND  �C���� � D �`D �D����  �C  !D ��C����  )D  �C @'D���� �SD  nD �VD����  wD �dD  	C����  �C  �C  D���� �dD ��C  �B����  xB  $D  `B����  C  D  D����  ZD  vD  �C����  �A  �C  DB���� �AD  JC �D����  
D @&D  �C���� ��C �<D �=D���� ��C �
D  SD����  �C �yD  ND���� ��C @7D  �B���� @ZD @FD @eD����  �B  /C  pB���� �D ��C @vD����  �A ��C  PC����  wD ��C  4D���� @tD  �B @hD����  ~C   A  AC����  	C  [D �fD����  �B �HD  LD����  }C ��C  dD����  �C ��C �tD���� ��C �D �aD���� �7D @6D �#D����  bC  �C  �C����  sD  C ��C���� ��C ��C  �C���� ��C �D ��C����  3C �-D ��C���� �$D  C @%D����  D �bD @pD����  D @!D  �C����  DC  D  sD���� �PD �D �JD����  D  3D  eC���� @;D  C  oD����  5D  �C  D����  �C �D  �B���� �D ��C �uD���� �D  D �,D���� @[D  �B  &D����  �@ �kD ��C���� @D  UD �D����  5D �RD �D���� �D  C  C���� �UD  �C  C���� @cD @9D  ID����  �B �BD ��C����  D  -D  �B����  !C  �B  �C����  �C ��C  <B����  D �oD @D����  D  dC  C����  �C �BD ��C����  �C  C  �C����  RC  �B @:D����  4C  D �.D����  bC  �B �KD���� @D ��C  �C����  D  �@  �C���� �D  �B ��C����  %C �D  7D���� ��C �D  �B����  `D  �C   B���� �D �tD  �C����  )C  lB  uC���� @@D  �C  :D���� ��C �D  DB����  tB  C �D����  :C  >C �D����  �@ �D  bC���� ��C  @D  �C���� �
D  �C����  �A �GD  �C����  FD  �B �7D����  �A  6C  eD���� �=D  lB  �C����  C ��C �D���� ��C  �B ��C���� @[D  CD  �C���� �+D ��C  �C���� �3D  |C ��C����  �C ��C  B����  �B  pD �)D���� ��C  �C @mD���� @8D  C �D���� @D �jD @RD���� ��C  B  �B���� �iD @ZD  @D���� ��C  �B  �B����  D �8D @D���� �6D ��C �fD���� �D  �C @TD����   B ��C �TD����  D  _C @@D����  �C  �B ��C���� �jD �D  C����  �C  fD ��C����  �A @+D �=D����  �C  �C  eC����  DC  /C  �C���� �D  wD �*D���� ��C  >C �lD���� ��C �D ��C���� �#D @D  EC���� �FD  hB  C���� @QD ��C �ED���� �(D  hC  aD���� �oD  �C  8D����  PA  �B  D���� �5D  }C  �C���� �'D  �C ��C����  �C ��C  D����  �B  �C @	D���� @D �PD  GC���� �6D ��C  	C���� �D  iC @D���� �6D @D  �C���� ��C  �C  0B����  eD  D    ���� @=D �yD �D���� �D @D  pC���� @;D  C @kD���� �D ��C �D����  uC ��C  �B����  �C  IC �5D���� �7D ��C  �B���� @:D �_D  �A���� @3D ��C  *C����  �C  <D  �C����  C  9D @AD���� �RD  �B  �C���� @?D  =D  �A����  �C  uD  �B����   A  �B  �C���� @3D �&D ��C����  �B �D  hD���� �wD �YD  D����  �B      �C���� �*D  �B ��C����  YD  �B �cD����  �C �ND  !D����  wD  D  �C����  TC @XD �QD���� �$D  �C �lD����  �C �@D  PB���� �&D  dB  �B����  C �'D  zC���� �ZD  C @gD���� ��C  ,B �AD����  �A �>D  D���� ��C �iD �VD���� �KD �#D  tB����  C �D  .C����  aD  �A  �B����  �C ��C �D����  XD  �C  �C���� @D �-D @D����  ED �TD @D���� �0D  :D �FD����  �C  �B  �B���� �tD �LD  ]D���� �ID  C  CC����  �C  �C @SD����  �C  MD  C���� @oD  ^C �?D���� �nD ��C @
D����  �A @D ��C���� �D  B   C����  "D  �C  �C����  @C @D  XB����  C �D �D����  �C �wD  `B���� ��C ��C �9D���� ��C �gD ��C���� ��C  B ��C����  �A  C  D����  D  �B �RD���� �bD @3D @pD����  &C  uC  fD���� �CD �CD  �B���� �YD ��C  �@����  C  CC �D����  D �@D �>D����  �C  YC �iD���� @4D ��C @\D����  OD  �C  nD����  �C ��C  fC����  C  C �.D����  �C  $B �FD����  C  4D ��C����  UC  �B  �C����  JC �$D  JC����  hB  yC @<D���� �D  �B �D���� @/D ��C �MD����  �B �D  MC����  �A  $B ��C����  MD  QC �D���� �D  ,B �iD����  �B �aD �(D����  A �AD  �B���� �D �FD  �C����  pA �^D  8D���� �CD �<D  �C���� �UD  <C @iD���� �fD @lD  pD���� ��C �$D �D����  D ��C �kD����  ED  'D ��C���� ��C  �C  FC����  WD @KD �0D���� ��C �SD ��C����  C  �C  6C���� ��C  D  �C���� �MD  ^D @	D����  >C  {C @ND����  �C  8B �D����  �B  D �AD���� @CD  �B @vD����  @B  �C ��C����  XC  �C  *C����  B �5D @6D����  �C  (D �dD���� �&D ��C �WD���� ��C ��C   C���� ��C  �B ��C���� �7D ��C �vD����  D �&D  C����  @@ �MD ��C����  
D ��C���� �OD  �C ��C���� ��C ��C  �C����  �B �&D  C����  �C �=D �1D����  `A �bD ��C���� @bD  �C  �C���� ��C  �A  KD���� @D  �C �D���� �uD  'C �D���� @FD ��C �CD����  �C �)D  HB���� @D  ]D  �C����  �B  �C ��C����  ND  xD ��C����  pB  �A �	D���� ��C �=D @HD����  �C  �B �fD����  C  �C  aC���� �ID �#D  @B����  >D ��C ��C����  [C  QD  �C���� @5D �)D  �C���� @/D @JD �D���� �ID  �C �D���� ��C ��C @5D����  $C  �C @rD���� �D  �B �pD���� @yD �TD �@D����  �B ��C  5D���� ��C  RD ��C���� @%D  �A �nD����   C  cC  �C����  mC �&D  C����  �C  TD  @B����  ]C @
D @D����  �C @lD  �B����  CC  �B  `C���� ��C  D �D����  �C ��C �D���� �D ��C @wD���� �#D  3C  B����  �C ��C  #D���� ��C @lD  C���� �D @"D �YD���� @0D  1C  LC����  D �;D ��C����  'D �4D  dC����  8C �AD @DD����  B  ]D �"D���� @*D  =C �dD���� �TD �=D  �B���� ��C  �C �D����  !C ��C  iC����  �C �D �LD���� �SD ��C ��C���� ��C �D  �C����  vC  �C @lD���� @nD  ~C  �C���� ��C �D  >D����  �B  D �7D����  �A  D ��C����  �C  �C ��C����  !D �:D @D���� �KD  �C   D����  IC �5D  �B����  �B  �C @AD���� �D �wD @D���� @VD  _C  VC����  �C @TD  /D���� �/D �>D ��C����  C  �C  �C���� �D  kC  TD����  �C  mC �>D���� ��C  HC  FD����  �C ��C �D����  vD  C �wD����  �B  �B �2D����  �C  �C ��C����  AC �"D  �B����  D  D  9C���� ��C ��C  ^D���� ��C @@D  `D���� �rD  �C �KD����  �B �D @D����  ^D �TD  �C���� ��C  TB ��C���� �%D ��C @9D���� �-D  �B �#D����  �B  ZD �D����  �B �D �'D����  sC �D ��C����  C �3D ��C���� ��C �D @jD����  �B �
D  @C���� ��C  �B  �A���� �LD  D �4D���� �cD  -D �ZD����  6D  C ��C���� ��C  C �D����  �C  �C �ND����  /C  �C @BD����  C  �B  lD����   D  �C @MD����  C  �B �qD���� �bD �mD �D���� @D  �B �	D���� ��C  A  �B����  �A  D  D���� ��C  �C �D���� �yD ��C �bD���� @D �hD  @B����  ]D  LD �$D����  D  �A ��C����  �C �ED �CD����  �C �vD   A���� �D �xD  D���� @$D @1D  �A���� ��C ��C  
D �@D����  �B �6D ��C����  BC ��C �HD���� @(D  &D  �C����  �C  �C  @B���� @2D  ?D ��C����  *C  �C  PA����  0D �?D �tD���� ��C @"D  �C���� ��C @D @QD����  �B �nD  �C����  �C @jD �D����  �C  C �dD���� �D  6C  �C���� �SD  jD  �B���� ��C  �C  nC����  $B �D �2D����  �C  �B @gD���� @;D �WD  �C����  �C @D  �C����  ,B  �C �!D���� @>D  D  tC����  C  /C ��C���� ��C ��C  .D���� �&D @2D  �C����  �B �ID  �A����  +D  lB  �C����  �B �=D �D����  �B ��C �D���� �mD �D  DC����  @A ��C @D����  %C @YD �&D���� �ED @sD �aD���� � D  C  D����  �C  sC  �B����  �B �1D ��C����  �B �D �ZD����  �B  �C  �@����  �C �$D �5D����  D  ]C ��C����  qC �eD �GD����  QC  2D �D���� �OD ��C �D���� �CD  �C � D���� ��C  wD   B����  kD �'D @0D����  6D ��C  YD����  �C @D @vD����  �C @yD  �B���� �D  �C �/D����  /D �
D @XD���� @D �2D  �C���� ��C  �A �FD���� �)D  ;D  D����  @@  GC ��C����  �A  �C  �C���� �VD  �B  �C���� �	D @
D �6D���� @"D  �C  �C����  D  \B ��C����  
D �VD @^D����  WD   D  C����  0B �D  8B���� ��C  jD  �C����  =D ��C  C���� �D ��C  LB����  �B  �C �hD����  D  0C  JC���� ��C  �C ��C���� �D   @ @]D����  >D  �C ��C����  \B  (B  �B����  hD  �B ��C���� �5D  �C  D����  IC �D �GD���� @yD  �B �D���� �;D  /C  #D���� @_D ��C �)D���� �D  �C �D���� @D  %C ��C����  �C  qD  �B���� �1D ��C  �B����  �C �>D  �C����  �C @MD  <C���� �4D �<D  @@���� �D �UD  �C���� �LD  D ��C���� �$D  �B  �C���� ��C �*D  �C���� �
D �<D  ^C����  bC  D �TD���� @8D �D  �C���� @;D @aD @aD���� ��C  �B �qD���� �1D �RD @\D���� �VD ��C  �C����  �B  �B  8C���� ��C �vD �D���� �D  sD  �C���� ��C ��C  �B����  UC  yD �%D����  ID  �B ��C����  
D����  `D ��C @QD���� �GD  �C �VD���� @RD @/D @#D����  WC @6D  �C����  D �"D @
C����  �C  `A �@D���� @?D  hC ��C����  mC  4D �D���� �/D @1D  �C���� @D  HB  D���� ��C  �C ��C����  .D �cD  
D  ZD����  �C @.D @tD���� ��C ��C @kD����  �B  rD @D���� �5D �#D ��C����  	C ��C �dD����  "C @	D  iD����  D  B �\D���� �D �hD  C����  ,D  6D  D����  iC �D  5D����  �B  �B �D����  �B  >D �CD���� ��C  �C �hD����  cD  �C  �C���� @
D ��C���� �9D @CD  �C����  /D  aD  �B����  *D �D  jC���� ��C �fD �ED���� �D ��C  (C����  �C  D ��C����  [C  HD  1D���� �lD ��C �D����  �C ��C  D����  C  �B  +D���� �@D  ^D  �C����  C ��C ��C����  D @D �D����  �B  zC �5D����  HB  �B  wC����  �C  �C �6D����  LB @ID ��C���� �?D  C  �B����  C  )D  �C����  �B @sD  D����  �C  �C @oD����  (D �RD ��C���� ��C  �C �KD���� @JD ��C @jD���� ��C �D  fD���� ��C  TB ��C���� @D ��C  D���� �MD @\D  �B���� �D �'D  �C���� @#D  �B @xD����  wC  �C  jC���� @:D  �C  �B����  �A  �C �sD����  �B �D ��C���� �FD @ID �qD���� ��C  �C �!D���� @GD  �B  �B���� ��C  �C  �B���� @D ��C @D����  �C  �C �ID����  lD  �C �D���� @_D  pA �5D����  �B @D ��C����  �C ��C  ZC����  pA  !C �'D����  �C @6D @ND���� �OD  CC  B���� @/D  �C @	D���� ��C @D �FD����  mD �oD  xD���� @rD @D  NC���� @D ��C �D���� @)D  �C �"D����  ^C �D  D����  DC  `C  �C����  dD  �A @TD���� ��C �2D �4D���� @uD  D  JD����  C  �C  C����  �C ��C  C����  �C  �C ��C���� �-D  LC �D����  C  �C  �C���� �D @D �mD����  uC �(D @SD����  �B ��C  D���� ��C �
D  CD���� @D  YD  tB���� @=D  �B @(D���� ��C ��C  �C����  XC  =C @aD���� @5D  oC  oD����  2C  D  �B���� �>D  3D ��C���� �8D  PC ��C����  �C @D �D���� ��C @D �GD����  $D ��C  }C����  �C �D @DD����  �C �GD @D���� �7D �&D �D����  &D �'D  [C���� �SD  D �D����  TC �dD  `A���� ��C �D ��C���� �yD  �C  �C����  �C ��C �`D����  YC  
D  @C���� @-D  wD  D����  �C  ,D  �B����  �C  �C  �C���� @:D @8D  �C���� @ND @GD  D����  mC  ]C �!D���� �'D �bD � D����  B �D @	D����  �A  @A  |C���� �,D  �C ��C����  �B @SD @D����  `B �AD  �C����  8D  ZD @)D���� ��C  `D   A����  ED  �C  C���� �hD  �@  :D���� �D @"D �5D���� @aD  .C  YD���� �qD  C ��C����  �C ��C �gD����  6D �LD  CD���� @kD @2D ��C���� �6D  GD  _C����  C  gC  RD����  �C �D  D����  UD  !D  zC����  �C @D ��C���� �_D @AD  B���� �\D �D ��C����   D �CD @D����  PB @ND  �C���� @SD  �A  KD���� @AD  JC  �C���� @)D ��C �=D���� ��C  �B  �C����  uD �D �"D���� ��C ��C  }C����  �B @GD  3D���� ��C @BD �3D���� @2D  C  �B���� �D  0C ��C����  qC  rC �ZD���� �%D  �B ��C����  C  rC  xC����  D  D  D����  D  eC @D���� �0D �D @CD���� �D  �C @D���� @bD  B  �C����  �C  �C @uD����  �C  �C @D����  'C  4B �qD����  D  #C  B���� �yD  �C �AD����  YC  �B @D����  �B ��C  ND����  9C ��C �(D���� ��C  hB  WC����  SC �D  fC���� �pD  �C  B���� @%D  �A �D����  [D  .D  D����  +C �oD  �B����  �B  xC  ?C���� �=D �	D �.D���� �9D  C  6D���� ��C  WD  �C���� �-D ��C ��C����   B  UC  �C���� ��C  bD @D����  (C  TD  ]C����  �B  D  �B����  �B  hC �QD����  �C �
C  �B����  pB �;D ��C����  	D  �B ��C����  �C �eD  �C���� �D  �B  TC����  �B �D  �B���� @;D �;D  (B���� @tD ��C  6D���� �D  �A  8D���� @nD �CD ��C����  C �D ��C����  �B @D  %C����  �C ��C  CD���� �4D ��C  7C����  D  |B  �B����  �C  �C  yC����  �A ��C �%D���� �tD �D �.D����  �C @ND �5D����  _C  4C ��C����  %C  �C ��C����  \B ��C  �B���� @JD �ID  ;C����  �B  �B  �C����  �B  �C  D����  =D  MC @,D����  VD  YC  5D����  �C  >D  @B���� �qD  �B �"D����  -C �BD  �B����  iD  @B �RD���� �D �>D  D���� @VD  D ��C���� @%D  &C  uD���� @hD @iD ��C���� �^D  zC  �B����  �C  �C @-D���� �D  dB �DD����  bC �cD �ND����  �C  qC  �C����  -D  �B �&D����  _D  �C  C���� �ND  C  XC���� �QD  (B  C����  �C  �C  #C����  DC  `D  zC����  �C  �B �)D���� ��C ��C ��C���� @SD  $B  �?����  �C  �B �D����  �C  XB @xD����  �B �D  LC���� � D @D @D����  �C  IC �ND���� @D  B  �A����  MC  �B �PD����  �C  7D  aD���� @eD  RC  D����  FD ��C @D����  �C �wD �cD����  �B @^D �AD���� �PD  C ��C����  -C  B  �C����  �C @D  �B����  �C  CC �ND����  �C  �B ��C����  ?D  �A  �@����  AD   A  mD����  C �ED   A����  �B �OD  <B���� ��C �bD  .D���� ��C  >C �KD���� ��C ��C @D���� �ND ��C @#D����  �C @hD �kD���� �]D  D �D����  C  <B ��C���� @D �D  �C���� ��C �rD @1D���� �RD  �C  rC���� @\D  aC  �C���� ��C  D �3D���� @XD @VD  |C���� ��C  �C �sD����  ED �;D  �B����  �C  �B �:D���� @xD @3D  [C����  �B  =D  >D����   B  GC @-D���� ��C @ZD @TD����  VD  �C @iD����  �C  �A  �C����  �A  �C    ���� @1D  TB  C����  �C �:D �_D����  C �D ��C���� �uD  tC  �B���� �D ��C  �C���� @
D ��C���� ��C  DD  �B����  RD  tD ��C����  >C @D  @B���� @D ��C @D����  @@ ��C ��C���� �7D  �B �xD���� @D ��C  �C���� �UD  IC @gD���� ��C  �C  �C���� @5D  �C ��C���� �,D  �B �ND���� �iD �0D  =C���� �\D �3D �SD���� ��C  �C �D����   D  �A  @B����  �C @1D �5D���� �]D @qD @
D ��C����  +C  #D  �C���� ��C  4D �D���� ��C ��C  �B����  �C  D  #C����  D �hD @"D���� @PD �D ��C����      4B  �B����  AD �wD @vD���� @FD �@D �D����  tB ��C  .D����  �C  3D  cD���� ��C �ND �8D���� ��C �D  ZD���� �$D @-D  AD����  �C  �C �yD���� �xD � D �D���� �UD @D �
C  �B  D����  �C @ED �wD����  �C   D  �C���� @)D  �C  KD���� �^D @D �D����  �C �wD  <B���� � D  �C @0D����  �C  
D  �B���� @9D  �B  �B����  iD  "D �>D����  C @nD �&D���� �0D �D  UC���� @6D  �C  �B���� �HD  �C  �C����  �C �D  �C����  C �nD �.D���� �BD  `C  �C����  9D  �A  �B����  D @gD ��C����  �C @D �D����  ED  dD @TD���� @D  �C �D����  0B ��C � D����  �C ��C �1D���� ��C  �C  �B����  �B  C  �C���� �MD  �B ��C����  [D @ZD  �C����  �B �0D  �C����  C ��C �FD����  �C  C  
D����  �C ��C  OC����  DB ��C ��C����  �C  DD �D���� �MD �D �DD���� ��C  �C @YD���� @vD  ID  HD����  D  �C ��C���� ��C   B �fD���� ��C  @A @jD����  C  �B �eD���� ��C ��C   C���� ��C  �C  �B���� �D �FD  ^D���� �!D  vD ��C����  �C ��C �#D���� @3D @>D  �B���� ��C �D @qD���� ��C �KD ��C���� ��C   @  TB���� �ID  �B @D����  �C �D  lD����  vD �D  KD���� @]D  �B �<D���� �<D  nC ��C����  `B @9D ��C���� �ND  �A @D���� �BD  xC �WD���� �;D �6D ��C����  �C  �B  {C����  �C  
C �tD���� ��C ��C �wD���� ��C ��C @CD����  C ��C  �C���� ��C ��C @D���� @pD �GD �$D����  D  �C  �C����  $D  �B �D���� �'D ��C @D����  }C  |C  �A����  �B �D ��C����  �C  D  B����  $C �D  @C����  nD ��C ��C���� �4D @iD @JD����  rC  �B  �C����  kD @YD  0D���� @ID  �B  C���� �ND  (D  D����  RC @D �nD���� �!D  �C  C���� ��C �D  YD����  �B �5D  GD���� @OD  5C  �B����  3D  <C  �C���� �dD �yD �QD���� @7D  ^C ��C����  C @D  �C���� @%D  �A  eD���� @9D  �C �ED���� �3D ��C ��C���� ��C  �C  �B���� @;D ��C  �B����  (C �wD  �C���� ��C ��C @D���� �D  �C  'C����  JC @gD �vD����  D  �B  6C����  4D  �C  C���� �D @KD ��C���� �aD  �A  �C���� �vD �+D  5D����  YD ��C ��C����  �C  ZC  D���� �[D  #C  D���� �RD  �C  pA����  �C  YC �ID����  dB  4C  �A����  �C  D @YD����  �C �D  /D���� �dD  OD �PD���� ��C �`D �oD����  �C @fD  D����  ,B  D  �B����  �A  C ��C����  ZC  �B  OD����  �C �kD �HD����  �B  YC  �A����  BD  (D ��C����  �B �1D @+D���� �kD  �C  ~C����  C  �C � D���� �D  QC �XD����  6C  �C  �C���� @-D @+D  eD���� @D  gD  }C���� ��C  �B  xC���� �D  �B  ,B���� ��C �4D  �B����  �B  2D  KC���� �ID ��C  �B����  D  �B �qD����  �C  �B ��C����  �B  D �D���� @xD @lD �2D���� ��C �RD @D����  C  C  yC����  �C  �B �?D���� �qD �/D @-D���� �2D  �C �*D���� �2D ��C  �C���� @5D @pD  gD����  AD �&D  HC����  ZC @LD  yC���� ��C  �C @KD���� ��C  �B ��C����  ;C  vC  5D����  �B  �C @D����  kC ��C  �C���� �lD ��C @D���� @eD  �C  �@����  8C  �C  sC����  �B �D @oD����  }C �D  �C����  �C  mC @&D���� ��C  �A �rD���� ��C  8C  �C����  kD ��C  �B���� ��C  �B �^D���� �ED  _C  �C����  ]C  �B �D���� �D �lD �xD���� �D  IC �<D���� �D  �B ��C����  TC  �C �+D����  +D  �C �eD����  �A  �C �2D���� �6D  �B  �C����  _D �ND ��C����  D  �A �?D���� �D  �B �<D���� ��C  �C  �C����  'D �D  =D����  UC  �C  GC���� �dD  C  �C����  �C  �C �D����  �C  �B  jC����  �B  �B ��C����  mD @\D  vD���� ��C  �C   B���� ��C �/D @CD���� @aD  �A  `D���� �tD �tD ��C���� @(D  �@  `A����  �B  �B �.D����  ?D  D @vD����  �B @D �D����  �C �D  �B���� �kD  ;C  (B����  �C  <C ��C���� @iD @D �QD���� �	D �OD  �C���� @D �$D  D����  �C  �B  �A����  =C ��C �bD���� ��C �8D  aC����  oC @\D ��C���� @dD @D  �B����  HB @+D  C���� �FD  yC ��C���� �iD   A  JC���� �0D  (D �5D����  sD �kD  �C���� �LD   A �"D���� �5D ��C @D����  !D @0D @D����  !D ��C  C����  <D �iD ��C���� �vD  `C �uD���� @aD ��C ��C����  �C �nD @<D����  {C  6D  D����  �C   A  tB���� �^D  �A  C���� @JD  |C ��C����  C ��C �5D���� @4D  C �WD����  �A  DB �&D���� @5D  0A @&D����  HB  "C  cD���� �D  WC  D���� �1D ��C �'D����  B  *D  ]C���� �D �ID @D���� �\D  �B �&D���� �D  �B  DB����  �B  �C  �C����  oD  B ��C����  �A �^D  �B���� @=D �]D @D����  �C  �A  �C����  C ��C  fD���� �VD  �B �sD����  �C ��C  XD���� @LD  �C @YD���� �LD  ^C �%D����  MD  1C  7C����  �C  �C �)D����  ND �HD �SD���� �wD �%D �D���� �nD  �C  �C���� ��C  yC  �A���� ��C ��C ��C���� @D �@D  "C����  �C  ^D  C����  D �qD ��C���� @ D �D ��C���� �;D ��C  �B����  sD   D  0D����  4B ��C ��C���� @D  �B  �C����  \D �D  �C���� @-D  �C  GD����  lB @D  BC����  3C  D �QD���� @6D �CD  �C����  RD  (B  D����  �B ��C  D����  D  �C �D���� �D �D ��C���� @D  D @\D���� �tD  �A  -C���� �MD @*D  KC����  D �D  hC����  �C �D  C���� @nD ��C ��C����  �B  B @JD���� @D �9D @D���� �DD  �C  �C����  �C @(D  B���� �ID  ID ��C���� ��C �D  nD���� �D  JD @D����  C �0D �6D����  C �kD �)D����  <B @pD  �C���� ��C  �C �@D���� @0D �+D �D���� @D ��C �D����  dB  �C  0B����  B  3D  �B����  �C  �C  C���� @aD �D ��C����  C �^D  wD���� ��C �PD  hC���� @PD  �C @?D���� @D  D �DD����  �@  �C ��C���� �JD  C �D���� �eD @GD @0D���� @^D  yD  �C���� �/D �D @PD����  C  �C �1D���� �!D  �C �,D���� �D ��C  WD����  PA  �C  C����  �B  �B  QD����  oD �0D �bD����  �C ��C @/D����  �B  �B  ~C���� �dD ��C �!D����  �B �DD � D���� ��C @D  EC����  LC  C  C����  �C  QD �WD���� ��C �D  fC����  
C  �A  D����  �B  �?  iD����  ID  �C  ?C����  RC @(D �D����  [C  B @qD����  2D  DB �LD����  B �3D �!D����  B �,D ��C���� ��C @wD  (D���� ��C  %C  D���� �8D @D �@D����  `C @QD  rD����  �C  �C  �C���� �D �$D  �C����  �A  PD  �C����  �B ��C  �C���� �DD @4D �RD���� ��C ��C  �B���� ��C �D @4D���� �dD  �C  gD����  1C �UD �%D���� � D ��C �0D���� @D  D  �C����  ZC ��C  -D���� ��C  ;D  C����  �C @D �[D���� ��C  �B �"D���� �D �VD �[D���� ��C  ;C �lD���� ��C @gD  �C����  �C  �C  WD����  �C  $D �?D����  C  �C  (D���� ��C  �C �[D����  �C ��C  KC���� ��C ��C @D���� �	D  �B  D���� @FD  �C @kD���� @D  C �xD����  �C �HD  C����  @A  �C  OD����  �B �iD  ?D����   @  fD @PD����  �C  @B  JC����  RC ��C @dD���� @D  �B  DD����  @A  �C �@D����  �B @ED �jD����  D @JD @@D���� �XD �AD �AD����  FC  �C �QD���� @1D @<D  dC���� @JD  4D  XD����  �@ @bD  �B����  6D  aD  �C���� ��C �ND �[D����  D �8D �nD���� ��C  *D  �A���� �5D  FD ��C���� �D ��C  mC���� ��C  
D @uD���� �ID �tD  �C���� ��C  CD @^D���� ��C �FD �CD���� @pD @gD  PA���� ��C ��C ��C����  C  �@  �B����  D @D  D����  zC  _D  `B���� ��C  �C  �B����  B �DD  D���� @D �!D ��C����  �B @^D �lD����  _C �PD  �C����  �A  @B  ,C���� ��C  &C ��C���� ��C @kD @uD����  .D  uC ��C����  aD ��C  eC����  OD �/D �"D���� �D @kD �6D����  �C �1D �kD���� ��C  ED  PA���� @8D  \C  �B���� ��C �KD �
D���� ��C @yD    ����  JC @eD   A���� �D  uC   C���� @iD �.D @D����  �B  ]D  D���� �D ��C @6D����  qD ��C �D����  (B  �C �-D����  �C  uC  D����  WD �KD  ^C���� @;D �@D @wD����  �A  �B ��C����  9D ��C  |C����  �C �PD @2D����  	D  GD ��C���� �D �VD  &D���� ��C  D  �B���� �%D �'D  �C���� ��C  &C �WD����  AD �D  \B����  �C @WD ��C���� @"D  $B ��C����  �B  mC  !D���� ��C �&D �D����  (B �D  LD����  \D ��C �D���� �SD  �C  �B����  .D  �C �ID���� ��C  �C �D����  0A �D  �C����  �B  �B �D���� ��C �\D ��C���� �D  �B  iD����  �C  D    ���� @cD �ID �RD���� �D @?D ��C����  hB ��C  �B���� ��C  �C ��C����  �B  9D @=D���� ��C ��C ��C���� �9D �D �rD����  �B �HD  �C����  |C �D  �C���� ��C ��C @tD����  D  3D ��C���� �@D ��C  �C����  �C  dD  �B���� @)D  �C ��C����  �B  MC  cC���� @\D  7C �,D����  D  D  �C���� @D  �B  �C����  C @TD  �C���� ��C �dD �ND����  1C  C  ED����  D  rD �TD����  �A  vC ��C���� @:D  "C  �C����  �C ��C @:D����  C  �C  hB���� �D @ D  �B���� ��C ��C  �C���� �SD  �B  {C����  YC ��C  [C����  D  �C  C����  
D����  %D  UD  cD����  �C  sC @OD����  �C @\D  �B���� �D �D  �C����  !D @ D  �C����  B  �B �AD���� @1D  
D���� @mD  UC  WC����  C  �C  D����  �B  kD  YC����  D ��C  8C���� ��C �pD ��C���� �DD     ��C����  ZC  �C ��C���� @`D  dD  �B���� @ED �lD @D���� ��C ��C �D����  @C @GD �wD����  fC �aD ��C���� �3D  nD @XD����  D �#D ��C���� @D  7C  �C����  ND  C @dD����  �C �	D �.D����  �B  �C  C���� @bD  �C @aD����  ;C  C  @D���� �#D  6D  C����  D ��C ��C���� @HD �<D @D���� @jD �KD  �B���� �D ��C �D���� ��C @uD  <B����  D �QD �(D����  �A @4D �JD���� @.D  �C ��C����  D �kD ��C����  �C  �B �sD���� �VD  &D  �A����  �B �	D �+D���� �pD ��C  ZD���� @KD @aD  �C����  <C  D @^D���� �wD �D  @A���� �D �3D @$D����  �B �VD @3D���� �'D ��C @!D����  +C  �B  (C����  �C  UD �D���� �D  pC  lD���� @(D �[D �BD����  �C  FD �$D���� �D  �A ��C���� �SD  �B  D����   @ @gD �D���� �PD �`D  �B���� ��C @rD  ID����  �C �D  D����  HC  �A  C����  �C  �C �D����  A  0B  <B����  �C  D �D���� ��C  B  qC����  �A @ D  DC����  `D  sD  �B���� �8D  �C  �C���� �<D ��C ��C����  �C �$D  8C���� @,D �sD @D���� ��C �FD ��C���� �AD  fD  lD���� �rD  �C  !C���� �oD  kC  �C����  `B ��C  D���� ��C ��C �#D����  �C  dB @1D����  �C  B ��C���� ��C @.D  eD���� �+D  =C  �C���� �
D  �B����  �B  hD  �C����  xC ��C �D����  RC  D �1D���� ��C   B  �B����  :D  B  @A����  kC  �B  YC����  LD   B  �B����  C  EC �,D����  �C  _C @UD���� �wD �pD  iC���� @uD  �B  �C����  vD ��C ��C����  yD �D  �C���� �D ��C �%D����  C ��C �&D���� �rD  C �D���� �>D  �C �%D����  C  �B  �C����  &D ��C �D���� �iD  D  �B���� �D ��C �ID���� �WD ��C �D���� �D �eD  D����  lD �TD  �C����  tB  3D  �A���� �ND  �@  �C���� �JD  B �/D����  �C �;D  VC���� ��C  +D @vD���� �pD  �C �D����  �C  @A  
D���� �5D ��C ��C����  �B  C ��C����  LB @oD �OD���� @D  (D  �C����  �A �UD  �A����  �C �D  {C���� ��C  �C �]D����  �A  �B  0A���� �D �MD ��C����  lB @nD �oD���� @D  $B  �C���� �"D  }C  �C���� �D �ID �D����  qD ��C @;D���� �_D  �C @ND���� �:D �lD �fD����  �C �D �D���� ��C  JD  �C����  �C ��C  �B����  @@  �C  �C����  �B  C  �?����  OD  �C �7D����  �C  ?C @xD���� ��C @D @oD���� �0D  �A  �C���� @D �nD  D����  �A �MD �#D���� ��C  �B �+D����  ID �fD �mD���� �cD ��C  >C����  �C  �B �GD����  $C �dD  @A����  lB @D  C���� ��C  �C @$D����  D  `B  �B���� �ND ��C  �C���� �hD �D �
D  �B����  �C  C ��C����  8C @+D �D���� �jD �<D @D���� �aD  )D  }C���� �CD  �C �HD���� �YD �:D ��C����  �C  �B  C���� �BD  �C  *D����  �B  �C  cC���� �RD  fC  �B���� @3D @nD �/D����  WC  *C  �C����  NC @6D �'D����  �B �FD  IC����   A �&D �dD����  �C @rD @D����  C  �C  �C���� ��C  bD ��C���� @7D @*D  �C����  sD  
C  �C���� @D �VD @QD���� @"D  �@  =D���� �<D  iC �D����  �C �ID @	D����  &C ��C @hD����  �B  �C @gD���� �0D  !D  ID����  FC � D �.D���� �D  jC  {C���� ��C �RD  B����  �C @D @D���� �TD  D  =C���� �tD @sD  �C���� @GD  �B �UD����  C @5D  D����  �@  yD  �C����  hD ��C @>D����  �B  kC ��C����  �C  �C �pD���� @D  �C  �C����  PD �aD  �B����  ^D �8D  D����  �C �D  �C���� ��C  �C �XD����  �C  �C  �C���� �D ��C  �C����  B �aD  B���� ��C  �C �[D���� @(D ��C  �C���� �JD  �C  �B����  �B �D �LD���� �D @D  �C���� @VD @D  xC���� �xD @!D  �C����  �A �ZD ��C���� @PD �*D �_D���� @vD  zC  %D���� �ID ��C  !D����  NC ��C �cD����  JD  -D  �B����  C ��C �_D����  �C  �C @<D����  �C ��C @2D���� �)D  
D  C���� �,D  �C  D���� ��C ��C  !C���� �_D �D  A����  �C  �C ��C���� �D �>D  �?����  �B @XD �:D����  pC  �@  �C���� �D  LB  �B���� @+D  TC @D����  �C  1D @+D����  �C  �C �dD���� �ID  �C  �C���� � D �MD  �C���� ��C  tB  �C���� ��C  wD ��C���� @	D  �C  �C����  VC  AD ��C���� �FD  �C ��C����  bD  /C  D���� ��C �D �oD���� �*D �jD  fD����  �B �5D �cD����  �C  pC  ND����  9D �VD � D���� ��C �0D ��C���� �cD �&D  �A����  C @D  9D����  �C �D �@D���� ��C �YD @eD����  �A  /D � D����  C ��C  UD���� ��C  D  dC����  ND  9D @lD����  DB  �C  �C����  �A ��C  �B���� �eD  ID �7D���� �D  �C @ D���� �D  wC  �A����  TD  C �7D����  UC  D @D����  C @,D �rD���� ��C �1D @CD���� �iD �0D  ZC����  �B @D ��C���� @ZD �2D  �B���� ��C @yD  �B���� �+D  mD  �C���� ��C �xD  �C���� �fD @CD @9D���� ��C  �C  xB���� ��C  @B @D����  wC  �C  @D���� @D �BD @fD����  SC  �B  �B���� �D @FD  �C����  PA  �B �iD����  D  C  C����  �C ��C �HD���� �ND @AD  �B���� �;D  ND @bD����  =C  #D  ZD���� �tD  �C �XD����  7C  )D ��C���� @CD  UD  pD����  HB  �C �nD����  FC  �C �iD����  �C �:D  �C����  �C �D �xD���� �D �D  �C����  �C �WD  {C����  �C  �C ��C����  [D �ID  �A����  �A  �A �$D���� �D  NC �3D����  �C  C  C���� � D  qC  C���� @eD ��C @D���� ��C @+D  CC���� �VD @D �>D���� �	D  KD  �A����  D  nD ��C���� ��C  ,D  :D���� ��C @SD  �C����  �C @lD  xB���� �`D  �B  �C���� �oD @'D  �C���� @DD  �C @AD���� �3D �)D �D����  �B  D  .D���� ��C  `B @BD���� �
D���� �#D @TD  �C����  mC �;D �kD���� � D  D @yD����  !C ��C  �B����  =D �7D  OD����  �C  �B  bD����  �C  C  D���� �/D �:D �BD���� �D  cC ��C���� �AD ��C  �C���� �]D �D �>D����  `A �.D  �C����  �@ ��C @fD���� @0D  VD  $D����  BC  �B  nC����   A   @ @ND���� @nD @iD �D���� �D ��C �kD����  �C  PC �D���� @$D  �C  \D���� @D �yD  �C���� @?D ��C @8D���� ��C ��C ��C����  >D ��C  hD���� @>D ��C  D����      XD ��C����  �B �iD ��C���� @vD �/D  FD����  'C  zC  �B���� @D  �@ @D���� @D  �A �-D���� �bD  �B �wD���� ��C @D  tD���� @.D �AD  �C���� ��C @%D  �B����  �B  RC @[D����  D  �C  D���� �D �6D  CD���� �(D  PA �D����  �C  C �UD���� �kD �dD  D���� �aD  �C  �C����  sD �D �(D����  C  OC ��C����  �C  UD ��C����  oD �VD �cD���� �oD @\D ��C����  C �D ��C����  �C @WD @GD���� �>D @ND  HD���� �\D �9D  6C���� �SD  ZD �;D���� �D �%D  �C����  HB  HD @ID����  �B  �C �$D����  �B �MD  �C���� �aD  �A �OD���� @D �D �dD����  5D  C @\D���� �BD  EC ��C���� �uD  ,C  gC���� ��C �uD  D����  6D �#D �D����  OC  �C �9D���� ��C �AD  �@���� @JD ��C �D����  �B  �C �LD����  �C  �C  eD���� ��C  �C  <B���� �SD �mD  8B���� ��C �7D �jD���� ��C �nD  OD����  �C �$D  8C���� �gD �.D  jC���� �+D �DD  =D����  :D �uD  C���� @nD  �B @
C �hD����  �C  C  pC���� �	D �1D @D����  'D  �C �oD���� �D �vD  D���� @D �`D  �B���� ��C  �C @jD���� @?D  hC  �C���� ��C ��C  <D����  .C  �C �0D���� �WD @eD  {C����  �A  �C  xD����  1D  "C ��C����  �C ��C  �B���� �D  dB ��C���� �>D  �B  D����  �C  �C ��C���� �ID �`D �8D����  �C ��C �6D����  �B  +D @oD���� �RD  <D  ,C���� ��C �D  )C����  �B  lD @eD����  @C �D  7D����  PB @D �D���� ��C ��C �HD����  �B  
D���� ��C �vD @qD���� @D �D �3D����  �A �D  D���� @FD �
D  C���� ��C  �B �+D���� @6D �uD  ZC����   D �hD  �A���� ��C  jC �QD���� ��C ��C @OD����  �C @D  �A����  B  �C ��C���� ��C �jD  �B����  ,C @+D  /D����  8C @gD  HC���� �2D  �C �sD���� ��C  2C  C���� ��C  �C  D����  3D �YD @ZD���� @_D  EC �D���� �[D @%D  �C����  yC  �B �#D����  �C  B  �B����  �C ��C �aD����  "D @uD ��C���� �MD @3D  
D���� �(D  ,C @VD����  �B ��C  �C���� �MD  XC  %D���� �+D @BD  <C���� �D ��C  8D����  C �D  �C���� �3D  �C  �C���� �;D  TC  5C���� �&D �jD  VD���� @vD @0D @
D����  �B  �B �XD���� �5D  D  �C���� �"D �bD  �C����  CC  �C  RC����  8C  �A �mD���� �GD  -D  �B����  �B �!D �:D���� ��C �oD �D���� �[D ��C  D����  �B  OC  �C���� �FD �
D���� @D ��C  �C����  
D����  8D  �B  �C���� �nD  bC ��C����   B  DC  �B����  B �GD �bD���� �
D ��C �
C  �C @

