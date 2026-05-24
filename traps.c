
#include <hardware.h>
#include <traps.h>
#include <load_info.h>
#include <yalnix.h>
#include <ykernel.h>
#include <kern.h>
#include <syscalls.h>

extern PCB_t* current_process;

// Plan is to move all of these to their own functions when actually implementing
void thandler(UserContext *usr_cont) {
	 switch (usr_cont->vector) {
        
        case TRAP_CLOCK:
            TracePrintf(1, "TRAP_CLOCK\n");
		
			memcpy(&current_process->usr_ctx, usr_cont, sizeof(UserContext));
			PCB_t *prev = current_process;
			current_process = current_process->sibling;  // update BEFORE switch

			if (KernelContextSwitch(KCSwitchFunc, prev, current_process) == -1) {
				TracePrintf(0, "trap_clock: KernelContextSwitch failed\n");
				current_process = prev;  // roll back on failure
				break;
			}
			memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
			break;

        case TRAP_KERNEL:
            /*Hex for format. example, 0xabcdef01.*/
            TracePrintf(1, "TRAP_KERNEL: syscall code 0x%x\n", usr_cont->code);
			switch (usr_cont->code) {
				case YALNIX_BRK:
					TracePrintf(1, "Brk Syscall\n");
					//sys_brk(current_process, (void*)usr_cont->regs[0]);
					break;
				case YALNIX_GETPID:
					TracePrintf(1, "GetPid Syscall\n");
					if (current_process == NULL) {
        				TracePrintf(0, "GETPID: current_process is NULL!\n");
        				usr_cont->regs[0] = ERROR;
        				break;
    				}
					usr_cont->regs[0] = sys_getpid(current_process);
					TracePrintf(1, "PID = %d\n", usr_cont->regs[0]);
					break;
				case YALNIX_DELAY:
					//usr_cont->regs[0] = sys_delay(current_process, usr_cont, (int)usr_cont->regs[0]);
					TracePrintf(1, "Delay Syscall\n");
					break;
				default:
					TracePrintf(1, "Unhandled Syscall\n");
					break;
			}
            break;
		
		
        default: /*unexpected trap occurences*/
            TracePrintf(0, "Unhandled trap: %d at PC %p\n", usr_cont->vector, usr_cont->pc);
            /*maybe halt here irl if kernel error*/
			Pause();
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