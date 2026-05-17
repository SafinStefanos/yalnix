#include <hardware.h>
#include <pagetable.h>
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
  int state;
}PCB_t;

#endif
