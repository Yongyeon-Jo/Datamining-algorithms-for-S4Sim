
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
