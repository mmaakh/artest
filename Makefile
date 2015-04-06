CC=gcc
CFLAGS=-Wall -Wextra -march=native -mtune=native -O3 -flto -funroll-all-loops
#CC=clang
#CFLAGS=-Wall -Wextra -g

all: ar_test

ar_test: ar_test.c
	$(CC) ar_test.c -o ar_test -pthread -lm $(CFLAGS)

clean:
	rm -f ar_test *.o
