#include <stdio.h>

struct Foo {
    int x, y, z;
    const char *name;
};

void print_foo(struct Foo *p) {
    printf("{ %d, %d, %d, \"%s\" }\n", p->x, p->y, p->z, p->name);
}

struct Foo foo = { 1, 2, 3, "hello" };

int main() {
    struct Foo bar = { 4, 5, 6, "world" };
    print_foo(&foo);
    print_foo(&bar);
    struct Foo *p = &(struct Foo){ 7, 8, 9, "direct" };
    print_foo(p);
    print_foo(&(struct Foo){ 10, 11, 12, "inline" });
}
