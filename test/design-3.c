#include <stdio.h>

union {
    int x, y, z;
} a = {
    1,
    .x = 2,
    .z = 3,
    .y = 4,
};

int main() {
    printf("a = {");
    printf(" .x = %d,", a.x);
    printf(" .y = %d,", a.y);
    printf(" .z = %d,", a.z);
    printf(" };\n");
}
