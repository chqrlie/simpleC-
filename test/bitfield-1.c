#include <stdio.h>

struct Foo {
    int i1 : 4, i2 : 4;
    unsigned int u1 : 4, u2 : 4;
};

int main() {
    struct Foo foo;
    foo.i1 = 1;
    foo.i2 = 2;
    foo.u1 = 3;
    foo.u2 = 4;
    printf("foo = { .i1: %d, .i2: %d, .u1: %u, .u2: %u }\n",
           foo.i1, foo.i2, foo.u1, foo.u2);
}
