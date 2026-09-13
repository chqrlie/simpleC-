#include <stdio.h>

int a[] = {
    1, 2, 3,
    [10] = 99, 100, 101,
    [11] = 0,
    [0] = 0, 0,
    [5] = 5, 6,
};

int main() {
    int n = sizeof(a) / sizeof(*a);
    printf("int a[%d] = {", n);
    for (int i = 0; i < n; i++) printf(" %d,", a[i]);
    printf(" };\n");
}
