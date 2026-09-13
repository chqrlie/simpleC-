#include <stdio.h>

int a[] = {
    1, 2, 3, 4, 5, 6,
    [10 ... 12] = 99,
    [11] = 0,
    [0 ... 1] = 0, 0,
    [5] = 5, 6,
};

int main() {
    int n = sizeof(a) / sizeof(*a);
    printf("int a[%d] = {", n);
    for (int i = 0; i < n; i++) printf(" %d,", a[i]);
    printf(" };\n");
}
