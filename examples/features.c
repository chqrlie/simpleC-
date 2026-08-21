// features.c — exercises the control-flow / operator additions (M1):
//   for-loops, do/while, break, continue, prefix ++/--, ternary ?:
//
//   ./nano_cc features.c features.s
//   gcc -nostdlib -no-pie features.s -o features_prog
//   ./features_prog
#include <nano-nolibc.h>

int main() {
    int sum;
    int i;

    // for-loop: 1 + 2 + ... + 10 = 55
    sum = 0;
    for (i = 1; i <= 10; i = i + 1) sum = sum + i;
    _puts("for   1..10        = "); _print_int(sum); _puts("\n");

    // for + break + continue + prefix ++: even numbers below 9 -> 2+4+6+8 = 20
    sum = 0;
    for (i = 1; i <= 10; ++i) {
        if (i == 9) break;
        if (i % 2 == 1) continue;
        sum = sum + i;
    }
    _puts("evens below 9      = "); _print_int(sum); _puts("\n");

    // do/while: prints 3 2 1
    int n;
    n = 3;
    _puts("do/while countdown = ");
    do { _print_int(n); _puts(" "); --n; } while (n > 0);
    _puts("\n");

    // prefix ++ returns the new value
    int a; int b;
    a = 5;
    b = ++a;                         // a == 6, b == 6
    _puts("prefix ++a         = "); _print_int(a); _puts(", b = "); _print_int(b); _puts("\n");

    // ternary
    int hi;
    hi = (a > b) ? a : 99;           // a == b, so 99
    _puts("ternary (a>b?a:99) = "); _print_int(hi); _puts("\n");

    return 0;
}

