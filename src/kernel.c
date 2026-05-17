#include <hardware.h>
#include <traps.h>
#include <load_info.h>
#include <yalnix.h>
#include "procs.h"


unsigned char frame_tracker[(MAX_PMEM_SIZE / PAGESIZE) / 8];
pte_t KernelPT[MAX_PT_LEN];
void (*IVT[TRAP_VECTOR_SIZE])(UserContext *));


/*

SetKernelBrk
	Check if virtual memory is enabled
	Compute page indices
 	If more pages needed, check if enough physical frames are free then allocate, else undo partially allocated pages and error

	
KernelStart
	Init free frame tracker, process queues
	Build region 0 page table
	Create void* array for Interrupt Vector table where each entry points to a handler
	Write table address to REG_VECTOR_BASE register
  	Init first proc, create PCB, alloc kernel stack, alloc region 1 page table with one page for user stack
  	Compute kernel mem boundaries
  	Map kernel heap, data, text pages
  	Init region 0 page table
  	Write regions into REG_PTRs
  	set REG_VM_ENABLE to 1
  	Create init process PCB
  	Load init program
  	Flush tlb

LoadProgram
	Open exec file
	Read exec file metadata
	Compute arg sizes
	Save argos
	Compute stack layout
	Calc required virtual pages
	Check if mem requirements are doable
	Init user stack pointer
	Destroy old addr space
	Allocate text, data, bss, stack pages
	Flush TLB
	Load program contents/PCB
	Swap text pages to read only
	Set program counter
	Build argc/argv into user stack
	Clear out CPU state for program

KCSwitch
	Validate PCB pointers
	Save current proc kernel context
	Validate next proc kernel context
	Switch kernel stack mappings
	Flush tlb
	Reset scheduling tick count

*/

extern void KernelStart (char **argv, unsigned int pmem_size, UserContext *ctx){
	TracePrintf(DEBUG, "KernelStart\n");

	int num_frames = pmem_size / PAGESIZE; /*how many r actually possible?*/
	int i;
	for(i = 0; i < (MAX_PMEM_SIZE / PAGESIZE); i++){ /*make free*/
    	frames[i] = 0;
	}
	for(i = 0; i < _orig_kernel_brk_page; i++){
    	frames[i] = 1;
	}

	frames[KERNEL_STACK_BASE >> PAGESHIFT] = 1; /*bc kernel stack is 2 pages*/
	frames[(KERNEL_STACK_BASE >> PAGESHIFT) + 1] = 1;
	
	for (i = 0; i < MAX_PT_LEN; i++){
    	KernelPT[i].valid = 0; /*Not used yet*/
	}

	for(i=_first_kernel_text_page; i < _first_kernel_data_page; i++){
    	KernelPT[i].valid = 1;
   		KernelPT[i].pfn = i; /*VPN i -> PFN i*/
    	KernelPT[i].prot = PROT_READ | PROT_EXEC;
	}
	for (i = _first_kernel_data_page; i < _orig_kernel_brk_page; i++) {
    	KernelPT[i].valid = 1;
    	KernelPT[i].pfn = i; /*map*/
    	KernelPT[i].prot = PROT_READ | PROT_WRITE;
	}
	
}
