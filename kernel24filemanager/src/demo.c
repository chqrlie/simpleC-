// demo.c — the program the OS compiles, to prove that it can.
//
// Nothing here is clever. What it is, is BROAD: an include, two function-like
// macros, a struct passed by pointer, a recursive function, an array, a switch,
// a while loop, a ternary and some pointer arithmetic. Between them they reach
// the preprocessor, the parser, the type checker and the code generator, so a
// compiler that gets this file right is not accidentally producing plausible
// output for a program with one statement in it.
//
// It calls nothing outside itself. There is no C library on the other side of
// this -- the point is the assembly that comes out, not running it.

#include "util.h"

char message[32];
long results[8];

long fib(long n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

long sum_upto(long n) {
    long total;
    long i;
    total = 0;
    for (i = 1; i <= n; i++) total = total + i;
    return total;
}

long classify(long v) {
    switch (v % 3) {
        case 0:  return 100;
        case 1:  return 200;
        default: return 300;
    }
}

int main(int argc, char **argv) {
    struct Point a;
    struct Point b;
    char *p;
    long i;
    long total;

    a.x = 3; a.y = 4;
    b.x = 5; b.y = 6;

    total = dot(&a, &b);              // 15 + 24 = 39
    total = total + SQUARE(7);        // + 49
    total = total + MAX(a.x, b.y);    // + 6

    i = 0;
    while (i < 8) {
        results[i] = fib(i) + classify(i);
        total = total + results[i];
        i = i + 1;
    }

    total = total + sum_upto(10);     // + 55

    p = message;
    *p++ = 'o';
    *p++ = 'k';
    *p = 0;

    return (int)(argc > 1 ? total : total + (message[0] == 'o' ? 1 : 0));
}
