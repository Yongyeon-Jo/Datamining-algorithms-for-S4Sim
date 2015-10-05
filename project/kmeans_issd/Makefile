#
S4SIM_HOME = ../..
CFLAGS = -g

INCLUDE=-I${S4SIM_HOME}/include
PTHREAD = ${S4SIM_HOME}/external/m5threads
CC = gcc
CPP = g++

ARMCC = arm-linux-gnueabi-gcc
ARMFLAGS = -march=armv7-a -marm

all : run_kmeans kmeans_isp_read kmeans_isp_setmid kmeans_isp_setclust kmeans_isp_calcmid kmeans_isp_write

run_kmeans : run_kmeans.c ${S4SIM_HOME}/src/isp_socket.c
	$(CC) $(CFLAGS) -o $@ $^ -lpthread $(INCLUDE)
	
kmeans_isp_read : kmeans_isp_read.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

kmeans_isp_setmid : kmeans_isp_setmid.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

kmeans_isp_setclust : kmeans_isp_setclust.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

kmeans_isp_calcmid : kmeans_isp_calcmid.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

kmeans_isp_write : kmeans_isp_write.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)
	
