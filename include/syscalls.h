#include <hardware.h>
#include <load_info.h>
#include <process.h>
#include <struct_helpers.h>
#include <unistd.h>
#include <yalnix.h>


int kernel_fork();
int kernel_exec(char* name, char** args);
int kernel_exit(usr_cntx);
int kernel_wait(usr_cntx);
int get_pid();
int kernel_break(void* addr);
int kernel_delay(int ticks, usr_cntx);
int tty_write(int termID, void* buff, int size, usr_cntx);
int tty_read(int termID, void* buff, int size, usr_cntx);
int pipe_init();
int pipe_read(int ID, void* buff, int size, usr_cntx);
int pipe_write(int ID, void* buff, int size, usr_cntx);
int lock_init();
int lock_acquire(int ID, usr_cntx);
int lock_release(int ID);
int cvar_init();
int cvar_signal(int ID);
int cvar_broadcast(int ID);
int cvar_wait(int cvar_id, int lock_id, usr_cntx);
int kernel_reclaim(int ID);
