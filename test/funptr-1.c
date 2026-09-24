#include <stdio.h>

static int one(void) { return 1; }
static int two(void) { return 2; }
static int three(void) { return 3; }
int four(void) { return 4; }
#if 0
static int (*f1)(void) = one;
static void test_f1(void) {
    //printf("one() = %d\n", one());
    printf("f1() = %d\n", f1());
    //printf("(*f1)() = %d\n", (*f1)());
}
#else
#define test_f1()
#endif

static void test_f2(void) {
    int (*f2)(void) = two;
    printf("f2() = %d\n", f2());
    //printf("(*f2)() = %d\n", (*f2)());
}

static void test_f2a(void) {
    int (*f2a[3])(void) = { one, two, three };
    printf("f2a[0]() = %d\n", f2a[0]());
    printf("f2a[1]() = %d\n", f2a[1]());
    printf("f2a[2]() = %d\n", f2a[2]());
}

static void test_f2b(void) {
    struct {
        int (*a)(void);
        int (*b)(void);
        int (*c)(void);
    } f2b = { one, two, three }, *p2b = &f2b;
    printf("f2b.a() = %d\n", f2b.a());
    printf("f2b.b() = %d\n", f2b.b());
    printf("f2b.c() = %d\n", f2b.c());
    printf("p2b->a() = %d\n", p2b->a());
    printf("p2b->b() = %d\n", p2b->b());
    printf("p2b->c() = %d\n", p2b->c());
}

#if 0
static void test_f3(void) {
    static int (*f3)(void) = three;
    printf("f3() = %d\n", f3());
    //printf("(*f3)() = %d\n", (*f3)());
}
#else
#define test_f3()
#endif

int main(void) {
    test_f1();
    test_f2();
    test_f2a();
    test_f2b();
    test_f3();
}
