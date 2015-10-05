#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define TRAN 10000
#define ITEM 20
#define LENGTH 10
#define MIN 300

#define GEM5_NUMPROCS 4

typedef struct aprioriset{
	int length;
	int support;
	char value[LENGTH];
}aprioriset;

typedef struct aprioristruct{
	int num;
	aprioriset valuelist[10000];
}aprioristruct;

typedef struct gencreturnstruct{
	int num;
	aprioriset valuelist[ITEM];
	int proper[ITEM];
}gencreturnstruct;

typedef struct gencstruct{
	aprioristruct* l;
	aprioriset* s;
	gencreturnstruct ret;
	int num;
	int length;
}gencstruct;

typedef struct genlstruct{
	aprioriset* c;
	aprioristruct* data;
}genlstruct;

typedef struct aprioriassvalue{
	char left[LENGTH];
	char right[LENGTH];
	float support;
	float confidence;
}aprioriassvalue;

typedef struct aprioriassstruct{
	int num;
	aprioriassvalue aprioriasslist[10000];
}aprioriassstruct;

typedef struct associationstruct{
	aprioriassvalue* dest;
	aprioriset* left;
	aprioriset* right;
}associationstruct;

double readtime, makec1time, makectime, makeltime, mergetime, asstime, writetime;

void readapriorib(aprioristruct* data, FILE* fp){
	data->num=TRAN;
	fread(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void saveapriorib(aprioristruct* data, FILE* fp){
	fwrite(data->valuelist, sizeof(aprioriset), TRAN, fp);
}

void readapriorinnb(aprioristruct* data, FILE* fp){
	fread(data, sizeof(aprioristruct), 1, fp);
}

void saveapriorinnb(aprioristruct* data, FILE* fp){
	fwrite(data, sizeof(aprioristruct), 1, fp);
}

void saveassstructb(aprioriassstruct* dest, FILE* fp){
	fwrite(dest->aprioriasslist, sizeof(aprioriassvalue), dest->num, fp);
}

void saveassstructnnb(aprioriassstruct* dest, FILE* fp){
	fwrite(dest, sizeof(aprioriassstruct), 1, fp);
}

void insertion(char* buf, char val, int length){
	int i, j;
	for(i=0;i<length;i++){
		if(buf[i]>val){
			break;
		}
	}
	for(j=length;j>i;j--){
		buf[j]=buf[j-1];
	}
	buf[i]=val;
}

int checkbuf(char* buf, char val, int length){
	int a;
	for(a=0;a<length;a++){
		if(buf[a]==val){
			return 1;
		}
	}
	return 0;
}

void initaprioriset(aprioriset** data, int length){
	(*data)=(aprioriset*)malloc(sizeof(aprioriset));
	(*data)->support=0;
	(*data)->length=length;
}

void deleteaprioriset(aprioriset** data){
//	free((*data)->value);
//	free(*data);
}

int compareset(aprioriset* a, aprioriset* b){
	if(a->length<b->length)
		return 1;
	else if(b->length<a->length)
		return 0;
	else{
		if(strncmp(a->value, b->value, a->length)<0)
			return 1;
		else return 0;
	}
}

int numofmatch(aprioriset* a, aprioriset* b){
	int ret=0;
	int acount=0, bcount=0;
	while(1){
		if(acount==a->length||bcount==b->length)
			break;
		if(a->value[acount]==b->value[bcount]){
			acount++;
			bcount++;
			ret++;
		}
		else if(a->value[acount]>b->value[bcount])
			bcount++;
		else
			acount++;
	}
	return ret;
}

int isequal(aprioriset* a, aprioriset* b){
	if(a->length==b->length)
		if(numofmatch(a, b)==a->length)
			return 1;
	return 0;
}

void add(aprioristruct* data, aprioriset* value, int equal){
	int i;
	if(equal){
		for(i=0;i<data->num;i++){
			if(isequal(value, &data->valuelist[i])){
				deleteaprioriset(&value);
				return;
			}
		}
	}
	data->valuelist[data->num]=*value;
	data->num++;
}

void mergeset(aprioriset* res, aprioriset* a, aprioriset* b){
	int i;
	char newval;
	for(i=0;i<a->length;i++){
		res->value[i]=a->value[i];
	}
	for(i=0;i<b->length;i++){
		if(checkbuf(res->value, b->value[i], a->length)==0){
			newval=b->value[i];
			break;
		}
	}
	insertion(res->value, newval, b->length);
}

int issubset(aprioriset* large, aprioriset* small){
	int largecount=0;
	int smallcount=0;
	char cl, cs;
	if(small->length>large->length)
		return 0;
	while(smallcount<small->length){
		if(largecount>=large->length)
			return 0;
		cl=large->value[largecount];
		cs=small->value[smallcount];
		if(cl==cs){
			largecount++;
			smallcount++;
		}
		else if(cl<cs)
			largecount++;
		else
			return 0;
	}
	return 1;
}

int isproper(aprioriset* set, aprioristruct* str){
	aprioriset* tset;
	int strcount;
	int setcount=set->length-1;
	initaprioriset(&tset, setcount);
	int i;
	for(i=0;i<set->length-1;i++){
		if(i==setcount)
			continue;
		else if(i<setcount){
			tset->value[i]=set->value[i];
		}
		else{
			tset->value[i-1]=set->value[i];
		}
	}
	
	for(strcount=0;strcount<str->num;strcount++){
		if(isequal(tset, &str->valuelist[strcount])){
			if(setcount==0)
				return 1;
			else{
				setcount--;
				for(i=0;i<set->length;i++){
					if(i==setcount)
						continue;
					else if(i<setcount){
						tset->value[i]=set->value[i];
					}
					else{
						tset->value[i-1]=set->value[i];
					}
				}
			}
		}
	}
	return 0;
}

void loadapriorifromfile(aprioristruct* data, FILE* fp){
	aprioriset* nowdata;
	int len, i, j;
	for(j=0;j<TRAN;j++){
		fscanf(fp, "%*s %*s %*s %d %*s", &len);
		initaprioriset(&nowdata, len);
		for(i=0;i<len;i++){
			fscanf(fp, "%*c %c", &(nowdata->value[i]));
		}
		add(data, nowdata, 0);
	}
}

void loadapriorifromfileb(aprioristruct* dest, FILE* fp){
	fread(dest, sizeof(aprioristruct), 1, fp);
}

void saveaprioritofile(aprioristruct* data, FILE* fp){
	int length;
	int i;
	int j;
	for(i=0;i<data->num;i++){
		length=data->valuelist[i].length;
		fprintf(fp, "LEN %02d SUP %03d :", length, data->valuelist[i].support);
		for(j=0;j<length;j++){
			fprintf(fp, " %c", data->valuelist[i].value[j]);
		}
		fprintf(fp, "\n");
	}
}

void saveaprioritofileb(aprioristruct* dest, FILE* fp){
	fwrite(dest, sizeof(aprioristruct), 1, fp);
}

void makec1(aprioristruct* target, aprioristruct* data){
	char itemlist[ITEM];
	int i=0, j, k;
	aprioriset* nowdata;
	for(k=0;k<data->num;k++){
		if(i==ITEM) break;
		nowdata=&data->valuelist[k];
		for(j=0;j<nowdata->length;j++){
			if(checkbuf(itemlist, nowdata->value[j], ITEM)==0){
				if(i==0)
					itemlist[0]=nowdata->value[j];
				else{
					insertion(itemlist, nowdata->value[j], i);
				}
				i++;
			}
		}
	}
	for(j=0;j<i;j++){
		initaprioriset(&nowdata, 1);
		nowdata->value[0]=itemlist[j];
		add(target, nowdata, 1);
	}
}

void* genlthreadfunc(void* thearg){
	genlstruct* arg=(genlstruct*)thearg;
	int datacount;
arg->c->support=0;
	for(datacount=0;datacount<arg->data->num;datacount++){
		if(issubset(&arg->data->valuelist[datacount], arg->c)){
			arg->c->support++;
		}
	}
	return NULL;
}

void genL(aprioristruct* l, aprioristruct* c, aprioristruct* data, int minnum){
	int ccount=0, cdatacount=0, datacount, i, j;
	aprioriset* nowdata;
	pthread_t thread[GEM5_NUMPROCS];
	genlstruct* genlstructs=(genlstruct*)malloc(sizeof(genlstruct)*c->num);
	if(c->num<GEM5_NUMPROCS){
		for(ccount=0;ccount<c->num;ccount++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
		}
		for(ccount=0;ccount<c->num;ccount++){
			pthread_join(thread[ccount], NULL);
		}
		for(ccount=0;ccount<c->num;ccount++){
			nowdata=&c->valuelist[ccount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			genlstructs[ccount].c=&c->valuelist[ccount];
			genlstructs[ccount].data=data;
			pthread_create(&thread[ccount], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
			ccount++;
		}
		while(ccount<c->num){
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				pthread_join(thread[i], NULL);	
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				nowdata=&c->valuelist[cdatacount];
				if(nowdata->support<minnum){
					deleteaprioriset(&nowdata);
				}
				else{
					add(l, nowdata, 1);
				}
				cdatacount++;
			}
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				genlstructs[ccount].c=&c->valuelist[ccount];
				genlstructs[ccount].data=data;
				pthread_create(&thread[i], NULL, genlthreadfunc, (void*)&genlstructs[ccount]);
				ccount++;
				if(ccount==c->num)
					break;
			}
		}
		i++;
		for(j=0;j<i;j++){
			pthread_join(thread[j], NULL);
		}
		for(j=0;j<i;j++){
			nowdata=&c->valuelist[cdatacount];
			if(nowdata->support<minnum){
				deleteaprioriset(&nowdata);
			}
			else{
				add(l, nowdata, 1);
			}
			cdatacount++;
			if(cdatacount==c->num)
				break;
		}
	}
	c->num=0;
	free(genlstructs);
}



void* gencthreadfunc(void* thearg){
	gencstruct* arg=(gencstruct*)thearg;
	gencreturnstruct* ret=&arg->ret;
	aprioriset tset;
	aprioriset* pset=&tset;

	int i, j=0, k, flag, ccount=0;

	ret->num=0;
	for(i=arg->num;i<arg->l->num;i++){
		flag=0;
		if(numofmatch(&arg->l->valuelist[i], arg->s)==arg->length-1){
			mergeset(pset, &arg->l->valuelist[i], arg->s);
				pset->length=arg->length+1;
			for(k=0;k<j;k++){
				if(isequal(pset, &ret->valuelist[k])){
					flag=1;
					break;
				}
			}
			if(flag){
				deleteaprioriset(&pset);
			}
			else{
				ret->valuelist[j]=*pset;
				ret->proper[j]=isproper(&ret->valuelist[j], arg->l);
				ret->num++;
				j++;
			}
		}
	}
}

void genC(aprioristruct* c, aprioristruct* l){
	pthread_t thread[GEM5_NUMPROCS];
	gencstruct strarg[GEM5_NUMPROCS];
	gencstruct strarg2[GEM5_NUMPROCS];
	gencstruct* parg;
	int length=l->valuelist[0].length;
	int threadrun[GEM5_NUMPROCS]={0,};
	int threadrun2[GEM5_NUMPROCS]={0,};
	int* pthreadrun;
	int i, j, count=0, toggle=0, ccount=0;

	if(l->num<GEM5_NUMPROCS){
		for(i=0;i<l->num;i++){
			strarg[i].l=l;
			strarg[i].s=&l->valuelist[i];
			strarg[i].length=length;
			strarg[i].num=i+1;
			pthread_create(&thread[i], NULL, gencthreadfunc, (void*)&strarg[i]);
		}
		for(i=0;i<l->num;i++){
			pthread_join(thread[i], NULL);
		}
		for(i=0;i<l->num;i++){
			for(j=0;j<strarg[i].ret.num;j++){
				if(strarg[i].ret.proper[j]){
					add(c, &strarg[i].ret.valuelist[j], 1);
				}
			}
		}
	}
	else{
		for(count=0;count<GEM5_NUMPROCS-1;count++){
			strarg[count].l=l;
			strarg[count].s=&l->valuelist[count];
			strarg[count].length=length;
			strarg[count].num=count+1;
			pthread_create(&thread[count], NULL, gencthreadfunc, (void*)&strarg[count]);
			ccount++;
		}
		toggle=1;
		while(count<l->num){
			if(ccount==GEM5_NUMPROCS-1){
				for(i=0;i<GEM5_NUMPROCS-1;i++){
					pthread_join(thread[i], NULL);
					
				}
				ccount=0;
			}
			parg=toggle?strarg2:strarg;
			pthreadrun=toggle?threadrun2:threadrun;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				(parg+i)->l=l;
				(parg+i)->s=&l->valuelist[count];
				(parg+i)->length=length;
				(parg+i)->num=count+1;
				pthread_create(&thread[i], NULL, gencthreadfunc, (void*)(parg+i));
				count++;
				ccount++;
				*(pthreadrun+i)=1;
				if(count==l->num)
					break;
			}
			parg=toggle?strarg:strarg2;
			pthreadrun=toggle?threadrun:threadrun2;
			toggle=!toggle;
			for(i=0;i<GEM5_NUMPROCS-1;i++){
				*(pthreadrun+i)=0;
				for(j=0;j<(parg+i)->ret.num;j++){
					if((parg+i)->ret.proper[j]){
						add(c, &(parg+i)->ret.valuelist[j], 1);
					}
				}
			}
		}
		pthreadrun=toggle?threadrun:threadrun2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			pthread_join(thread[i], NULL);
		}
		parg=toggle?strarg:strarg2;
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			if(*(pthreadrun+i)==0){
				break;
			}
			for(j=0;j<(parg+i)->ret.num;j++){
				if((parg+i)->ret.proper[j]){
					add(c, &(parg+i)->ret.valuelist[j], 1);
				}
			}
		}
	}
}
void mergestruct(aprioristruct* target, aprioristruct* l){
	int i;
	for(i=0;i<l->num;i++){
		add(target, &l->valuelist[i], 1);
	}
}

void* getassociationrulefunc(void* thearg){
	associationstruct* assstruct=(associationstruct*)thearg;
	aprioriset* left=assstruct->left, *right=assstruct->right;
	int k=0, l=0, m=0;
	for(k=0;k<right->length;k++){
		if(checkbuf(left->value, right->value[k], left->length)){
			assstruct->dest->left[l]=right->value[k];
			l++;
		}
		else{
			assstruct->dest->right[m]=right->value[k];
			m++;
		}
	}
	assstruct->dest->support=((float)right->support)/((float)TRAN);
	assstruct->dest->confidence=((float)right->support)/((float)left->support);
}

void getassociationrule(aprioriassstruct* dest, aprioristruct* list){
	aprioriset* right, *left;
	int i, j, k;
	pthread_t thread[GEM5_NUMPROCS];
	associationstruct assstruct[GEM5_NUMPROCS];
	int proccount=0;
	int threadrun[GEM5_NUMPROCS]={0,};
	for(i=0;i<list->num;i++){
		right=&list->valuelist[i];
		if(right->length<2)
			continue;
		for(j=0;j<list->num;j++){
			left=&list->valuelist[j];
			if(right->length==left->length)
				break;
			if(issubset(right, left)){

				if(proccount==GEM5_NUMPROCS-1){
					for(k=0;k<proccount;k++){
						pthread_join(thread[k], NULL);
						threadrun[k]=0;
					}
					proccount=0;
				}
				assstruct[proccount].dest=&dest->aprioriasslist[dest->num];
				assstruct[proccount].left=left;
				assstruct[proccount].right=right;
				pthread_create(&thread[proccount], NULL, getassociationrulefunc, (void*)&assstruct[proccount]);
			
				threadrun[proccount]=1;
				dest->num++;
				proccount++;
			}
		}
	}
	for(i=0;i<proccount;i++){
		pthread_join(thread[i], NULL);
	}
}

int apriori(){
	aprioristruct result;
	aprioriassstruct ass={0,};
	FILE* input=fopen("merged", "rb");
	FILE* output=fopen("ass", "wb");
	readapriorinnb(&result, input);
getassociationrule(&ass, &result);

	saveassstructnnb(&ass, output);
fclose(output);
	fclose(input);
	return 0;
}

int main(){
	apriori();
	return 0;
}
