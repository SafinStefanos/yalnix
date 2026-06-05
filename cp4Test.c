#include <yalnix.h>
#include <hardware.h>
#include <syscalls.h>
#include <traps.h>
#include <ykernel.h>
void test_fork_basic();
void test_fork_parent_child();
void test_wait_basic();
void test_exit_status();
void test_exec();
void test_wait_no_children();
void test_multiple_children();

int main() {
    TracePrintf(0, "\n=== Checkpoint 4 Syscall Tests ===\n");

    test_fork_basic();
    test_fork_parent_child();
    test_wait_basic();
    test_exit_status();
    test_wait_no_children();
    test_multiple_children();
    test_exec();

    TracePrintf(0, "\n=== All Checkpoint 4 Tests Complete ===\n");

    Exit(0);
    return 0;
}

/* test that fork returns different pids to parent and child */
void test_fork_basic() {
    TracePrintf(0, "\n[TEST] Fork Basic\n");

    int parent_pid = GetPid();

    TracePrintf(0, "Before Fork pid=%d\n", parent_pid);

    int rc = Fork();

    TracePrintf(0, "After Fork rc=%d pid=%d\n", rc, GetPid());

    if (rc == 0) {
        TracePrintf(0, "Child exiting now\n");
        Exit(0);

        TracePrintf(0, "ERROR: child returned from Exit\n");
    }
    else if (rc > 0) {
        TracePrintf(0, "Parent waiting now\n");

        int status;
        int wpid = Wait(&status);

        TracePrintf(0, "Parent woke up pid=%d status=%d\n",
                    wpid, status);
    }
    else {
        TracePrintf(0, "Fork failed\n");
    }
}

/* test that parent and child have independent address spaces */
void test_fork_parent_child() {
    TracePrintf(0, "\n[TEST] Fork Address Space Independence\n");
    int shared = 42;
    int rc = Fork();

    if (rc == 0) {
        shared = 99;
        TracePrintf(0, "  Child: shared=%d (should be 99)\n", shared);
        if (shared == 99) {
            TracePrintf(0, "  Child PASS: modified own copy\n");
        } else {
            TracePrintf(0, "  Child FAIL: unexpected value %d\n", shared);
        }
        Exit(0);
    } else if (rc > 0) {
        int status;
        Wait(&status);
        TracePrintf(0, "  Parent: shared=%d (should still be 42)\n", shared);
        if (shared == 42) {
            TracePrintf(0, "  Parent PASS: address space independent\n");
        } else {
            TracePrintf(0, "  Parent FAIL: child corrupted parent memory\n");
        }
    } else {
        TracePrintf(0, "  FAIL: Fork returned ERROR\n");
    }
}

/* test wait blocks until child exits */
void test_wait_basic() {
    TracePrintf(0, "\n[TEST] Wait Basic\n");
    int rc = Fork();
    if (rc == 0) {
        TracePrintf(0, "  Child: delaying 3 ticks\n");
        Delay(3);
        TracePrintf(0, "  Child: done delaying, exiting\n");
        Exit(0);
    } else if (rc > 0) {
        int child_pid = rc;
        TracePrintf(0, "  Parent: waiting for child pid=%d\n", child_pid);
        int status;
        int wpid = Wait(&status);
        TracePrintf(0, "  Parent: Wait returned pid=%d status=%d\n", wpid, status);
        if (wpid == child_pid && status == 0)
            TracePrintf(0, "  PASS: correct pid and status\n");
        else
            TracePrintf(0, "  FAIL: expected pid=%d status=0\n", child_pid);
    } else {
        TracePrintf(0, "  FAIL: Fork returned ERROR\n");
    }
}

/* test exit status is correctly passed to parent */
void test_exit_status() {
    TracePrintf(0, "\n[TEST] Exit Status\n");
    int rc = Fork();

    if (rc == 0) {
        Exit(42);
    } else if (rc > 0) {
        int status;
        int wpid = Wait(&status);
        TracePrintf(0, "  Parent: child exited with status=%d\n", status);
        if (status == 42) {
            TracePrintf(0, "  PASS: exit status correct\n");
        } else {
            TracePrintf(0, "  FAIL: expected 42 got %d\n", status);
        }
    } else {
        TracePrintf(0, "  FAIL: Fork returned ERROR\n");
    }
}

/* test wait with no children returns error */
void test_wait_no_children() {
    TracePrintf(0, "\n[TEST] Wait No Children\n");
    int rc = Fork();

    if (rc == 0) {
        /* child has no children of its own */
        int status;
        int wpid = Wait(&status);
        if (wpid == ERROR) {
            TracePrintf(0, "  PASS: Wait with no children returned ERROR\n");
        } else {
            TracePrintf(0, "  FAIL: Wait should have returned ERROR got %d\n", wpid);
        }
        Exit(0);
    } else if (rc > 0) {
        int status;
        Wait(&status);
    } else {
        TracePrintf(0, "  FAIL: Fork returned ERROR\n");
    }
}

/* test multiple children all get waited on */
void test_multiple_children() {
    TracePrintf(0, "\n[TEST] Multiple Children\n");
    int pids[3];
    int i;

    for (i = 0; i < 3; i++) {
        int rc = Fork();
        if (rc == 0) {
            TracePrintf(0, "  Child %d: exiting with status %d\n", GetPid(), i);
            Exit(i);
        } else if (rc > 0) {
            pids[i] = rc;
        } else {
            TracePrintf(0, "  FAIL: Fork %d returned ERROR\n", i);
            return;
        }
    }

    /* wait for all 3 children */
    int all_ok = 1;
    for (i = 0; i < 3; i++) {
        int status;
        int wpid = Wait(&status);
        TracePrintf(0, "  Parent: reaped pid=%d status=%d\n", wpid, status);
        if (wpid == ERROR) {
            TracePrintf(0, "  FAIL: Wait returned ERROR on iteration %d\n", i);
            all_ok = 0;
        }
    }
    if (all_ok) {
        TracePrintf(0, "  PASS: all children reaped\n");
    }
}

/* test exec replaces process image */
void test_exec() {
    TracePrintf(0, "\n[TEST] Exec\n");
    int rc = Fork();

    if (rc == 0) {
        TracePrintf(0, "  Child: execing 'init'\n");
        char *args[] = {"simple", NULL};
        int rc2 = Exec("simple", args);
        /* should not reach here if exec succeeds */
        TracePrintf(0, "  Child FAIL: Exec returned %d\n", rc2);
        Exit(1);
    } else if (rc > 0) {
        TracePrintf(0, "  Parent: waiting for exec'd child\n");
        int status;
        int wpid = Wait(&status);
        TracePrintf(0, "  Parent: child returned pid=%d status=%d\n", wpid, status);
        TracePrintf(0, "  PASS: Exec test done\n");
    } else {
        TracePrintf(0, "  FAIL: Fork returned ERROR\n");
    }
}