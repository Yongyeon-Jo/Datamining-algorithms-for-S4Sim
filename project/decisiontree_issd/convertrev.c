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
