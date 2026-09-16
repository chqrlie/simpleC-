#include <stdio.h>

long idiv(long a, long b) { return a / b; }
long imod(long a, long b) { return a % b; }
long imul(long a, long b) { return a * b; }

unsigned long div(unsigned long a, unsigned long b) { return a / b; }
unsigned long mod(unsigned long a, unsigned long b) { return a % b; }
unsigned long mul(unsigned long a, unsigned long b) { return a * b; }

#define TEST(a, b, op, f, fmt, t)  \
    if (a op (b) != f(a, b)) { \
        status = printf(#t": "fmt" %s "fmt" = "fmt", expected "fmt"\n", \
                        (t)(a), #op, (t)(b), (t)(a op b), f(a, b));     \
    } \
    if (1 + (a op (b)) != 1 + f(a, b)) { \
        status = printf(#t": 1 + ("fmt" %s "fmt") = "fmt", expected "fmt"\n", \
                        (t)(a), #op, (t)(b), 1 + (t)(a op b), 1 + f(a, b));     \
    }

#define ITEST(a, b)  TEST(a, b, *, imul, "%ld", long); \
                     TEST(a, b, /, idiv, "%ld", long); \
                     TEST(a, b, %, imod, "%ld", long)
#define STEST(a, b)  ITEST(a, b); ITEST(a, -(b))
#define UTEST(a, b)  TEST(a, b, *, mul, "%lu", unsigned long); \
                     TEST(a, b, /, div, "%lu", unsigned long); \
                     TEST(a, b, %, mod, "%lu", unsigned long)

int main() {
    int status = 0;
    for (long n = -1000; n <= 1000; n++) {
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
    }
    for (unsigned long n = 0; n <= 1000; n++) {
        UTEST(n, 1);
        UTEST(n, 2);
        UTEST(n, 3);
        UTEST(n, 4);
        UTEST(n, 5);
        UTEST(n, 6);
        UTEST(n, 7);
        UTEST(n, 8);
        UTEST(n, 9);
        UTEST(n, 10);
        UTEST(n, 11);
        UTEST(n, 16);
        UTEST(n, 32);
        UTEST(n, 64);
        UTEST(n, 100);
        UTEST(n, 128);
        UTEST(n, 0x100);
        UTEST(n, 0x1000);
        UTEST(n, 0x10000);
        UTEST(n, 0x100000);
        UTEST(n, 0x1000000);
        UTEST(n, 0x10000000);
        UTEST(n, 0x100000000);
        UTEST(n, 0x1000000000);
        UTEST(n, 0x10000000000);
        UTEST(n, 0x100000000000);
        UTEST(n, 0x1000000000000);
        UTEST(n, 0x10000000000000);
        UTEST(n, 0x100000000000000);
        UTEST(n, 0x1000000000000000);
        UTEST(n, 0x2000000000000000);
        UTEST(n, 0x4000000000000000);
        UTEST(n, 0x7fffffffffffffff);
        UTEST(n, 0x8000000000000000);
        UTEST(n, 0xffffffffffffffff);
    }
    return status;
}
