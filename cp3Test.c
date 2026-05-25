#include <yalnix.h>
#include <hardware.h>
#include <syscalls.h>
#include <traps.h>
#include <ykernel.h>

int main() {
    TracePrintf(0, "=== Yalnix Syscall Test Program ===\n");

    /* Test GetPid */
    TracePrintf(0, "\n[TEST] GetPid\n");
    int pid = GetPid();
    TracePrintf(0, "  GetPid returned: %d\n", pid);
    if (pid >= 0) {
        TracePrintf(0, "  PASS: valid pid\n");
    } else {
        TracePrintf(0, "  FAIL: invalid pid\n");
    }

    /* Test Brk */
    TracePrintf(0, "\n[TEST] Brk\n");
    extern void *_end;  // linker symbol for end of data segment
    void *base = (void *)UP_TO_PAGE(&_end);
    
    int rc = Brk((void *)((unsigned long)base + 0x2000));
    if (rc == 0) {
        TracePrintf(0, "  PASS: Brk grow succeeded\n");
    } else {
        TracePrintf(0, "  FAIL: Brk grow failed rc=%d\n", rc);
    }

    // shrink back
    rc = Brk(base);
    if (rc == 0) {
        TracePrintf(0, "  PASS: Brk shrink succeeded\n");
    } else {
        TracePrintf(0, "  FAIL: Brk shrink failed rc=%d\n", rc);
    }

    rc = Brk((void *)0x100000);
    if (rc == ERROR) {
        TracePrintf(0, "  PASS: Brk invalid addr correctly returned ERROR\n");
    } else {
        TracePrintf(0, "  FAIL: Brk invalid addr should have failed\n");
    }

    /* Test Delay */
    TracePrintf(0, "\n[TEST] Delay\n");

    // invalid delay
    rc = Delay(-1);
    if (rc == ERROR) {
        TracePrintf(0, "  PASS: Delay(-1) correctly returned ERROR\n");
    } else {
        TracePrintf(0, "  FAIL: Delay(-1) should have returned ERROR\n");
    }

    // zero delay should return immediately
    rc = Delay(0);
    if (rc == 0) {
        TracePrintf(0, "  PASS: Delay(0) returned immediately\n");
    } else {
        TracePrintf(0, "  FAIL: Delay(0) failed rc=%d\n", rc);
    }

    // should context switch to idle and come back
    TracePrintf(0, "  Delaying for 3 ticks...\n");
    rc = Delay(3);
    if (rc == 0) {
        TracePrintf(0, "  PASS: Delay(3) completed successfully\n");
    } else {
        TracePrintf(0, "  FAIL: Delay(3) failed rc=%d\n", rc);
    }

    /* Test multiple delays to verify scheduling */
    TracePrintf(0, "\n[TEST] Multiple Delays\n");
    for (int i = 1; i <= 3; i++) {
        TracePrintf(0, "  Delay iteration %d: delaying 2 ticks\n", i);
        Delay(2);
        TracePrintf(0, "  Delay iteration %d: woke up\n", i);
    }
    TracePrintf(0, "  PASS: multiple delays completed\n");

    /* Test GetPid again after delays */
    TracePrintf(0, "\n[TEST] GetPid after delays\n");
    int pid2 = GetPid();
    if (pid2 == pid) {
        TracePrintf(0, "  PASS: pid consistent after delays: %d\n", pid2);
    } else {
        TracePrintf(0, "  FAIL: pid changed from %d to %d\n", pid, pid2);
    }

    TracePrintf(0, "\n=== All tests complete ===\n");

    // loop forever so kernel keeps running
    while (1) {
        TracePrintf(0, "\ntest done.\n");
        Pause();
    }

    return 0;
}
