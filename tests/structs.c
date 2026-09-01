// structs.c — exercises the M2 additions:
//   struct + union definitions, member access (. and ->), nested structs,
//   sizeof, and returning a string (char*) from a function.
//
//   ./nano_cc structs.c structs.s
//   gcc -nostdlib -no-pie structs.s -o structs_prog
//   ./structs_prog
#include "nano-nolibc.h"

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
    puts("p.x + p.y = "); print_int(p.x + p.y); puts("\n");    // 7

    struct Rect r;
    r.tl.x = 0; r.tl.y = 0;
    r.br.x = 5; r.br.y = 4;
    puts("area      = "); print_int(area(&r)); puts("\n");     // 20

    union Box b;
    b.l = 258;                                                  // 0x102
    puts("union .i  = "); print_int(b.i);
    puts(", .c = ");      print_int(b.c); puts("\n");           // 258, 2

    puts("sizeof(struct Rect) = "); print_int(sizeof(struct Rect)); puts("\n");

    puts("string return: ");
    puts(label(1)); puts(" / "); puts(label(0)); puts("\n");   // yes / no

    // Whole-struct assignment copies the WHOLE struct.
    //
    // This used to emit one eight-byte move whatever the size, so a copy of a
    // three-field struct took the first field and left the other two holding
    // whatever was there before -- silently, because a single `mov` is
    // perfectly valid code for the wrong amount of data. Every field is
    // printed here, and the destination is pre-filled with a sentinel so that
    // a field which is merely NOT COPIED is distinguishable from one that is
    // copied correctly by luck.
    {
        struct Rect src;
        struct Rect dst;
        struct Rect *sp;
        src.tl.x = 11; src.tl.y = 22; src.br.x = 33; src.br.y = 44;
        dst.tl.x = -1; dst.tl.y = -1; dst.br.x = -1; dst.br.y = -1;
        dst = src;
        puts("struct copy   = ");
        print_int(dst.tl.x); puts(","); print_int(dst.tl.y); puts(",");
        print_int(dst.br.x); puts(","); print_int(dst.br.y); puts("\n");

        // ...and through a pointer, which is the form that decays.
        dst.tl.x = -1; dst.tl.y = -1; dst.br.x = -1; dst.br.y = -1;
        sp = &src;
        dst = *sp;
        puts("copy via *p   = ");
        print_int(dst.tl.x); puts(","); print_int(dst.tl.y); puts(",");
        print_int(dst.br.x); puts(","); print_int(dst.br.y); puts("\n");

        // A nested member, so the size used is the MEMBER's and not the outer
        // struct's -- copying 32 bytes into an 16-byte member would run off
        // the end of it.
        src.tl.x = 7; src.tl.y = 8;
        dst.br = src.tl;
        puts("member copy   = ");
        print_int(dst.br.x); puts(","); print_int(dst.br.y); puts("\n");
    }

    exit(0);
    return 0;
}
