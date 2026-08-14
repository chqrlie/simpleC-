#include "nano-nolibc.h"

int main() {
    _puts("Hello from bare-metal nolibc!\n");
    _print_int(42 + 100);
    _puts("\n");

    int fd = open("/dev/null", 0);
    _print_int(fd);
    _puts("\n");
    close(fd);

    exit(0);
    return 0;
}