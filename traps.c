#include <hardware.h>
#include <traps.h>
#include <yalnix.h>
#include <ykernel.h>
#include <kern.h>
#include <syscalls.h>

extern PCB_t* current_process;
extern PCB_t* sleep_queue_head;

/* thandler: handles all hardware traps and syscalls */
void thandler(UserContext *usr_cont) {
    switch (usr_cont->vector) {
        case TRAP_CLOCK:
            /*save the incoming user state into the pcb of the current process */
            memcpy(&current_process->usr_ctx, usr_cont, sizeof(UserContext));

            /* iterate through sleep queue to wake up ready processes */
            PCB_t *curr_sleep = sleep_queue_head;
            PCB_t *prev_sleep = NULL;
            while (curr_sleep != NULL){
                curr_sleep->delay--;
                if (curr_sleep->delay <= 0) {
                    /*remove from sleep list and mark as ready */
                    if(prev_sleep == NULL)sleep_queue_head = curr_sleep->next;
                    else prev_sleep->next = curr_sleep->next;
                    curr_sleep = curr_sleep->next;
                } else {
                    prev_sleep = curr_sleep;
                    curr_sleep = curr_sleep->next;
                }
            }

            /* round robin picking next sibling that is runnable*/
            PCB_t *old_process = current_process;
            current_process = current_process->sibling;
            /*skip processes that are sleeping or blocked in Wait() */
            while (current_process->delay > 0 || current_process->state == STATE_WAITING) {
                current_process = current_process->sibling;
            }

            /* perform the kernel stack and context switch */
            KernelContextSwitch(KCSwitchFunc, old_process, current_process);

            /* hardware remap of region one and tlb flush */
            WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
            WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

            /* restore hardware state for return to user mode */
            memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
            break;

        case TRAP_KERNEL:
            /*syscall return values must be placed in regs*/
            switch (usr_cont->code) {
                case YALNIX_GETPID:
                    usr_cont->regs = current_process->pid;
                    break;
                case YALNIX_BRK:
                    usr_cont->regs = sys_brk(current_process, (void *)usr_cont->regs);
                    break;
                case YALNIX_DELAY:
                    usr_cont->regs = sys_delay(current_process, (int)usr_cont->regs);
                    break;
                /* for the fork, exec, etc. syscalls */
                case YALNIX_FORK:
                    usr_cont->regs = sys_fork(current_process);
                    break;
                case YALNIX_EXEC:
                    usr_cont->regs = sys_exec((char *)usr_cont->regs, (char **)usr_cont->regs[7]);
                    break;
                case YALNIX_WAIT:
                    usr_cont->regs = sys_wait((int *)usr_cont->regs);
                    break;
                case YALNIX_EXIT:
                    sys_exit(current_process, (int)usr_cont->regs);
                    break;
                default:
                    usr_cont->regs = ERROR;
                    break;
            }
            break;

        case TRAP_MEMORY:
            /*check for stack growth */
            if (should_grow_stack(usr_cont->addr)) {
                if (grow_stack(usr_cont->addr) == SUCCESS) return;
            }

            /* if PID 1 fails, halt the whole system*/
            if (current_process->pid == 1) {
                TracePrintf(0, "init process aborted due to memory fault\n");
                Halt();
            }
            /*abort failing process but continue others */
            TracePrintf(0, "Process %d aborted: memory fault at %p\n", current_process->pid, usr_cont->addr);
            sys_exit(current_process, ERROR); 
            break;

        case TRAP_MATH:
        case TRAP_ILLEGAL:
            /*abort process for math or illegal instructions */
            if (current_process->pid == 1) Halt();
            TracePrintf(0, "Process %d aborted due to trap %d\n", current_process->pid, usr_cont->vector);
            sys_exit(current_process, ERROR);
            break;

        default: 
            Halt();
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
