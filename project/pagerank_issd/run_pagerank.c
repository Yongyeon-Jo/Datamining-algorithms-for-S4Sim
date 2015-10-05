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
	char cmd[128];
	char pname[32];
	char funcname[32];
	int numcpu=issd_numcpu;
	int clock=issd_clock;
	sprintf(cpuhz, "%dMHz", clock);
	sprintf(pname, "./pagerank_isp_setr0");
	sprintf(cmd, "cp ./m5out/stats.txt ./m5out/pagerank_setr0_%d_%s.txt", numcpu, cpuhz);
	cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
	system(cmd);
	for(i=0;i<28;i++){
		sprintf(pname, "./pagerank_isp_calcendrank");
		sprintf(cmd, "cp ./m5out/stats.txt ./m5out/pagerank_calcendrank_%d_%s_%d.txt", numcpu, cpuhz, i+1);

		cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
		system(cmd);

		sprintf(pname, "./pagerank_isp_setthreadval");
		sprintf(cmd, "cp ./m5out/stats.txt ./m5out/pagerank_setthreadval_%d_%s_%d.txt", numcpu, cpuhz, i+1);
		cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
		system(cmd);

		sprintf(pname, "./pagerank_isp_updaterank");
		sprintf(cmd, "cp ./m5out/stats.txt ./m5out/pagerank_updaterank_%d_%s_%d.txt", numcpu, cpuhz, i+1);
		cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
		system(cmd);

		sprintf(pname, "./pagerank_isp_checkvec");
		sprintf(cmd, "cp ./m5out/stats.txt ./m5out/pagerank_checkvec_%d_%s_%d.txt", numcpu, cpuhz, i+1);
		cycle = ispRunBinaryFileEx(device, pname, NULL, "output.txt", numcpu, cpuhz);
		system(cmd);

		system("rm rankcsr");
		system("cp rankcsrupdate rankcsr");
	}
	sprintf(cmd, "cp rankcsr rankcsr_%d_%s", numcpu, cpuhz);
	system(cmd);
	system("rm rankcsr");
	printf("ISP cycle = %d\n", cycle);
return 0;
}
