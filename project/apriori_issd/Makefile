#
S4SIM_HOME = ../..
CFLAGS = -g

INCLUDE=-I${S4SIM_HOME}/include
PTHREAD = ${S4SIM_HOME}/external/m5threads
CC = gcc
CPP = g++

ARMCC = arm-linux-gnueabi-gcc
ARMFLAGS = -march=armv7-a -marm

all : run_apriori apriori_isp_makec1 apriori_isp_makec2 apriori_isp_makec3 apriori_isp_makec4 apriori_isp_makel1 apriori_isp_makel2 apriori_isp_makel3 apriori_isp_makel4 apriori_isp_merge apriori_isp_read apriori_isp_write apriori_isp_genass

run_apriori : run_apriori.c ${S4SIM_HOME}/src/isp_socket.c
	$(CC) $(CFLAGS) -o $@ $^ -lpthread $(INCLUDE)

apriori_isp_makec1 : apriori_isp_makec1.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_makec2 : apriori_isp_makec2.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_makec3 : apriori_isp_makec3.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_makec4 : apriori_isp_makec4.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_makel1 : apriori_isp_makel1.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_makel2 : apriori_isp_makel2.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_makel3 : apriori_isp_makel3.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_makel4 : apriori_isp_makel4.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_merge : apriori_isp_merge.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_read : apriori_isp_read.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_write : apriori_isp_write.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)

apriori_isp_genass : apriori_isp_genass.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static $(INCLUDE)
