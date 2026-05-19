#include <hardware.h>
#include <ykernel.h>
#include <yalnix.h>

unsigned char frames[MAX_PMEM_SIZE/PAGESIZE]; 
pte_t KernelPT[MAX_PT_LEN]; 
void *curr_kbrk;

int find_free() {
    for (int j = 0; j < MAX_PMEM_SIZE/PAGESIZE; j++) {
        if (frames[j] == 0) return j;
    }
    return ERROR;
}

int SetKernelBrk(void *addr) {
    unsigned int new_brk = (unsigned int)UP_TO_PAGE(addr);
    unsigned int curr_brk = (unsigned int)UP_TO_PAGE(curr_kbrk);

    if (new_brk >= KERNEL_STACK_BASE) { /*looking for collision between new break and kernel stack base. The heap cannot start behind the stack*/
        return ERROR;
    }

    if (ReadRegister(REG_VM_ENABLE) == 0) { /*check for enabled VM*/
        curr_kbrk = (void *)new_brk;
        return SUCCESS;
    }

    int curr_vpn = curr_brk >> PAGESHIFT; /*getting VPNs*/
    int new_vpn = new_brk >> PAGESHIFT;

    if(new_vpn > curr_vpn) {
        for(int i = curr_vpn; i < new_vpn; i++) {
            int f = find_free();
            if(f == ERROR) {
                for(int j = curr_vpn; j < i; j++) {
                    helper_force_free(KernelPT[j].pfn);
                    KernelPT[j].valid = 0;
                }
                WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_0);
                return ERROR;
            }
            KernelPT[i].valid = 1; /*mapping new page*/
            KernelPT[i].pfn = f;
            KernelPT[i].prot = PROT_READ | PROT_WRITE;
        }
    } else if(new_vpn < curr_vpn) { /*shrinking heap*/
        for(int i = new_vpn; i < curr_vpn; i++) {
            if(KernelPT[i].valid) {
                helper_force_free(KernelPT[i].pfn); /*free frame*/
                KernelPT[i].valid = 0;
            }
        }
    }

    curr_kbrk = (void *)new_brk;
    WriteRegister(REG_TLB_FLUSH, TLB_FLUSH_0);
    return SUCCESS;
}

void InitKernelMemoryMap(unsigned int pmem_size) {
    int num_frames = pmem_size/PAGESIZE; /*how many r actually possible?*/
    int i;
    for(i = 0; i < num_frames; i++) { /*make free*/
        frames[i] = 0;
    }
    for(i = 0; i < _orig_kernel_brk_page; i++) { /*kernel space*/
        frames[i] = 1;
    }
    for (i = 0; i < MAX_PT_LEN; i++) {
        KernelPT[i].valid = 0; /*Not used yet*/
    }
    for(i = _first_kernel_text_page; i < _first_kernel_data_page; i++) { /*text*/
        KernelPT[i].valid = 1;
        KernelPT[i].pfn = i; /*VPN i -> PFN i*/
        KernelPT[i].prot = PROT_READ | PROT_EXEC;
    }
    for(i = _first_kernel_data_page; i < _orig_kernel_brk_page; i++) { /*heap*/
        KernelPT[i].valid = 1;
        KernelPT[i].pfn = i; /*map*/
        KernelPT[i].prot = PROT_READ | PROT_WRITE;
        frames[i] = 1;
    }
}
