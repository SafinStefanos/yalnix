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
    KernelContextSwitch(KCSwitchFunc, old, current_process); /*switcing contexts*/
 
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
        TracePrintf(0, "sys_acquire: pid=%d already owns lock %d\n",
                    proc->pid, lock_id);
        return SUCCESS;
    }

    while (l->owner_pid != -1){ /*let's spin even tho it's not great for CPU usage*/
        TracePrintf(0, "sys_acquire: pid=%d blocking on lock %d (owner=%d)\n",
                    proc->pid, lock_id, l->owner_pid);
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
        TracePrintf(0, "sys_release: pid=%d does not own lock %d\n",
                    proc->pid, lock_id);
        return ERROR;
    }
 
   
    PCB_t *next_owner = waiter_dequeue(&l->waiters); /*give to next waiter if any */
    if (next_owner != NULL) {
        l->owner_pid = next_owner->pid;
        TracePrintf(0, "sys_release: lock %d handed to pid=%d\n",
                    lock_id, next_owner->pid);
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
        TracePrintf(0, "sys_cvar_signal: waking pid=%d from cvar %d\n",
                    waiter->pid, cvar_id);
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
        TracePrintf(0, "sys_cvar_broadcast: waking pid=%d from cvar %d\n",
                    waiter->pid, cvar_id);
        enqueue_ready(waiter);
    }
    return SUCCESS;
}
 
