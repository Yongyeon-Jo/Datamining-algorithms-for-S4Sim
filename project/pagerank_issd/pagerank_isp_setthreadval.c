#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>

#define N 7115
#define damp 0.85

#define ERROR 0.000001

#define GEM5_NUMPROCS 4

typedef struct linkmapcsrvalue{
	int col;
	float value;
}linkmapcsrvalue;

typedef struct linkmapcsr{
	int outnumzero[1005];
	int rownum[7116];
	linkmapcsrvalue value[103689];
}linkmapcsr;

typedef struct threadval{
	int num;
	int count;
}threadval;

typedef struct updatestruct{
	float* dest;
	linkmapcsr* map;
	float* rankvec;
	int num;
	int k;
	float defaultvalue;
}updatestruct;

void savethreadval(threadval* dest, FILE* fp){
	fwrite(dest, sizeof(threadval), GEM5_NUMPROCS, fp);
}

void loadthreadval(threadval* dest, FILE* fp){
	fread(dest, sizeof(threadval), GEM5_NUMPROCS, fp);
}

void savemap(linkmapcsr* dest, FILE* fp){
	fwrite(dest, sizeof(linkmapcsr), 1, fp);
}

void loadmap(linkmapcsr* dest, FILE* fp){
	fread(dest, sizeof(linkmapcsr), 1, fp);
}

void saverank(float* dest, FILE* fp){
	fwrite(dest, sizeof(float), N, fp);
}

void loadrank(float* dest, FILE* fp){
	fread(dest, sizeof(float), N, fp);
}

void genrank0(float* dest){
	int i;
	float val=1.0;
	for(i=0;i<N;i++){
		dest[i]=val;
	}
}

void gencsrmap(linkmapcsr* dest, FILE* fp, FILE* fp2){
	int i, j, k=0, l, m=-1, n=0;
FILE* ffp=fopen("csrmap.txt", "w");
	linkmapcsrvalue* val;
	for(i=0;i<103689;i++){
		val=&dest->value[k];

		fscanf(fp, "%d %d %d", &j, &val->col, &l);
		val->value=1.0/(float)l;
		
		if(m!=j){
			while(m!=j){
				m++;
				dest->rownum[n]=k;
				n++;
			}
		}
		k++;
	}
	dest->rownum[N]=k;
	for(i=0;i<1005;i++){
		fscanf(fp2, "%d", &j);
		dest->outnumzero[i]=j;
	}
for(i=0;i<1005;i++){
fprintf(ffp, "%d\n", dest->outnumzero[i]);
}
for(i=0;i<7115;i++){
for(j=dest->rownum[i];j<dest->rownum[i+1];j++){
fprintf(ffp, "%d %d %f\n", i, dest->value[j].col, dest->value[j].value);
}
}
fclose(ffp);


}

int checkvec(float* a, float* b){
	int i;
	for(i=0;i<N;i++){
		if(a[i]>b[i]){
			if(a[i]-b[i]>ERROR)
				return 0;
		}
		else{
			if(b[i]-a[i]>ERROR)
				return 0;
		}
	}
	return 1;
}

void calcendrank(float* dest, linkmapcsr* map, float* rankvec){
	int i;
	*dest=0.0f;
	for(i=0;i<1005;i++){
		*dest+=rankvec[map->outnumzero[i]];
	}
	*dest/=(float)N;
}

void setthreadval(threadval* val, linkmapcsr* map){
	int i, j=0, count=0;
	int rest=N%(GEM5_NUMPROCS-1);
	if(N<GEM5_NUMPROCS){
		for(i=0;i<N;i++){
			val[i].count=i;
			val[i].num=1;
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			val[i].count=count;
			if(rest==0){
				val[i].num=N/(GEM5_NUMPROCS-1);
			}
			else{
				val[i].num=N/(GEM5_NUMPROCS-1)+1;
				rest--;
			}
			count+=val[i].num;
		}
	}
}

void* updaterankfunc(void* thearg){
	updatestruct* arg=(updatestruct*)thearg;
	int i, j;
	float val;
	for(i=0;i<arg->num;i++){
		val=arg->defaultvalue;

		for(j=arg->map->rownum[arg->k+i];j<arg->map->rownum[arg->k+i+1];j++){
			val+=arg->map->value[j].value*arg->rankvec[arg->map->value[j].col];
		}
		arg->dest[i]=damp*val+(1.0-damp);
	}
}

void updaterank(float* dest, float defaultvalue, linkmapcsr* map, float* rankvec, threadval* val){
	int i, j;
	pthread_t thread[GEM5_NUMPROCS];
	updatestruct structs[GEM5_NUMPROCS];
	float* destp=dest;

	if(N<GEM5_NUMPROCS){
		for(i=0;i<N;i++){
			structs[i].dest=destp;
			structs[i].map=map;
			structs[i].rankvec=rankvec;
			structs[i].num=val[i].num;
			structs[i].k=val[i].count;
			structs[i].defaultvalue=defaultvalue;
			pthread_create(&thread[i], NULL, updaterankfunc, (void*)&structs[i]);
			destp++;
		}
		for(i=0;i<N;i++){
			pthread_join(thread[i], NULL);
		}
	}
	else{
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			structs[i].rankvec=rankvec;
			structs[i].map=map;
			structs[i].dest=destp;
			structs[i].k=val[i].count;
			structs[i].defaultvalue=defaultvalue;
			structs[i].num=val[i].num;
			pthread_create(&thread[i], NULL, updaterankfunc, (void*)&structs[i]);
			destp=destp+structs[i].num;
		}
		for(i=0;i<GEM5_NUMPROCS-1;i++){
			pthread_join(thread[i], NULL);
		}
	}
}

int main(){
	FILE* mapinput=fopen("csrmap", "rb");
	FILE* output=fopen("threadvalcsr", "wb");

	threadval tval[GEM5_NUMPROCS];
	
	linkmapcsr mapcsr;
	
	loadmap(&mapcsr, mapinput);
	
	setthreadval(tval, &mapcsr);

	savethreadval(tval, output);

	fclose(mapinput);
	fclose(output);
	
	return 0;
}
