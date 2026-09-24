#include <stdio.h>

#define TEST(x)  printf("sizeof " #x " = %zu\n", x)

int main() {
    int t1[20];
    int (*t2)[20];

    TEST(sizeof(t1));
    TEST(sizeof(*t1));
    TEST(sizeof(t2));
    TEST(sizeof(*t2));
    TEST(sizeof(**t2));
}
