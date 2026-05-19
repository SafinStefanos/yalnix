#ifndef _MEMORY_H
#define _MEMORY_H

#include <hardware.h>
#include <ykernel.h>

/* --- Global Memory Variables --- */
extern unsigned char frames[MAX_PMEM_SIZE/PAGESIZE]; /* [1, 2] */
extern pte_t KernelPT[MAX_PT_LEN];                  /* [1, 2] */
extern void *curr_kbrk;                             /* [1] */

/* --- Function Prototypes --- */

/* Logic from: how many frames r actually possible? */
int find_free(void);                                /* [3] */

/* Logic from: initialized for top of kernel heap */
void InitKernelMemoryMap(unsigned int pmem_size);   /* [4, 5] */

/* Logic from: check for enabled VM and handle kernel brk */
extern int SetKernelBrk(void *addr);                /* [6, 7] */

#endif /* _MEMORY_H */
