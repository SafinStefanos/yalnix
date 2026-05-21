#include <hardware.h>
#include <yalnix.h>
#include <ykernel.h>
#include <idle.h>

int DoIdle(int argc, char *argv[]) {
    while (1) {
        TracePrintf(1,"DoIdle\n");
        Pause();
    }
    return 0;
}
