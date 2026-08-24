// structs.c — exercises the M2 additions:
//   struct + union definitions, member access (. and ->), nested structs,
//   sizeof, and returning a string (char*) from a function.
//
//   ./nano_cc structs.c structs.s
//   gcc -nostdlib -no-pie structs.s -o structs_prog
//   ./structs_prog
#include <nano-nolibc.h>

struct Point { int x; int y; };
struct Rect  { struct Point tl; struct Point br; };   // nested struct
union  Box   { long l; int i; char c; };              // overlapping members

// a function that returns a string (char*)
char *label(int ok) {
    return ok ? "yes" : "no";
}

int area(struct Rect *r) {                            // access via ->
    int w; int h;
    w = r->br.x - r->tl.x;
    h = r->br.y - r->tl.y;
    return w * h;
}

int main() {
    struct Point p;
    p.x = 3; p.y = 4;
    _puts("p.x + p.y = "); _print_int(p.x + p.y); _puts("\n");    // 7

    struct Rect r;
    r.tl.x = 0; r.tl.y = 0;
    r.br.x = 5; r.br.y = 4;
    _puts("area      = "); _print_int(area(&r)); _puts("\n");     // 20

    union Box b;
    b.l = 258;                                                  // 0x102
    _puts("union .i  = "); _print_int(b.i);
    _puts(", .c = ");      _print_int(b.c); _puts("\n");           // 258, 2

    _puts("sizeof(struct Rect) = "); _print_int(sizeof(struct Rect)); _puts("\n");

    _puts("string return: ");
    _puts(label(1)); _puts(" / "); _puts(label(0)); _puts("\n");   // yes / no

    return 0;
}
