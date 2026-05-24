#ifndef __KERN_H__
#define __KERN_H__

#include <hardware.h>
#include <traps.h>
#include <load_info.h>
#include <yalnix.h>
#include <ykernel.h>
#include <unistd.h>
#include <fcntl.h>
#include <struct_helpers.h>


/* Kernel global variables (extern so other files can access) */
extern unsigned char frames[MAX_PMEM_SIZE / PAGESIZE];
extern pte_t KernelPT[MAX_PT_LEN];
extern void *IVT[TRAP_VECTOR_SIZE];
extern pte_t r1pt[MAX_PT_LEN];
extern PCB_t *ipcb;
extern PCB_t *current_process;
extern PCB_t *ready_queue_head;
extern PCB_t *sleep_queue_head;
extern void *curr_kbrk;

/* Function prototypes */

/**
 * KernelStart
 *   Initialize kernel data structures, page tables, and first processes.
 *   Sets up VM, page tables, interrupt vectors, idle process, and init process.
 *
 * Arguments:
 *   argv       - command line arguments for init process
 *   pmem_size  - total physical memory available (in bytes)
 *   ctx        - pointer to UserContext to initialize idle process
 */
extern void KernelStart(char **argv, unsigned int pmem_size, UserContext *ctx);

/**
 * SetKernelBrk
 *   Adjust the end of the kernel heap.
 *
 * Arguments:
 *   addr - new requested break address
 *
 * Returns:
 *   SUCCESS if break updated
 *   ERROR if out of memory or illegal address
 */
extern int SetKernelBrk(void *addr);

/**
 * find_free
 *   Find a free physical frame in the system.
 *
 * Returns:
 *   Index of free frame or ERROR if none are available
 */
extern int find_free(void);

/**
 * free_region1
 *   Free all region1 pages in a given page table.
 *   Typically used when destroying a process's address space.
 *
 * Arguments:
 *   pt - pointer to the process's region1 page table
 */
extern void free_region1(pte_t *pt);


KernelContext *KCSInitFunc(KernelContext *kc_in, void *pcb_v, void *unused);

KernelContext *KCSwitchFunc(KernelContext *kc_in, void *curr_pcb_v, void *next_pcb_v);

#endif