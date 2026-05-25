#include <hardware.h>
#include <traps.h>
#include <yalnix.h>
#include <ykernel.h>
#include <kern.h>
#include <syscalls.h>

extern PCB_t* current_process;
extern PCB_t* sleep_queue_head;

/* thandler: handles all hardware traps and syscalls */
void thandler(usercontext *usr_cont) {
    /* identify trap type */
    switch (usr_cont->vector) {
        case trap_clock:
            /* save current state */
            memcpy(&current_process->usr_ctx, usr_cont, sizeof(usercontext));

            /* update sleep queue */
            pcb_t *curr_sleep = sleep_queue_head;
            pcb_t *prev_sleep = null;
            while (curr_sleep != null) {
                curr_sleep->delay--;
                if (curr_sleep->delay <= 0) {
                    pcb_t *waking = curr_sleep;
                    if (prev_sleep == null) sleep_queue_head = curr_sleep->next;
                    else prev_sleep->next = curr_sleep->next;
                    curr_sleep = curr_sleep->next;
                    /* add waking process back to ready queue */
                    waking->state = state_ready;
                    enqueue(ready_queue, waking, sizeof(pcb_t *));
                } else {
                    prev_sleep = curr_sleep;
                    curr_sleep = curr_sleep->next;
                }
            }

            /* handle scheduling */
            pcb_t *old_process = current_process;
            if (old_process->state == state_ready && old_process != idle_pcb) {
                enqueue(ready_queue, old_process, sizeof(pcb_t *));
            }

            /* select next process */
            if (dequeue(ready_queue, &current_process, sizeof(pcb_t *)) == 1) {
                current_process = idle_pcb;
            }

            /* context switch and hardware update */
            kernelcontextswitch(kcswitchfunc, old_process, current_process);
            writeregister(reg_ptbr1, (unsigned int)current_process->r1pt);
            writeregister(reg_tlb_flush, tlb_flush_1);

            /* restore state for selected process */
            memcpy(usr_cont, &current_process->usr_ctx, sizeof(usercontext));
            break;

        case trap_kernel:
            /* process syscalls by code */
            switch (usr_cont->code) {
                case yalnix_getpid:
                    usr_cont->regs = current_process->pid;
                    break;
                case yalnix_brk:
                    usr_cont->regs = sys_brk(current_process, (void *)usr_cont->regs);
                    break;
                case yalnix_delay:
                    usr_cont->regs = sys_delay(current_process, (int)usr_cont->regs);
                    break;
                case yalnix_fork:
                    usr_cont->regs = sys_fork(current_process);
                    break;
                case yalnix_exec:
                    usr_cont->regs = sys_exec((char *)usr_cont->regs, (char **)usr_cont->regs[2]);
                    break;
                case yalnix_wait:
                    usr_cont->regs = sys_wait((int *)usr_cont->regs);
                    break;
                case yalnix_exit:
                    sys_exit(current_process, (int)usr_cont->regs);
                    break;
                default:
                    usr_cont->regs = error;
                    break;
            }
            break;

        case trap_memory:
            /* dynamic stack growth check */
            if (should_grows(usr_cont->addr)) {
                if (grow_stack(usr_cont->addr) == success) return;
            }
            /* halt if init fails */
            if (current_process->pid == 1) {
                traceprintf(0, "init process memory fault\n");
                halt();
            }
            /* terminate other processes on fault */
            traceprintf(0, "pid %d memory fault at %p\n", current_process->pid, usr_cont->addr);
            sys_exit(current_process, error);
            break;

        case trap_math:
        case trap_illegal:
            /* handle fatal errors */
            if (current_process->pid == 1) halt();
            traceprintf(0, "pid %d aborted trap %d\n", current_process->pid, usr_cont->vector);
            sys_exit(current_process, error);
            break;

        default:
            /* unknown hardware event */
            halt();
            break;
    }
}
/*
	if(usr_cont==TRAP_KERNEL)
		Long conditional statement to figure out which syscall happened
		Make sure syscall is valid and possible
		Do intended kernel function
		Put return value in regs in UserContext
	else if(usr_cont==TRAP_CLOCK)
		Global tick counter ++
		Check for procs waiting on delay and wake them if enough time has passed
		Implement round robin scheduling
		else if(usr_cont==TRAP_ILLEGAL)
		Print illegal instruction error and exit
	else if(usr_cont==TRAP_MEMORY)
		Check if faulty addr is in region 1 and sits between current brk and bottom of user stack
		If valid growth, allocate new frame and update region 1 page table, then return and retry instruction
		else abort
	else if(usr_cont==TRAP_MATH)
		Print illegal math operation and exit
	else if(usr_cont==TRAP_TTY_RECEIVE)
		Find interrupt terminal ID
		Save curr proc
		Find pending read req
		Read input into request buffer
		Context switch to waiting proc if there is one
		Restore original proc
	else if(usr_cont==TRAP_TTY_TRANSMIT)
		Find interrupt terminal ID
		Find completed write request
		Wake blocked writer proc
	
*/
