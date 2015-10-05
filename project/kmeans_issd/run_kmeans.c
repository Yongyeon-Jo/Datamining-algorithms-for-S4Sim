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
	char str1[1024];
	char str2[1024];
	char str3[1024];
	char str4[1024];
	char str5[1024];
	char cpuhz[16];
	int numcpu=issd_numcpu;
	int clock=issd_clock;
	sprintf(cpuhz, "%dMHz", clock);
	sprintf(str1, "cp m5out/stats.txt m5out/stats_%d_%s_read.txt", numcpu, cpuhz);
	sprintf(str2, "cp m5out/stats.txt m5out/stats_%d_%s_setmid.txt", numcpu, cpuhz);
	sprintf(str5, "cp m5out/stats.txt m5out/stats_%d_%s_write.txt", numcpu, cpuhz);
	
	cycle = ispRunBinaryFileEx(device, "./kmeans_isp_read", NULL, "output.txt", numcpu, cpuhz);
	system(str1);
	cycle = ispRunBinaryFileEx(device, "./kmeans_isp_setmid", "1", "output.txt", numcpu, cpuhz);
	system(str2);
	for(i=0;i<30;i++){
		sprintf(str3, "cp m5out/stats.txt m5out/stats_%d_%s_setclust_%d.txt", numcpu, cpuhz, i+1);
		sprintf(str4, "cp m5out/stats.txt m5out/stats_%d_%s_calcmid_%d.txt", numcpu, cpuhz, i+1);
		cycle = ispRunBinaryFileEx(device, "./kmeans_isp_setclust", NULL, "output.txt", numcpu, cpuhz);
		system(str3);
		cycle = ispRunBinaryFileEx(device, "./kmeans_isp_calcmid", NULL, "output.txt", numcpu, cpuhz);
		system(str4);
	}
	cycle = ispRunBinaryFileEx(device, "./kmeans_isp_write", NULL, "output.txt", numcpu, cpuhz);
	system(str5);

	printf("ISP cycle = %d\n", cycle);
}
