#include <hardware.h>
#include <ykernel.h>
#include <yalnix.h>
#include "kern.h"
#include "sync.h"
#include "syscalls.h"
 
Lock_t *lock_table = NULL;
Cvar_t *cvar_table = NULL;
Pipe_t *pipe_table = NULL;
 
extern PCB_t *current_process;
extern PCB_t *ready_queue_head;
extern PCB_t *idle_pcb;
extern KernelContext *KCSwitchFunc();
 
 
static int next_sync_id(void) { /*self explanatory*/
    static int counter = 0;
    return ++counter;
}
 
static void waiter_enqueue(Waiter_t **head, PCB_t *proc){ /*adding a waiter from the back*/
    Waiter_t *w = (Waiter_t *)malloc(sizeof(Waiter_t));
    w->proc = proc;
    w->next = NULL;
    if (*head == NULL) {
        *head = w;
        return;
    }
    Waiter_t *tail = *head;
    while (tail->next != NULL) tail = tail->next;
    tail->next = w;
}
 

static PCB_t *waiter_dequeue(Waiter_t **head){ /*remove from the front to make a FIFO*/
    if (*head == NULL) return NULL;
    Waiter_t *w = *head;
    *head = w->next;
    PCB_t *proc = w->proc;
    free(w);
    return proc;
}
 
static void enqueue_ready(PCB_t *proc) { /*make it ready*/
    proc->state = READY;
    proc->next  = NULL;
    if (ready_queue_head == NULL) {
        ready_queue_head = proc;
        return;
    }
    PCB_t *tail = ready_queue_head;
    while (tail->next != NULL) tail = tail->next;
    tail->next = proc;
}
 
static void block_and_switch(PCB_t *proc, UserContext *uc) { /*block the current and switch to the next runnable one*/
    memcpy(&proc->usr_ctx, uc, sizeof(UserContext));
    proc->state = BLOCKED;
    PCB_t *next = (ready_queue_head != NULL) ? ready_queue_head : idle_pcb; /*see if need to wait for more stuff*/
    if (ready_queue_head != NULL) {
        ready_queue_head = ready_queue_head->next;
        next->next = NULL;
    }
 
    PCB_t *old = current_process; /*switching the proc*/
    current_process = next;
    int ks_base_pg = KERNEL_STACK_BASE >> PAGESHIFT;
    KernelContextSwitch(KCSwitchFunc, old, current_process); /*switcing contexts*/
    int ks_npg = KERNEL_STACK_MAXSIZE >> PAGESHIFT;
    for (int i = 0; i < ks_npg; i++) {
        KernelPT[ks_base_pg + i].pfn = next->kstack_pfn[i];
        KernelPT[ks_base_pg + i].valid = 1;
        KernelPT[ks_base_pg + i].prot = PROT_READ | PROT_WRITE;
    }
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_KSTACK);
 
    /*resume after being re-queued*/
    WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt); /*region 1*/
    WriteRegister(REG_PTLR1, MAX_PT_LEN);
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
    memcpy(uc, &current_process->usr_ctx, sizeof(UserContext));
}
 
/*the next 3 all return NULL when not found */
static Lock_t *find_lock(int id) {
    for (Lock_t *l = lock_table; l != NULL; l = l->next)
        if (l->id == id) return l;
    return NULL;
}
static Cvar_t *find_cvar(int id) {
    for (Cvar_t *c = cvar_table; c != NULL; c = c->next)
        if (c->id == id) return c;
    return NULL;
}
static Pipe_t *find_pipe(int id) {
    for (Pipe_t *p = pipe_table; p != NULL; p = p->next)
        if (p->id == id) return p;
    return NULL;
}



void sync_init(void) { /*initialization for kernel start bc im tired of doing this all in kernel.c*/
    lock_table = NULL;
    cvar_table = NULL;
    pipe_table = NULL;
}
 
/*These r all the locks*/
int sys_lock_init(int *lock_idp){
    if (lock_idp == NULL) return ERROR;
 
    Lock_t *l = (Lock_t *)malloc(sizeof(Lock_t));
    if (l == NULL) return ERROR;
 
    l->id = next_sync_id();
    l->owner_pid = -1; /*-1 is unlocked*/
    l->waiters = NULL;
    l->next = lock_table;
    lock_table = l;
 
    *lock_idp = l->id;
    TracePrintf(0, "sys_lock_init: created lock id=%d\n", l->id);
    return SUCCESS;
}
 
int sys_acquire(PCB_t *proc, UserContext *uc, int lock_id){ /*we try to get lock*/
    Lock_t *l = find_lock(lock_id);
    if (l == NULL) {
        TracePrintf(0, "sys_acquire: lock %d not found\n", lock_id);
        return ERROR;
    }
 
    if (l->owner_pid == proc->pid){ /*if we own the lock, we suceed*/
        TracePrintf(0, "sys_acquire: pid=%d already owns lock %d\n", proc->pid, lock_id);
        return SUCCESS;
    }

    while (l->owner_pid != -1 && l->owner_pid != proc->pid){ /*let's spin even tho it's not great for CPU usage*/
        TracePrintf(0, "sys_acquire: pid=%d blocking on lock %d (owner=%d)\n", proc->pid, lock_id, l->owner_pid);
        waiter_enqueue(&l->waiters, proc);
        block_and_switch(proc, uc);
        /* After waking, the lock may still be taken by another waiter
         * that was ahead of us in the queue; loop to re-check. */
    }
 
    l->owner_pid = proc->pid; /*set*/
    TracePrintf(0, "sys_acquire: pid=%d acquired lock %d\n", proc->pid, lock_id);
    return SUCCESS;
}
 
int sys_release(PCB_t *proc, int lock_id) { /*release mechanism*/
    Lock_t *l = find_lock(lock_id);
    if (l == NULL) {
        TracePrintf(0, "sys_release: lock %d not found\n", lock_id);
        return ERROR;
    }
    if (l->owner_pid != proc->pid) {
        TracePrintf(0, "sys_release: pid=%d does not own lock %d\n", proc->pid, lock_id);
        return ERROR;
    }
 
   
    PCB_t *next_owner = waiter_dequeue(&l->waiters); /*give to next waiter if any */
    if (next_owner != NULL) {
        l->owner_pid = next_owner->pid;
        TracePrintf(0, "sys_release: lock %d handed to pid=%d\n", lock_id, next_owner->pid);
        enqueue_ready(next_owner);
    } else {
        l->owner_pid = -1;
        TracePrintf(0, "sys_release: lock %d now free\n", lock_id);
    }
    return SUCCESS;
}
 
/*CV time*/ 
int sys_cvar_init(int *cvar_idp) {
    if (cvar_idp == NULL) return ERROR;
 
    Cvar_t *c = (Cvar_t *)malloc(sizeof(Cvar_t));
    if (c == NULL) return ERROR;
 
    c->id      = next_sync_id();
    c->waiters = NULL;
    c->next    = cvar_table;
    cvar_table = c;
 
    *cvar_idp = c->id;
    TracePrintf(0, "sys_cvar_init: created cvar id=%d\n", c->id);
    return SUCCESS;
}
 
int sys_cvar_signal(int cvar_id){ /*signaling for cv*/
    Cvar_t *c = find_cvar(cvar_id);
    if (c == NULL) {
        TracePrintf(0, "sys_cvar_signal: cvar %d not found\n", cvar_id);
        return ERROR;
    }
 
    PCB_t *waiter = waiter_dequeue(&c->waiters);
    if (waiter != NULL) {
        TracePrintf(0, "sys_cvar_signal: waking pid=%d from cvar %d\n", waiter->pid, cvar_id);
        enqueue_ready(waiter);
    }
    return SUCCESS;
}
 
int sys_cvar_broadcast(int cvar_id){
    Cvar_t *c = find_cvar(cvar_id);
    if (c == NULL) {
        TracePrintf(0, "sys_cvar_broadcast: cvar %d not found\n", cvar_id);
        return ERROR;
    }
 
    PCB_t *waiter;
    while ((waiter = waiter_dequeue(&c->waiters)) != NULL) {
        TracePrintf(0, "sys_cvar_broadcast: waking pid=%d from cvar %d\n", waiter->pid, cvar_id);
        enqueue_ready(waiter);
    }
    return SUCCESS;
}
 
int sys_cvar_wait(PCB_t *proc, UserContext *uc, int cvar_id, int lock_id) { /*atomic release with sleep*/
    Cvar_t *c = find_cvar(cvar_id);
    if (c == NULL) {
        TracePrintf(0, "sys_cvar_wait: cvar %d not found\n", cvar_id);
        return ERROR;
    }
    Lock_t *l = find_lock(lock_id);
    if (l == NULL) {
        TracePrintf(0, "sys_cvar_wait: lock %d not found\n", lock_id);
        return ERROR;
    }
    if (l->owner_pid != proc->pid) {
        TracePrintf(0, "sys_cvar_wait: pid=%d does not own lock %d\n", proc->pid, lock_id);
        return ERROR;
    }
 
    /*release lock*/
    PCB_t *next_owner = waiter_dequeue(&l->waiters);
    if (next_owner != NULL) {
        l->owner_pid = next_owner->pid;
        enqueue_ready(next_owner);
    } else {
        l->owner_pid = -1;
    }
 
    /*hop in the wait queue*/
    waiter_enqueue(&c->waiters, proc);
    block_and_switch(proc, uc);
 
    /*get the lock back and go back*/
    return sys_acquire(proc, uc, lock_id);
}
 
/*Here, we lay the pipes*/


int sys_pipe_init(int *pipe_idp){ /*initialize the pipes*/
    if(pipe_idp == NULL)return ERROR;
 
    Pipe_t *p = (Pipe_t *)malloc(sizeof(Pipe_t));
    if (p == NULL) return ERROR;
 
    p->id = next_sync_id();
    p->len = 0;
    p->readers = NULL;
    p->writers = NULL;
    p->next = pipe_table;
    pipe_table = p;
 
    *pipe_idp = p->id;
    TracePrintf(0, "sys_pipe_init: created pipe id=%d\n", p->id);
    return SUCCESS;
}
 
int sys_pipe_read(PCB_t *proc, UserContext *uc, int pipe_id, void *buf, int len){/*read from the pipe*/
    if (buf == NULL || len <= 0) return ERROR;
 
    Pipe_t *p = find_pipe(pipe_id);
    if (p == NULL) {
        TracePrintf(0, "sys_pipe_read: pipe %d not found\n", pipe_id);
        return ERROR;
    }
 
    while (p->len == 0) {/* Block while pipe is empty */
        TracePrintf(0, "sys_pipe_read: pid=%d blocking on pipe %d (empty)\n", proc->pid, pipe_id);
        waiter_enqueue(&p->readers, proc);
        block_and_switch(proc, uc);
        p = find_pipe(pipe_id);
        if (p == NULL) return ERROR;
    }
 
    int n = (len < p->len) ? len : p->len;/*copy out up to all bytes*/
    memcpy(buf, p->buf, n);
 
    /*shift data up*/
    int remaining = p->len - n;
    if (remaining > 0)
        memmove(p->buf, p->buf + n, remaining);
    p->len = remaining;
 
    TracePrintf(0, "sys_pipe_read: pid=%d read %d bytes from pipe %d\n", proc->pid, n, pipe_id);
 
    /* Wake one blocked writer*/
    PCB_t *writer = waiter_dequeue(&p->writers);
    if (writer != NULL) {
        TracePrintf(0, "sys_pipe_read: waking writer pid=%d\n", writer->pid);
        enqueue_ready(writer);
    }
 
    return n;
}
 
int sys_pipe_write(PCB_t *proc, UserContext *uc, int pipe_id, void *buf, int len){
    if (buf == NULL || len <= 0) return ERROR;
    Pipe_t *p = find_pipe(pipe_id);
    if (p == NULL) {
        TracePrintf(0, "sys_pipe_write: pipe %d not found\n", pipe_id);
        return ERROR;
    }
 
    int written = 0;
    char *src = (char *)buf;
    while (written < len) {
        while (p->len >= PIPE_BUFFER_LEN) { /*block full buff*/
            TracePrintf(0, "sys_pipe_write: pid=%d blocking on pipe %d (full)\n", proc->pid, pipe_id);
            waiter_enqueue(&p->writers, proc);
            block_and_switch(proc, uc);
            p = find_pipe(pipe_id);
            if (p == NULL) return ERROR;
        }
 
        int space = PIPE_BUFFER_LEN - p->len; /* Write as much as possible */
        int chunk = (len - written < space) ? (len - written) : space;
        memcpy(p->buf + p->len, src + written, chunk);
        p->len  += chunk;
        written += chunk;
        TracePrintf(0, "sys_pipe_write: pid=%d wrote %d bytes to pipe %d\n", proc->pid, chunk, pipe_id);
 
        /* Wake one blocked reader */
        PCB_t *reader = waiter_dequeue(&p->readers);
        if (reader != NULL) {
            TracePrintf(0, "sys_pipe_write: waking reader pid=%d\n", reader->pid);
            enqueue_ready(reader);
        }
    }
 
    return written;
}
 
int sys_reclaim(int id){
    /*lock table */
    Lock_t *lp = NULL, *lprev = NULL;
    for (Lock_t *l = lock_table; l != NULL; lprev = l, l = l->next) {
        if (l->id == id) { lp = l; break; }
    }
    if (lp != NULL) {/* broadcast*/
        PCB_t *w;
        while ((w = waiter_dequeue(&lp->waiters)) != NULL)enqueue_ready(w);
        if (lprev == NULL) lock_table = lp->next;
        else lprev->next = lp->next;
        free(lp);
        TracePrintf(0, "sys_reclaim: freed lock %d\n", id);
        return SUCCESS;
    }
  
    Cvar_t *cp = NULL, *cprev = NULL;   /*cvar table */
    for (Cvar_t *c = cvar_table; c != NULL; cprev = c, c = c->next) {
        if (c->id == id) { cp = c; break; }
    }
    if (cp != NULL) {
        PCB_t *w;
        while ((w = waiter_dequeue(&cp->waiters)) != NULL)
            enqueue_ready(w);
        if (cprev == NULL) cvar_table = cp->next;
        else               cprev->next = cp->next;
        free(cp);
        TracePrintf(0, "sys_reclaim: freed cvar %d\n", id);
        return SUCCESS;
    }
 
    Pipe_t *pp = NULL, *pprev = NULL;/*pipe table */
    for (Pipe_t *p = pipe_table; p != NULL; pprev = p, p = p->next) {
        if (p->id == id) { pp = p; break; }
    }
    if (pp != NULL) {
        PCB_t *w;
        while ((w = waiter_dequeue(&pp->readers)) != NULL)
            enqueue_ready(w);
        while ((w = waiter_dequeue(&pp->writers)) != NULL)
            enqueue_ready(w);
        if (pprev == NULL) pipe_table = pp->next;
        else               pprev->next = pp->next;
        free(pp);
        TracePrintf(0, "sys_reclaim: freed pipe %d\n", id);
        return SUCCESS;
    }
 
    TracePrintf(0, "sys_reclaim: id %d not found\n", id);
    return ERROR;
}



