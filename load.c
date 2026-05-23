/*
 * ==>> This is a TEMPLATE for how to write your own LoadProgram function.
 * ==>> Places where you must change this file to work with your kernel are
 * ==>> marked with "==>>".  You must replace these lines with your own code.
 * ==>> You might also want to save the original annotations as comments.
 */

#include <fcntl.h>
#include <unistd.h>
#include <ykernel.h>
#include <load_info.h>
#include <hardware.h>
#include <load.h>
#include <yalnix.h>
#include <kern.h>


extern unsigned char frames[];
extern int find_free();
extern void free_region1();


/*
 *  Load a program into an existing address space.  The program comes from
 *  the Linux file named "name", and its arguments come from the array at
 *  "args", which is in standard argv format.  The argument "proc" points
 *  to the process or PCB structure for the process into which the program
 *  is to be loaded.
 */
int LoadProgram(char *name, char *args[], PCB_t* pcb)
{
    int fd;
    int (*entry)();
    struct load_info li;
    int i;
    char *cp;
    char **cpp;
    char *cp2;
    int argcount;
    int size;
    int text_pg1;
    int data_pg1;
    int data_npg;
    int stack_npg;
    long segment_size;
    char *argbuf;

   /*
   * Open the executable file
   */
    if ((fd = open(name, O_RDONLY)) < 0) {
        TracePrintf(0, "LoadProgram: can't open file '%s'\n", name);
        return ERROR;
    }
    int li_result;
    if ((li_result = LoadInfo(fd, &li)) != LI_NO_ERROR) {
        TracePrintf(0, "LoadProgram: '%s' not in Yalnix format\n", name);
        close(fd);
        return ERROR;
    }
    if (li.entry < VMEM_1_BASE) {
        TracePrintf(0, "LoadProgram: '%s' not linked for Yalnix\n", name);
        close(fd);
        return ERROR;
    }

    /*
    * Figure out in what region 1 page the different program sections
    * start and end
    */
    text_pg1 = (li.t_vaddr - VMEM_1_BASE) >> PAGESHIFT;
    
    data_pg1 = (li.id_vaddr - VMEM_1_BASE) >> PAGESHIFT;
    data_npg = li.id_npg + li.ud_npg;

    /*
    *  Figure out how many bytes are needed to hold the arguments on
    *  the new stack that we are building.  Also count the number of
    *  arguments, to become the argc that the new "main" gets called with.
    */
    size = 0;
    for (i = 0; args != NULL && args[i] != NULL; i++) {
        TracePrintf(3, "counting arg %d = '%s'\n", i, args[i]);
        size += strlen(args[i]) + 1;
    }
    argcount = i;

    TracePrintf(3, "LoadProgram: argsize %d, argcount %d\n", size, argcount);

    /*
    *  The arguments will get copied starting at "cp", and the argv
    *  pointers to the arguments (and the argc value) will get built
    *  starting at "cpp".  The value for "cpp" is computed by subtracting
    *  off space for the number of arguments (plus 3, for the argc value,
    *  a NULL pointer terminating the argv pointers, and a NULL pointer
    *  terminating the envp pointers) times the size of each,
    *  and then rounding the value *down* to a double-word boundary.
    */
    cp = ((char *)VMEM_1_LIMIT) - size;

    cpp = (char **)
        (((int)cp -
        ((argcount + 3 + POST_ARGV_NULL_SPACE) *sizeof (void *)))
        & ~7);

    /*
    * Compute the new stack pointer, leaving INITIAL_STACK_FRAME_SIZE bytes
    * reserved above the stack pointer, before the arguments.
    */
    cp2 = (caddr_t)cpp - INITIAL_STACK_FRAME_SIZE;

    TracePrintf(3, "prog_size %d, text %d data %d bss %d pages\n",
    li.t_npg + data_npg, li.t_npg, li.id_npg, li.ud_npg);

    /*
    * Compute how many pages we need for the stack 
    */
    stack_npg = (VMEM_1_LIMIT - DOWN_TO_PAGE(cp2)) >> PAGESHIFT;

    /* leave at least one page between heap and stack */
    if (stack_npg + data_pg1 + data_npg >= MAX_PT_LEN) {
    close(fd);
    return ERROR;
    }

    /*
    * This completes all the checks before we proceed to actually load
    * the new program.  From this point on, we are committed to either
    * loading succesfully or killing the process.
    */

    /*
    * Set the new stack pointer value in the process's exception frame.
    */
    pcb->usr_ctx.sp = cp2;

    /*
    * Now save the arguments in a separate buffer in region 0, since
    * we are about to blow away all of region 1.
    */
    cp2 = argbuf = (char *)malloc(size);
    if(cp2 != NULL) {
        for (i = 0; args != NULL && args[i] != NULL; i++) {
            TracePrintf(3, "saving arg %d = '%s'\n", i, args[i]);
            strcpy(cp2, args[i]);
            cp2 += strlen(cp2) + 1;
        }
    }
    else {
        TracePrintf(0, "Unable to allocate space for new program for arguments\n");
        return ERROR;
    }

    /*
    * Set up the page tables for the process so that we can read the
    * program into memory.  Get the right number of physical pages
    * allocated, and set them all to writable.
    */
    pte_t* pt = pcb->r1pt;

    free_region1(pt);

    WriteRegister(REG_PTBR1, (unsigned int) pcb->r1pt);
    WriteRegister(REG_PTLR1, MAX_PT_LEN);

    TracePrintf(0, "pcb->r1pt = %p\n", pcb->r1pt);
    TracePrintf(0, "PTBR1 value = 0x%x\n", ReadRegister(REG_PTBR1));
    TracePrintf(0, "pt[text_pg1] after free: valid=%d pfn=%d prot=%d\n",
    pt[text_pg1].valid, pt[text_pg1].pfn, pt[text_pg1].prot);

    int pg;
    int allocPages = 0;
    for(pg = text_pg1; pg < VMEM_1_SIZE / PAGESIZE && allocPages < li.t_npg; pg++) {
        pt[pg].valid = 1;
        pt[pg].prot = PROT_READ | PROT_WRITE;
        int f = find_free();
        if (f == ERROR) return KILL;
        frames[f] = 1;
        pt[pg].pfn = f;
        allocPages++;
    }

    allocPages = 0;
    for(pg = data_pg1; pg < VMEM_1_SIZE / PAGESIZE && allocPages < data_npg; pg++) {
        pt[pg].valid = 1;
        pt[pg].prot = PROT_READ | PROT_WRITE;
        int f = find_free();
        if (f == ERROR) return KILL;
        frames[f] = 1;
        pt[pg].pfn = f;
        allocPages++;
    }


    /*
    * Allocate memory for the user stack too.
    */
    allocPages = 0;
    for(pg = VMEM_1_SIZE / PAGESIZE - 1; pg > 0 && allocPages < stack_npg; pg--) {
        pt[pg].valid = 1;
        pt[pg].prot = PROT_READ | PROT_WRITE;
        int f = find_free();
        if (f == ERROR) return KILL;
        frames[f] = 1;
        pt[pg].pfn = f;
        allocPages++;
    }

    /*
    * All pages for the new address space are now in the page table.
    * But they are not yet in the TLB, remember!
    */
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

    /*
    * Read the text from the file into memory.
    */
    lseek(fd, li.t_faddr, SEEK_SET);
    segment_size = li.t_npg << PAGESHIFT;
    if (read(fd, (void *) li.t_vaddr, segment_size) != segment_size) {
        close(fd);
        return KILL;
    }
    /*
    * Read the data from the file into memory.
    */
    lseek(fd, li.id_faddr, 0);
    segment_size = li.id_npg << PAGESHIFT;

    if (read(fd, (void *) li.id_vaddr, segment_size) != segment_size) {
        close(fd);
        return KILL;
    }

    /*
    * Now set the page table entries for the program text to be readable
    * and executable, but not writable.
    */


    for(pg = text_pg1; pg < text_pg1 + li.t_npg; pg++) {
        pt[pg].prot = PROT_READ | PROT_EXEC;
    }

    // flush region1 TLB
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

    close(fd);			/* we've read it all now */

    /*
    * Zero out the uninitialized data area
    */
    bzero(li.id_end, li.ud_end - li.id_end);

    /*
    * Set the entry point in the exception frame.
    */
    pcb->usr_ctx.pc = (caddr_t) li.entry;

    /*
    * Now, finally, build the argument list on the new stack.
    */
    #ifdef LINUX
    memset(cpp, 0x00, VMEM_1_LIMIT - ((int) cpp));
    #endif

    *cpp++ = (char *)argcount;		/* the first value at cpp is argc */
    cp2 = argbuf;
    for (i = 0; i < argcount; i++) {      /* copy each argument and set argv */
        *cpp++ = cp;
        strcpy(cp, cp2);
        cp += strlen(cp) + 1;
        cp2 += strlen(cp2) + 1;
    }

    free(argbuf);
    *cpp++ = NULL;			/* the last argv is a NULL pointer */
    *cpp++ = NULL;			/* a NULL pointer for an empty envp */
    return SUCCESS;
}