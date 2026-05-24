#include <hardware.h>
#include <traps.h>
#include <load_info.h>
#include <yalnix.h>
#include <ykernel.h>
#include <unistd.h>
#include <fcntl.h>
#include <struct_helpers.h>
#include <load.h>


unsigned char frames[MAX_PMEM_SIZE/PAGESIZE];
pte_t KernelPT[MAX_PT_LEN];
void *IVT[TRAP_VECTOR_SIZE];
pte_t r1pt[MAX_PT_LEN];
PCB_t *ipcb = NULL;
PCB_t *current_process = NULL;
PCB_t *ready_queue_head;
PCB_t *sleep_queue_head;
PCB_t *idle_pcb = NULL;
PCB_t *init_pcb = NULL;
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
	return ERROR;
}



void free_region1(pte_t *pt) {
    for (int i = 0; i < MAX_PT_LEN; i++) {
        if (pt[i].valid /* && i >= (VMEM_1_BASE >> PAGESHIFT)*/) {
            frames[pt[i].pfn] = 0;
            pt[i].valid = 0;
			pt[i].prot = 0;
            pt[i].pfn = 0;
        }
    }
}

KernelContext *KCSInitFunc(KernelContext *kc_in, void *pcb_v, void *unused) {
    PCB_t *pcb = (PCB_t *)pcb_v;
    pcb->krn_ctx = *kc_in;
    return kc_in;
}
KernelContext *KCSwitchFunc(KernelContext *kc_in, void *curr_pcb_v, void *next_pcb_v) {
    PCB_t *curr = (PCB_t *)curr_pcb_v;
    PCB_t *next = (PCB_t *)next_pcb_v;

    TracePrintf(0, "next: pid=%d init=%d r1pt=%p kstack_pfn=[%d,%d,%d,%d] sibling=%p parent=%p child=%p state=%d\n",
        next->pid, next->init, next->r1pt,
        next->kstack_pfn[0], next->kstack_pfn[1], next->kstack_pfn[2], next->kstack_pfn[3],
        next->sibling, next->parent, next->child, next->state);
    TracePrintf(0, "KCSWitchFunc: curr=%p next=%p\n", curr, next);

    if (curr == NULL || next == NULL) {
        TracePrintf(0, "KCSSwitchFunc: NULL pcb!\n");
        return NULL;
    }

    // Save current kernel context
    curr->krn_ctx = *kc_in;
    curr->init = 1;

    // Switch region 1 page table always
    WriteRegister(REG_PTBR1, (unsigned int)next->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

    if (!next->init) {
		TracePrintf(0, "KCSSwitchFunc: next not initialized, copying stack\n");
		next->init = 1;

		int ks_base_pg = KERNEL_STACK_BASE >> PAGESHIFT;
		int ks_npg = KERNEL_STACK_MAXSIZE >> PAGESHIFT;

		// Copy current stack contents into idle's frames before swapping
		// Use a temporary mapping in region 0 to access idle's frames
		// Actually: copy via physical addresses using a temp page
		// Simpler: copy the stack contents while still on init's frames,
		// then swap frames
		void *ks_base = (void *)KERNEL_STACK_BASE;
		size_t ks_size = KERNEL_STACK_MAXSIZE;

		// Save stack contents to a temp buffer
		char *tmp = (char *)malloc(ks_size);
		memcpy(tmp, ks_base, ks_size);

		// Swap kernel stack to next's frames
		for (int i = 0; i < ks_npg; i++) {
			KernelPT[ks_base_pg + i].pfn = next->kstack_pfn[i];
		}
		WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_0);

		// Copy saved stack contents into idle's frames
		memcpy(ks_base, tmp, ks_size);
		free(tmp);

		return kc_in;
	}

    // Normal case: swap kernel stack frames
    int ks_base_pg = KERNEL_STACK_BASE >> PAGESHIFT;
    int ks_npg = KERNEL_STACK_MAXSIZE >> PAGESHIFT;
    for (int i = 0; i < ks_npg; i++) {
        KernelPT[ks_base_pg + i].pfn = next->kstack_pfn[i];
    }
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_0);

    return &next->krn_ctx;
}


extern void KernelStart(char **argv, unsigned int pmem_size, UserContext *ctx) {
    TracePrintf(0, "KernelStart\n");

    int num_frames = pmem_size / PAGESIZE;
    int i;

    // Initialize frame tracker
    for (i = 0; i < num_frames; i++) {
        frames[i] = (i < _orig_kernel_brk_page) ? 1 : 0;
    }

    // Initialize kernel page table
    for (i = 0; i < MAX_PT_LEN; i++) {
        KernelPT[i].valid = 0;
    }

    // Map kernel text pages
    for (i = _first_kernel_text_page; i < _first_kernel_data_page; i++) {
        KernelPT[i].valid = 1;
        KernelPT[i].pfn = i;
        KernelPT[i].prot = PROT_READ | PROT_EXEC;
    }

    // Map kernel data/heap pages
    for (i = _first_kernel_data_page; i < _orig_kernel_brk_page; i++) {
        KernelPT[i].valid = 1;
        KernelPT[i].pfn = i;
        KernelPT[i].prot = PROT_READ | PROT_WRITE;
    }

    // Map kernel stack pages (for init, the first process to run)
    int ks_base_pg = KERNEL_STACK_BASE >> PAGESHIFT;
    int ks_npg = KERNEL_STACK_MAXSIZE >> PAGESHIFT;  // 2 pages
    int kstack_pfns[2] = {0x7e, 0x7f};
    for (i = 0; i < ks_npg; i++) {
        frames[kstack_pfns[i]] = 1;
        KernelPT[ks_base_pg + i].valid = 1;
        KernelPT[ks_base_pg + i].pfn = kstack_pfns[i];
        KernelPT[ks_base_pg + i].prot = PROT_READ | PROT_WRITE;
    }

    // Initialize Interrupt Vector Table
    for (i = 0; i < TRAP_VECTOR_SIZE; i++) {
        IVT[i] = &thandler;  // generic handler for unimplemented traps
    }
    WriteRegister(REG_VECTOR_BASE, (unsigned int)&IVT[0]);

    curr_kbrk = (void *)UP_TO_PAGE(sbrk(0));



    // ---- Create init PCB ----
    init_pcb = (PCB_t *)malloc(sizeof(PCB_t));
	memset(init_pcb, 0, sizeof(PCB_t));
    init_pcb->r1pt = (pte_t *)malloc(sizeof(pte_t) * MAX_PT_LEN);
    for (i = 0; i < MAX_PT_LEN; i++) init_pcb->r1pt[i].valid = 0;
    init_pcb->pid = helper_new_pid(init_pcb->r1pt);

	
    // ---- Create idle PCB ----
    idle_pcb = (PCB_t *)malloc(sizeof(PCB_t));
	memset(idle_pcb, 0, sizeof(PCB_t));
	TracePrintf(0, "idle_pcb after memset: pid=%d init=%d kstack_pfn=[%d,%d] sibling=%p parent=%p state=%d\n",
    idle_pcb->pid, idle_pcb->init,
    idle_pcb->kstack_pfn[0], idle_pcb->kstack_pfn[1],
    idle_pcb->sibling, idle_pcb->parent, idle_pcb->state);
	

    idle_pcb->r1pt = (pte_t *)malloc(sizeof(pte_t) * MAX_PT_LEN);
    for (i = 0; i < MAX_PT_LEN; i++) idle_pcb->r1pt[i].valid = 0;
    idle_pcb->pid = helper_new_pid(idle_pcb->r1pt);

    // Allocate idle's kernel stack frames (separate from init's)
    for (i = 0; i < ks_npg; i++) {
        int f = find_free();
        frames[f] = 1;
        idle_pcb->kstack_pfn[i] = f;
    }

    // Allocate one user stack page for idle in region 1
    int svpn = MAX_PT_LEN - 1;
    int fus = find_free();
    frames[fus] = 1;
    idle_pcb->r1pt[svpn].valid = 1;
    idle_pcb->r1pt[svpn].pfn = fus;
    idle_pcb->r1pt[svpn].prot = PROT_READ | PROT_WRITE;

    // Init shares the current kernel stack (0x7e, 0x7f)
    for (i = 0; i < ks_npg; i++) {
        init_pcb->kstack_pfn[i] = kstack_pfns[i];
    }

    // Enable VM before LoadProgram
    WriteRegister(REG_PTBR0, (unsigned int)KernelPT);
    WriteRegister(REG_PTLR0, MAX_PT_LEN);
    WriteRegister(REG_PTBR1, (unsigned int)init_pcb->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_VM_ENABLE, 1);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_ALL);

    // Load init program
	
    // Use cmd_args[0] as init program name, or default to "init"
    char *init_name = "init";
    if (argv != NULL && argv[0] != NULL) {
        init_name = argv[0];
    }

    int init_check = LoadProgram(init_name, argv, init_pcb);
    if (init_check != SUCCESS) {
        TracePrintf(0, "KernelStart: LoadProgram failed\n");
        Halt();
    }
	WriteRegister(REG_PTBR1, (unsigned int)idle_pcb->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
	char* idle_name = "idle";


    int idle_check = LoadProgram(idle_name, NULL, idle_pcb);
    if (idle_check != SUCCESS) {
        TracePrintf(0, "KernelStart: LoadProgram idle failed\n");
        Halt();
    }

	idle_pcb->sibling = init_pcb;
	init_pcb->sibling = idle_pcb;
    // Switch back to init's page table
    WriteRegister(REG_PTBR1, (unsigned int)init_pcb->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
    // Set up process globals
    current_process = init_pcb;
    ready_queue_head = init_pcb;
    sleep_queue_head = NULL;

    // Save initial kernel contexts before first clock tick
    KernelContextSwitch(KCSInitFunc, init_pcb, NULL);

    // Make sure hardware is using init's page table
    WriteRegister(REG_PTBR1, (unsigned int)init_pcb->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

    // Return to usermode running init
    *ctx = init_pcb->usr_ctx;

    TracePrintf(0, "KernelStart done. Idle & Init ready.\n");
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

