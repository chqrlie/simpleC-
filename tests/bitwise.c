// bitwise.c — exercises the new additions:
//   * bitwise operators  &  |  ^  ~  <<  >>  (with correct C precedence)
//   * function-like macros  #define MAX(a,b) ...   #define SET_BIT(x,n) ...
//
//   ./nano_cc bitwise.c bitwise.s
//   gcc -nostdlib -no-pie bitwise.s -o bitwise_prog
//   ./bitwise_prog
#include "nano-nolibc.h"

// function-like macros (argument substitution + nested parens)
#define MAX(a, b)     ((a) > (b) ? (a) : (b))
#define MIN(a, b)     ((a) < (b) ? (a) : (b))
#define SET_BIT(x, n) ((x) | (1 << (n)))
#define IS_ODD(x)     ((x) & 1)

static void show(char *label, long v) {
    puts(label); print_int(v); puts("\n");
}

int main() {
    show("0x0F & 0x05      = ", 0x0F & 0x05);      // 5
    show("0x0F | 0x30      = ", 0x0F | 0x30);      // 63
    show("0xF0 ^ 0x0F      = ", 0xF0 ^ 0x0F);      // 255
    show("~0               = ", ~0);               // -1  (64-bit all ones)
    show("1 << 4           = ", 1 << 4);           // 16
    show("255 >> 2         = ", 255 >> 2);         // 63

    // precedence checks (no parentheses):
    show("1 << 2 + 3       = ", 1 << 2 + 3);       // 32  (+ binds tighter than <<)
    show("0x0F & 0x03 | 16 = ", 0x0F & 0x03 | 16); // 19  (& tighter than |)

    // function-like macros:
    show("MAX(3, 9)        = ", MAX(3, 9));        // 9
    show("MIN(3, 9)        = ", MIN(3, 9));        // 3
    show("MAX(2 + 3, 4)    = ", MAX(2 + 3, 4));    // 5
    show("SET_BIT(0, 3)    = ", SET_BIT(0, 3));    // 8
    show("IS_ODD(7)        = ", IS_ODD(7));        // 1
    show("IS_ODD(8)        = ", IS_ODD(8));        // 0

    exit(0);
    return 0;
}
