ALL = yalnix idle

LIBDIR = ../lib
INCDIR = ../include

CC = gcc

CFLAGS = -m32 -I$(INCDIR)
LDFLAGS = -m32 -L$(LIBDIR)

KERNEL_LIBS = -lkernel -lhardware -lelf
USER_LIBS = -lyuser

KERNEL_SRC = kernel.c syscalls.c traps.c
KERNEL_OBJ = kernel.o syscalls.o traps.o

USER_SRC = idle.c
USER_OBJ = idle.o

all: $(ALL)

#
# Kernel build
#
yalnix: $(KERNEL_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(KERNEL_OBJ) $(KERNEL_LIBS)

#
# User program build
#
idle: $(USER_OBJ)
	$(CC) $(LDFLAGS) -static -o $@ $(USER_OBJ) $(USER_LIBS)

#
# Generic compile rule
#
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(KERNEL_OBJ) $(USER_OBJ) yalnix idle
