#include "lib/nano-nolibc.h"

void msg(const char *s, int n) {
    _puts(s); _print_int(n); _putc('\n');
}

#define msg2(f, s)  _write(f, s, strlen(s))

int main() {
    msg("Hello from bare-metal nolibc! The answer is ", 6 * 7);

    int fd = _open("/dev/tty", 2);
    msg("/dev/tty open as ", fd);

    msg2(fd, "Hello /dev/tty\n");
    _close(fd);
}
