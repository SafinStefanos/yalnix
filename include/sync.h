#ifndef SYNC_H
#define SYNC_H
 
#include <hardware.h>
#include <ykernel.h>
#include "kern.h"
 
#define SYNC_LOCK  1
#define SYNC_CVAR  2
#define SYNC_PIPE  3
 
/*these r the blocked PCBs*/
typedef struct Waiter {
    PCB_t *proc;
    struct Waiter *next;
} Waiter_t;
 

/*  Lock  */
typedef struct Lock {
    int id;
    int owner_pid;   /* -1 means unlocked */
    Waiter_t *waiters;     /* head of waiter list */
    struct Lock *next;     /* global lock table linkage */
} Lock_t;
 
/*  Condition variable*/
typedef struct Cvar {
    int id;
    Waiter_t *waiters;
    struct Cvar *next;
} Cvar_t;

/*this might be somewhere alr tbh*/
#ifndef PIPE_BUFFER_LEN
#define PIPE_BUFFER_LEN 256
#endif
 
typedef struct Pipe {
    int id;
    char buf[PIPE_BUFFER_LEN];
    int len;            /* bytes currently in buffer */
    Waiter_t *readers;    /* processes blocked waiting for data */
    Waiter_t *writers;    /* processes blocked waiting for space */
    struct Pipe *next;
} Pipe_t;
 
/*declaring the globals here might be a little easier */
extern Lock_t *lock_table;
extern Cvar_t *cvar_table;
extern Pipe_t *pipe_table;
 

/* function prototypes */
void sync_init(void);
 
int sys_lock_init(int *lock_idp);
int sys_acquire(PCB_t *proc, UserContext *uc, int lock_id);
int sys_release(PCB_t *proc, int lock_id);
 
int sys_cvar_init(int *cvar_idp);
int sys_cvar_signal(int cvar_id);
int sys_cvar_broadcast(int cvar_id);
int sys_cvar_wait(PCB_t *proc, UserContext *uc, int cvar_id, int lock_id);
 
int sys_pipe_init(int *pipe_idp);
int sys_pipe_read(PCB_t *proc, UserContext *uc, int pipe_id, void *buf, int len);
int sys_pipe_write(PCB_t *proc, UserContext *uc, int pipe_id, void *buf, int len);
 
int sys_reclaim(int id);
 
#endif /* SYNC_H */
