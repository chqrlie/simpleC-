// wild.c — a program that does exactly one thing wrong, on purpose.
//
// Three different wrong things, chosen by the argument, because they fail for
// three different reasons and a loader can get one right while getting the
// others wrong:
//
//   0  a null pointer          -- the page at 0 is unmapped kernel-wide
//   1  a store into its own code -- the page is mapped read-only
//   2  a store far above anything mapped -- there is no page there at all
//   3  a jump into its own data  -- the page is mapped no-execute
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

// A buffer with a `ret` in it, and its address. Both are globals, so they live
// in .bss -- part of the writable, non-executable segment.
char g_code[16];
long g_target;

int main(int argc, char **argv) {
    long mode;
    char *p;

    mode = argc > 1 ? uatol(argv[1]) : 0;

    if (mode == 3) {
        // Write a perfectly valid instruction into data and jump to it. If the
        // page is executable this returns and the program carries on, which is
        // exactly the failure this case exists to catch: `ret` does not crash,
        // so nothing else about the run would look wrong.
        g_code[0] = (char)0xC3;             // ret
        g_target = (long)g_code;
        printf("wild: pid %d, jumping into its own data at 0x%x\n",
               getpid(), g_target);
        // CALL, not JMP. The instruction in the buffer is `ret`, and a `ret`
        // reached by a jump pops whatever happens to be on the stack and
        // faults on that -- which looks exactly like the fault this test is
        // supposed to be checking for, and happens whether the page is
        // executable or not. The first version of this test passed with NX
        // deliberately disabled, which is the only reason that was noticed.
        __asm__(
            "mov rax, [rip + g_target]\n"
            "call rax\n"
        );
        puts("wild: STILL RUNNING -- the data page was executable\n");
        return 0;
    }

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
