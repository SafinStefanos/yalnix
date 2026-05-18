ALL = yalnix

LIBDIR = ../lib
INCDIR = ../include

CC = gcc

CFLAGS = -m32 -I$(INCDIR)
LDFLAGS = -m32 -L$(LIBDIR)

LIBS = -lkernel -lhardware -lelf

SRC = kernel.c syscalls.c traps.c
OBJ = kernel.o syscalls.o traps.o

all: $(ALL)

yalnix: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) yalnix