CC=gcc
CFLAGS=-Wall -Wextra -march=native -mtune=native -O3 -flto -funroll-all-loops
#CC=clang
#CFLAGS=-Wall -Wextra -g

all: artest

artest: artest.c
	$(CC) artest.c -o artest -pthread -lm $(CFLAGS)

artest_pgo: artest.c
	$(CC) artest.c -o artest -pthread -lm -pg $(CFLAGS)

clean:
	rm -f artest *.o
