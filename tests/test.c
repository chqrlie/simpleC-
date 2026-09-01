#include "nano-nolibc.h"

int main() {
    puts("Hello from bare-metal nolibc!\n");
    print_int(42 + 100);
    puts("\n");
    
    int fd = open("/dev/null", 0);
    print_int(fd);
    puts("\n");
    close(fd);
    
    exit(0);
    return 0;
}