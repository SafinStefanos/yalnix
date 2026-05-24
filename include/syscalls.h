#ifndef _syscalls_h
#define _syscalls_h

#include <hardware.h>
#include <load_info.h>
#include <yuser.h>
#include <unistd.h>
#include <yalnix.h>
#include <ykernel.h>
#include <traps.h>
#include <kern.h>

int sys_brk(PCB_t *proc, void *addr);
int sys_getpid(PCB_t *proc);
int sys_delay(PCB_t *proc, UserContext *uc, int ticks);

#endif /* _syscalls_h */