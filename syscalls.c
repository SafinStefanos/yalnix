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
extern KernelContext *KCCopyFunc();

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
            //TracePrintf(0, "ALLOC frame %d at %s:%d\n", f, __FILE__, __LINE__);
            proc->r1pt[i].valid = 1;
            proc->r1pt[i].pfn = f;
            proc->r1pt[i].prot = PROT_READ | PROT_WRITE;
        }
    } else if (new_vpn < cur_vpn) {
        /* shrinking the heap so free existing frames */
        for (int i = new_vpn; i < cur_vpn; i++) {
            if (proc->r1pt[i].valid) {
                frames[proc->r1pt[i].pfn] = 0;
                //TracePrintf(0, "FREE frame %d at %s:%d\n", proc->r1pt[i].pfn, __FILE__, __LINE__);
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
void sys_exit(PCB_t *proc, UserContext *uc, int status) {
    TracePrintf(0, "process %d exiting status=%d\n", proc->pid, status);

    if (proc->pid == 1) {
        TracePrintf(0, "init exited -- halting\n");
        Halt();
    }

    proc->state = ZOMBIE;
    proc->exstat = status;

    if (proc->parent != NULL && proc->parent->state == WAITING) {
        proc->parent->state = READY;
        proc->parent->next = NULL;
        if (ready_queue_head == NULL) {
            ready_queue_head = proc->parent;
        } else {
            PCB_t *tail = ready_queue_head;
            while (tail->next != NULL) tail = tail->next;
            tail->next = proc->parent;
        }
    }

    PCB_t *next = ready_queue_head;
    if (next != NULL) {
        ready_queue_head = next->next;
        next->next = NULL;
    } else {
        next = idle_pcb;
    }

    int ks_base_pg = KERNEL_STACK_BASE >> PAGESHIFT;
    int ks_npg = KERNEL_STACK_MAXSIZE >> PAGESHIFT;
    for (int i = 0; i < ks_npg; i++) {
        KernelPT[ks_base_pg + i].pfn = next->kstack_pfn[i];
        KernelPT[ks_base_pg + i].valid = 1;
        KernelPT[ks_base_pg + i].prot = PROT_READ | PROT_WRITE;
    }
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_KSTACK);

    PCB_t *old = current_process;
    current_process = next;

    TracePrintf(0, "sys_exit: switching from pid=%d to pid=%d\n", old->pid, next->pid);

    if (next == idle_pcb) {
        WriteRegister(REG_PTBR1, (unsigned int)idle_pcb->r1pt);
        WriteRegister(REG_PTLR1, MAX_PT_LEN);
        WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
        memcpy(uc, &idle_pcb->usr_ctx, sizeof(UserContext));
        return;
    }

    KernelContextSwitch(KCSwitchFunc, old, next);

    TracePrintf(0, "ERROR: returned from sys_exit\n");
    Halt();
}

int sys_fork(PCB_t *parent, UserContext *uc) {
    int ks_base_pg = KERNEL_STACK_BASE >> PAGESHIFT;
    int ks_npg     = KERNEL_STACK_MAXSIZE >> PAGESHIFT;
    int temp_vpn   = (KERNEL_STACK_BASE >> PAGESHIFT) - 1;

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

    // copy parent's region 1 pages into child
    for (int i = 0; i < MAX_PT_LEN; i++) {
        if (!parent->r1pt[i].valid) continue;

        int f = find_free();
        if (f == ERROR) {
            for (int j = 0; j < i; j++) {
                if (child->r1pt[j].valid) {
                    frames[child->r1pt[j].pfn] = 0;
                    child->r1pt[j].valid = 0;
                }
            }
            for (int j = 0; j < ks_npg; j++) frames[child->kstack_pfn[j]] = 0;
            free(child->r1pt);
            free(child);
            return ERROR;
        }
        frames[f] = 1;

        // map temp_vpn to child's new frame
        KernelPT[temp_vpn].valid = 1;
        KernelPT[temp_vpn].pfn   = f;
        KernelPT[temp_vpn].prot  = PROT_READ | PROT_WRITE;
        WriteRegister(REG_TLB_FLUSH, (unsigned int)(temp_vpn << PAGESHIFT));

        // copy parent page into child frame
        void *parent_page = (void *)(VMEM_1_BASE + (i << PAGESHIFT));
        void *temp_page   = (void *)(temp_vpn << PAGESHIFT);
        memcpy(temp_page, parent_page, PAGESIZE);

        child->r1pt[i].valid = 1;
        child->r1pt[i].pfn   = f;
        child->r1pt[i].prot  = parent->r1pt[i].prot;
    }

    // unmap temp window
    KernelPT[temp_vpn].valid = 0;
    WriteRegister(REG_TLB_FLUSH, (unsigned int)(temp_vpn << PAGESHIFT));

    // set up child PCB fields
    child->pid       = helper_new_pid(child->r1pt);
    child->ppid      = parent->pid;
    child->parent    = parent;
    child->brk       = parent->brk;
    child->heap_base = parent->heap_base;
    child->state     = READY;
    child->delay     = 0;
    child->init      = 0;  // KCCopyFunc will set this to 1

    // child returns 0 from fork
    child->usr_ctx         = *uc;
    child->usr_ctx.regs[0] = 0;

    // link child into parent's child list
    child->sibling = parent->child;
    parent->child  = child;

    // save parent's user context
    memcpy(&parent->usr_ctx, uc, sizeof(UserContext));

    // add child to ready queue
    child->next        = ready_queue_head;
    ready_queue_head   = child;
    TracePrintf(0, "sys_fork: child pid=%d\n", child->pid);

    // save child pid before switch since current_process changes
    int child_pid = child->pid;

    // copy parent kstack into child's frames and save child's kernel context
    KernelContextSwitch(KCCopyFunc, child, NULL);

    // both parent and child resume here after being scheduled
    // current_process tells us which one we are
    if (current_process->pid == child_pid) {
        return 0;   // we are the child
    }
    return child_pid;   // we are the parent
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
        sys_exit(current_process, uc, ERROR);
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
                    //TracePrintf(0, "FREE frame %d at %s:%d\n", child->kstack_pfn[i], __FILE__, __LINE__);
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
        //current_process = proc;
        WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
        WriteRegister(REG_PTLR1, MAX_PT_LEN);
        WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
        // loop back to check for zombie
    }
}

int sys_tty_write(PCB_t *proc, UserContext *uc, int tty_id, void *buf, int len) {
    if (tty_id < 0 || tty_id >= NUM_TERMINALS) return ERROR;
    if (len <= 0) return 0;
    if (buf == NULL) return ERROR;

    // wait if terminal already busy
    while (tty_busy[tty_id]) {
        memcpy(&proc->usr_ctx, uc, sizeof(UserContext));
        proc->state = BLOCKED;
        PCB_t *next = (ready_queue_head != NULL) ? ready_queue_head : idle_pcb;
        if (ready_queue_head != NULL) {
            ready_queue_head = ready_queue_head->next;
            next->next = NULL;
        }
        PCB_t *old = current_process;
        current_process = next;
        KernelContextSwitch(KCSwitchFunc, old, current_process);
        current_process = proc;
        WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
        WriteRegister(REG_PTLR1, MAX_PT_LEN);
        WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
        memcpy(uc, &current_process->usr_ctx, sizeof(UserContext));
    }

    char *kbuf = (char *)malloc(len);
    if (kbuf == NULL) return ERROR;
    memcpy(kbuf, buf, len);

    TtyWriteReq_t *req = (TtyWriteReq_t *)malloc(sizeof(TtyWriteReq_t));
    if (req == NULL) { free(kbuf); return ERROR; }
    req->proc = proc;
    req->buf  = kbuf;
    req->len  = len;
    req->sent = 0;

    tty_write_req[tty_id] = req;
    tty_busy[tty_id] = 1;

    int chunk = (len > TERMINAL_MAX_LINE) ? TERMINAL_MAX_LINE : len;
    TtyTransmit(tty_id, kbuf, chunk);

    // block until transmit completes
    memcpy(&proc->usr_ctx, uc, sizeof(UserContext));
    proc->state = BLOCKED;

    PCB_t *next = (ready_queue_head != NULL) ? ready_queue_head : idle_pcb;
    if (ready_queue_head != NULL) {
        ready_queue_head = ready_queue_head->next;
        next->next = NULL;
    }

    PCB_t *old = current_process;
    current_process = next;
    KernelContextSwitch(KCSwitchFunc, old, current_process);

    current_process = proc;
    WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
    memcpy(uc, &current_process->usr_ctx, sizeof(UserContext));
    uc->regs[0] = len;
    return len;
}

int sys_tty_read(PCB_t *proc, UserContext *uc, int tty_id, void *buf, int len) {
    if (tty_id < 0 || tty_id >= NUM_TERMINALS) return ERROR;
    if (len <= 0) return 0;
    if (buf == NULL) return ERROR;

    // if data already buffered, serve immediately
    if (tty_read_len[tty_id] > 0) {
        int n = (len < tty_read_len[tty_id]) ? len : tty_read_len[tty_id];
        memcpy(buf, tty_read_buf[tty_id], n);
        // shift remaining data
        int remaining = tty_read_len[tty_id] - n;
        if (remaining > 0)
            memmove(tty_read_buf[tty_id], tty_read_buf[tty_id] + n, remaining);
        tty_read_len[tty_id] = remaining;
        return n;
    }

    // no data yet -- build read request and block
    TtyReadReq_t *req = (TtyReadReq_t *)malloc(sizeof(TtyReadReq_t));
    if (req == NULL) return ERROR;
    char *kbuf = (char *)malloc(len);
    if (kbuf == NULL) { free(req); return ERROR; }

    req->proc     = proc;
    req->buf      = kbuf;
    req->len      = len;
    req->received = 0;
    req->next     = NULL;

    // enqueue at back of read queue for this terminal
    if (tty_read_queue[tty_id] == NULL) {
        tty_read_queue[tty_id] = req;
    } else {
        TtyReadReq_t *tail = tty_read_queue[tty_id];
        while (tail->next != NULL) tail = tail->next;
        tail->next = req;
    }

    // block caller
    memcpy(&proc->usr_ctx, uc, sizeof(UserContext));
    proc->state = BLOCKED;

    PCB_t *next = (ready_queue_head != NULL) ? ready_queue_head : idle_pcb;
    if (ready_queue_head != NULL) {
        ready_queue_head = ready_queue_head->next;
        next->next = NULL;
    }
    PCB_t *old = current_process;
    current_process = next;
    KernelContextSwitch(KCSwitchFunc, old, current_process);
    
    // resumed by TRAP_TTY_RECEIVE handler
    current_process = proc;
    WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

    // copy from kernel buffer to user buffer
    int n = req->received;
    memcpy(buf, req->buf, n);
    free(req->buf);
    free(req);

    memcpy(uc, &current_process->usr_ctx, sizeof(UserContext));
    uc->regs[0] = n;
    return n;
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



