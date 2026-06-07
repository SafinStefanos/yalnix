#include <hardware.h>
#include <traps.h>
#include <load_info.h>
#include <yalnix.h>
#include <ykernel.h>
#include <unistd.h>
#include <fcntl.h>
#include <load.h>
#include <kern.h>
#include "sync.h"


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

TtyWriteReq_t *tty_write_req[NUM_TERMINALS];
TtyReadReq_t  *tty_read_queue[NUM_TERMINALS];
char           tty_read_buf[NUM_TERMINALS][TERMINAL_MAX_LINE];
int            tty_read_len[NUM_TERMINALS];    
int            tty_busy[NUM_TERMINALS];     



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
int find_free(){
    
	for (int j = 0; j < MAX_PMEM_SIZE/PAGESIZE; j++){
    // TracePrintf(0, "find_free: returning frame %d\n", j);
		if(frames[j] == 0) return j;
	}
	return ERROR;
}


void free_region1(pte_t *pt){
    for (int i = 0; i < MAX_PT_LEN; i++){
        if (pt[i].valid /* && i >= (VMEM_1_BASE >> PAGESHIFT)*/){
            frames[pt[i].pfn] = 0;
            //TracePrintf(0, "FREE frame %d at %s:%d\n", pt[i].pfn, __FILE__, __LINE__);
            pt[i].valid = 0;
			pt[i].prot = 0;
            pt[i].pfn = 0;
        }
    }
}

extern int LoadProgram();

KernelContext *KCSInitFunc(KernelContext *kc_in, void *pcb_v, void *unused){
    PCB_t *pcb = (PCB_t *)pcb_v;
    pcb->krn_ctx = *kc_in;
    return kc_in;
}

KernelContext *KCSwitchFunc(KernelContext *kc_in, void *curr_pcb_v, void *next_pcb_v) {
    PCB_t *curr = (PCB_t *)curr_pcb_v;
    PCB_t *next = (PCB_t *)next_pcb_v;

    if (curr == next) {
        TracePrintf(0, "KCSwitchFunc: ERROR switching to self pid=%d\n", curr->pid);
        return kc_in;
    }
    if (curr == NULL || next == NULL) {
        TracePrintf(0, "KCSwitchFunc: NULL pcb!\n");
        return NULL;
    }

    curr->krn_ctx = *kc_in;

    int ks_base_pg = KERNEL_STACK_BASE >> PAGESHIFT;
    int ks_npg     = KERNEL_STACK_MAXSIZE >> PAGESHIFT;
    for (int i = 0; i < ks_npg; i++) {
        KernelPT[ks_base_pg + i].pfn  = next->kstack_pfn[i];
        KernelPT[ks_base_pg + i].valid = 1;
        KernelPT[ks_base_pg + i].prot  = PROT_READ | PROT_WRITE;
    }
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_KSTACK);

    WriteRegister(REG_PTBR1, (unsigned int)next->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

    return &next->krn_ctx;
}


KernelContext *KCCopyFunc(KernelContext *kc_in, void *new_pcb_v, void *unused) {
    PCB_t *new_pcb = (PCB_t *)new_pcb_v;

    int temp_vpn  = (KERNEL_STACK_BASE >> PAGESHIFT) - 1;
    int ks_base_pg = KERNEL_STACK_BASE >> PAGESHIFT;
    int ks_npg     = KERNEL_STACK_MAXSIZE >> PAGESHIFT;

    // copy each kernel stack page into child's frames via temp mapping
    for (int i = 0; i < ks_npg; i++) {
        KernelPT[temp_vpn].pfn  = new_pcb->kstack_pfn[i];
        KernelPT[temp_vpn].valid = 1;
        KernelPT[temp_vpn].prot  = PROT_READ | PROT_WRITE;
        WriteRegister(REG_TLB_FLUSH, (unsigned int)(temp_vpn << PAGESHIFT));
        memcpy((void *)(temp_vpn << PAGESHIFT),
               (void *)((ks_base_pg + i) << PAGESHIFT),
               PAGESIZE);
    }

    // unmap temp window
    KernelPT[temp_vpn].valid = 0;
    WriteRegister(REG_TLB_FLUSH, (unsigned int)(temp_vpn << PAGESHIFT));

    // save context into child, kc_in->sp is in the current kstack,
    // which we just copied into child's frames, so it's valid for child too
    new_pcb->krn_ctx = *kc_in;
    new_pcb->init = 1;

    // return kc_in, stay on parent's stack, parent continues in sys_fork
    return kc_in;
}

extern void KernelStart(char **argv, unsigned int pmem_size, UserContext *ctx) {
    TracePrintf(0, "KernelStart\n");

    int num_frames = pmem_size / PAGESIZE;
    int i;

    // initialize frame tracker
    for (i = 0; i < num_frames; i++) {
        frames[i] = (i < _orig_kernel_brk_page) ? 1 : 0;
    }

    // initialize kernel page table
    for (i = 0; i < MAX_PT_LEN; i++) {
        KernelPT[i].valid = 0;
    }

    // map kernel text pages (read/exec)
    for (i = _first_kernel_text_page; i < _first_kernel_data_page; i++) {
        KernelPT[i].valid = 1;
        KernelPT[i].pfn   = i;
        KernelPT[i].prot  = PROT_READ | PROT_EXEC;
    }

    // map kernel data/heap pages (read/write)
    for (i = _first_kernel_data_page; i < _orig_kernel_brk_page; i++) {
        KernelPT[i].valid = 1;
        KernelPT[i].pfn   = i;
        KernelPT[i].prot  = PROT_READ | PROT_WRITE;
    }

    // reserve kstack frames for init, these are the frames the hardware
    // will use for the very first kernel stack before any switch happens
    int kstack_pfns[2] = {0x7e, 0x7f};
    int ks_base_pg = KERNEL_STACK_BASE >> PAGESHIFT;
    int ks_npg     = KERNEL_STACK_MAXSIZE >> PAGESHIFT;

    for (i = 0; i < ks_npg; i++) {
        frames[kstack_pfns[i]] = 1;
        KernelPT[ks_base_pg + i].valid = 1;
        KernelPT[ks_base_pg + i].pfn   = kstack_pfns[i];
        KernelPT[ks_base_pg + i].prot  = PROT_READ | PROT_WRITE;
    }

    // set up interrupt vector table, every slot points to thandler
    for (i = 0; i < TRAP_VECTOR_SIZE; i++) {
        IVT[i] = &thandler;
    }
    WriteRegister(REG_VECTOR_BASE, (unsigned int)&IVT[0]);

    curr_kbrk = (void *)UP_TO_PAGE(sbrk(0));

    // initialize terminal state
    for (i = 0; i < NUM_TERMINALS; i++) {
        tty_write_req[i] = NULL;
        tty_read_queue[i] = NULL;
        tty_read_len[i]   = 0;
        tty_busy[i]       = 0;
    }
	sync_init();

    //  create init PCB 
    init_pcb = (PCB_t *)malloc(sizeof(PCB_t));
    memset(init_pcb, 0, sizeof(PCB_t));
    init_pcb->r1pt = (pte_t *)malloc(sizeof(pte_t) * MAX_PT_LEN);
    for (i = 0; i < MAX_PT_LEN; i++) init_pcb->r1pt[i].valid = 0;
    init_pcb->pid = helper_new_pid(init_pcb->r1pt);
    for (i = 0; i < ks_npg; i++) {
        init_pcb->kstack_pfn[i] = kstack_pfns[i];
    }

    //  create idle PCB 
    idle_pcb = (PCB_t *)malloc(sizeof(PCB_t));
    memset(idle_pcb, 0, sizeof(PCB_t));
    idle_pcb->r1pt = (pte_t *)malloc(sizeof(pte_t) * MAX_PT_LEN);
    for (i = 0; i < MAX_PT_LEN; i++) idle_pcb->r1pt[i].valid = 0;
    idle_pcb->pid = helper_new_pid(idle_pcb->r1pt);

    // allocate idle's kernel stack frames (distinct from init's)
    for (i = 0; i < ks_npg; i++) {
        int f = find_free();
        if (f == ERROR) {
            TracePrintf(0, "KernelStart: no free frames for idle kstack\n");
            Halt();
        }
        frames[f] = 1;
        idle_pcb->kstack_pfn[i] = f;
        TracePrintf(0, "idle kstack_pfn[%d] = %d (0x%x)\n", i, f, f);
    }
    TracePrintf(0, "idle_pcb address = %p\n", idle_pcb);
    TracePrintf(0, "idle_pcb->kstack_pfn address = %p\n", idle_pcb->kstack_pfn);

    // allocate one user stack page for idle's region 1
    {
        int svpn = MAX_PT_LEN - 1;
        int fus  = find_free();
        if (fus == ERROR) { TracePrintf(0, "KernelStart: no frame for idle user stack\n"); Halt(); }
        frames[fus] = 1;
        idle_pcb->r1pt[svpn].valid = 1;
        idle_pcb->r1pt[svpn].pfn   = fus;
        idle_pcb->r1pt[svpn].prot  = PROT_READ | PROT_WRITE;
    }

    // set up sibling pointers
    idle_pcb->sibling = init_pcb;
    init_pcb->sibling = idle_pcb;

    // set up process globals before enabling VM
    current_process  = init_pcb;
    ready_queue_head = init_pcb;
    sleep_queue_head = NULL;

    // enable VM -- from here on all addresses are virtual
    WriteRegister(REG_PTBR0, (unsigned int)KernelPT);
    WriteRegister(REG_PTLR0, MAX_PT_LEN);
    WriteRegister(REG_PTBR1, (unsigned int)init_pcb->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_VM_ENABLE, 1);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_ALL);

    // load init program into init's address space
    char *init_name = (argv != NULL && argv[0] != NULL) ? argv[0] : "init";
    if (LoadProgram(init_name, argv, init_pcb) != SUCCESS) {
        TracePrintf(0, "KernelStart: LoadProgram init failed\n");
        Halt();
    }

    // load idle program into idle's address space
    WriteRegister(REG_PTBR1, (unsigned int)idle_pcb->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
    if (LoadProgram("idle", NULL, idle_pcb) != SUCCESS) {
        TracePrintf(0, "KernelStart: LoadProgram idle failed\n");
        Halt();
    }

    TracePrintf(0, "idle usr_ctx.pc=%p sp=%p\n", idle_pcb->usr_ctx.pc, idle_pcb->usr_ctx.sp);
    TracePrintf(0, "init usr_ctx.pc=%p sp=%p\n", init_pcb->usr_ctx.pc, init_pcb->usr_ctx.sp);

    // switch back to init's region 1 before saving contexts
    WriteRegister(REG_PTBR1, (unsigned int)init_pcb->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

    //  save init's kernel context 
    // kstack is already mapped to init's frames (kstack_pfns), so this is correct
    KernelContextSwitch(KCSInitFunc, init_pcb, NULL);
    init_pcb->init = 1;

    int temp_vpn = (KERNEL_STACK_BASE >> PAGESHIFT) - 1;

    for (i = 0; i < ks_npg; i++) {

        KernelPT[temp_vpn].pfn   = idle_pcb->kstack_pfn[i];
        KernelPT[temp_vpn].valid = 1;
        KernelPT[temp_vpn].prot  = PROT_READ | PROT_WRITE;

        WriteRegister(REG_TLB_FLUSH, (unsigned int)(temp_vpn << PAGESHIFT));

    memcpy((void *)(temp_vpn << PAGESHIFT), (void *)((ks_base_pg + i) << PAGESHIFT), PAGESIZE);
    }

    KernelPT[temp_vpn].valid = 0;
    WriteRegister(REG_TLB_FLUSH,(unsigned int)(temp_vpn << PAGESHIFT));
    // save idle's kernel context 
    // must remap the kstack PTEs to idle's frames before calling KCSInitFunc
    // so that the saved SP in idle->krn_ctx matches idle's physical frames
    for (i = 0; i < ks_npg; i++) {
        KernelPT[ks_base_pg + i].pfn  = idle_pcb->kstack_pfn[i];
        KernelPT[ks_base_pg + i].valid = 1;
        KernelPT[ks_base_pg + i].prot  = PROT_READ | PROT_WRITE;
    }
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_KSTACK);

    WriteRegister(REG_PTBR1, (unsigned int)idle_pcb->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
    KernelContextSwitch(KCSInitFunc, idle_pcb, NULL);
    idle_pcb->init = 1;
    TracePrintf(0, "IDLE INIT: krn_ctx=%p stack0=%d stack1=%d\n", &idle_pcb->krn_ctx, idle_pcb->kstack_pfn[0], idle_pcb->kstack_pfn[1]);

    // restore init's kstack before returning to userland
    for (i = 0; i < ks_npg; i++) {
        KernelPT[ks_base_pg + i].pfn  = init_pcb->kstack_pfn[i];
        KernelPT[ks_base_pg + i].valid = 1;
        KernelPT[ks_base_pg + i].prot  = PROT_READ | PROT_WRITE;
    }
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_KSTACK);

    // restore init's region 1 (it was changed during idle load)
    WriteRegister(REG_PTBR1, (unsigned int)init_pcb->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

    TracePrintf(0, "init_pcb=%p pid=%d sibling=%p\n", init_pcb, init_pcb->pid, init_pcb->sibling);
    TracePrintf(0, "idle_pcb=%p pid=%d sibling=%p\n", idle_pcb, idle_pcb->pid, idle_pcb->sibling);
    TracePrintf(0, "current_process=%p\n", current_process);

    // hand the hardware init's user context to run first
    *ctx = init_pcb->usr_ctx;
    TracePrintf(0, "KernelStart done. Idle & Init ready.\n");
}

int SetKernelBrk(void *addr){
    unsigned int new_brk = (unsigned int)UP_TO_PAGE(addr);
    unsigned int curr_brk = (unsigned int)UP_TO_PAGE(curr_kbrk);

	if(new_brk >= KERNEL_STACK_BASE){
        TracePrintf(0, "SetKernelBrk: Error - Kernel heap collision with kernel stack.\n");
        return ERROR;
    } /*looking for collision between  new break and kernel stack base. The heap cannot start behind the stack*/

	if(ReadRegister(REG_VM_ENABLE) == 0){
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
            frames[f] = 1;
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


