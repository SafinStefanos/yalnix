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
extern pte_t KernelPT[];
extern KernelContext *KCSwitchFunc();
extern KernelContext *KCCopyFunc();
extern KernelContext *KCSSaveFunc();

/*
 * remap_kstack: point kstack PTEs at pcb's physical frames and flush.
 * Must be called BEFORE KernelContextSwitch(KCSSaveFunc,...) when
 * switching to idle, so the save function sees the correct mapping.
 */
static void remap_kstack(PCB_t *pcb) {
    int ks_base_pg = KERNEL_STACK_BASE >> PAGESHIFT;
    int ks_npg     = KERNEL_STACK_MAXSIZE >> PAGESHIFT;
    for (int i = 0; i < ks_npg; i++) {
        KernelPT[ks_base_pg + i].pfn   = pcb->kstack_pfn[i];
        KernelPT[ks_base_pg + i].valid = 1;
        KernelPT[ks_base_pg + i].prot  = PROT_READ | PROT_WRITE;
    }
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_KSTACK);
}

/*
 * switch_to_idle: save old's context, remap everything to idle, return.
 * After this returns we are still running on old's kernel stack inside
 * the kernel, but the hardware will enter idle's userspace next time
 * it leaves kernel mode via the trap return path -- which is fine because
 * the TRAP handler will copy current_process->usr_ctx into usr_cont.
 */
static void switch_to_idle(PCB_t *old) {
    /* remap kstack to idle's frames BEFORE saving so KCSSaveFunc
     * does not touch the PTE mapping */
    remap_kstack(idle_pcb);
    KernelContextSwitch(KCSSaveFunc, old, NULL);
    /* after KCSSaveFunc returns we are resumed here when old is
     * eventually scheduled again -- at that point current_process
     * has been updated by whoever woke old up, so just return */
    remap_kstack(current_process);
    WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
}

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

    if ((unsigned int)addr < (unsigned int)proc->heap_base) return ERROR;

    unsigned int stack_limit = (unsigned int)DOWN_TO_PAGE(proc->usr_ctx.sp);
    if (new_brk >= stack_limit - PAGESIZE) {
        TracePrintf(0, "sys_brk: collision with stack red zone\n");
        return ERROR;
    }

    int cur_vpn = (cur_brk - VMEM_1_BASE) >> PAGESHIFT;
    int new_vpn = (new_brk - VMEM_1_BASE) >> PAGESHIFT;

    if (new_vpn > cur_vpn) {
        for (int i = cur_vpn; i < new_vpn; i++) {
            int f = find_free();
            if (f == ERROR) return ERROR;
            frames[f] = 1;
            proc->r1pt[i].valid = 1;
            proc->r1pt[i].pfn = f;
            proc->r1pt[i].prot = PROT_READ | PROT_WRITE;
        }
    } else if (new_vpn < cur_vpn) {
        for (int i = new_vpn; i < cur_vpn; i++) {
            if (proc->r1pt[i].valid) {
                frames[proc->r1pt[i].pfn] = 0;
                proc->r1pt[i].valid = 0;
            }
        }
    }

    proc->brk = (void *)new_brk;
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
    return SUCCESS;
}

int sys_delay(PCB_t *proc, UserContext *uc, int ticks) {
    if (ticks == 0) return SUCCESS;
    if (ticks < 0) return ERROR;

    memcpy(&proc->usr_ctx, uc, sizeof(UserContext));
    proc->delay = ticks;
    proc->state = BLOCKED;

    proc->next = sleep_queue_head;
    sleep_queue_head = proc;

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

    if (next == idle_pcb) {
        switch_to_idle(old);
    } else {
        KernelContextSwitch(KCSwitchFunc, old, current_process);
        remap_kstack(current_process);
        WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
        WriteRegister(REG_PTLR1, MAX_PT_LEN);
        WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
    }

    memcpy(uc, &current_process->usr_ctx, sizeof(UserContext));
    uc->regs[0] = SUCCESS;
    return SUCCESS;
}

void sys_exit(PCB_t *proc, int status) {
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

    PCB_t *old = current_process;
    current_process = next;

    TracePrintf(0, "sys_exit: switching from pid=%d to pid=%d\n",
                old->pid, next->pid);

    if (next == idle_pcb) {
        remap_kstack(idle_pcb);
        WriteRegister(REG_PTBR1, (unsigned int)idle_pcb->r1pt);
        WriteRegister(REG_PTLR1, MAX_PT_LEN);
        WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
        return;
    }

    remap_kstack(old);
    TracePrintf(0, "sys_exit: next->krn_ctx.sp=%p next->kstack_pfn[0]=%d [1]=%d\n",
                next->krn_ctx.lc, next->kstack_pfn[0], next->kstack_pfn[1]);
    KernelContextSwitch(KCSwitchFunc, old, next);

    TracePrintf(0, "ERROR: returned from sys_exit\n");
    Halt();
}

int sys_fork(PCB_t *parent, UserContext *uc) {
    int ks_npg   = KERNEL_STACK_MAXSIZE >> PAGESHIFT;
    int temp_vpn = (KERNEL_STACK_BASE >> PAGESHIFT) - 1;

    PCB_t *child = (PCB_t *)malloc(sizeof(PCB_t));
    if (child == NULL) return ERROR;
    memset(child, 0, sizeof(PCB_t));

    child->r1pt = (pte_t *)malloc(sizeof(pte_t) * MAX_PT_LEN);
    if (child->r1pt == NULL) { free(child); return ERROR; }
    memset(child->r1pt, 0, sizeof(pte_t) * MAX_PT_LEN);

    for (int i = 0; i < ks_npg; i++) {
        int f = find_free();
        if (f == ERROR) { free(child->r1pt); free(child); return ERROR; }
        frames[f] = 1;
        child->kstack_pfn[i] = f;
    }

    for (int i = 0; i < MAX_PT_LEN; i++) {
        if (!parent->r1pt[i].valid) continue;
        int f = find_free();
        if (f == ERROR) {
            for (int j = 0; j < i; j++) {
                if (child->r1pt[j].valid) { frames[child->r1pt[j].pfn] = 0; child->r1pt[j].valid = 0; }
            }
            for (int j = 0; j < ks_npg; j++) frames[child->kstack_pfn[j]] = 0;
            free(child->r1pt); free(child);
            return ERROR;
        }
        frames[f] = 1;
        KernelPT[temp_vpn].valid = 1;
        KernelPT[temp_vpn].pfn   = f;
        KernelPT[temp_vpn].prot  = PROT_READ | PROT_WRITE;
        WriteRegister(REG_TLB_FLUSH, (unsigned int)(temp_vpn << PAGESHIFT));
        memcpy((void *)(temp_vpn << PAGESHIFT),
               (void *)(VMEM_1_BASE + (i << PAGESHIFT)), PAGESIZE);
        child->r1pt[i].valid = 1;
        child->r1pt[i].pfn   = f;
        child->r1pt[i].prot  = parent->r1pt[i].prot;
    }

    KernelPT[temp_vpn].valid = 0;
    WriteRegister(REG_TLB_FLUSH, (unsigned int)(temp_vpn << PAGESHIFT));

    child->pid       = helper_new_pid(child->r1pt);
    child->ppid      = parent->pid;
    child->parent    = parent;
    child->brk       = parent->brk;
    child->heap_base = parent->heap_base;
    child->state     = READY;
    child->delay     = 0;
    child->init      = 0;

    child->usr_ctx         = *uc;
    child->usr_ctx.regs[0] = 0;

    child->sibling = parent->child;
    parent->child  = child;

    memcpy(&parent->usr_ctx, uc, sizeof(UserContext));

    child->next      = ready_queue_head;
    ready_queue_head = child;
    TracePrintf(0, "sys_fork: child pid=%d\n", child->pid);

    int child_pid = child->pid;
    KernelContextSwitch(KCCopyFunc, child, NULL);

    if (current_process->pid == child_pid) return 0;
    return child_pid;
}

int sys_exec(PCB_t *proc, UserContext *uc, char *filename, char **args) {
    if (filename == NULL) return ERROR;
    memcpy(&proc->usr_ctx, uc, sizeof(UserContext));
    int rc = LoadProgram(filename, args, proc);
    if (rc != SUCCESS) {
        TracePrintf(0, "sys_exec: LoadProgram failed for %s\n", filename);
        sys_exit(current_process, ERROR);
        return ERROR;
    }
    memcpy(uc, &proc->usr_ctx, sizeof(UserContext));
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
                for (int i = 0; i < KERNEL_STACK_MAXSIZE >> PAGESHIFT; i++)
                    frames[child->kstack_pfn[i]] = 0;
                free(child);
                return cpid;
            }
            prev = child;
            child = child->sibling;
        }

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

        if (next == idle_pcb) {
            switch_to_idle(old);
        } else {
            KernelContextSwitch(KCSwitchFunc, old, current_process);
            remap_kstack(current_process);
            WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
            WriteRegister(REG_PTLR1, MAX_PT_LEN);
            WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
        }
        /* loop back to check for zombie */
    }
}

int sys_tty_write(PCB_t *proc, UserContext *uc, int tty_id, void *buf, int len) {
    if (tty_id < 0 || tty_id >= NUM_TERMINALS) return ERROR;
    if (len <= 0) return 0;
    if (buf == NULL) return ERROR;

    while (tty_busy[tty_id]) {
        memcpy(&proc->usr_ctx, uc, sizeof(UserContext));
        proc->state = BLOCKED;
        PCB_t *next = (ready_queue_head != NULL) ? ready_queue_head : idle_pcb;
        if (ready_queue_head != NULL) { ready_queue_head = ready_queue_head->next; next->next = NULL; }
        PCB_t *old = current_process;
        current_process = next;
        if (next == idle_pcb) {
            switch_to_idle(old);
        } else {
            KernelContextSwitch(KCSwitchFunc, old, current_process);
            remap_kstack(current_process);
            WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
            WriteRegister(REG_PTLR1, MAX_PT_LEN);
            WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
        }
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

    memcpy(&proc->usr_ctx, uc, sizeof(UserContext));
    proc->state = BLOCKED;

    PCB_t *next = (ready_queue_head != NULL) ? ready_queue_head : idle_pcb;
    if (ready_queue_head != NULL) { ready_queue_head = ready_queue_head->next; next->next = NULL; }
    PCB_t *old = current_process;
    current_process = next;

    if (next == idle_pcb) {
        switch_to_idle(old);
    } else {
        KernelContextSwitch(KCSwitchFunc, old, current_process);
        remap_kstack(current_process);
        WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
        WriteRegister(REG_PTLR1, MAX_PT_LEN);
        WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
    }

    memcpy(uc, &current_process->usr_ctx, sizeof(UserContext));
    uc->regs[0] = len;
    return len;
}

int sys_tty_read(PCB_t *proc, UserContext *uc, int tty_id, void *buf, int len) {
    if (tty_id < 0 || tty_id >= NUM_TERMINALS) return ERROR;
    if (len <= 0) return 0;
    if (buf == NULL) return ERROR;

    if (tty_read_len[tty_id] > 0) {
        int n = (len < tty_read_len[tty_id]) ? len : tty_read_len[tty_id];
        memcpy(buf, tty_read_buf[tty_id], n);
        int remaining = tty_read_len[tty_id] - n;
        if (remaining > 0)
            memmove(tty_read_buf[tty_id], tty_read_buf[tty_id] + n, remaining);
        tty_read_len[tty_id] = remaining;
        return n;
    }

    TtyReadReq_t *req = (TtyReadReq_t *)malloc(sizeof(TtyReadReq_t));
    if (req == NULL) return ERROR;
    char *kbuf = (char *)malloc(len);
    if (kbuf == NULL) { free(req); return ERROR; }

    req->proc     = proc;
    req->buf      = kbuf;
    req->len      = len;
    req->received = 0;
    req->next     = NULL;

    if (tty_read_queue[tty_id] == NULL) {
        tty_read_queue[tty_id] = req;
    } else {
        TtyReadReq_t *tail = tty_read_queue[tty_id];
        while (tail->next != NULL) tail = tail->next;
        tail->next = req;
    }

    memcpy(&proc->usr_ctx, uc, sizeof(UserContext));
    proc->state = BLOCKED;

    PCB_t *next = (ready_queue_head != NULL) ? ready_queue_head : idle_pcb;
    if (ready_queue_head != NULL) { ready_queue_head = ready_queue_head->next; next->next = NULL; }
    PCB_t *old = current_process;
    current_process = next;

    if (next == idle_pcb) {
        switch_to_idle(old);
    } else {
        KernelContextSwitch(KCSwitchFunc, old, current_process);
        remap_kstack(current_process);
        WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
        WriteRegister(REG_PTLR1, MAX_PT_LEN);
        WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
    }

    int n = req->received;
    memcpy(buf, req->buf, n);
    free(req->buf);
    free(req);

    memcpy(uc, &current_process->usr_ctx, sizeof(UserContext));
    uc->regs[0] = n;
    return n;
}

int sys_getpid(PCB_t *proc) {
    return proc->pid;
}
