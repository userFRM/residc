CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -fPIC
INCLUDE = -I../core

.PHONY: all clean

all: libresdc.so libresdc.a

residc_sdk.o: residc_sdk.c residc_sdk.h ../core/residc.h
	$(CC) $(CFLAGS) $(INCLUDE) -c residc_sdk.c -o residc_sdk.o

residc.o: ../core/residc.c ../core/residc.h
	$(CC) $(CFLAGS) -c ../core/residc.c -o residc.o

libresdc.so: residc_sdk.o residc.o
	$(CC) -shared -o $@ $^

libresdc.a: residc_sdk.o residc.o
	ar rcs $@ $^

clean:
	rm -f *.o libresdc.so libresdc.a
