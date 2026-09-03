// wild.c — a program that does exactly one thing wrong, on purpose.
//
// Three different wrong things, chosen by the argument, because they fail for
// three different reasons and a loader can get one right while getting the
// others wrong:
//
//   0  a null pointer          -- the page at 0 is unmapped kernel-wide
//   1  a store into its own code -- the page is mapped read-only
//   2  a store far above anything mapped -- there is no page there at all
//
// Case 1 is the one worth having. Until CR0.WP was set, that store SUCCEEDED:
// the CPU only enforces a read-only page against ring 3, and these programs run
// in ring 0. The mapping said read-only and the hardware ignored it, so a test
// that only ran cases 0 and 2 would have passed while the permission bits did
// nothing at all.
//
// Whichever one runs, the rest of the system has to carry on. That is the point
// of the exercise, not the fault itself.

#include "nano-user.h"

int main(int argc, char **argv) {
    long mode;
    char *p;

    mode = argc > 1 ? uatol(argv[1]) : 0;

    printf("wild: pid %d, about to do something it should not (mode %d)\n",
           getpid(), mode);

    if (mode == 1) {
        p = (char *)0x8000000000;      // the first byte of its own text segment
    } else if (mode == 2) {
        p = (char *)0x8020000000;      // 512 MiB into user space: nothing there
    } else {
        p = (char *)0;                 // the classic
    }

    p[0] = 1;

    // Reaching this line means the store went through, which means the
    // protection this test exists to check is not there.
    puts("wild: STILL RUNNING -- the write was not stopped\n");
    return 0;
}
