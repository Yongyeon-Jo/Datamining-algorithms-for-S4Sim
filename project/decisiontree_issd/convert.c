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
