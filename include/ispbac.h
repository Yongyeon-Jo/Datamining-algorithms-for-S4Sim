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
