// casts.c — a cast binds to a unary-expression, not to what follows it.
//
// This file exists because of one bug. The parser used to read the operand of
// a cast with the full assignment-expression parser, so
//
//     (char *)b + 40
//
// became `(char *)(b + 40)`. The addition then scaled by sizeof(*b) instead of
// by one, and the answer was sizeof(*b) times too far along. It only ever
// looked right when the element size happened to be 0 or 1, which is exactly
// the common case -- `(char *)p - 16` on a `void *` is correct by accident,
// because sizeof(void) is 0 here.
//
// It surfaced in a kernel heap: kmalloc returned `(char *)block + sizeof(hdr)`
// and got a pointer 40 times too far in, so kfree could not find the header it
// had just written. Every value below is checked against gcc.

#include "nano-nolibc.h"

struct Big { long a[5]; };          // 40 bytes, so a wrong scale is obvious

long g_words[8];

int main() {
    struct Big arr[4];
    struct Big *b;
    char *base;
    long v;

    arr[0].a[0] = 65;
    arr[1].a[0] = 66;
    b = &arr[0];
    base = (char *)arr;

    printf("-- pointer casts --\n");
    // the whole point: 40 bytes on from b, not 40 elements
    printf("(char*)b + 40      = %d\n", (int)((char *)b + 40 - base));
    printf("(char*)&arr[2] - 40= %d\n", (int)((char *)(&arr[2]) - 40 - base));
    printf("(char*)(b + 1)     = %d\n", (int)((char *)(b + 1) - base));
    printf("(char*)b then [0]  = %d\n", (int)(((char *)b)[0]));
    printf("(char*)b + 40 read = %d\n", (int)(*((char *)b + 40)));

    // a cast to a wider element must scale by the WIDER size
    printf("(long*)base + 1    = %d\n", (int)((char *)((long *)base + 1) - base));
    printf("(struct Big*)base+1= %d\n", (int)((char *)((struct Big *)base + 1) - base));

    printf("-- casts in arithmetic --\n");
    v = 300;
    printf("(int)v * 2         = %d\n", (int)v * 2);
    printf("(int)v + 5         = %d\n", (int)v + 5);
    printf("(long)v / 7        = %d\n", (long)v / 7);
    printf("(long)v << 2       = %d\n", (long)v << 2);
    printf("(long)v %% 7        = %d\n", (long)v % 7);
    printf("(char)v & 255      = %d\n", (char)v & 255);
    printf("-(long)v           = %d\n", -(long)v);
    printf("(long)v > 100      = %d\n", (long)v > 100);
    printf("(long)v == 300     = %d\n", (long)v == 300);

    printf("-- casts of globals --\n");
    g_words[0] = 0x4142434445464748;
    printf("byte 0 of g_words  = %d\n", (int)(((char *)g_words)[0]));
    printf("byte 7 of g_words  = %d\n", (int)(((char *)g_words)[7]));
    printf("(char*)g_words + 8 = %d\n", (int)((char *)g_words + 8 - (char *)g_words));

    printf("-- narrowing casts --\n");
    // A cast has to CONVERT, not just relabel. (char)129 is -127, and while
    // the cast was a no-op, `(char)129 == c` was false even when c held
    // exactly that byte. It surfaced in a filesystem test comparing a byte
    // read back off disk against the expression that produced it.
    printf("(char)129          = %d\n", (char)129);
    printf("(char)255          = %d\n", (char)255);
    printf("(char)128          = %d\n", (char)128);
    printf("(char)127          = %d\n", (char)127);
    printf("(char)256          = %d\n", (char)256);
    printf("(char)257          = %d\n", (char)257);
    printf("(char)-1           = %d\n", (char)-1);
    printf("(char)0x1234567890ABCDEF = %d\n", (char)0x1234567890ABCDEF);
    printf("(char)200 & 255    = %d\n", (char)200 & 255);
    printf("(long)(char)200    = %d\n", (long)(char)200);
    {
        char cb[4];
        long i;
        i = 18;
        cb[0] = (char)((i * 7 + 3) & 255);
        printf("stored vs cast     = %d\n", cb[0] == (char)((i * 7 + 3) & 255));
        printf("stored value       = %d\n", cb[0]);
    }
    // and a cast that does NOT narrow must leave the value alone
    printf("(long)300          = %d\n", (long)300);
    printf("(int)300           = %d\n", (int)300);

    printf("-- nested casts --\n");
    printf("(long)(char*)b - (long)base = %d\n", (long)(char *)b - (long)base);
    printf("(int)(long)300 + 1 = %d\n", (int)(long)300 + 1);
    return 0;
}
