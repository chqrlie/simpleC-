// features.c — exercises the control-flow / operator additions (M1):
//   for-loops, do/while, break, continue, prefix ++/--, ternary ?:
//
//   ./nano_cc features.c features.s
//   gcc -nostdlib -no-pie features.s -o features_prog
//   ./features_prog
#include "nano-nolibc.h"

int main() {
    int sum;
    int i;

    // for-loop: 1 + 2 + ... + 10 = 55
    sum = 0;
    for (i = 1; i <= 10; i = i + 1) sum = sum + i;
    puts("for   1..10        = "); print_int(sum); puts("\n");

    // for + break + continue + prefix ++: even numbers below 9 -> 2+4+6+8 = 20
    sum = 0;
    for (i = 1; i <= 10; ++i) {
        if (i == 9) break;
        if (i % 2 == 1) continue;
        sum = sum + i;
    }
    puts("evens below 9      = "); print_int(sum); puts("\n");

    // do/while: prints 3 2 1
    int n;
    n = 3;
    puts("do/while countdown = ");
    do { print_int(n); puts(" "); --n; } while (n > 0);
    puts("\n");

    // prefix ++ returns the new value
    int a; int b;
    a = 5;
    b = ++a;                         // a == 6, b == 6
    puts("prefix ++a         = "); print_int(a); puts(", b = "); print_int(b); puts("\n");

    // ternary
    int hi;
    hi = (a > b) ? a : 99;           // a == b, so 99
    puts("ternary (a>b?a:99) = "); print_int(hi); puts("\n");

    exit(0);
    return 0;
}
