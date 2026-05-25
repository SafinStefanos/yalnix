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
#include <load.h>


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
int LoadProgram(char *name, char *args[], PCB_t* pcb){
    int fd;
    struct load_info li;
    int i, pg, allocPages;
    char *cp, *cp2, *argbuf;
    char **cpp;
    int argcount, size;
    int text_pg1, data_pg1, data_npg, stack_npg;
    long segment_size;
    pte_t* pt;

    /* open the executable file */
    if((fd = open(name, O_RDONLY)) < 0){
        TracePrintf(0, "LoadProgram: can't open file '%s'\n", name);
        return ERROR;
    }

    /* validate yalnix executable format */
    if (LoadInfo(fd, &li) != LI_NO_ERROR){
        TracePrintf(0, "LoadProgram: '%s' not in Yalnix format\n", name);
        close(fd);
        return ERROR;
    }

    /* check that entry point is in region one */
    if(li.entry < VMEM_1_BASE){
        TracePrintf(0, "LoadProgram: '%s' not linked for Yalnix\n", name);
        close(fd);
        return ERROR;
    }

    /* figure out in what region page the different program sections start and end */
    text_pg1 = (li.t_vaddr - VMEM_1_BASE) >> PAGESHIFT;
    data_pg1 = (li.id_vaddr - VMEM_1_BASE) >> PAGESHIFT;
    data_npg = li.id_npg + li.ud_npg; 

    /* figure out how many bytes are needed to hold the arguments on the new stack that we are building and count the number of arguments to become the argc that the new main gets called with */
    size = 0;
    for(i = 0; args != NULL && args[i] != NULL; i++){
        TracePrintf(3, "counting arg %d = '%s'\n", i, args[i]);
        size += strlen(args[i]) + 1;
    }
    argcount=i;

    /* the arguments will get copied starting at cp and the argv pointers to the arguments and the argc value will get built starting at cpp. the value for cpp is computed by subtracting off space for the number of arguments plus for the argc value a null pointer terminating the argv pointers and a null pointer terminating the envp pointers times the size of each and then rounding the value down to a double-word boundary */
    cp = ((char *)VMEM_1_LIMIT) - size;
    cpp = (char **) (((int)cp - ((argcount + 3 + POST_ARGV_NULL_SPACE) * sizeof(void *))) & ~7); 

    /* compute the new stack pointer leaving initial stack frame size bytes reserved above the stack pointer before the arguments */
    cp2 = (caddr_t)cpp - INITIAL_STACK_FRAME_SIZE;

    /* compute how many pages we need for the stack */
    stack_npg = (VMEM_1_LIMIT - DOWN_TO_PAGE(cp2)) >> PAGESHIFT;

    /* leave at least one page between heap and stack */
    if(stack_npg + data_pg1 + data_npg >= MAX_PT_LEN){
        close(fd);
        return ERROR;
    }

    /* this completes all the checks before we proceed to actually load the new program. from this point on we are committed to either loading succesfully or killing the process */

    /* set the new stack pointer value in the process's exception frame */
    pcb->usr_ctx.sp = cp2;

    /* now save the arguments in a separate buffer in region zero since we are about to blow away all of region one */
    argbuf=(char *)malloc(size);
    if(argbuf == NULL){
        TracePrintf(0, "unable to allocate space for new program for arguments\n");
        close(fd);
        return ERROR;
    }
    cp2 = argbuf;
    for(i = 0; i < argcount; i++){
        strcpy(cp2, args[i]);
        cp2 += strlen(cp2) + 1;
    }

    /* set up the page tables for the process so that we can read the program into memory. get the right number of physical pages allocated and set them all to writable */
    pt = pcb->r1pt;
    free_region1(pt);

    /* first text. allocate physical pages and map them starting at the text page in region address space. these pages should be marked valid with a protection of read and write */
    allocPages = 0;
    for(pg = text_pg1; pg < MAX_PT_LEN && allocPages < li.t_npg; pg++){
        int f = find_free();
        if (f == ERROR) { free(argbuf); return KILL; }
        frames[f] = 1;
        pt[pg].valid = 1;
        pt[pg].pfn = f;
        pt[pg].prot = PROT_READ | PROT_WRITE;
        allocPages++;
    }

    /* then data. allocate physical pages and map them starting at the data page in region address space. these pages should be marked valid with a protection of read and write */
    allocPages = 0;
    for(pg = data_pg1; pg < MAX_PT_LEN && allocPages < data_npg; pg++){
        int f = find_free();
        if (f == ERROR) { free(argbuf); return KILL; }
        frames[f] = 1;
        pt[pg].valid = 1;
        pt[pg].pfn = f;
        pt[pg].prot = PROT_READ | PROT_WRITE;
        allocPages++;
    }
    pcb->heap_base = (void *)UP_TO_PAGE(li.ud_end);
    pcb->brk = pcb->heap_base;

    /* then stack. allocate physical pages and map them to the top of the region virtual address space. these pages should be marked valid with a protection of read and write */
    allocPages = 0;
    for(pg = MAX_PT_LEN - 1; pg >= 0 && allocPages < stack_npg; pg--){
        int f = find_free();
        if (f == ERROR) { free(argbuf); return KILL; }
        frames[f] = 1;
        pt[pg].valid = 1;
        pt[pg].pfn = f;
        pt[pg].prot = PROT_READ | PROT_WRITE;
        allocPages++;
    }

    /* all pages for the new address space are now in the page table. but they are not yet in the tlb remember */
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

    /* read the text from the file into memory */
    lseek(fd, li.t_faddr, SEEK_SET);
    segment_size = li.t_npg << PAGESHIFT;
    if(read(fd, (void *)li.t_vaddr, segment_size) != (int)segment_size){
        close(fd); free(argbuf); return KILL;
    }

    /* read the data from the file into memory */
    lseek(fd, li.id_faddr, SEEK_SET);
    segment_size = li.id_npg << PAGESHIFT;
    if(read(fd, (void *)li.id_vaddr, segment_size) != (int)segment_size){
        close(fd); free(argbuf); return KILL;
    }
    close(fd); 

    /* now set the page table entries for the program text to be readable and executable but not writable */
    for(pg = text_pg1; pg < text_pg1 + (int)li.t_npg; pg++){
        pt[pg].prot = PROT_READ | PROT_EXEC;
    }
    /* if any of these page table entries is also in the tlb, flush the old mapping */
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_1);

    /* zero out the uninitialized data area */
    bzero((void *)li.id_end, li.ud_end - li.id_end);

    /* set the entry point in the process's usercontext */
    pcb->usr_ctx.pc = (void *)li.entry;

    /* now finally build the argument list on the new stack */
    memset(cpp, 0x00, VMEM_1_LIMIT - ((int)cpp));
    char **argv_on_stack = cpp;
    *argv_on_stack++ = (char *)argcount; /* the first value at cpp is argc */
    cp2 = argbuf;
    for (i = 0; i < argcount; i++) {
        /* copy each argument and set argv */
        *argv_on_stack++ = cp;
        strcpy(cp, cp2);
        cp += strlen(cp) + 1;
        cp2 += strlen(cp2) + 1;
    }
    *argv_on_stack++ = NULL; /* the last argv is a null pointer */
    *argv_on_stack++ = NULL; /* a null pointer for an empty envp */

    free(argbuf);
    return SUCCESS;
}
