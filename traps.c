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
            memcpy(&current_process->usr_ctx, usr_cont, sizeof(UserContext));

            // tick down sleeping processes and wake ready ones
            PCB_t *curr_sleep = sleep_queue_head;
            PCB_t *prev_sleep = NULL;
            while (curr_sleep != NULL) {
                curr_sleep->delay--;
                if (curr_sleep->delay <= 0) {
                    if (prev_sleep == NULL) sleep_queue_head = curr_sleep->next;
                    else prev_sleep->next = curr_sleep->next;
                    PCB_t *tmp = curr_sleep->next;
                    curr_sleep->next = NULL;
                    // append woken process to back of ready queue
                    if (ready_queue_head == NULL) {
                        ready_queue_head = curr_sleep;
                    } else {
                        PCB_t *tail = ready_queue_head;
                        while (tail->next != NULL) tail = tail->next;
                        tail->next = curr_sleep;
                    }
                    curr_sleep = tmp;
                } else {
                    prev_sleep = curr_sleep;
                    curr_sleep = curr_sleep->next;
                }
            }

            // only switch if someone else is waiting
            if (ready_queue_head != NULL && current_process != idle_pcb) {
                // re-queue current process at the back
                PCB_t *tail = ready_queue_head;
                while (tail->next != NULL) tail = tail->next;
                current_process->next = NULL;
                tail->next = current_process;

                // dequeue next
                PCB_t *old = current_process;
                current_process = ready_queue_head;
                ready_queue_head = ready_queue_head->next;
                current_process->next = NULL;

                KernelContextSwitch(KCSwitchFunc, old, current_process);
            } else if (ready_queue_head != NULL && current_process == idle_pcb) {
                // idle is running but someone is ready -- switch immediately
                PCB_t *old = current_process;
                current_process = ready_queue_head;
                ready_queue_head = ready_queue_head->next;
                current_process->next = NULL;

                KernelContextSwitch(KCSwitchFunc, old, current_process);
            }
            // else: nothing ready, keep running current process (or idle)

            WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
            WriteRegister(REG_PTLR1, MAX_PT_LEN);
            WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
            memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
            break;

        case TRAP_KERNEL:
            TracePrintf(0, "TRAP_KERNEL: current_process=%p pid=%d sibling=%p\n",
            current_process, current_process->pid, current_process->sibling);
            switch (usr_cont->code) {
                case YALNIX_GETPID:
                    usr_cont->regs[0] = current_process->pid;
                    break;
                case YALNIX_BRK:
                    usr_cont->regs[0] = sys_brk(current_process, (void *)usr_cont->regs[0]);
                    break;
                case YALNIX_DELAY:
                    usr_cont->regs[0] = sys_delay(current_process, usr_cont, (int)usr_cont->regs[0]);
                    break;
                case YALNIX_WAIT:
                    usr_cont->regs[0] = sys_wait(current_process, usr_cont, (int *)usr_cont->regs[0]);
                    //memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                    break;
                case YALNIX_FORK:
                    usr_cont->regs[0] = sys_fork(current_process, usr_cont);
                    break;
                case YALNIX_EXEC:
                    usr_cont->regs[0] = sys_exec(current_process, usr_cont, (char *)usr_cont->regs[0], (char **)usr_cont->regs[1]);
                    break;
                default:
                    usr_cont->regs[0] = ERROR;
                    break;
            }
            break;

        case TRAP_MEMORY:
            /* check for the init death rule*/
            if (current_process->pid == 1) {
                TracePrintf(0, "init process aborted due to memory fault\n");
                Halt();
            }
            /*for now halt if process hits memory fault */
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
