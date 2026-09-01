// features.c — exercises the control-flow / operator additions (M1):
//   for-loops, do/while, break, continue, prefix ++/--, ternary ?:
//
//   ./nano_cc features.c features.s
//   gcc -nostdlib -no-pie features.s -o features_prog
//   ./features_prog
#include <nano-nolibc.h>

int main() {
    // for-loop: 1 + 2 + ... + 10 = 55
    int sum = 0;
    for (int i = 1; i <= 10; i = i + 1) sum = sum + i;
    println("for   1..10        = ", sum);

    // for + break + continue + prefix ++: even numbers below 9 -> 2+4+6+8 = 20
    sum = 0;
    for (int i = 1; i <= 10; ++i) {
        if (i == 9) break;
        if (i % 2 == 1) continue;
        sum = sum + i;
    }
    println("evens below 9      = ", sum);

    // do/while: prints 3 2 1
    int n = 3;
    _puts("do/while countdown =");
    do { print(" ", n); --n; } while (n > 0);
    _puts("\n");

    // prefix ++ returns the new value
    int a = 5;
    int b = ++a;                         // a == 6, b == 6
    print("prefix ++a         = ", a); println(", b = ", b);

    // ternary
    int hi = (a > b) ? a : 99;           // a == b, so 99
    println("ternary (a>b?a:99) = ", hi);

    return 0;
}
