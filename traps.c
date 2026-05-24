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
            /* save the incoming user state into the pcb of the current process */
            memcpy(&current_process->usr_ctx, usr_cont, sizeof(UserContext));

            /* iterate through the sleep queue to wake up ready processes */
            PCB_t *curr_sleep = sleep_queue_head;
            PCB_t *prev_sleep = NULL;
            while (curr_sleep != NULL) {
                curr_sleep->delay--;
                if (curr_sleep->delay <= 0) {
                    /* remove from sleep list and mark as ready */
                    if (prev_sleep == NULL) sleep_queue_head = curr_sleep->next;
                    else prev_sleep->next = curr_sleep->next;
                    curr_sleep = curr_sleep->next;
                } else {
                    prev_sleep = curr_sleep;
                    curr_sleep = curr_sleep->next;
                }
            }

            /* round robin: pick the next sibling that is not sleeping */
            PCB_t *old_process = current_process;
            current_process = current_process->sibling;
            while (current_process->delay > 0) {
                current_process = current_process->sibling;
            }

            /* perform the kernel stack and context switch */
            KernelContextSwitch(KCSwitchFunc, old_process, current_process);

            /* mandatory hardware remap of region one and tlb flush */
            WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
            WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

            /* restore hardware state for return to user mode */
            memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
            break;

        case TRAP_KERNEL:
            /* syscall dispatcher for checkpoint three calls */
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
                default:
                    usr_cont->regs = ERROR;
                    break;
            }
            break;

        case TRAP_MEMORY:
            /* check for the init death rule */
            if (current_process->pid == 1) {
                TracePrintf(0, "init process aborted due to memory fault\n");
                Halt();
            }
            /* for now simply halt if any process hits a memory fault */
            Halt();
            break;

        default: 
            /* halt on math or illegal instruction traps for now */
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
