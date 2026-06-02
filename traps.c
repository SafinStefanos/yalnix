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
        case TRAP_TTY_TRANSMIT: {
            int tty_id = usr_cont->code;
            TtyWriteReq_t *req = tty_write_req[tty_id];
            if (req == NULL) break;

            // advance sent by however much we just transmitted
            int just_sent = (req->len - req->sent > TERMINAL_MAX_LINE)
                            ? TERMINAL_MAX_LINE : (req->len - req->sent);
            req->sent += just_sent;

            if (req->sent < req->len) {
                // more chunks remain
                int chunk = (req->len - req->sent > TERMINAL_MAX_LINE)
                            ? TERMINAL_MAX_LINE : (req->len - req->sent);
                TtyTransmit(tty_id, req->buf + req->sent, chunk);
            } else {
                // fully transmitted -- wake the writer
                tty_busy[tty_id] = 0;
                tty_write_req[tty_id] = NULL;
                free(req->buf);

                PCB_t *proc = req->proc;
                free(req);

                proc->state = READY;
                proc->next = ready_queue_head;
                ready_queue_head = proc;
            }
            break;
        }

        case TRAP_TTY_RECEIVE: {
            int tty_id = usr_cont->code;

            // read into terminal's input buffer
            int n = TtyReceive(tty_id,
                            tty_read_buf[tty_id],
                            TERMINAL_MAX_LINE);
            tty_read_len[tty_id] = n;

            // wake first waiting reader if any
            TtyReadReq_t *req = tty_read_queue[tty_id];
            if (req != NULL) {
                tty_read_queue[tty_id] = req->next;

                int give = (req->len < n) ? req->len : n;
                memcpy(req->buf, tty_read_buf[tty_id], give);
                req->received = give;

                // shift buffer
                int remaining = n - give;
                if (remaining > 0)
                    memmove(tty_read_buf[tty_id],
                            tty_read_buf[tty_id] + give, remaining);
                tty_read_len[tty_id] = remaining;

                req->proc->state = READY;
                req->proc->next = ready_queue_head;
                ready_queue_head = req->proc;
            }
            break;
        }
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
                // in TRAP_KERNEL switch, add:
                case YALNIX_TTY_WRITE:
                    usr_cont->regs[0] = sys_tty_write(current_process, usr_cont,
                        (int)usr_cont->regs[0],
                        (void *)usr_cont->regs[1],
                        (int)usr_cont->regs[2]);
                    break;
                case YALNIX_TTY_READ:
                    usr_cont->regs[0] = sys_tty_read(current_process, usr_cont,
                        (int)usr_cont->regs[0],
                        (void *)usr_cont->regs[1],
                        (int)usr_cont->regs[2]);
                    break;
                case YALNIX_EXIT:
                    sys_exit(current_process, (int)usr_cont->regs[0]);
                    break;
                default:
                    usr_cont->regs[0] = ERROR;
                    break;
            }   
            break;

        case TRAP_ILLEGAL:
            TracePrintf(0, "TRAP_ILLEGAL: pid=%d pc=%p -- killing\n", current_process->pid, usr_cont->pc);
            if (current_process->pid == 1) {
                TracePrintf(0, "TRAP_ILLEGAL: init process -- halting\n");
                Halt();
            }
            sys_exit(current_process, ERROR);
            memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
            break;

        case TRAP_MATH:
            TracePrintf(0, "TRAP_MATH: pid=%d pc=%p -- killing\n", current_process->pid, usr_cont->pc);
            if (current_process->pid == 1) {
                TracePrintf(0, "TRAP_MATH: init process -- halting\n");
                Halt();
            }
            sys_exit(current_process, ERROR);
            memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
            break;

        case TRAP_MEMORY: {
            void *fault_addr = usr_cont->addr;
            unsigned int addr = (unsigned int)fault_addr;

            // Must be in region 1
            if (addr < VMEM_1_BASE || addr >= VMEM_1_LIMIT) {
                TracePrintf(0, "TRAP_MEMORY: pid=%d addr=%p out of region 1, killing\n", current_process->pid, fault_addr);
                if (current_process->pid == 1) Halt();
                sys_exit(current_process, ERROR);
                memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                break;
            }

            // Compute which page faulted (region 1 relative)
            int fault_vpn = (addr - VMEM_1_BASE) >> PAGESHIFT;

            // Already mapped? Then it's a protection fault, not growable
            if (current_process->r1pt[fault_vpn].valid) {
                TracePrintf(0, "TRAP_MEMORY: pid=%d protection fault at %p -- killing\n",
                            current_process->pid, fault_addr);
                if (current_process->pid == 1) Halt();
                sys_exit(current_process, ERROR);
                memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                break;
            }

            // The fault must be in the gap between heap and stack (stack growth)
            // Stack pointer page (region 1 relative)
            unsigned int sp = (unsigned int)usr_cont->sp;
            int sp_vpn = (sp - VMEM_1_BASE) >> PAGESHIFT;

            // brk page (region 1 relative) -- heap top
            int brk_vpn = ((unsigned int)current_process->brk - VMEM_1_BASE) >> PAGESHIFT;

            // Fault must be at or above sp and below the current stack bottom,
            // and must not be in the heap
            if (fault_vpn < brk_vpn || fault_vpn > sp_vpn) {
                TracePrintf(0, "TRAP_MEMORY: pid=%d addr=%p not in stack growth region "
                            "(brk_vpn=%d sp_vpn=%d fault_vpn=%d) -- killing\n",
                            current_process->pid, fault_addr, brk_vpn, sp_vpn, fault_vpn);
                if (current_process->pid == 1) Halt();
                sys_exit(current_process, ERROR);
                memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                break;
            }

            // Grow the stack: allocate all pages from fault_vpn up to sp_vpn (exclusive)
            // (there may be a gap of several pages if the stack jumped far)
            int ok = 1;
            for (int i = fault_vpn; i <= sp_vpn; i++) {
                if (current_process->r1pt[i].valid) continue; // already mapped
                int f = find_free();
                if (f == ERROR) {
                    TracePrintf(0, "TRAP_MEMORY: out of frames for stack growth\n");
                    ok = 0;
                    break;
                }
                frames[f] = 1;
                current_process->r1pt[i].valid = 1;
                current_process->r1pt[i].pfn   = f;
                current_process->r1pt[i].prot  = PROT_READ | PROT_WRITE;
            }

            if (!ok) {
                if (current_process->pid == 1) Halt();
                sys_exit(current_process, ERROR);
                memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                break;
            }

            WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
            // hardware retries the faulting instruction automatically
            break;
        }
        
        case TRAP_DISK:
            TracePrintf(0, "TRAP_DISK: unexpected disk interrupt (not implemented)\n");
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
