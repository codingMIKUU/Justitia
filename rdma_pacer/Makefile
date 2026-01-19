.PHONY: clean

CFLAGS  := -Wall -O3
LD      := gcc
LDLIBS  := ${LDLIBS} -lpthread -lrt -libverbs -lm

APPS    := pacer thread_slot_test

all: ${APPS}

pacer: pingpong_utils.o pingpong.o get_clock.o queue.o massdal.o prng.o countmin.o monitor.o pacer.o
	${LD} -o $@ $^ ${LDLIBS}

thread_slot_test: thread_slot_test.o
	${LD} -o $@ $^ -lpthread

clean:
	rm -f *.o ${APPS}
