#if 1
#include <stdio.h>
#else
int printf(const char *fmt, ...);
int puts(const char *s);
#endif

int main() {
    printf("Hello world\n");
}
