#include <hardware.h>
#include <yalnix.h>
#include <ykernel.h>
#include <idle.h>

int main(int argc, char *argv[]) {
    while (1) {
        TracePrintf(1,"\n====Idle====\n");
        Pause();
    }
    return 0;
}
