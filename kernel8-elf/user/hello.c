// hello.c — the first nano_cc-compiled program to run as a process.
//
// It is deliberately not a print statement. Printing proves the loader put SOME
// bytes somewhere executable; opening a file, reading it back and creating
// another one proves the syscall boundary carries pointers and lengths in both
// directions, and that the process is talking to the same filesystem the shell
// sees.

#include "nano-user.h"

char buf[256];

int main(long arg) {
    long fd;
    long n;
    long total;

    puts("hello from a user program\n");
    printf("  pid %d, started with argument %d\n", getpid(), arg);

    // --- read a file the kernel put there ---
    fd = open("/doc/readme", 0);
    if (fd < 0) {
        puts("  could not open /doc/readme\n");
        return 1;
    }
    printf("  /doc/readme is %d bytes\n", fsize(fd));
    total = 0;
    for (;;) {
        n = read(fd, buf, 64);
        if (n <= 0) break;
        total = total + n;
    }
    close(fd);
    printf("  read %d bytes back through the syscall boundary\n", total);

    // --- and write one of its own, for the shell to find afterwards ---
    fd = open("/hello.out", 1);
    if (fd < 0) {
        puts("  could not create /hello.out\n");
        return 2;
    }
    write(fd, "written by a user process\n", 26);
    close(fd);
    puts("  wrote /hello.out\n");

    // A distinctive exit code, so the kernel side is checking a value that
    // could only have come from here. Zero would also be what a process that
    // never ran at all would leave behind.
    return 7;
}
