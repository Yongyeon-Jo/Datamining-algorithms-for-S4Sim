#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "isp.h"
#include "s4sim.h"

#define BUFF_SIZE 1024
static char sendBuff[BUFF_SIZE+1];
static char recvBuff[BUFF_SIZE+1];
static int sockfd = 0;
static int callbackfd = 0;
static pthread_t p_thread;
void* callbackFunctionHandler(void* data);

int getTickCycle(const char* theFileName);

void sendS4(const char* theString)
{
	write(sockfd,theString,strlen(theString));
}

int receiveS4()
{
	memset(recvBuff,0,sizeof(recvBuff));
	return read(sockfd, recvBuff, BUFF_SIZE);
}

/* in-storage-processing API's */
isp_int ispGetDeviceIDs(isp_platform_id platform,
        isp_device_type device_type,
        isp_uint num_entries,
        isp_device_id *devices,
        isp_uint* num_devices)
{
	// setup socket connection
	int n = 0;
	struct sockaddr_in serv_addr; 

	if((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		printf("\n Error : Could not create socket \n");
		return 0;
	} 

	if((callbackfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		printf("\n Error : Could not create callback socket \n");
		return 0;
	} 

	memset(&serv_addr, '0', sizeof(serv_addr)); 

	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(5000); 

	if(inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr)<=0)
	{
		printf("\n inet_pton error occured\n");
		return 0;
	} 


	if( connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
	{
		printf("\n Error : Connect Failed \n");
		return 0;
	} 

	if( connect(callbackfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
	{
		printf("\n Error : Callback Connect Failed \n");
		return 0;
	} 

	// callback function handler thread is starting
	pthread_create(&p_thread, NULL, callbackFunctionHandler, NULL);

	*devices = (isp_device_id) sockfd;

	return 1;
}

// download a script string to a storage. It is appended to the existing scripts
isp_int ispAddScript(isp_device_id device_id,
        const char* script_string)
{
	sendS4("AddScript");
	receiveS4();
	//printf("%s\n",recvBuff);
//	printf("send in host %s\n",script_string);
	sendS4(script_string);
	receiveS4();
	//printf("%s\n",recvBuff);
	return 1;
}

// remove whole scripts in the storage
isp_int ispClearScript(isp_device_id device_id)
{
	sendS4("ClearScript");
	receiveS4();
	return 1;
}

// ispSetActorArgument provides data values to actors.
// For instance, it provides LBA list.
// Maybe some actors receive these data through callback functions.
isp_int ispSetActorArgument(isp_device_id device_id,
        const char* instance_name,      // actor instance name
        const char* argument_name,
        void* argument,
        int size_of_argument)
{
	return 1;
}

#define MAX_REG_FUNCS 1024
typedef struct {
	char name[1024];
        void(*func)(isp_device_id,void* data, int* data_size);
} CallbackFunctionType;
static CallbackFunctionType mCallbackFunctions[MAX_REG_FUNCS];
static int mNumCallbackFunctions = 0;

// callback function handler thread
void* callbackFunctionHandler(void* data)
{
	char buffer[BUFF_SIZE];
	char nodeName[BUFF_SIZE],funcName[BUFF_SIZE], size[BUFF_SIZE], value[BUFF_SIZE];
	int n;
	memset(buffer,0,BUFF_SIZE);
	while((n=read(callbackfd, buffer, BUFF_SIZE))>0) {
		if(strcmp("send callbackFunction",buffer)==0) {
			sprintf(buffer,"OK");
			write(callbackfd, buffer, 3);
		
			// receive node,funcName
			read(callbackfd,nodeName,BUFF_SIZE);
			write(callbackfd, buffer, 3);
			read(callbackfd,funcName,BUFF_SIZE);
			write(callbackfd, buffer, 3);
			read(callbackfd,size,BUFF_SIZE);
			write(callbackfd, buffer, 3);
			read(callbackfd,value,BUFF_SIZE);
			write(callbackfd, buffer, 3);

			sprintf(buffer,"%s_%s",nodeName,funcName);

			// call callback function
			int i;
			for(i=0; i<mNumCallbackFunctions; i++) {
				if(strcmp(mCallbackFunctions[i].name,buffer)==0) {
					int valueSize = atoi(size);
					mCallbackFunctions[i].func(1,(void*)value,&valueSize);
					break;
				}
			}
		} else if(strcmp("receive callbackFunction",buffer)==0) {
			sprintf(buffer,"OK");
			write(callbackfd, buffer, 3);
		
			// receive node,funcName
			read(callbackfd,nodeName,BUFF_SIZE);
			write(callbackfd, buffer, 3);
			read(callbackfd,funcName,BUFF_SIZE);
			write(callbackfd, buffer, 3);
			read(callbackfd,size,BUFF_SIZE);

			sprintf(buffer,"%s_%s",nodeName,funcName);

			// call callback function
			int i;
			for(i=0; i<mNumCallbackFunctions; i++) {
				if(strcmp(mCallbackFunctions[i].name,buffer)==0) {
					int valueSize = atoi(size);
					mCallbackFunctions[i].func(1,(void*)value,&valueSize);

					sprintf(buffer,"%d",valueSize);
					write(callbackfd, buffer, sizeof(int));

					read(callbackfd,size,BUFF_SIZE);
					write(callbackfd, value, valueSize);
					
					break;
				}
			}
		} else if(strcmp("s4_fopen",buffer)==0) {
			char filename[BUFF_SIZE],mode[BUFF_SIZE];

			sprintf(buffer,"OK");
			write(callbackfd, buffer, 3);
		
			// fopen(filename,mode)
			read(callbackfd,filename,BUFF_SIZE);
			write(callbackfd, buffer, 3);

			read(callbackfd,mode,BUFF_SIZE);

			FILE* file = fopen(filename,mode);
			write(callbackfd, (char*)file, sizeof(FILE*));
		} else if(strcmp("s4_fread",buffer)==0) {
			size_t size, nitems;
			FILE* stream;

			sprintf(buffer,"OK");
			write(callbackfd, buffer, 3);
		
			// fread(/*void * ptr*/, size_t size, size_t nitems, FILE * stream)
			read(callbackfd,(char*)&size,sizeof(size_t));
			write(callbackfd, buffer, 3);

			read(callbackfd,(char*)&nitems,sizeof(size_t));
			write(callbackfd, buffer, 3);

			read(callbackfd,(FILE*)&stream,sizeof(FILE*));

			// FIXME : we need to compute a LBA in a real board.
			char ptr[BUFF_SIZE];
			fread(ptr,size,nitems,stream);
			write(callbackfd, &ptr, sizeof(int));
		}
	}
}
			
// callback function is called by actor instance
void ispRegisterCallbackFunction(isp_device_id device_id,
        const char* instance_name,                                      // actor instance name
        const char* func_name,                                          // callback function name in the actor
        void(*func)(isp_device_id,void* data, int* data_size))       // callback function pointer in host
{
	sprintf(mCallbackFunctions[mNumCallbackFunctions].name,"%s_%s",instance_name,func_name); 
	mCallbackFunctions[mNumCallbackFunctions].func = func;

	mNumCallbackFunctions++;

//	return mNumCallbackFunctions;
}

// It supports two modes of execution: release and debug.
// If debug mode is true then print callback function is called when a message is arrived.
// After the callback function is resolved, execution of the script continues.
isp_int ispRunScript(isp_device_id device_id,
        int isp_debug_mode,
        void(*printFunction)(char* message))
{
	sendS4("RunScript");
	while(1) {
		receiveS4();
		if(strcmp("OK",recvBuff)==0) break;
	}
	printf("%s\n",recvBuff);
	return 1;
}

isp_int ispExit(isp_device_id device_id)
{
	sendS4("exit");

	return 1;
}

// utility functions
isp_int ispGetLBAList(isp_device_id device_id, 
        const char* fileName, 
        isp_lba_list* lba_list)

{
	lba_list->start[0] = 0;
	lba_list->end[0] = 1;
	lba_list->size = 1;
}


// run the downloaded binary program in ISSD
// simulation implementation
isp_int ispRunBinaryFile(isp_device_id device_id, const char* program_file_name, const char* program_argument, const char* program_output_file)
{
	char command[1024];
	if(program_argument)
		sprintf(command,"%s %s -n %d -c %s -o \"%s \" --output=%s > gem5_result.txt",GEM5_EXECFILE, GEM5_PLATFORM, GEM5_NUMPROCS,program_file_name, program_argument, program_output_file);
	else
		sprintf(command,"%s %s -n %d -c %s --output=%s > gem5_result.txt", GEM5_EXECFILE, GEM5_PLATFORM, GEM5_NUMPROCS, program_file_name, program_output_file);

	printf("%s\n",command);
	system(command);

	// read gem5_result.txt to obtain the isp program cycles and return it.
	// TODO ...
	int cycle;
	cycle = getTickCycle("gem5_result.txt");
	cycle += getTickCycle("io_stat.txt");

	return cycle;

}


int getTickCycle(const char* theFileName)
{
	char command[1024];

	sprintf(command, "grep Exiting %s | sed 's/^[A-z @]*//' > gem5_cycle.txt", theFileName);
	system(command);
	FILE* ifp;
	char buffer[1024];
	ifp = fopen("gem5_cycle.txt","r");
	int c, index=0;
	while((c=fgetc(ifp))!=EOF) {
		if(!isdigit(c)) {
			break;
		}

		buffer[index] = c;
		++index;
	}

	buffer[index] = 0;
	
	int cycle = 0;
	if(index>0) cycle = atoi(buffer);
	system("rm gem5_cycle.txt");
		

	sprintf(command,"rm %s", theFileName);
//	system(command);

	return cycle; 
}


