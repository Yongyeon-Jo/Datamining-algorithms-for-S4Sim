#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#define MAX_ATTR_NUM 19
#define MAX_ATTR_VAL 33
#define MAX_INFO_VAL 10
#define TRAIN_N 700
#define TEST_N 8500
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

typedef struct calcinfostruct{
	float* info;
	value* val;
	treenode* node;
	int snum;
	int nnum;
}calcinfostruct;

typedef struct checkleafnodestruct{
	int* res;
	value* val;
	int firstnum;
	int startnum;
	int num;
}checkleafnodestruct;

typedef struct teststruct{
	value* val;
	int startnum;
	int num;
	dicisiontree* tree;
}teststruct;



int subnum[MAX_ATTR_NUM]=ATTR_MAX;
void readsubinfo(float dest[][MAX_ATTR_VAL], FILE* fp){
	fread(dest, MAX_ATTR_VAL*MAX_ATTR_NUM, sizeof(float), fp);
}

void savesubinfo(float dest[][MAX_ATTR_VAL], FILE* fp){
	fwrite(dest, MAX_ATTR_VAL*MAX_ATTR_NUM, sizeof(float), fp);
}

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
















void* checkleafnodefunc(void* thearg){
	checkleafnodestruct* arg=(checkleafnodestruct*) thearg;
	int* res=arg->res;
	value* val=arg->val;
	int firstnum=arg->firstnum;
	int startnum=arg->startnum;
	int num=arg->num;
	int rres=0;
	int i;
	for(i=startnum;i<startnum+num;i++){
		if(rres==0){
			if(val[firstnum].res!=val[i].res){
				rres=1;
				break;
			}
		}
	}

	*res=rres;
}

int checkleafnode(value* val, dicisiontree* tree){
	treenode* node=&tree->node[++tree->num];
	int i;

	int res[GEM5_NUMPROCS];
	int rres=0;

	int rest=node->num%(GEM5_NUMPROCS-1);
	int snum=node->startnum;
	pthread_t thread[GEM5_NUMPROCS];
	checkleafnodestruct structs[GEM5_NUMPROCS];
	
	if(node->num==0)
		return 0;
	else{
		if(node->num<GEM5_NUMPROCS){
			for(i=0;i<node->num;i++){
				structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				structs[i].num=1;
				snum++;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<node->num;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<node->num;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		else{
			for(i=0;i<GEM5_NUMPROCS;i++){structs[i].res=&res[i];
				structs[i].val=val;
				structs[i].firstnum=node->startnum;
				structs[i].startnum=snum;
				if(rest==0){
					structs[i].num=node->num/(GEM5_NUMPROCS-1);
				}
				else{
					structs[i].num=node->num/(GEM5_NUMPROCS-1)+1;
					rest--;
				}
				snum+=structs[i].num;
				pthread_create(&thread[i], NULL, checkleafnodefunc, (void*)&structs[i]);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				pthread_join(thread[i], NULL);
			}
			for(i=0;i<GEM5_NUMPROCS;i++){
				if(rres==0){
					rres=res[i];
				}
			}
		}
		if(rres==0)
			node->treeval=val[node->startnum].res;
	}
	return rres;
}

void* calcinfofunc(void* thearg){
	calcinfostruct* arg=(calcinfostruct*)thearg;
	float* dest=arg->info;
	value* val=arg->val;
	treenode* node=arg->node;
	int snum=arg->snum;
	int num=arg->nnum;
	int i, j, k;
	int n;
	int attnum[MAX_ATTR_NUM][MAX_ATTR_VAL]={0,};
	int attnumval[MAX_ATTR_NUM][MAX_ATTR_VAL][MAX_INFO_VAL]={0,};
	float sum, psum, p, fnum, anum;
	value* pval;

	for(i=snum;i<snum+num;i++){

		for(j=node->startnum;j<node->startnum+node->num;j++){
		pval=&val[j];
			attnumval[i][pval->attr[i]][pval->res]++;
			attnum[i][pval->attr[i]]++;
		}

		sum=0.0f;
		fnum=(float)node->num;
		for(j=0;j<MAX_ATTR_VAL;j++){
			psum=0.0f;
			anum=(float)attnum[i][j];
			for(k=0;k<MAX_INFO_VAL;k++){
				n=attnumval[i][j][k];
				if(n!=0&&n!=attnum[i][j]){
					p=(float)n/anum;
					p=p*(log(p)/log(2.0f));
					psum-=p;
				}
			}
			sum+=psum*anum/fnum;
		}
		dest[i]=sum;
	}
}

void calcinfo(value* val, dicisiontree* tree, float* info){
	int i;
	int snum=0;
	int nnum;
	treenode* node=&tree->node[tree->num];

	int rest=MAX_ATTR_NUM%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcinfostruct structs[GEM5_NUMPROCS];

	if(MAX_ATTR_NUM<GEM5_NUMPROCS){
		for(i=0;i<MAX_ATTR_NUM;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			structs[i].nnum=1;
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum++;
		}
		for(i=0;i<MAX_ATTR_NUM;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].info=info;
			structs[i].val=val;
			structs[i].node=node;
			structs[i].snum=snum;
			if(rest==0){
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].nnum=MAX_ATTR_NUM/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			pthread_create(&thread[i], NULL, calcinfofunc, (void*)&structs[i]);
			snum+=structs[i].nnum;
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}

void compareinfo(value* val, dicisiontree* tree, float* info){
	treenode* node=&tree->node[tree->num];
	int i;
	int sel;
	float self=0xFFFFFFFF;

	for(i=0;i<MAX_ATTR_NUM;i++){
		if(node->flag[i]==0){
			if(self>info[i]){
				sel=i;
				self=info[i];
			}
		}
	}
	node->attnum=sel;
	node->flag[sel]=1;
}

void dividesection(value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	value vallist[TRAIN_N][MAX_ATTR_VAL];
	int i, j, k;

	for(i=node->startnum;i<node->startnum+node->num;i++){
		j=val[i].attr[node->attnum];
		vallist[node->listcount[j]][j]=val[i];
		node->listcount[j]++;
	}
	i=node->startnum;
	while(i<node->startnum+node->num){
		for(j=0;j<MAX_ATTR_VAL;j++){
			for(k=0;k<node->listcount[j];k++){
				val[i]=vallist[k][j];
				i++;
			}
		}
	}
}

void makesubtree(float subinfo[][MAX_ATTR_VAL], value* val, dicisiontree* tree){
	treenode* node=&tree->node[tree->num];
	treenode* nextnode;
	int i;
	int num=0;
	node->subnum=subnum[node->attnum];
	for(i=0;i<node->subnum;i++){
		nextnode=&tree->node[tree->maxnum];
		memcpy(nextnode, node, sizeof(treenode));
		nextnode->startnum=node->startnum+num;
		nextnode->num=node->listcount[i];
		memset(nextnode->listcount, 0, 4*MAX_ATTR_VAL);
		num+=nextnode->num;
		node->subptr[i]=tree->maxnum;
		tree->maxnum++;
		nextnode->info=subinfo[node->attnum][i];
	}
}







void* testfunc(void* thearg){
	teststruct* arg=(teststruct*)thearg;
	int i;
	int res;
	value* vval;
	treenode* node;
	value* val=arg->val;
	int startnum=arg->startnum;
	int num=arg->num;
	dicisiontree* tree=arg->tree;

	for(i=startnum;i<startnum+num;i++){
		vval=&val[i];
		node=&tree->node[0];
		while(node->treeval<0){
			if(node->num==0)
				break;
			node=&tree->node[node->subptr[vval->attr[node->attnum]]];
		}
		vval->res=node->treeval;
	}
}

void test(value* val, dicisiontree* tree){
	int i;

	int rest=TEST_N%(GEM5_NUMPROCS-1);
	int count=0;
	pthread_t thread[GEM5_NUMPROCS];
	teststruct structs[GEM5_NUMPROCS];
	if(TEST_N<GEM5_NUMPROCS){
		for(i=0;i<TEST_N;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			structs[i].num=1;
			structs[i].tree=tree;
			count++;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<TEST_N;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].val=val;
			structs[i].startnum=count;
			if(rest==0){
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1);
			}
			else{
				structs[i].num=TEST_N/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			structs[i].tree=tree;
			count+=structs[i].num;
			pthread_create(&thread[i], NULL, testfunc, (void*)&structs[i]);
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}









int main(){
	value val[TRAIN_N];
	FILE* ftree=fopen("tree", "rb");
	FILE* fval=fopen("val", "rb");
	dicisiontree tree;
	FILE* finfo=fopen("fsubinfo", "rb");
	float subinfo[MAX_ATTR_NUM][MAX_ATTR_VAL];
	
	readsubinfo(subinfo, finfo);
	fclose(finfo);
	
	readtree(&tree, ftree);
	readvalb(val, fval, TRAIN_N);

	makesubtree(subinfo, val, &tree);
	
	fclose(ftree);
	fclose(fval);
	ftree=fopen("tree", "wb");
	savetree(&tree, ftree);
	fclose(ftree);

	return 0;
}
