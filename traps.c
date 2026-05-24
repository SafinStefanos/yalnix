#include <hardware.h>
#include <traps.h>
#include <load_info.h>
#include <yalnix.h>
#include <ykernel.h>
#include <kern.h>
#include <syscalls.h>

extern PCB_t* current_process;

/* thandler: Integrated trap handler for Checkpoint 3 */
void thandler(UserContext *usr_cont) {
    switch (usr_cont->vector) {
        case TRAP_CLOCK:
            TracePrintf(1, "TRAP_CLOCK: PID %d\n", current_process->pid);

            /*current state saved to PCB*/
            memcpy(&current_process->usr_ctx, usr_cont, sizeof(UserContext));

            /*round robin*/
            PCB_t *old_process = current_process;
            current_process = current_process->sibling;

            /*stack swap*/
            if (KernelContextSwitch(KCSwitchFunc, old_process, current_process) == -1) {
                TracePrintf(0, "trap_clock: KernelContextSwitch failed\n");
                Halt();
            }

            /*tell process new r1*/
            WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
            /*TLB flush of region 1*/
            WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

            /*restore state for trap return*/
            memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
            break;

        case TRAP_KERNEL:
            TracePrintf(1, "TRAP_KERNEL: syscall code 0x%x\n", usr_cont->code);
            
            switch (usr_cont->code) {
                case YALNIX_GETPID:
                    /*current pid --> regs*/
                    usr_cont->regs = current_process->pid;
                    break;

                case YALNIX_BRK:
                    /*brk*/
                    usr_cont->regs = sys_brk(current_process, (void *)usr_cont->regs);
                    break;

                case YALNIX_DELAY:
                    /*delay stuff*/
                    usr_cont->regs = sys_delay(current_process, (int)usr_cont->regs);
                    break;

                default:
                    TracePrintf(1, "Unhandled Syscall 0x%x\n", usr_cont->code);
                    usr_cont->regs = ERROR;
                    break;
            }
            break;

        case TRAP_MEMORY:
            /*abort on bad mem access*/
            TracePrintf(0, "TRAP_MEMORY: Fault at addr %p, PID %d aborted\n", 
                        usr_cont->addr, current_process->pid);
            Halt(); /*halt*/
            break;

        default: 
            /* TRAP_ILLEGAL, TRAP_MATH, etc*/
            TracePrintf(0, "Unhandled trap %d at PC %p, PID %d aborted\n", 
                        usr_cont->vector, usr_cont->pc, current_process->pid);
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
