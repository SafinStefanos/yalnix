ALL = $(os)
os = yalnix

# Directories
LIBDIR = ../lib
INCDIR = ../include

# Compiler flags
CFLAGS = -I$(INCDIR)
LDFLAGS = -L$(LIBDIR)

# Libraries
LIBS = -lkernel -lyuser -lhardware

# Source and object files
kernel_files = kernel.c struct_helpers.c syscalls.c traps.c
kernel_objs = kernel.o struct_helpers.o syscalls.o traps.o

# Build rules
all: $(ALL)

$(os): $(kernel_objs)
	gcc $(LDFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	gcc $(CFLAGS) -c $< -o $@

clean:
	rm -f $(kernel_objs) $(os)
