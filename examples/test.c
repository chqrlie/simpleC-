#include <nano-nolibc.h>

#define msg(s, n)   println(s, n)
#define msg2(f, s)  write(f, s, strlen(s))

int main() {
    msg("Hello from bare-metal nolibc! The answer is ", 6 * 7);

    int fd = open("/dev/tty", 2);
    msg("/dev/tty open as ", fd);

    msg2(fd, "Hello /dev/tty\n");
    close(fd);
}
