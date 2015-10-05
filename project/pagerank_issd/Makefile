#
S4SIM_HOME = ../..
CFLAGS = -g

INCLUDE=-I${S4SIM_HOME}/include
PTHREAD = ${S4SIM_HOME}/external/m5threads
CC = gcc
CPP = g++

ARMCC = arm-linux-gnueabi-gcc
ARMFLAGS = -march=armv7-a -marm

all : run_pagerank pagerank_isp_setr0 pagerank_isp_calcendrank pagerank_isp_checkvec pagerank_isp_setthreadval pagerank_isp_updaterank

run_pagerank : run_pagerank.c ${S4SIM_HOME}/src/isp_socket.c
	$(CC) $(CFLAGS) -o $@ $^ -lpthread $(INCLUDE)

pagerank_isp_setr0 : pagerank_isp_setr0.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

pagerank_isp_calcendrank : pagerank_isp_calcendrank.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

pagerank_isp_checkvec : pagerank_isp_checkvec.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

pagerank_isp_setthreadval : pagerank_isp_setthreadval.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

pagerank_isp_updaterank : pagerank_isp_updaterank.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)
