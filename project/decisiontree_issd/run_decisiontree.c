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
