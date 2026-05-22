#ifndef _LOAD_H
#define _LOAD_H

#include "hardware.h"   // for PCB_t, VMEM_1_BASE, etc.
#include "load_info.h"  // for struct load_info

/*
 * LoadProgram
 *
 * Load a Yalnix executable into the address space of the given process.
 *
 * Arguments:
 *   name  - the filename of the executable
 *   args  - argument array (argv format, NULL terminated)
 *   proc  - pointer to the PCB of the process into which to load
 *
 * Returns:
 *   SUCCESS on success
 *   ERROR   on file/format issues
 *   KILL    if the process should be terminated (e.g., bad read)
 *
 * Notes:
 *   - This function assumes the executable is in Yalnix format.
 *   - Allocates region 1 pages for text, data, and stack.
 *   - Builds the user stack with argc/argv.
 */
int LoadProgram(char *name, char *args[], PCB_t *proc);

#endif /* _LOAD_H */