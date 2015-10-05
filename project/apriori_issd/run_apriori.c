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
