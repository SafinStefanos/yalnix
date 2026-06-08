#include <hardware.h>
#include <traps.h>
#include <yalnix.h>
#include <ykernel.h>
#include <kern.h>
#include <syscalls.h>
#include "sync.h"

extern PCB_t* current_process;
extern PCB_t* sleep_queue_head;
extern pte_t KernelPT[];
extern KernelContext *KCSwitchFunc();

static void remap_kstack(PCB_t *pcb) {
    int ks_base_pg = KERNEL_STACK_BASE >> PAGESHIFT;
    int ks_npg     = KERNEL_STACK_MAXSIZE >> PAGESHIFT;
    for (int i = 0; i < ks_npg; i++) {
        KernelPT[ks_base_pg + i].pfn   = pcb->kstack_pfn[i];
        KernelPT[ks_base_pg + i].valid = 1;
        KernelPT[ks_base_pg + i].prot  = PROT_READ | PROT_WRITE;
    }
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_KSTACK);
}

/* thandler: handles all hardware traps and syscalls */
void thandler(UserContext *usr_cont) {
    switch (usr_cont->vector) {
        case TRAP_CLOCK:
            memcpy(&current_process->usr_ctx, usr_cont, sizeof(UserContext));

            /* tick down sleeping processes and wake ready ones */
            PCB_t *curr_sleep = sleep_queue_head;
            PCB_t *prev_sleep = NULL;
            while (curr_sleep != NULL) {
                curr_sleep->delay--;
                if (curr_sleep->delay <= 0) {
                    if (prev_sleep == NULL) sleep_queue_head = curr_sleep->next;
                    else prev_sleep->next = curr_sleep->next;
                    PCB_t *tmp = curr_sleep->next;
                    curr_sleep->next = NULL;
                    curr_sleep->state = READY;
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

            if (ready_queue_head != NULL && current_process != idle_pcb) {
                /* preempt current process -- put it at the back */
                PCB_t *tail = ready_queue_head;
                while (tail->next != NULL) tail = tail->next;
                current_process->next = NULL;
                current_process->state = READY;
                tail->next = current_process;

                PCB_t *old = current_process;
                current_process = ready_queue_head;
                ready_queue_head = ready_queue_head->next;
                current_process->next = NULL;

                KernelContextSwitch(KCSwitchFunc, old, current_process);
            } else if (ready_queue_head != NULL && current_process == idle_pcb) {
                /* idle is running but someone is ready -- switch immediately */
                PCB_t *old = current_process;
                current_process = ready_queue_head;
                ready_queue_head = ready_queue_head->next;
                current_process->next = NULL;

                KernelContextSwitch(KCSwitchFunc, old, current_process);
            }
            /* else: nothing ready, keep running current (or idle) */

            /* always remap kstack and region 1 to whoever is current now */
            remap_kstack(current_process);
            WriteRegister(REG_PTBR1, (unsigned int)current_process->r1pt);
            WriteRegister(REG_PTLR1, MAX_PT_LEN);
            WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);
            memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
            break;

        case TRAP_TTY_TRANSMIT: {
            int tty_id = usr_cont->code;
            TtyWriteReq_t *req = tty_write_req[tty_id];
            if (req == NULL) break;

            int just_sent = (req->len - req->sent > TERMINAL_MAX_LINE)
                            ? TERMINAL_MAX_LINE : (req->len - req->sent);
            req->sent += just_sent;

            if (req->sent < req->len) {
                int chunk = (req->len - req->sent > TERMINAL_MAX_LINE)
                            ? TERMINAL_MAX_LINE : (req->len - req->sent);
                TtyTransmit(tty_id, req->buf + req->sent, chunk);
            } else {
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

            int n = TtyReceive(tty_id, tty_read_buf[tty_id], TERMINAL_MAX_LINE);
            tty_read_len[tty_id] = n;

            TtyReadReq_t *req = tty_read_queue[tty_id];
            if (req != NULL) {
                tty_read_queue[tty_id] = req->next;

                int give = (req->len < n) ? req->len : n;
                memcpy(req->buf, tty_read_buf[tty_id], give);
                req->received = give;

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
            TracePrintf(0, "TRAP_KERNEL: code=0x%x\n", usr_cont->code);
            TracePrintf(0, "TRAP_KERNEL: current_process=%p pid=%d sibling=%p\n",
                current_process, current_process->pid, current_process->sibling);
            switch ((unsigned int)usr_cont->code) {
                case YALNIX_GETPID:
                    usr_cont->regs[0] = current_process->pid;
                    break;
                case YALNIX_BRK:
                    usr_cont->regs[0] = sys_brk(current_process, (void *)usr_cont->regs[0]);
                    break;
                case YALNIX_DELAY:
                    usr_cont->regs[0] = sys_delay(current_process, usr_cont, (int)usr_cont->regs[0]);
                    memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                    break;
                case YALNIX_WAIT:
                    usr_cont->regs[0] = sys_wait(current_process, usr_cont, (int *)usr_cont->regs[0]);
                    memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                    break;
                case YALNIX_FORK:
                    usr_cont->regs[0] = sys_fork(current_process, usr_cont);
                    memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                    break;
                case YALNIX_EXEC:
                    usr_cont->regs[0] = sys_exec(current_process, usr_cont,
                        (char *)usr_cont->regs[0], (char **)usr_cont->regs[1]);
                    break;
                case YALNIX_TTY_WRITE:
                    usr_cont->regs[0] = sys_tty_write(current_process, usr_cont,
                        (int)usr_cont->regs[0],
                        (void *)usr_cont->regs[1],
                        (int)usr_cont->regs[2]);
                    memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                    break;
                case YALNIX_TTY_READ:
                    usr_cont->regs[0] = sys_tty_read(current_process, usr_cont,
                        (int)usr_cont->regs[0],
                        (void *)usr_cont->regs[1],
                        (int)usr_cont->regs[2]);
                    memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                    break;
                case YALNIX_EXIT:
                    sys_exit(current_process, (int)usr_cont->regs[0]);
                    memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                    break;
                case YALNIX_LOCK_INIT:
                    usr_cont->regs[0] = sys_lock_init((int *)usr_cont->regs[0]);
                    break;
                case YALNIX_LOCK_ACQUIRE:
                    usr_cont->regs[0] = sys_acquire(current_process, usr_cont, (int)usr_cont->regs[0]);
                    memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                    break;
                case YALNIX_LOCK_RELEASE:
                    usr_cont->regs[0] = sys_release(current_process, (int)usr_cont->regs[0]);
                    break;
                case YALNIX_CVAR_INIT:
                    usr_cont->regs[0] = sys_cvar_init((int *)usr_cont->regs[0]);
                    break;
                case YALNIX_CVAR_SIGNAL:
                    usr_cont->regs[0] = sys_cvar_signal((int)usr_cont->regs[0]);
                    break;
                case YALNIX_CVAR_BROADCAST:
                    usr_cont->regs[0] = sys_cvar_broadcast((int)usr_cont->regs[0]);
                    break;
                case YALNIX_CVAR_WAIT:
                    usr_cont->regs[0] = sys_cvar_wait(current_process, usr_cont,
                        (int)usr_cont->regs[0], (int)usr_cont->regs[1]);
                    memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                    break;
                case YALNIX_PIPE_INIT:
                    TracePrintf(0, "PIPE_INIT: regs[0]=%p\n", (void *)usr_cont->regs[0]);
                    usr_cont->regs[0] = sys_pipe_init((int *)usr_cont->regs[0]);
                    break;
                case YALNIX_PIPE_READ:
                    usr_cont->regs[0] = sys_pipe_read(current_process, usr_cont,
                        (int)usr_cont->regs[0],
                        (void *)usr_cont->regs[1],
                        (int)usr_cont->regs[2]);
                    memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                    break;
                case YALNIX_PIPE_WRITE:
                    usr_cont->regs[0] = sys_pipe_write(current_process, usr_cont,
                        (int)usr_cont->regs[0],
                        (void *)usr_cont->regs[1],
                        (int)usr_cont->regs[2]);
                    memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                    break;
                case YALNIX_RECLAIM:
                    usr_cont->regs[0] = sys_reclaim((int)usr_cont->regs[0]);
                    break;
                default:
                    usr_cont->regs[0] = ERROR;
                    break;
            }
            break;

        case TRAP_ILLEGAL:
            TracePrintf(0, "TRAP_ILLEGAL: pid=%d pc=%p -- killing\n", current_process->pid, usr_cont->pc);
            if (current_process->pid == 1) { TracePrintf(0, "TRAP_ILLEGAL: init -- halting\n"); Halt(); }
            sys_exit(current_process, ERROR);
            memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
            break;

        case TRAP_MATH:
            TracePrintf(0, "TRAP_MATH: pid=%d pc=%p -- killing\n", current_process->pid, usr_cont->pc);
            if (current_process->pid == 1) { TracePrintf(0, "TRAP_MATH: init -- halting\n"); Halt(); }
            sys_exit(current_process, ERROR);
            memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
            break;

        case TRAP_MEMORY: {
            void *fault_addr = usr_cont->addr;
            unsigned int addr = (unsigned int)fault_addr;

            if (addr < VMEM_1_BASE || addr >= VMEM_1_LIMIT) {
                TracePrintf(0, "TRAP_MEMORY: pid=%d addr=%p out of region 1, killing\n",
                            current_process->pid, fault_addr);
                if (current_process->pid == 1) Halt();
                sys_exit(current_process, ERROR);
                memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                break;
            }

            int fault_vpn = (addr - VMEM_1_BASE) >> PAGESHIFT;

            if (current_process->r1pt[fault_vpn].valid) {
                TracePrintf(0, "TRAP_MEMORY: pid=%d protection fault at %p -- killing\n",
                            current_process->pid, fault_addr);
                if (current_process->pid == 1) Halt();
                sys_exit(current_process, ERROR);
                memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                break;
            }

            unsigned int sp = (unsigned int)usr_cont->sp;
            int sp_vpn  = (sp - VMEM_1_BASE) >> PAGESHIFT;
            int brk_vpn = ((unsigned int)current_process->brk - VMEM_1_BASE) >> PAGESHIFT;

            if (fault_vpn < brk_vpn || fault_vpn > sp_vpn) {
                TracePrintf(0, "TRAP_MEMORY: pid=%d addr=%p not in stack growth region "
                            "(brk_vpn=%d sp_vpn=%d fault_vpn=%d) -- killing\n",
                            current_process->pid, fault_addr, brk_vpn, sp_vpn, fault_vpn);
                if (current_process->pid == 1) Halt();
                sys_exit(current_process, ERROR);
                memcpy(usr_cont, &current_process->usr_ctx, sizeof(UserContext));
                break;
            }

            int ok = 1;
            for (int i = fault_vpn; i <= sp_vpn; i++) {
                if (current_process->r1pt[i].valid) continue;
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
            break;
        }

        case TRAP_DISK:
            TracePrintf(0, "TRAP_DISK: unexpected disk interrupt (not implemented)\n");
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
