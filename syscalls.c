#include <hardware.h>
#include <load_info.h>
#include <yuser.h>
#include <unistd.h>
#include <yalnix.h>
#include <ykernel.h>
#include <traps.h>
#include <kern.h>
#include <load.h>

extern PCB_t *idle_pcb;
extern KernelContext *KCSwitchFunc();

/* sys_brk adjusts user heap with red zone and collision checks */
int sys_brk(PCB_t *proc, void *addr) {
	TracePrintf(0, "sys_brk: addr=%p brk=%p heap_base=%p sp=%p\n",
        addr, proc->brk, proc->heap_base, proc->usr_ctx.sp);
	if (addr == NULL) return (int)proc->brk;
	if ((unsigned int)addr >= VMEM_1_LIMIT || 
        (unsigned int)addr < VMEM_1_BASE) {
        TracePrintf(0, "sys_brk: addr %p out of region 1\n", addr);
        return ERROR;
    }	

    unsigned int new_brk = (unsigned int)UP_TO_PAGE(addr);
    unsigned int cur_brk = (unsigned int)UP_TO_PAGE(proc->brk);

    /* ensure the heap does not go below the original data segment */
    if ((unsigned int)addr < (unsigned int)proc->heap_base) return ERROR;

    /* leave at least one unmapped page below the stack */
    unsigned int stack_limit = (unsigned int)DOWN_TO_PAGE(proc->usr_ctx.sp);
    if (new_brk >= stack_limit - PAGESIZE) {
        TracePrintf(0, "sys_brk: collision with stack red zone\n");
        return ERROR;
    }

    int cur_vpn = (cur_brk - VMEM_1_BASE) >> PAGESHIFT;
    int new_vpn = (new_brk - VMEM_1_BASE) >> PAGESHIFT;

    if (new_vpn > cur_vpn) {
        /* growing the heap: allocate new frames */
        for (int i = cur_vpn; i < new_vpn; i++) {
            int f = find_free();
            if (f == ERROR) return ERROR;
            frames[f] = 1;
            proc->r1pt[i].valid = 1;
            proc->r1pt[i].pfn = f;
            proc->r1pt[i].prot = PROT_READ | PROT_WRITE;
        }
    } else if (new_vpn < cur_vpn) {
        /* shrinking the heap so free existing frames */
        for (int i = new_vpn; i < cur_vpn; i++) {
            if (proc->r1pt[i].valid) {
                frames[proc->r1pt[i].pfn] = 0;
                proc->r1pt[i].valid = 0;
            }
        }
    }

    proc->brk = (void *)new_brk;
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1); /* flush stale mappings */
    return SUCCESS;
}

/* sys_delay validates input and moves process to sleep queue */
int sys_delay(PCB_t *proc, UserContext *uc, int ticks) {
    if (ticks == 0) return SUCCESS;
    if (ticks < 0) return ERROR;

    memcpy(&proc->usr_ctx, uc, sizeof(UserContext));
    proc->delay = ticks;

    // add to sleep queue
    proc->next = sleep_queue_head;
    sleep_queue_head = proc;

    // pick next from ready queue, fall back to idle
    PCB_t *next;
    if (ready_queue_head != NULL) {
        next = ready_queue_head;
        ready_queue_head = ready_queue_head->next;
        next->next = NULL;
    } else {
        next = idle_pcb;
    }

    PCB_t *old = current_process;
    current_process = next;
    KernelContextSwitch(KCSwitchFunc, old, current_process);

	memcpy(uc, &current_process->usr_ctx, sizeof(UserContext));
    uc->regs[0] = SUCCESS;  // return value for the process waking up

    WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

    return SUCCESS;
}

/* sys_exit terminates process and halts if is init */
void sys_exit(PCB_t *proc, int status) {
    TracePrintf(0, "process %d exiting status=%d\n",
                proc->pid, status);

    // init exiting halts whole system
    if (proc->pid == 1) {
        TracePrintf(0, "init exited -- halting\n");
        Halt();
    }

    // mark as zombie
    proc->state = ZOMBIE;
    proc->exstat = status;

    // wake parent if waiting
    if (proc->parent != NULL &&
        proc->parent->state == WAITING) {

        proc->parent->state = READY;

        proc->parent->next = ready_queue_head;
        ready_queue_head = proc->parent;
    }

    // choose next runnable process
    PCB_t *next = ready_queue_head;

    if (next != NULL) {
        ready_queue_head = next->next;
        next->next = NULL;
    } else {
        next = idle_pcb;
    }

    PCB_t *old = current_process;
    current_process = next;

    TracePrintf(0, "sys_exit: switching from pid=%d to pid=%d\n",
                old->pid,
                next->pid);

    KernelContextSwitch(KCSwitchFunc, old, next);

    // should never return
    TracePrintf(0, "ERROR: returned from sys_exit\n");

    Halt();
}

int sys_fork(PCB_t *parent, UserContext *uc) {
    // create child PCB
    PCB_t *child = (PCB_t *)malloc(sizeof(PCB_t));
    if (child == NULL) return ERROR;
    memset(child, 0, sizeof(PCB_t));

    // allocate child region 1 page table
    child->r1pt = (pte_t *)malloc(sizeof(pte_t) * MAX_PT_LEN);
    if (child->r1pt == NULL) {
        free(child);
        return ERROR;
    }
    memset(child->r1pt, 0, sizeof(pte_t) * MAX_PT_LEN);

    // allocate child kernel stack frames
    int ks_base_pg = KERNEL_STACK_BASE >> PAGESHIFT;
    int ks_npg = KERNEL_STACK_MAXSIZE >> PAGESHIFT;
    for (int i = 0; i < ks_npg; i++) {
        int f = find_free();
        if (f == ERROR) {
            free(child->r1pt);
            free(child);
            return ERROR;
        }
        frames[f] = 1;
        child->kstack_pfn[i] = f;
    }

    // copy parent's region 1 page table -- copy on write would be better
    // but for now do a full copy
    int temp_vpn = (KERNEL_STACK_BASE >> PAGESHIFT) - 1;  // temp mapping window

    for (int i = 0; i < MAX_PT_LEN; i++) {
        if (!parent->r1pt[i].valid) continue;

        // allocate a new frame for the child
        int f = find_free();
        if (f == ERROR) {
            // undo allocations
            for (int j = 0; j < i; j++) {
                if (child->r1pt[j].valid) {
                    frames[child->r1pt[j].pfn] = 0;
                    child->r1pt[j].valid = 0;
                }
            }
            for (int j = 0; j < ks_npg; j++) {
                frames[child->kstack_pfn[j]] = 0;
            }
            free(child->r1pt);
            free(child);
            return ERROR;
        }
        frames[f] = 1;

        // map temp_vpn to the child's new frame so we can write to it
        KernelPT[temp_vpn].valid = 1;
        KernelPT[temp_vpn].pfn = f;
        KernelPT[temp_vpn].prot = PROT_READ | PROT_WRITE;
        WriteRegister(REG_TLB_FLUSH, (unsigned int)(temp_vpn << PAGESHIFT));

        // copy parent page to child frame via temp mapping
        void *parent_page = (void *)(VMEM_1_BASE + (i << PAGESHIFT));
        void *temp_page = (void *)(temp_vpn << PAGESHIFT);
        memcpy(temp_page, parent_page, PAGESIZE);

        // set up child PTE
        child->r1pt[i].valid = 1;
        child->r1pt[i].pfn = f;
        child->r1pt[i].prot = parent->r1pt[i].prot;
    }

    // unmap temp window
    KernelPT[temp_vpn].valid = 0;
    WriteRegister(REG_TLB_FLUSH, (unsigned int)(temp_vpn << PAGESHIFT));

    // set up child PCB fields
    child->pid = helper_new_pid(child->r1pt);
    child->ppid = parent->pid;
    child->parent = parent;
    child->brk = parent->brk;
    child->heap_base = parent->heap_base;
    child->state = 0;
    child->delay = 0;

    // copy parent user context into child -- child returns 0 from fork
    child->usr_ctx = *uc;
    child->usr_ctx.regs[0] = 0;

    // link child into parent's child list
    child->sibling = parent->child;
    parent->child = child;

    // add child to ready queue
    child->next = ready_queue_head;
    ready_queue_head = child;
	TracePrintf(0, "sys_fork: child pid=%d on ready_queue, head=%p\n", child->pid, ready_queue_head);
    // save parent's user context
    memcpy(&parent->usr_ctx, uc, sizeof(UserContext));

	
    // initialize child kernel context via KCSwitchFunc
	KernelContextSwitch(KCSwitchFunc, parent, child);

	for (int i = 0; i < ks_npg; i++) {
		KernelPT[ks_base_pg + i].pfn = parent->kstack_pfn[i];
		KernelPT[ks_base_pg + i].valid = 1;
		KernelPT[ks_base_pg + i].prot = PROT_READ | PROT_WRITE;
	}
	WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_KSTACK);

	//child->init = 1;
	current_process = parent;
	return child->pid;
}

int sys_exec(PCB_t *proc, UserContext *uc, char *filename, char **args) {
    // validate filename
    if (filename == NULL) return ERROR;

    // save current user context
    memcpy(&proc->usr_ctx, uc, sizeof(UserContext));

    // LoadProgram replaces the current address space
    int rc = LoadProgram(filename, args, proc);
    if (rc != SUCCESS) {
        TracePrintf(0, "sys_exec: LoadProgram failed for %s\n", filename);
        // process is now in bad state -- kill it
        sys_exit(proc, ERROR);
        return ERROR;
    }

    // update uc so we return to new program
    memcpy(uc, &proc->usr_ctx, sizeof(UserContext));

    // remap region 1 to new page table
    WriteRegister(REG_PTBR1, (unsigned int)proc->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

    return SUCCESS;
}

int sys_wait(PCB_t *proc, UserContext *uc, int *status_ptr) {
    TracePrintf(0, "sys_wait: pid=%d child=%p ready_queue=%p idle=%p\n",
        proc->pid, proc->child, ready_queue_head, idle_pcb);

    if (proc->child == NULL) return ERROR;

    while (1) {
        // check for zombie children
        PCB_t *prev = NULL;
        PCB_t *child = proc->child;
        while (child != NULL) {
            if (child->state == ZOMBIE) {
                if (status_ptr != NULL) *status_ptr = child->exstat;
                int cpid = child->pid;

                if (prev == NULL) proc->child = child->sibling;
                else prev->sibling = child->sibling;

                free_region1(child->r1pt);
                free(child->r1pt);
                for (int i = 0; i < KERNEL_STACK_MAXSIZE >> PAGESHIFT; i++) {
                    frames[child->kstack_pfn[i]] = 0;
                }
                free(child);
                return cpid;
            }
            prev = child;
            child = child->sibling;
        }

        // no zombie yet -- block
        proc->state = WAITING;
        memcpy(&proc->usr_ctx, uc, sizeof(UserContext));

        PCB_t *next = ready_queue_head;
        if (next != NULL) {
            ready_queue_head = next->next;
            next->next = NULL;
        } else {
            next = idle_pcb;
        }

        TracePrintf(0, "sys_wait: blocking pid=%d switching to next=%p idle=%p\n",
            proc->pid, next, idle_pcb);

        PCB_t *old = current_process;
        current_process = next;
        KernelContextSwitch(KCSwitchFunc, old, current_process);

        // resumed by sys_exit waking us up
        current_process = proc;
        WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
        WriteRegister(REG_PTLR1, MAX_PT_LEN);
        WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
        // loop back to check for zombie
    }
}
/*

Fork
	Create child pcb and page table
	Copy parent pcb state into child
	Alloc temp kernel page
	For each valid parent region 1 page
		Alloc child frame
		Copy parent page to temp page
		Switch to child page table
		Copy temp page to child page
		Restore parent table
	Free temp page
	Alloc child kernel stack pages
	Create child kernel context
	If running as child return 0
	Else put child in ready queue and return PID to parent

Exec
	Get curr PCB and metadata
	Open and validate exec file
	LoadProgram(file, args, currPCB)

Exit
	Get curr proc
	If proc = init halt
	Find parent proc
	If parent exists
	store child exit status in parent queue
		If parent waiting wake
	Schedule another proc
	
Wait
	Check if proc has children
	If no child return error
	While no exited children block curr proc and schedule others
	Retrieve exit status
	Return child PID

GetPid
	Return current PID

Brk
	Validate addr is in region 1
	Get current proc and current brk
	If addr > current brk and enough mem is available
		Alloc additional heap pages
	Else free heap pages
	Update proc brk
	
Delay
	If ticks == 0 return 0
	Set proc sleep timer
	Move proc to sleep queue
	Schedule another proc

TtyWrite
	Wait until terminal free
	Create terminal write request
	Copy user buffer into kernel buffer
	Add request to terminal queue
	Split output into terminal-sized chunks
	For all chunks
		Transmit chunk
		Block until transmit interrupt
		Wait if another proc writing
	Update serviced byte count
	Remove req

TtyRead
	Create terminal read req
	Allocate kernel buffer
	Append req to terminal read queue
	Move proc to read-blocked queue
	Move to other proc
	When awakened restore proc state
	Copy received data from kernel buffer to user buffer
	Remove terminal req
	
PipeInit
	Generate unique pipe id
	Create pipe struct
	Store pipe id in struct

PipeRead
	Find pipe
	Check validity of pipe
	Wait for enough data to be sent through
	Copy pipe data into user buffer
	Shift remaining pipe content
	Update pipe length

PipeWrite
	Find pipe
	Check validity and if full
	Copy data into pipe buffer
	Update pipe length
	Wake waiting readers

LockInit
	Create new lock
	Assign ownership to current proc
	Store lock id
	
Acquire
	Find lock
	Make sure lock is valid and not already owned by proc calling func
	If unlocked give lock, otherwise put on wait queue

Release
	Find lock
	Make sure lock is valid and owned by current proc
	Give up lock
	Transfer ownership to waiting proc
	Wake waiting proc

CvarInit
	Create cond var
	Store cvar ID
	
CvarSignal
	Find cvar
	Wake one waiting proc
	Move proc to ready queue
	
CvarBroadcast
	Find cvar
	Wake all waiting procs
	Move all to ready queue

CvarWait
	Validate cvar and lock
	Ensure caller owns lock
	Associate cvar w lock
	Release lock
	Block proc on cvar queue
	When awakened, reacquire lock

Reclaim
	Determine sync object type
	Free resources


*/
/*
int sys_brk(PCB_t *proc, void *addr) {
    unsigned int new_brk = (unsigned int)UP_TO_PAGE(addr);
    unsigned int cur_brk = (unsigned int)UP_TO_PAGE(proc->brk);

    // can't go below initial data end or above stack
    if ((unsigned int)addr < (unsigned int)proc->heap_base) {
        return ERROR;
    }
    if (new_brk >= (unsigned int)proc->usr_ctx.sp) {
        return ERROR;
    }

    int data_pg1 = ((int)proc->heap_base - VMEM_1_BASE) >> PAGESHIFT;
    int new_vpn = (new_brk - VMEM_1_BASE) >> PAGESHIFT;
    int cur_vpn = (cur_brk - VMEM_1_BASE) >> PAGESHIFT;

    if (new_vpn > cur_vpn) {
        // growing heap
        for (int i = cur_vpn; i < new_vpn; i++) {
            int f = find_free();
            if (f == ERROR) {
                // undo partial allocation
                for (int j = cur_vpn; j < i; j++) {
                    frames[proc->r1pt[j].pfn] = 0;
                    proc->r1pt[j].valid = 0;
                    proc->r1pt[j].pfn = 0;
                }
                WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
                return ERROR;
            }
            frames[f] = 1;
            proc->r1pt[i].valid = 1;
            proc->r1pt[i].pfn = f;
            proc->r1pt[i].prot = PROT_READ | PROT_WRITE;
        }
    } else if (new_vpn < cur_vpn) {
        // shrinking heap
        for (int i = new_vpn; i < cur_vpn; i++) {
            if (proc->r1pt[i].valid) {
                frames[proc->r1pt[i].pfn] = 0;
                proc->r1pt[i].valid = 0;
                proc->r1pt[i].pfn = 0;
            }
        }
    }

    proc->brk = (void *)new_brk;
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
    return SUCCESS;
}
*/
int sys_getpid(PCB_t *proc) {
    return proc->pid;
}

