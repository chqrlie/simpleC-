#include <stdio.h>

int main() {
    int a = 0, b = 0;
    *(1 ? &a : &b) = 1;
    *(0 ? &a : &b) = 2;
    printf("a = %d, b = %d\n", a, b);
}
