#include <hardware.h>
#include <load_info.h>
#include <process.h>
#include <struct_helpers.h>
#include <unistd.h>
#include <yalnix.h>

/*

TRAP_KERNEL
	Long conditional statement to figure out which syscall happened
	Make sure syscall is valid and possible
	Do intended kernel function
	Put return value in regs in UserContext
TRAP_CLOCK
	Global tick counter ++
  	Check for procs waiting on delay and wake them if enough time has passed
  	Implement round robin scheduling
TRAP_ILLEGAL
	Print illegal instruction error and exit
TRAP_MEMORY
  	Check if faulty addr is in region 1 and sits between current brk and bottom of user stack
  	If valid growth, allocate new frame and update region 1 page table, then return and retry instruction
  	Else abort
TRAP_MATH
	Print illegal math operation and exit
TRAP_TTY_RECEIVE
	Find interrupt terminal ID
	Save curr proc
	Find pending read req
	Read input into request buffer
	Context switch to waiting proc if there is one
	Restore original proc
TRAP_TTY_TRANSMIT
  	Find interrupt terminal ID
  	Find completed write request
  	Wake blocked writer proc

*/
