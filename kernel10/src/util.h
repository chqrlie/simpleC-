// util.h — a header, so that compiling demo.c inside the OS has to open a
// second file through the syscall boundary rather than just the one it was
// handed on the command line.
//
// It is included as "util.h", not "/src/util.h", on purpose. The compiler opens
// that name verbatim, so it only resolves if the process has a working
// directory -- and it has to resolve to the same file on Linux and in nano-os,
// or the two outputs could not be compared.

#ifndef UTIL_H
#define UTIL_H

#define SQUARE(x)  ((x) * (x))
#define MAX(a, b)  ((a) > (b) ? (a) : (b))

struct Point { long x; long y; };

long dot(struct Point *a, struct Point *b) {
    return a->x * b->x + a->y * b->y;
}

#endif
