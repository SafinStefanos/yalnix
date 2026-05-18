#define TRAP_KERNEL		0
#define	TRAP_CLOCK		1
#define	TRAP_ILLEGAL		2
#define	TRAP_MEMORY		3
#define	TRAP_MATH		4
#define	TRAP_TTY_RECEIVE	5
#define	TRAP_TTY_TRANSMIT	6
#define	TRAP_DISK		7

#include <hardware.h>
#include <traps.h>
#include <load_info.h>
#include <yalnix.h>
#include <ykernel.h>
#include <struct_helpers.h>


// Plan is to move all of these to their own functions when actually implementing
void thandler(UserContext usr_cont) {
	 switch (ctx->vector) {
        
        case TRAP_CLOCK:
            TracePrintf(1, "TRAP_CLOCK\n");
            break;
        case TRAP_KERNEL:
            /*Hex for format. example, 0xabcdef01.*/
            TracePrintf(1, "TRAP_KERNEL: syscall code 0x%x\n", ctx->code);
            break;
        default: /*unexpected trap occurences*/
            TracePrintf(0, "Unhandled trap: %d at PC %p\n", ctx->vector, ctx->pc);
            /*maybe halt here irl if kernel error*/
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
	


