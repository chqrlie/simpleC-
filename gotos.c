// gotos.c — goto and labels.
//
// Checked against gcc compiling this same source. The cases that matter are
// the ones a hand-written jump table gets wrong: a forward goto whose label
// has not been seen yet, a backward goto used as a loop, jumping out of two
// nested loops at once, and a label living inside a switch body.

#include "nano-nolibc.h"

long find_first(long a[], long n, long want) {
    long i;
    for (i = 0; i < n; i = i + 1) { if (a[i] == want) goto found; }
    return -1;
found:                                  // forward goto, label defined later
    return i;
}

long classify(long x) {
    long r = 0;
    if (x < 0) goto neg;
    if (x == 0) goto zero;
    r = 1; goto done;
neg:
    r = -1; goto done;
zero:
    r = 0;
done:
    return r;
}

long escape(void) {
    long i, j, hits = 0;
    for (i = 0; i < 4; i = i + 1) {
        for (j = 0; j < 4; j = j + 1) {
            if (i * j > 4) goto out;    // out of two loops at once
            hits = hits + 1;
        }
    }
out:
    return hits;
}

long backward(void) {
    long n = 0;
top:                                    // backward goto used as a loop
    n = n + 1;
    if (n < 5) goto top;
    return n;
}

long in_switch(long x) {
    long r = 0;
    switch (x) {
        case 1: r = 10; goto tail;      // out of the switch entirely
        case 2: r = 20; break;
        lbl:    r = r + 100; break;     // a label sharing the switch body
        case 3: goto lbl;
        default: r = -1;
    }
    return r;
tail:
    return r + 1;
}

// Two functions may reuse a label name; the mapping is per function.
long reuse_a(void) { long n = 1; goto done; done: return n; }
long reuse_b(void) { long n = 2; goto done; done: return n; }

int main() {
    long a[5] = { 4, 8, 15, 16, 23 };
    printf("find 15  = %d\n", find_first(a, 5, 15));
    printf("find 99  = %d\n", find_first(a, 5, 99));
    printf("classify = %d %d %d\n", classify(-7), classify(0), classify(7));
    printf("escape   = %d\n", escape());
    printf("backward = %d\n", backward());
    printf("switch   = %d %d\n", in_switch(1), in_switch(2));
    printf("switch   = %d %d\n", in_switch(3), in_switch(9));
    printf("reuse    = %d %d\n", reuse_a(), reuse_b());
    return 0;
}
