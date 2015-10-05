#
S4SIM_HOME = ../..
CFLAGS = -g

INCLUDE=-I${S4SIM_HOME}/include
PTHREAD = ${S4SIM_HOME}/external/m5threads
CC = gcc
CPP = g++

ARMCC = arm-linux-gnueabi-gcc
ARMFLAGS = -march=armv7-a -marm

all : convert convertrev run_decisiontree decisiontree_isp_calc decisiontree_isp_check decisiontree_isp_compare decisiontree_isp_divide decisiontree_isp_makesub decisiontree_isp_test decisiontree_isp_read

convert : convert.c
	$(CC) $(CFLAGS) -o $@ $^ $(INCLUDE)
convertrev : convertrev.c
	$(CC) $(CFLAGS) -o $@ $^ $(INCLUDE)
	
run_decisiontree : run_decisiontree.c ${S4SIM_HOME}/src/isp_socket.c
	$(CC) $(CFLAGS) -o $@ $^ -lpthread $(INCLUDE)

decisiontree_isp_calc : decisiontree_isp_calc.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static -lm $(INCLUDE)
	
decisiontree_isp_check : decisiontree_isp_check.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static -lm $(INCLUDE)
	
decisiontree_isp_compare : decisiontree_isp_compare.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static -lm $(INCLUDE)
	
decisiontree_isp_divide : decisiontree_isp_divide.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static -lm $(INCLUDE)
	
decisiontree_isp_makesub : decisiontree_isp_makesub.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static -lm $(INCLUDE)
	
decisiontree_isp_test : decisiontree_isp_test.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static -lm $(INCLUDE)
	
decisiontree_isp_read : decisiontree_isp_read.c ${S4SIM_HOME}/src/s4lib.c ${PTHREAD}/pthread.c
	$(ARMCC) ${ARMFLAGS} -o $@ $^ -static -lm $(INCLUDE)
	
