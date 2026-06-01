#include <yalnix.h>
#include <hardware.h>
#include <ykernel.h>

int main() {
    TracePrintf(0, "=== Trap/Exception Test Program (no fork) ===\n");

    //  TEST 1: stack growth, single page 
    TracePrintf(0, "[TEST 1] single page stack growth\n");
    volatile char page1[PAGESIZE];
    page1[0] = 'A';
    page1[PAGESIZE - 1] = 'B';
    if (page1[0] == 'A' && page1[PAGESIZE - 1] == 'B') {
        TracePrintf(0, "[TEST 1] PASS: single page stack growth worked\n");
    } else {
        TracePrintf(0, "[TEST 1] FAIL\n");
    }

    //  TEST 2: stack growth, multiple pages at once 
    TracePrintf(0, "[TEST 2] multi-page stack growth\n");
    volatile char pages[PAGESIZE * 4];
    pages[0]              = '1';
    pages[PAGESIZE]       = '2';
    pages[PAGESIZE * 2]   = '3';
    pages[PAGESIZE * 3]   = '4';
    pages[PAGESIZE * 4 - 1] = '5';
    if (pages[0] == '1' && pages[PAGESIZE * 2] == '3' && pages[PAGESIZE * 4 - 1] == '5') {
        TracePrintf(0, "[TEST 2] PASS: multi-page stack growth worked\n");
    } else {
        TracePrintf(0, "[TEST 2] FAIL\n");
    }

    //  TEST 3: heap growth via malloc -
    TracePrintf(0, "[TEST 3] heap growth via malloc\n");
    char *p = (char *)malloc(PAGESIZE * 3);
    if (p == NULL) {
        TracePrintf(0, "[TEST 3] FAIL: malloc returned NULL\n");
    } else {
        p[0]            = 'X';
        p[PAGESIZE]     = 'Y';
        p[PAGESIZE * 2] = 'Z';
        if (p[0] == 'X' && p[PAGESIZE] == 'Y' && p[PAGESIZE * 2] == 'Z') {
            TracePrintf(0, "[TEST 3] PASS: heap alloc and access worked\n");
        } else {
            TracePrintf(0, "[TEST 3] FAIL: heap data wrong\n");
        }
        free(p);
    }

    //  TEST 4: heap grow then shrink via Brk -
    TracePrintf(0, "[TEST 4] Brk grow and shrink\n");
    // get a pointer just past current brk by malloc'ing and peeking
    char *m = (char *)malloc(1);
    if (m == NULL) {
        TracePrintf(0, "[TEST 4] FAIL: malloc returned NULL\n");
    } else {
        // touch the page
        m[0] = 42;
        if (m[0] == 42) {
            TracePrintf(0, "[TEST 4] PASS: Brk-based allocation touched successfully\n");
        } else {
            TracePrintf(0, "[TEST 4] FAIL\n");
        }
        free(m);
    }

    //  TEST 5: Brk collision guard 
    // sys_brk should reject an address too close to the stack
    TracePrintf(0, "[TEST 5] Brk collision guard\n");
    // sp is somewhere near VMEM_1_LIMIT, push brk way up toward it
    void *high = (void *)(VMEM_1_LIMIT - PAGESIZE * 2);
    int rc = Brk(high);
    if (rc == ERROR) {
        TracePrintf(0, "[TEST 5] PASS: Brk correctly rejected near-stack address\n");
    } else {
        TracePrintf(0, "[TEST 5] FAIL: Brk should have rejected %p\n", high);
    }

    // TEST 6: large stack then large heap back-to-back
    TracePrintf(0, "[TEST 6] interleaved stack and heap growth\n");
    {
        volatile char stackbuf[PAGESIZE * 3];
        stackbuf[0] = 's';
        stackbuf[PAGESIZE * 2] = 't';
        char *hbuf = (char *)malloc(PAGESIZE * 3);
        if (hbuf != NULL) {
            hbuf[0] = 'h';
            hbuf[PAGESIZE * 2] = 'i';
            if (stackbuf[0] == 's' && hbuf[PAGESIZE * 2] == 'i') {
                TracePrintf(0, "[TEST 6] PASS: interleaved stack+heap growth worked\n");
            } else {
                TracePrintf(0, "[TEST 6] FAIL: data mismatch\n");
            }
            free(hbuf);
        } else {
            TracePrintf(0, "[TEST 6] FAIL: malloc returned NULL\n");
        }
    }

    // TEST 7: TRAP_MATH  (process will die after this) 
    // Tests 7 and 8 are fatal. Run them last and confirm in TRACE that
    // the right trap name appears.  The process exits via sys_exit(ERROR).
    // Uncomment whichever one you want to verify, run separately.
    //
    
    /*
    TracePrintf(0, "[TEST 7] about to divide by zero (expect TRAP_MATH)\n");
    volatile int zero = 0;
    volatile int one = 1;
    volatile int result;
    __asm__ volatile ("idivl %1" : "=a"(result) : "r"(zero), "a"(one), "d"(0));
    TracePrintf(0, "[TEST 7] FAIL: should not reach here\n");
    */

    //  TEST 8: TRAP_MEMORY bad addr (process will die) 

    /*
    TracePrintf(0, "[TEST 8] about to write addr 0x4 -- expect TRAP_MEMORY\n");
    volatile int *bad_ptr = (volatile int *)0x4;
    *bad_ptr = 99;
    TracePrintf(0, "[TEST 8] FAIL: should not reach here\n");
    */

    TracePrintf(0, "=== All safe tests complete. Uncomment TEST 7 or 8 to test fatal traps. ===\n");
    Exit(0);
    return 0;
}