#include <hardware.h>
#include <traps.h>
#include <load_info.h>
#include <yalnix.h>
#include <struct_helpers.h>


unsigned char frames[MAX_PMEM_SIZE/PAGESIZE];
pte_t KernelPT[MAX_PT_LEN];
void *IVT[TRAP_VECTOR_SIZE];
pte_t r1pt[MAX_PT_LEN];
PCB_t *ipcb = NULL;
PCB_t *current_process = NULL;
PCB_t *ready_queue_head;
PCB_t *sleep_queue_head;

void *curr_kbrk;


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
	
	ready_queue_head = NULL;
    sleep_queue_head = NULL;
	int num_frames = pmem_size/PAGESIZE; /*how many r actually possible?*/
	int i;
	for(i=0; i<num_frames; i++){ /*make free*/
    	frames[i] = 0;
	}
	
	for(i = 0; i < _orig_kernel_brk_page; i++){ /*kernel space*/
    	frames[i] = 1;
	}
	
	for (i = 0; i < MAX_PT_LEN; i++){
    	KernelPT[i].valid = 0; /*Not used yet*/
	}

	for(i=_first_kernel_text_page; i< _first_kernel_data_page; i++){ /*text*/
    	KernelPT[i].valid = 1;
   		KernelPT[i].pfn = i; /*VPN i -> PFN i*/
    	KernelPT[i].prot = PROT_READ | PROT_EXEC;
	}
	for (i = _orig_kernel_brk_page; i < (UP_TO_PAGE(sbrk(0)) >> PAGESHIFT); i++) { /*data*/
    	KernelPT[i].valid = 1;
    	KernelPT[i].pfn = i; 
    	KernelPT[i].prot = PROT_READ | PROT_WRITE;
	}
	
	for(i=_first_kernel_data_page; i<_orig_kernel_brk_page; i++){ /*heap*/
    	KernelPT[i].valid = 1;
    	KernelPT[i].pfn = i; /*map*/
    	KernelPT[i].prot = PROT_READ | PROT_WRITE;
		frames[i] = 1;
	}
	
	for(i = 0; i < TRAP_VECTOR_SIZE; i++){
    	IVT[i] = &thandler; 
	}
	WriteRegister(REG_VECTOR_BASE, (unsigned int)(&(IVT[0])));
	
	
	current_process = (PCB_t *)malloc(sizeof(PCB_t)); /*Init process PCB*/
	current_process->r1pt = (pte_t *)malloc(sizeof(pte_t) * MAX_PT_LEN);
	
	for(i = 0; i < MAX_PT_LEN; i++){/*clear r1 table*/
    	current_process->r1pt[i].valid = 0;
	}
	
	WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
	WriteRegister(REG_PTLR1, MAX_PT_LEN);
	WriteRegister(REG_PTBR0, (unsigned int)KernelPT);
	WriteRegister(REG_PTLR0, MAX_PT_LEN);

	int kstack_idx = KERNEL_STACK_BASE >> PAGESHIFT;
	for (i = 0; i < 2; i++) { /*allocate for 2 frames*/
		int f = 0;
		for (int j = 0; j < num_frames; j++) {
			if (frames[j] == 0) {
				f = j;
				break;
			}
		}
		// int f = find_free();
    	frames[f] = 1;
    	KernelPT[kstack_idx + i].valid = 1;
    	KernelPT[kstack_idx + i].pfn = f; 
    	KernelPT[kstack_idx + i].prot = PROT_READ | PROT_WRITE;
	}
	
	curr_kbrk = (void *)UP_TO_PAGE(sbrk(0)); /*initialized for top of kernel heap*/

	WriteRegister(REG_VM_ENABLE, 1);/*ENABLE THE BIG VM*/
	WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_ALL); /*tmv flush*/

    if (LoadProgram(argv, argv, ctx) != LI_NO_ERROR) {
    	TracePrintf(0, "Failed to load init program\n");
    	Halt();
	} /*stack is at top of region 1 [2]*/

    TracePrintf(0, "Leaving KernelStart, entering user mode...\n");
	
}




int SetKernelBrk(void *addr) {
    unsigned int new_brk = (unsigned int)UP_TO_PAGE(addr);
    unsigned int curr_brk = (unsigned int)UP_TO_PAGE(current_kernel_brk);

	if (new_brk >= KERNEL_STACK_BASE) {
        TracePrintf(0, "SetKernelBrk: Error - Kernel heap collision with kernel stack.\n");
        return ERROR;
    } /*looking for collision between  new break and kernel stack base. The heap cannot start behind the stack*/

	if (ReadRegister(REG_VM_ENABLE) == 0) {
        current_kernel_brk = (void *)new_brk;
        return SUCCESS;
    } /*check for enabled VM*/

	int curr_vpn = curr_brk >> PAGESHIFT; /*getting VPNs*/
    int new_vpn = new_brk >> PAGESHIFT;

	if(new_vpn>curr_vpn){
		for(int i=curr_vpn; i<new_vpn; i++){
			int f =find_free();
			if(f==ERROR){
				TracePrintf(0, "SetKernelBrk: Out of physical memory.\n");
				for(int j=curr_vpn; j<i; j++){
					helper_force_free(KernelPT[j].pfn);
					KernelPT[j].valid=0;
				}
				WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_0);
				return ERROR;
			}
			KernelPT[i].valid=1; /*mapping new page*/
			KernelPT[i].pfn=f;
			KernelPT[i].prot= PROT_READ | PROT_WRITE;
		}
	} 
	else if(new_vpn<curr_vpn){ /*shrinking heap*/
		for(int i=new_vpn; i<curr_vpn; i++){
			if(KernelPT[i].valid){
				helper_force_free(KernelPT[i].pfn); /*free frame*/
				KernelPT[i].valid = 0;
			}
		}
	}

	curr_kbrk=(void *)new_brk;
	WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_0);
	
    return SUCCESS; 
}

