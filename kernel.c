#include <hardware.h>
#include <traps.h>
#include <yalnix.h>
#include <ykernel.h>

void *IVT[TRAP_VECTOR_SIZE];
PCB_t *current_process = NULL;
PCB_t *ready_queue_head = NULL;
PCB_t *sleep_queue_head = NULL;

extern void KernelStart(char **argv, unsigned int pmem_size, UserContext *ctx) {
    InitKernelMemoryMap(pmem_size);

    for(int i = 0; i < TRAP_VECTOR_SIZE; i++) {
        IVT[i] = &thandler;
    }
    WriteRegister(REG_VECTOR_BASE, (unsigned int)(&(IVT)));

    current_process = (PCB_t *)malloc(sizeof(PCB_t)); /*Init process PCB*/
    current_process->r1pt = (pte_t *)malloc(sizeof(pte_t) * MAX_PT_LEN);
    for(int i = 0; i < MAX_PT_LEN; i++) { /*clear r1 table*/
        current_process->r1pt[i].valid = 0;
    }

    WriteRegister(REG_PTBR0, (unsigned int)KernelPT);
    WriteRegister(REG_PTLR0, MAX_PT_LEN);
    WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);

    int kstack_idx = KERNEL_STACK_BASE >> PAGESHIFT;
    for (int i = 0; i < 2; i++) { /*allocate for 2 frames*/
        int f = find_free();
        frames[f] = 1;
        KernelPT[kstack_idx + i].valid = 1;
        KernelPT[kstack_idx + i].pfn = f;
        KernelPT[kstack_idx + i].prot = PROT_READ | PROT_WRITE;
    }

    curr_kbrk = (void *)UP_TO_PAGE(sbrk(0)); /*initialized for top of kernel heap*/
    int svpn = (VMEM_1_LIMIT >> PAGESHIFT) - 1; /*index for last page in r1*/
    int fus = find_free(); /*find a physical frame for the user stack*/
    frames[fus] = 1;

    current_process->r1pt[svpn].valid = 1;
    current_process->r1pt[svpn].pfn = fus;
    current_process->r1pt[svpn].prot = PROT_READ | PROT_WRITE;

    current_process->pid = helper_new_pid(current_process->r1pt); /*get pid for new process*/

    WriteRegister(REG_VM_ENABLE, 1); /*ENABLE THE BIG VM*/
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_ALL); /*tmv flush*/

    current_process->usr_ctx = *ctx;
    current_process->usr_ctx.pc = (void *)DoIdle;
    current_process->usr_ctx.sp = (void *)VMEM_1_LIMIT;
    *ctx = current_process->usr_ctx;
}

void thandler(UserContext usr_cont) {
    switch (usr_cont.vector) {
        case TRAP_CLOCK:
            TracePrintf(1, "TRAP_CLOCK\n");
            break;
        case TRAP_KERNEL: /*Hex for format. example, 0xabcdef01.*/
            TracePrintf(1, "TRAP_KERNEL: syscall code 0x%x\n", usr_cont.code);
            break;
        default: /*unexpected trap occurences*/
            TracePrintf(0, "Unhandled trap: %d at PC %p\n", usr_cont.vector, usr_cont.pc);
            break;
    }
}

