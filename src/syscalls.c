#include <hardware.h>
#include <load_info.h>
#include <process.h>
#include <syscalls.h>
#include <struct_helpers.h>
#include <unistd.h>
#include <yalnix.h>

/*

Fork
	Create child pcb and page table
	Copy parent pcb state into child
	Alloc temp kernel page
	For each valid parent region 1 page
		Alloc child frame
		Copy parent page to temp page
		Switch to child page table
		Copy temp page to child page
		Restore parent table
	Free temp page
	Alloc child kernel stack pages
	Create child kernel context
	If running as child return 0
	Else put child in ready queue and return PID to parent

Exec
	Get curr PCB and metadata
	Open and validate exec file
	LoadProgram(file, args, currPCB)

Exit
	Get curr proc
	If proc = init halt
	Find parent proc
	If parent exists
	store child exit status in parent queue
		If parent waiting wake
	Schedule another proc
	
Wait
	Check if proc has children
	If no child return error
	While no exited children block curr proc and schedule others
	Retrieve exit status
	Return child PID

GetPid
	Return current PID

Brk
	Validate addr is in region 1
	Get current proc and current brk
	If addr > current brk and enough mem is available
		Alloc additional heap pages
	Else free heap pages
	Update proc brk
	
Delay
	If ticks == 0 return 0
	Set proc sleep timer
	Move proc to sleep queue
	Schedule another proc

TtyWrite
	Wait until terminal free
	Create terminal write request
	Copy user buffer into kernel buffer
	Add request to terminal queue
	Split output into terminal-sized chunks
	For all chunks
		Transmit chunk
		Block until transmit interrupt
		Wait if another proc writing
	Update serviced byte count
	Remove req

TtyRead
	Create terminal read req
	Allocate kernel buffer
	Append req to terminal read queue
	Move proc to read-blocked queue
	Move to other proc
	When awakened restore proc state
	Copy received data from kernel buffer to user buffer
	Remove terminal req
	
PipeInit
	Generate unique pipe id
	Create pipe struct
	Store pipe id in struct

PipeRead
	Find pipe
	Check validity of pipe
	Wait for enough data to be sent through
	Copy pipe data into user buffer
	Shift remaining pipe content
	Update pipe length

PipeWrite
	Find pipe
	Check validity and if full
	Copy data into pipe buffer
	Update pipe length
	Wake waiting readers

LockInit
	Create new lock
	Assign ownership to current proc
	Store lock id
	
Acquire
	Find lock
	Make sure lock is valid and not already owned by proc calling func
	If unlocked give lock, otherwise put on wait queue

Release
	Find lock
	Make sure lock is valid and owned by current proc
	Give up lock
	Transfer ownership to waiting proc
	Wake waiting proc

CvarInit
	Create cond var
	Store cvar ID
	
CvarSignal
	Find cvar
	Wake one waiting proc
	Move proc to ready queue
	
CvarBroadcast
	Find cvar
	Wake all waiting procs
	Move all to ready queue

CvarWait
	Validate cvar and lock
	Ensure caller owns lock
	Associate cvar w lock
	Release lock
	Block proc on cvar queue
	When awakened, reacquire lock

Reclaim
	Determine sync object type
	Free resources


*/
