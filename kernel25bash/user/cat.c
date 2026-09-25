// cat.c — copy stdin to stdout.
//
// The smallest program that proves a pipe works, because it does nothing
// except move bytes between the two descriptors the shell gave it. With
// `hello | cat` the output arriving unchanged means the write end, the
// buffer, the read end and the blocking read all did their jobs.
//
// With no stdin it reads end-of-file immediately and exits, which is what
// makes `cat` with nothing piped in terminate instead of hanging.

#include "nano-user.h"

char g_buf[512];

int main(int argc, char **argv) {
    long n;
    long total;

    total = 0;
    for (;;) {
        n = read(0, g_buf, 512);
        if (n <= 0) break;
        write(1, g_buf, n);
        total = total + n;
    }
    return 0;
}
