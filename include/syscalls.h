#ifndef _syscalls_h
#define _syscalls_h

#include <hardware.h>

/* process state constants */


/* external globals needed by syscalls */
extern unsigned char frames[];
extern pte_t KernelPT[];
extern PCB_t *current_process;
extern PCB_t *ready_queue_head;
extern PCB_t *sleep_queue_head;
extern PCB_t *idle_pcb;
extern PCB_t *init_pcb;


/* syscall implementations */
int sys_brk(PCB_t *proc, void *addr);
int sys_getpid(PCB_t *proc);
int sys_delay(PCB_t *proc, UserContext* uc, int ticks);
void sys_exit(PCB_t *proc, int status);
int sys_fork(PCB_t *parent, UserContext *uc);
int sys_exec(PCB_t *proc, UserContext *uc, char *filename, char **args);
int sys_wait(PCB_t *proc, UserContext *uc, int *status_ptr);
int sys_tty_write(PCB_t *proc, UserContext *uc, int tty_id, void *buf, int len);
int sys_tty_read(PCB_t *proc, UserContext *uc, int tty_id, void *buf, int len);


#endif /* _syscalls_h */