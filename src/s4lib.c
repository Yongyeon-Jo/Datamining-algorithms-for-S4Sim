// Smart Solid State System (S4)
// It is a socket server


#include "s4.h"

// unit is pico second
//#define S4_FLASH_READ_LATENCY 200000000
#define S4_FLASH_READ_LATENCY 0

int s4_tick_time; 

void s4_spend_time(int theTick)
{
	s4_tick_time += theTick;
}

void s4_init_simulation()
{
	s4_tick_time = 0;
}

void s4_wrapup_simulation()
{
	FILE* stat = fopen("io_stat.txt","w");
	fprintf(stat, "Exiting @ tick %d\n",s4_tick_time);
}

FILE *
s4_fopen(const char * filename, const char * mode)
{
        return fopen(filename,mode);
}

int
s4_fseek(FILE *stream, long offset, int whence)
{
        return fseek(stream, offset, whence);
}

void
s4_rewind(FILE *stream)
{
        (void)s4_fseek(stream, 0L, SEEK_SET);
}

size_t
s4_fread(void * ptr, size_t size, size_t nitems, FILE * stream)
{
        return fread(ptr,size,nitems,stream);
}

size_t
s4_pageread(size_t pageStartNumber, size_t numPages, FILE * stream)
{
	int n = s4_fread(&s4_buffer[pageStartNumber],sizeof(char), S4_PAGE_SIZE*numPages,stream);

	// modeling for clock tick delay
	// default read time for a page is  internally 200us
	s4_spend_time(S4_FLASH_READ_LATENCY);

	if(n%S4_PAGE_SIZE==0) return n/S4_PAGE_SIZE;
	else return (n/S4_PAGE_SIZE)+1;
}

size_t
s4_fwrite(const void * ptr, size_t size, size_t nitems, FILE * stream)
{
        return fwrite(ptr,size,nitems,stream);
}
