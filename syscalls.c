#include <hardware.h>
#include <load_info.h>
#include <yuser.h>
#include <unistd.h>
#include <yalnix.h>
#include <ykernel.h>
#include <traps.h>
#include <kern.h>

/* sys_brk adjusts user heap with red zone and collision checks */
int sys_brk(PCB_t *proc, void *addr) {
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
int sys_delay(PCB_t *proc, int ticks) {
    if (ticks == 0) return SUCCESS;
    if (ticks < 0) return ERROR; /* time travel is not allowed */

    proc->delay = ticks;
    proc->next = sleep_queue_head;
    sleep_queue_head = proc;

    /* immediately switch to the next ready process */
    PCB_t *old_proc = proc;
    current_process = proc->sibling; 
    while (current_process->delay > 0) {
        current_process = current_process->sibling;
    }
    KernelContextSwitch(KCSwitchFunc, old_proc, current_process);

    return SUCCESS;
}

/* sys_exit terminates process and halts if is init */
void sys_exit(PCB_t *proc, int status) {
    TracePrintf(0, "process %d exiting with status %d\n", proc->pid, status);
    /* the init death rule: halt if init exist */
    if (proc->pid == 1) {
        TracePrintf(0, "init process exited: halting system\n");
        Halt();
    }
    Halt();
}

/* clones current process */
int sys_fork(PCB_t *parent){
    /*get new pcb and page table */
    PCB_t *child = (PCB_t *)malloc(sizeof(PCB_t));
    child->r1pt = (pte_t *)malloc(sizeof(pte_t) * MAX_PT_LEN);
    child->pid = helper_new_pid(child->r1pt); /* tell hardware helper */

    /* copy all region 1 pages */
    int temp_vpn = (KERNEL_STACK_BASE >> PAGESHIFT)-1; /* temp window pg */
    for(int i=0; i<MAX_PT_LEN; i++){
        if(parent->r1pt[i].valid){
            int f = find_free();
            child->r1pt[i] = parent->r1pt[i];
            child->r1pt[i].pfn = f;
            
            /* map frame to temp page so we can copy bytes into it */
            KernelPT[temp_vpn].pfn = f;
            KernelPT[temp_vpn].valid = 1;
            KernelPT[temp_vpn].prot = PROT_READ | PROT_WRITE;
            WriteRegister(REG_TLB_FLUSH, (unsigned int)(temp_vpn << PAGESHIFT));
            
            /* copy data from parent addr to child's new frame */
            memcpy((void *)(temp_vpn << PAGESHIFT), (void *)(VMEM_1_BASE + (i * PAGESHIFT)), PAGESIZE);
        }
    }
    KernelPT[temp_vpn].valid = 0; /* unmap window */

    /* copy kernel stack and context */
    KernelContextSwitch(KCCopyFunc, child, NULL);

    /* switch logic based on who returns */
    if(current_process == child){
        return 0; /* child returns 0 */
    } else {
        /* parent links child into tree and ready queue */
        child->parent = parent;
        child->sibling = parent->children;
        parent->children = child;
        /*put child in your ready list */
        return child->pid; /*parent returns child pid*/
    }
}

int sys_exec(char *filename, char **argvec) {
    /* check pointers aren't junk*/
    if (filename == NULL) return ERROR;

    /* count and copy args to kernel heap so they don't get deleted */
    /* argbuf = copy_to_kernel(argvec);  pseudocode for string copying */

    /* this destroys old region 1 and loads new code */
    if (LoadProgram(filename, argvec, current_process) == ERROR) {
        /* free(argbuf); */
        return ERROR;
    }

    /* free(argbuf); */
    return SUCCESS; 
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

