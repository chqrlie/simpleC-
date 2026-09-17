#include <limits.h>
#include <stdio.h>

long idiv(long a, long b) { return a / b; }
long imod(long a, long b) { return a % b; }
long imul(long a, long b) { return a * b; }

unsigned long div(unsigned long a, unsigned long b) { return a / b; }
unsigned long mod(unsigned long a, unsigned long b) { return a % b; }
unsigned long mul(unsigned long a, unsigned long b) { return a * b; }

#define TEST(a, b, op, f, fmt, t)  \
        if (a op (long)(b) != f(a, b)) { \
            printf(#t": "fmt" %s "fmt" = "fmt", expected "fmt"\n", \
                   (t)(a), #op, (t)(b), (t)(a op b), f(a, b));     \
            status = 1; \
        } \
        if (1 + (a op (long)(b)) != 1 + f(a, b)) { \
            printf(#t": 1 + ("fmt" %s "fmt") = "fmt", expected "fmt"\n", \
                   (t)(a), #op, (t)(b), 1 + (t)(a op b), 1 + f(a, b));     \
            status = 1; \
        }

#define ITEST(a, b)  TEST(a, b, *, imul, "%ld", long); \
                     if ((b) && !((a) == LONG_MIN && (b) == -1)) { \
                         TEST(a, b, /, idiv, "%ld", long); \
                         TEST(a, b, %, imod, "%ld", long); \
                     }
#define STEST(a, b)  ITEST(a, b); ITEST(a, -(b))
#define UTEST(a, b)  TEST(a, b, *, mul, "%lu", unsigned long); \
                     if (b) { \
                         TEST(a, b, /, div, "%lu", unsigned long); \
                         TEST(a, b, %, mod, "%lu", unsigned long); \
                     }

int test(long n) {
    int status = 0;
    STEST(n, 1);
    STEST(n, 2);
    STEST(n, 3);
    STEST(n, 4);
    STEST(n, 5);
    STEST(n, 6);
    STEST(n, 7);
    STEST(n, 8);
    STEST(n, 9);
    STEST(n, 10);
    STEST(n, 11);
    STEST(n, 16);
    STEST(n, 32);
    STEST(n, 64);
    STEST(n, 100);
    STEST(n, 128);
    STEST(n, 0x100);
    STEST(n, 0x1000);
    STEST(n, 0x10000);
    STEST(n, 0x100000);
    STEST(n, 0x1000000);
    STEST(n, 0x10000000);
    STEST(n, 0x100000000);
    STEST(n, 0x1000000000);
    STEST(n, 0x10000000000);
    STEST(n, 0x100000000000);
    STEST(n, 0x1000000000000);
    STEST(n, 0x10000000000000);
    STEST(n, 0x100000000000000);
    STEST(n, 0x1000000000000000);
    STEST(n, 0x2000000000000000);
    STEST(n, 0x4000000000000000);
    STEST(n, 0x7fffffffffffffff);
    ITEST(n, -0x7fffffffffffffff-1);

    unsigned long u = (unsigned long)n;
    UTEST(u, 1);
    UTEST(u, 2);
    UTEST(u, 3);
    UTEST(u, 4);
    UTEST(u, 5);
    UTEST(u, 6);
    UTEST(u, 7);
    UTEST(u, 8);
    UTEST(u, 9);
    UTEST(u, 10);
    UTEST(u, 11);
    UTEST(u, 16);
    UTEST(u, 32);
    UTEST(u, 64);
    UTEST(u, 100);
    UTEST(u, 128);
    UTEST(u, 0x100);
    UTEST(u, 0x1000);
    UTEST(u, 0x10000);
    UTEST(u, 0x100000);
    UTEST(u, 0x1000000);
    UTEST(u, 0x10000000);
    UTEST(u, 0x100000000);
    UTEST(u, 0x1000000000);
    UTEST(u, 0x10000000000);
    UTEST(u, 0x100000000000);
    UTEST(u, 0x1000000000000);
    UTEST(u, 0x10000000000000);
    UTEST(u, 0x100000000000000);
    UTEST(u, 0x1000000000000000);
    UTEST(u, 0x2000000000000000);
    UTEST(u, 0x4000000000000000);
    UTEST(u, 0x7fffffffffffffff);
    UTEST(u, 0x8000000000000000);
    UTEST(u, 0xffffffffffffffff);
    return status;
}

int main() {
    int status = 0;
    for (long n = -1000; n <= 1000; n++) {
        status |= test(n);
    }
    for (unsigned long n = 2; n; n += n) {
        status |= test((long)(n - 1));
        status |= test((long)n);
        status |= test((long)(n + 1));
    }
    return status;
}
