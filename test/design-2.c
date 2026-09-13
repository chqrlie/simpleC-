#include <stdio.h>

struct {
    int x, y, z;
} a = {
    1, 2, 3,
    .x = 42, 43,
    .x = 100,
};

int main() {
    printf("a = {");
    printf(" .x = %d,", a.x);
    printf(" .y = %d,", a.y);
    printf(" .z = %d,", a.z);
    printf(" };\n");
}
