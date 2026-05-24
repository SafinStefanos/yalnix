#include <hardware.h>
#include <load_info.h>
#include <yuser.h>
#include <unistd.h>
#include <yalnix.h>
#include <ykernel.h>
#include <traps.h>
#include <kern.h>

/* sys_brk adjusts user heap with red zone and collision checks */
int sys_brk(PCB_t *proc, void *addr){
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
int sys_delay(PCB_t *proc, int ticks){
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
void sys_exit(int status){
    if (current_process->pid == 1) Halt(); /* init death rule */

    current_process->exit_status = status;
    current_process->is_zombie = 1;

    /* reparent children to init */
    PCB_t *child = current_process->children;
    while (child) {
        child->parent = init_pcb;
        child = child->sibling;
    }
    /*wake parent if they are waiting */
    if (current_process->parent->state == STATE_WAITING) {
        current_process->parent->state = STATE_READY;
        enqueue(ready_queue, current_process->parent, sizeof(PCB_t *));
    }
    /* schedule someone else*/
    schedule_next(); 
}

/* clones current process */
int sys_fork(PCB_t *parent){
    PCB_t *child = (PCB_t *)malloc(sizeof(PCB_t));
    child->r1pt = (pte_t *)malloc(sizeof(pte_t) * MAX_PT_LEN);
    child->pid = helper_new_pid(child->r1pt);

    /* copy user memory via temp region 0 window */
    int temp_vpn = (KERNEL_STACK_BASE >> PAGESHIFT) - 1;
    for (int i = 0; i < MAX_PT_LEN; i++) {
        if (parent->r1pt[i].valid) {
            int f = find_free();
            child->r1pt[i] = parent->r1pt[i];
            child->r1pt[i].pfn = f;
            
            KernelPT[temp_vpn].pfn = f;
            KernelPT[temp_vpn].valid = 1;
            KernelPT[temp_vpn].prot = PROT_READ | PROT_WRITE;
            WriteRegister(REG_TLB_FLUSH, (unsigned int)(temp_vpn << PAGESHIFT));
            
            memcpy((void *)(temp_vpn << PAGESHIFT), (void *)(VMEM_1_BASE + (i * PAGESHIFT)), PAGESIZE);
        }
    }
    KernelPT[temp_vpn].valid = 0;

    /* allocate child kstack frames and clone */
    for (int i = 0; i < 2; i++) child->kstack_pfn[i] = find_free();
    KernelContextSwitch(KCCopyFunc, child, NULL);

    if (current_process == child) return 0;
    
    /* parent: link child and ready it */
    child->parent = parent;
    child->sibling = parent->children;
    parent->children = child;
    enqueue(ready_queue, child, sizeof(PCB_t *)); 
    return child->pid;
}
/*replace program image*/
int sys_exec(char *filename, char **argvec){
    /* check pointers before wipe */
    if (filename == NULL) return ERROR;

    /* LoadProgram handles arg buffering and region 1 cleanup */
    if (LoadProgram(filename, argvec, current_process) == ERROR) return ERROR;
    return SUCCESS; 
}

int sys_wait(int *status_ptr) {
    /* error if no children exist at all */
    if (current_process->child == NULL) return ERROR;

    /* check status_ptr validity if provided */
    if (status_ptr != NULL && !is_valid_ptr(status_ptr, PROT_WRITE)) return ERROR;

    while (1) {
        PCB_t *curr = current_process->child;
        PCB_t *prev = NULL;

        while (curr != NULL) {
            if (curr->is_zombie) {
                int pid = curr->pid;
                if (status_ptr != NULL) *status_ptr = curr->exstat;

                /* remove from parent's sibling list */
                if (prev == NULL) current_process->child = curr->sibling;
                else prev->sibling = curr->sibling;

                /* clean up child pcb and notify hardware helper */
                helper_retire_pid(pid); [6]
                free_region1(curr->r1pt); [7]
                free(curr->r1pt);
                free(curr); 

                return pid; /* return the pid of reaped child */
            }
            prev = curr;
            curr = curr->sibling;
        }

        /*no zombies yet so block the parent and pick someone else*/
        current_process->state = STATE_WAITING;
        /*call your scheduler here */
        schedule_next(); 
    }
}

/*check if memory fault is valid stack growth request */
int should_grows(void *addr) {
    unsigned int fault_addr = (unsigned int)addr;
    unsigned int stack_top = (unsigned int)DOWN_TO_PAGE(current_process->usr_ctx.sp);
    
    /* fault must be in region 1, below current stack, and above heap break */
    if (fault_addr < stack_top && fault_addr >= VMEM_1_BASE && fault_addr > (unsigned int)current_process->brk) {
        /* enforce 1-page red zone: cannot grow into the page right above the heap */
        if (fault_addr <= (unsigned int)current_process->brk + PAGESIZE) return 0;
        return 1;
    }
    return 0;
}

int grow_stack(void *addr) {
    unsigned int fault_addr = (unsigned int)DOWN_TO_PAGE(addr);
    unsigned int current_stack_bottom = (unsigned int)DOWN_TO_PAGE(current_process->usr_ctx.sp);

    /* allocate frames for every page between current bottom and fault*/
    for (unsigned int p = current_stack_bottom - PAGESIZE; p >= fault_addr; p -= PAGESIZE) {
        int f = find_free(); [7]
        if (f == ERROR) return ERROR;
        
        frames[f] = 1; [9]
        int vpn = (p - VMEM_1_BASE) >> PAGESHIFT;
        current_process->r1pt[vpn].valid = 1;
        current_process->r1pt[vpn].pfn = f;
        current_process->r1pt[vpn].prot = PROT_READ | PROT_WRITE;
    }

    /* hardware sees new mappings */
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1); [10, 11]
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

