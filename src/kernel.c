#include <hardware.h>
#include <traps.h>
#include <load_info.h>
#include <yalnix.h>
#include <ykernel.h>
#include <unistd.h>
#include <fcntl.h>
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
int find_free() {
	for (int j = 0; j < MAX_PMEM_SIZE/PAGESIZE; j++) {
		if (frames[j] == 0) return j;
	}
}



static void free_region1(pte_t *pt) {
    for (int i = 0; i < MAX_PT_LEN; i++) {
        if (pt[i].valid && i >= (VMEM_1_BASE >> PAGESHIFT)) {
            frames[pt[i].pfn] = 0;
            pt[i].valid = 0;
        }
    }
}


int LoadProgram(char *name, char *args[], PCB_t *pcb)
{
    int fd;
    struct load_info li;

    if ((fd = open(name, O_RDONLY)) < 0) {
        TracePrintf(0, "LoadProgram: cannot open %s\n", name);
        return ERROR;
    }

    if (LoadInfo(fd, &li) != LI_NO_ERROR) {
        TracePrintf(0, "LoadProgram: invalid executable\n");
        close(fd);
        return ERROR;
    }

    int arg_count = 0;
    int arg_size = 0;

    while (args[arg_count] != NULL) {
        arg_size += strlen(args[arg_count]) + 1;
        arg_count++;
    }

    char *stack_top = (char *)VMEM_1_LIMIT;
    char *arg_block = stack_top - arg_size;

    int text_start = li.t_vaddr >> PAGESHIFT;
    int data_start = li.id_vaddr >> PAGESHIFT;

    int total_pages = li.t_npg + li.id_npg + li.ud_npg;

    if (total_pages >= MAX_PT_LEN) {
        close(fd);
        return ERROR;
    }

    free_region1(pcb->r1pt);

    lseek(fd, li.t_faddr, SEEK_SET);

    for (int i = 0; i < li.t_npg; i++) {
        int vpn = text_start + i;
        int pfn = find_free();
        if (pfn < 0) return ERROR;

        frames[pfn] = 1;

        pcb->r1pt[vpn].valid = 1;
        pcb->r1pt[vpn].pfn = pfn;
        pcb->r1pt[vpn].prot = PROT_READ | PROT_EXEC;

        read(fd, (void *)(vpn << PAGESHIFT), PAGESIZE);
    }


    lseek(fd, li.id_faddr, SEEK_SET);

    for (int i = 0; i < li.id_npg; i++) {
        int vpn = data_start + i;
        int pfn = find_free();
        if (pfn < 0) return ERROR;

        frames[pfn] = 1;

        pcb->r1pt[vpn].valid = 1;
        pcb->r1pt[vpn].pfn = pfn;
        pcb->r1pt[vpn].prot = PROT_READ | PROT_WRITE;

        read(fd, (void *)(vpn << PAGESHIFT), PAGESIZE);
    }


    if (li.ud_npg > 0) {
        bzero((void *)li.id_end, li.ud_end - li.id_end);
    }


    int stack_vpn = (VMEM_1_LIMIT >> PAGESHIFT) - 1;

    int pfn = find_free();
    if (pfn < 0) return ERROR;
	frames[pfn] = 1;

    pcb->r1pt[stack_vpn].valid = 1;
    pcb->r1pt[stack_vpn].pfn = pfn;
    pcb->r1pt[stack_vpn].prot = PROT_READ | PROT_WRITE;


    pcb->usr_ctx.pc = (void *)li.entry;
    pcb->usr_ctx.sp = (void *)VMEM_1_LIMIT;

    char *cp = arg_block;
    for (int i = 0; i < arg_count; i++) {
        strcpy(cp, args[i]);
        cp += strlen(args[i]) + 1;
    }

    close(fd);

 
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

    return SUCCESS;
}

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

    if (LoadProgram(argv[0], argv, current_process) != LI_NO_ERROR) {
    	TracePrintf(0, "Failed to load init program\n");
    	Halt();
	} /*stack is at top of region 1 [2]*/

    TracePrintf(0, "Leaving KernelStart, entering user mode...\n");
	
}


int SetKernelBrk(void *addr) {
    unsigned int new_brk = (unsigned int)UP_TO_PAGE(addr);
    unsigned int curr_brk = (unsigned int)UP_TO_PAGE(curr_kbrk);

	if (new_brk >= KERNEL_STACK_BASE) {
        TracePrintf(0, "SetKernelBrk: Error - Kernel heap collision with kernel stack.\n");
        return ERROR;
    } /*looking for collision between  new break and kernel stack base. The heap cannot start behind the stack*/

	if (ReadRegister(REG_VM_ENABLE) == 0) {
        curr_kbrk = (void *)new_brk;
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

void DoIdle(void) {
	while(1) {
		TracePrintf(1,"DoIdle\n");
		Pause();
	}
}

