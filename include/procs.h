#include <hardware.h>
#include <stdbool.h>

#ifndef _PCB_H
#define _PCB_H

#define S_RUNNING 0
#define S_READY   1
#define S_BLOCKED 2

typedef struct PCB{
  UserContext usr_ctx;
  KernelContext krn_ctx;
  pte_t *r1pt;
  int pid;
  int ppid;
  int state;
  int exstat;
  int kstack_pfn[4];
  struct PCB *child_pcb->parent = current_process;
  struct PCB *child_pcb->next_sibling = current_process->children;
  struct PCB *current_process->children = child_pcb;
}PCB_t;

#endif
