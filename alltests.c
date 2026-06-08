/*
 * alltests.c -- combined Yalnix test: cp4 (fork/exit/wait + address-space
 * independence), tty writes, pipe reader->writer, pipe writer->reader,
 * lock, and cvar.  Self-scoring: prints "PASS:"/"FAIL:" per check and a
 * final tally.
 *
 * Build:  add alltests.c to U_SRCS in the Makefile, then `make`.
 * Run:    ./yalnix alltests
 *
 * PASS/FAIL go to the TRACE file via TracePrintf(0,...) AND to terminal 0
 * via TtyPrintf, so run_tests.py and a human both see them.
 *
 * All scoring is done in the PARENT.  Children report their observations
 * back through Exit() status, which the parent checks after Wait().
 */

#include <yuser.h>

#define TTY 0
#ifndef NUM_TERMINALS
#define NUM_TERMINALS 4      /* not exported by yuser.h; matches hardware.h */
#endif

static int g_pass  = 0;
static int g_total = 0;

/* Record one check.  Runs in the PARENT only. */
static void check(int cond, char *name) {
    g_total++;
    if (cond) {
        g_pass++;
        TracePrintf(0, "  PASS: %s\n", name);
        TtyPrintf(TTY, "  PASS: %s\n", name);
    } else {
        TracePrintf(0, "  FAIL: %s\n", name);
        TtyPrintf(TTY, "  FAIL: %s\n", name);
    }
}

static void banner(char *s) {
    TracePrintf(0, "\n===== %s =====\n", s);
    TtyPrintf(TTY, "\n===== %s =====\n", s);
}

/* STEP markers bracket every call that can block/hang so the LAST "STEP"
 * line in TRACE pinpoints exactly where the kernel got stuck.  A matching
 * "STEP done" line means the call returned -- so "STEP X" with no "STEP X
 * done" after it == hung inside that operation. */
static int g_step = 0;
#define STEP(msg)      TracePrintf(0, "STEP %d: %s\n",      ++g_step, msg)
#define STEP_DONE(msg) TracePrintf(0, "STEP %d: %s done\n",   g_step, msg)

/* a global used to prove fork copies the address space */
static int shared = 42;

/* ------------------------------------------------------------------ */
/* SECTION 1: basics                                                  */
/* ------------------------------------------------------------------ */
static void test_basics(void) {
    banner("basics");
    int pid = GetPid();
    TracePrintf(0, "  GetPid = %d\n", pid);
    check(pid >= 0, "GetPid returns valid pid");

    int b = Brk((void *)0);     /* query current break is not defined; just
                                   exercise a small grow via malloc instead */
    char *p = (char *)malloc(64);
    check(p != NULL, "malloc/Brk grow");
    if (p) { p[0] = 'x'; p[63] = 'y'; check(p[0]=='x' && p[63]=='y', "heap writable"); }
    (void)b;
}

/* ------------------------------------------------------------------ */
/* SECTION 2: cp4 -- fork / exit / wait / address-space independence  */
/* ------------------------------------------------------------------ */
static void test_fork_wait(void) {
    banner("fork / exit / wait");

    /* (a) exit status is delivered to parent */
    STEP("Fork (a)");
    int rc = Fork();
    if (rc == 0) { Delay(1); Exit(42); }
    STEP_DONE("Fork (a)");
    check(rc > 0, "Fork returned child pid");
    int status = -999;
    STEP("Wait (a)");
    int wpid = Wait(&status);
    STEP_DONE("Wait (a)");
    check(wpid == rc, "Wait reaped the right pid");
    check(status == 42, "Wait got correct exit status (42)");

    /* (b) address-space independence: child mutates `shared`, parent must
       still see 42.  Child reports its own view via exit status. */
    shared = 42;
    rc = Fork();
    if (rc == 0) {
        shared = 99;
        Exit(shared == 99 ? 0 : 1);   /* 0 => child saw its own write */
    }
    Wait(&status);
    check(status == 0, "child saw its own modified copy");
    check(shared == 42, "parent address space unaffected by child");

    /* (c) Wait with no children returns ERROR */
    rc = Wait(&status);
    check(rc == ERROR, "Wait with no children returns ERROR");

    /* (d) reap several children */
    int i, n = 3, ok = 1;
    for (i = 0; i < n; i++) {
        rc = Fork();
        if (rc == 0) Exit(100 + i);
        if (rc == ERROR) ok = 0;
    }
    for (i = 0; i < n; i++) {
        if (Wait(&status) == ERROR) ok = 0;
    }
    check(ok, "forked and reaped 3 children");
}

/* ------------------------------------------------------------------ */
/* SECTION 3: tty writes                                              */
/* ------------------------------------------------------------------ */
static void test_tty(void) {
    banner("tty writes");
    char *msg = "hello tty\n";
    int len = 10;
    int rc = TtyWrite(TTY, msg, len);
    check(rc == len, "TtyWrite simple returns len");

    rc = TtyWrite(TTY, msg, 0);
    check(rc == 0, "TtyWrite zero length returns 0");

    /* write to every terminal */
    int t, ok = 1;
    for (t = 0; t < NUM_TERMINALS; t++) {
        if (TtyWrite(t, "x\n", 2) != 2) ok = 0;
    }
    check(ok, "TtyWrite to all terminals");
}

/* ------------------------------------------------------------------ */
/* SECTION 4 & 5: pipes                                               */
/* helper: one child reads `len` bytes and exits 0 iff it matches want */
/* ------------------------------------------------------------------ */
static int pipe_id = -1;

static void pipe_reader_child(int len, char *want) {
    char buf[16];
    int rc = PipeRead(pipe_id, buf, len);
    if (rc != len) Exit(2);
    int i, match = 1;
    for (i = 0; i < len; i++) if (buf[i] != want[i]) match = 0;
    Exit(match ? 0 : 1);
}

static void test_pipe_reader_first(void) {
    banner("pipe: reader blocks first, then writer");
    check(PipeInit(&pipe_id) == 0, "PipeInit");

    int rc = Fork();
    if (rc == 0) pipe_reader_child(4, "abcd");   /* child blocks on empty pipe */

    STEP("parent Delay (let reader block)");
    Delay(2);                                     /* let child block first */
    STEP_DONE("parent Delay");
    STEP("PipeWrite to blocked reader");
    int w = PipeWrite(pipe_id, "abcd", 4);
    STEP_DONE("PipeWrite");
    check(w == 4, "PipeWrite after blocked reader");

    int status = -1;
    STEP("Wait for reader");
    Wait(&status);
    STEP_DONE("Wait for reader");
    check(status == 0, "reader unblocked and got correct data");
}

static void test_pipe_writer_first(void) {
    banner("pipe: writer first, then reader");
    check(PipeInit(&pipe_id) == 0, "PipeInit (2nd pipe)");

    int w = PipeWrite(pipe_id, "wxyz", 4);        /* data buffered */
    check(w == 4, "PipeWrite into empty pipe");

    int rc = Fork();
    if (rc == 0) pipe_reader_child(4, "wxyz");    /* should read immediately */

    int status = -1;
    Wait(&status);
    check(status == 0, "reader read buffered data correctly");
}

/* ------------------------------------------------------------------ */
/* SECTION 6: lock                                                    */
/* parent holds lock, child blocks on Acquire until parent releases   */
/* ------------------------------------------------------------------ */
static int lock_id = -1;

static void test_lock(void) {
    banner("lock");
    check(LockInit(&lock_id) == 0, "LockInit");
    check(Acquire(lock_id) == 0, "parent Acquire");

    int rc = Fork();
    if (rc == 0) {
        /* child blocks here until parent releases */
        if (Acquire(lock_id) != 0) Exit(1);
        Release(lock_id);
        Exit(0);
    }

    STEP("parent Delay (let child block on Acquire)");
    Delay(3);                 /* give child time to block on Acquire */
    STEP_DONE("parent Delay");
    check(Release(lock_id) == 0, "parent Release (wakes child)");

    int status = -1;
    STEP("Wait for lock child");
    Wait(&status);
    STEP_DONE("Wait for lock child");
    check(status == 0, "child acquired lock after release");
}

/* ------------------------------------------------------------------ */
/* SECTION 7: cvar                                                    */
/* parent waits on cvar; child signals; parent must wake              */
/* ------------------------------------------------------------------ */
static int cv_lock = -1;
static int cv_id   = -1;

static void test_cvar(void) {
    banner("cvar");
    check(LockInit(&cv_lock) == 0, "LockInit (cvar)");
    check(CvarInit(&cv_id) == 0, "CvarInit");

    int rc = Fork();
    if (rc == 0) {
        Delay(2);                 /* let parent block in CvarWait first */
        Acquire(cv_lock);
        CvarSignal(cv_id);
        Release(cv_lock);
        Exit(0);
    }

    Acquire(cv_lock);
    STEP("CvarWait (parent blocks for signal)");
    int wrc = CvarWait(cv_id, cv_lock);   /* releases lock, blocks, re-acquires */
    STEP_DONE("CvarWait");
    Release(cv_lock);
    check(wrc == 0, "CvarWait returned after signal");

    int status = -1;
    STEP("Wait for cvar child");
    Wait(&status);
    STEP_DONE("Wait for cvar child");
    check(status == 0, "signaling child exited cleanly");
}

/* ------------------------------------------------------------------ */
int main(void) {
    TracePrintf(0, "\n######## ALLTESTS START ########\n");
    TtyPrintf(TTY, "\n######## ALLTESTS START ########\n");

    test_basics();
    test_fork_wait();
    test_tty();
    test_pipe_reader_first();
    test_pipe_writer_first();
    test_lock();
    test_cvar();

    TracePrintf(0, "\n######## ALLTESTS RESULT: %d/%d passed ########\n",
                g_pass, g_total);
    TtyPrintf(TTY, "\n######## ALLTESTS RESULT: %d/%d passed ########\n",
              g_pass, g_total);
    Exit(0);
}
