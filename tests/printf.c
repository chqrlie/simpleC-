// printf.c — demonstrates variadic functions: a real printf() written in
// nano-nolibc.h and compiled by nano_cc.
//
//   ./nano_cc printf.c printf.s
//   gcc -nostdlib -no-pie printf.s -o printf_prog
//   ./printf_prog
#include "nano-nolibc.h"

int main() {
    printf("Hello, %s! You are %d years old.\n", "world", 28);
    printf("hex: %x   dec: %d   char: %c\n", 255, 255, 'A');
    printf("negative: %d\n", -42);
    printf("literal percent: 100%%\n");
    printf("four args: %s=%d, %s=%d\n", "a", 1, "b", 2);
    return 0;
}
