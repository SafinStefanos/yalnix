#include <yalnix.h>
#include <hardware.h>

int main() {
    TracePrintf(0, "=== TTY Test Program ===\n", 25);

    /* test 1: simple write */
    int rc = TtyWrite(TTY_CONSOLE, "hello world\n", 12);
    if (rc == 12) {
        TracePrintf(0, "[TEST 1] PASS: simple write\n", 28);
    } else {
        TracePrintf(0, "[TEST 1] FAIL: simple write\n", 28);
    }

    /* test 2: write exactly TERMINAL_MAX_LINE bytes */
    char buf[TERMINAL_MAX_LINE];
    int i;
    for (i = 0; i < TERMINAL_MAX_LINE - 1; i++) buf[i] = 'B';
    buf[TERMINAL_MAX_LINE - 1] = '\n';
    rc = TtyWrite(TTY_CONSOLE, buf, TERMINAL_MAX_LINE);
    if (rc == TERMINAL_MAX_LINE) {
        TracePrintf(0, "[TEST 2] PASS: exact max line write\n", 36);
    } else {
        TracePrintf(0, "[TEST 2] FAIL: exact max line write\n", 36);
    }

    /* test 3: write more than TERMINAL_MAX_LINE to exercise chunking */
    char bigbuf[TERMINAL_MAX_LINE * 2 + 1];
    for (i = 0; i < TERMINAL_MAX_LINE * 2; i++) bigbuf[i] = 'C' + (i % 20);
    bigbuf[TERMINAL_MAX_LINE * 2] = '\n';
    rc = TtyWrite(TTY_CONSOLE, bigbuf, TERMINAL_MAX_LINE * 2 + 1);
    if (rc == TERMINAL_MAX_LINE * 2 + 1) {
        TracePrintf(0, "[TEST 3] PASS: multi-chunk write\n", 33);
    } else {
        TracePrintf(0, "[TEST 3] FAIL: multi-chunk write\n", 33);
    }

    /* test 4: write to each terminal */
    TracePrintf(0, "[TEST 4] writing to all terminals\n", 34);
    rc = TtyWrite(TTY_1, "message to TTY_1\n", 17);
    if (rc == 17) {
        TracePrintf(0, "[TEST 4] PASS: TTY_1 write\n", 27);
    } else {
        TracePrintf(0, "[TEST 4] FAIL: TTY_1 write\n", 27);
    }
    rc = TtyWrite(TTY_2, "message to TTY_2\n", 17);
    if (rc == 17) {
        TracePrintf(0, "[TEST 4] PASS: TTY_2 write\n", 27);
    } else {
        TracePrintf(0, "[TEST 4] FAIL: TTY_2 write\n", 27);
    }
    rc = TtyWrite(TTY_3, "message to TTY_3\n", 17);
    if (rc == 17) {
        TracePrintf(0, "[TEST 4] PASS: TTY_3 write\n", 27);
    } else {
        TracePrintf(0, "[TEST 4] FAIL: TTY_3 write\n", 27);
    }

    /* test 5: zero length write */
    rc = TtyWrite(TTY_CONSOLE, "anything", 0);
    if (rc == 0) {
        TracePrintf(0, "[TEST 5] PASS: zero length write\n", 33);
    } else {
        TracePrintf(0, "[TEST 5] FAIL: zero length write\n", 33);
    }

    /* test 6: multiple sequential writes */
    TracePrintf(0, "[TEST 6] sequential writes: ", 28);
    TtyWrite(TTY_CONSOLE, "one ", 4);
    TtyWrite(TTY_CONSOLE, "two ", 4);
    TtyWrite(TTY_CONSOLE, "three\n", 6);
    TracePrintf(0, "[TEST 6] PASS: sequential writes done\n", 38);

    
    // test 7: read from console -- requires -x and user to type 
    TtyWrite(TTY_CONSOLE, "[TEST 7] type something and press enter: ", 41);
    TracePrintf(0, "\n[TEST 7]\n ", 28);
    char readbuf[256];
    int n = TtyRead(TTY_CONSOLE, readbuf, 255);
    if (n > 0) {
        TtyWrite(TTY_CONSOLE, "  you typed: ", 13);
        TtyWrite(TTY_CONSOLE, readbuf, n);
        TracePrintf(0, "[TEST 7] PASS: read worked\n", 27);
    } else {
        TracePrintf(0, "[TEST 7] FAIL: read got nothing\n", 32);
    }

    // test 8: read then immediately write back 
    TtyWrite(TTY_CONSOLE, "[TEST 8] type another line: ", 28);
    TracePrintf(0, "\n[TEST 8]\n ", 28);
    n = TtyRead(TTY_CONSOLE, readbuf, 255);
    if (n > 0) {
        TtyWrite(TTY_CONSOLE, "  echo: ", 8);
        TtyWrite(TTY_CONSOLE, readbuf, n);
        TracePrintf(0, "[TEST 8] PASS: read-write roundtrip\n", 36);
    } else {
        TracePrintf(0, "[TEST 8] FAIL\n", 14);
    }

    TracePrintf(0, "=== TTY Tests Complete ===\n", 27);
    Exit(0);
    return 0;   
    
}