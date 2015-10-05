#include <stdio.h>
#include <stdlib.h>
#include "s4.h"
#include "pthread.h"

#define GEM5_NUMPROCS 4

#define K 20
#define N 10000
#define D 3
#define RANGE 1000
#define ERROR 0.01

#define TIME 0x7FFFFFFF

typedef struct kmeansstruct{
	float x;
	float y;
	float z;
	int k;
}kmeansstruct;

typedef struct setcluststruct{
	kmeansstruct* data;
	kmeansstruct* klist;
	int num;
}setcluststruct;

typedef struct calcclustmidstruct{
	kmeansstruct* datas;
	kmeansstruct* klist;
	int clustnum;
	int num;
}calcclustmidstruct;

int checknuminlist(int* li, int val, int num){
	int i;
	for(i=0;i<num;i++){
		if(li[i]==val)
			return 1;
	}
	return 0;
}
void loadkstruct(kmeansstruct* s, FILE* fp){
	fscanf(fp, "%*s %*s %*s %f %f %f", &(s->x), (&s->y), (&s->z));
	s->k=-1;
}
void savekstruct(kmeansstruct* s, FILE* fp, int num){
	fprintf(fp, "Tran #%04d - x: %f, y: %f, z: %f, k: %d\n", num, s->x, s->y, s->z, s->k);
}
void savekmeansb(kmeansstruct* s, FILE* fp, int num){
	fwrite(s, sizeof(kmeansstruct), num, fp);
}
void readkmeansb(kmeansstruct* s, FILE* fp, int num){
	fread(s, sizeof(kmeansstruct), num, fp);
}
void setclustmid(kmeansstruct* datas, kmeansstruct* klist){
	int knum[K];
	int temp;
	int i;
	for(i=0;i<K;i++){
		temp=rand()%N;
		if(checknuminlist(knum, temp, i))
			i--;
		else
			knum[i]=temp;
	}
	for(i=0;i<K;i++){
		klist[i].x=datas[knum[i]].x;
		klist[i].y=datas[knum[i]].y;
		klist[i].z=datas[knum[i]].z;
		klist[i].k=0;
	}
}
void savekmeans(kmeansstruct* datas, kmeansstruct* klist, FILE* fp){
	int i;
	for(i=0;i<K;i++){
		fprintf(fp, "Clust #%d - x: %f, y: %f, z: %f, num: %d\n", i, klist[i].x, klist[i].y, klist[i].z, klist[i].k);
	}
	for(i=0;i<N;i++){
		savekstruct(datas+i, fp, i);
	}
}

void* setclustfunc(void* thearg){
	setcluststruct* arg=(setcluststruct*)thearg;
	float dist=0x7FFFFFFF;
	float distx, disty, distz, tempdist;
	int num=-1, i, j;
	for(i=0;i<arg->num;i++){
		dist=0x7FFFFFFF;
		num=-1;
		for(j=0;j<K;j++){
			distx=arg->data[i].x-arg->klist[j].x;
			disty=arg->data[i].y-arg->klist[j].y;
			distz=arg->data[i].z-arg->klist[j].z;
			tempdist=distx*distx+disty*disty+distz*distz;
			if(tempdist<dist){
				num=j;
				dist=tempdist;
			}
		}
		arg->data[i].k=num;
	}
	return NULL;
}
void setclust(kmeansstruct* datas, kmeansstruct* klist){
	float dist;
	int num;
	float distx, disty, distz;
	float tempdist;
	int i;
	int count=0;
	int rest=N%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	setcluststruct structs[GEM5_NUMPROCS];
	if(N<GEM5_NUMPROCS){
		for(i=0;i<N;i++){
			structs[i].klist=klist;
			structs[i].data=&datas[i];
			structs[i].num=1;
			pthread_create(&thread[i], NULL, setclustfunc, (void*)&structs[i]);
		}
		for(i=0;i<N;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].klist=klist;
			structs[i].data=&datas[count];
			if(rest==0){
				structs[i].num=N/(GEM5_NUMPROCS-1);
				count+=structs[i].num;
			}
			else{
				structs[i].num=N/(GEM5_NUMPROCS-1)+1;
				count+=structs[i].num;
				rest--;
			}
			pthread_create(&thread[i], NULL, setclustfunc, (void*)&structs[i]);
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}

void* calcclustmidfunc(void* thearg){
	calcclustmidstruct* arg=(calcclustmidstruct*)thearg;
	float distx=0.0f;
	float disty=0.0f;
	float distz=0.0f;
	float tdistx, tdisty, tdistz;
	int num=0;
	int i;
	int ret=0;
	int count;
	for(count=0;count<arg->num;count++){
		distx=0.0f;
		disty=0.0f;
		distz=0.0f;
		num=0;
		for(i=0;i<N;i++){
			if(arg->datas[i].k==arg->clustnum+count){
				distx+=arg->datas[i].x;
				disty+=arg->datas[i].y;
				distz+=arg->datas[i].z;
				num++;
			}
		}
		if(num==0)
			continue;
		distx/=num;
		disty/=num;
		distz/=num;
		if(ret==0){
			tdistx=distx-arg->klist[count].x;
			tdisty=disty-arg->klist[count].y;
			tdistz=distz-arg->klist[count].z;
			if((tdistx*tdistx+tdisty*tdisty+tdistz*tdistz)<ERROR*ERROR)
				ret=1;
		}
		arg->klist[count].x=distx;
		arg->klist[count].y=disty;
		arg->klist[count].z=distz;
		arg->klist[count].k=num;
	}
	return (void*)ret;
}
int calcclustmid(kmeansstruct* datas, kmeansstruct* klist){
	int ret=0;
	int i;
	int retu[K];
	int count=0;
	int rest=K%(GEM5_NUMPROCS-1);
	pthread_t thread[GEM5_NUMPROCS];
	calcclustmidstruct structs[GEM5_NUMPROCS];
	if(K<GEM5_NUMPROCS){
		for(i=0;i<K;i++){
			structs[i].datas=datas;
			structs[i].klist=&klist[i];
			structs[i].clustnum=i;
			structs[i].num=1;
			pthread_create(&thread[i], NULL, calcclustmidfunc, (void*)&structs[i]);
		}
		for(i=0;i<K;i++){
			pthread_join(thread[i], (void**)&retu[i]);
			if(ret==0){
				if(retu[i]==1)
					ret=1;
			}
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].datas=datas;
			structs[i].klist=&klist[count];
			structs[i].clustnum=count;
			if(rest==0){
				structs[i].num=K/(GEM5_NUMPROCS-1);
				count+=structs[i].num;
			}
			else{
				structs[i].num=K/(GEM5_NUMPROCS-1)+1;
				count+=structs[i].num;
				rest--;
			}
			pthread_create(&thread[i], NULL, calcclustmidfunc, (void*)&structs[i]);
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], (void**)&retu[i]);
			if(ret==0){
				if(retu[i]==1)
					ret=1;
			}
		}
	}
	return ret;
}

int kmeans(int T){
	int i;
	kmeansstruct kdata[N];
	kmeansstruct klist[K];
	FILE* fp=fopen("kdata", "rb");
	FILE* output=fopen("kclust", "wb");

	readkmeansb(kdata, fp, N);
	setclustmid(kdata, klist);
	savekmeansb(klist, output, K);

	fclose(output);
	fclose(fp);
	return 0;
}

int main(int argc, char* argv[]){
	s4_init_simulation();
	srand((unsigned)atoi(argv[1]));
	kmeans(TIME);
	s4_wrapup_simulation();
	return 0;
}
