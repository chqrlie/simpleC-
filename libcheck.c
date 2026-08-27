// libcheck.c — nano-libc.h, checked against glibc doing the same thing.
//
// Every line printed here is compared against gcc compiling this same source
// with <stdio.h>/<stdlib.h>/<string.h>/<ctype.h> swapped in. That makes glibc
// the reference for the formatter, which is the part most likely to be subtly
// wrong: padding interacting with a sign, zero-padding losing to a precision,
// a negative width meaning left-align.
//
// Length modifiers are written properly (%ld, %lx, %lu) even though nano_cc
// makes int and long the same size — otherwise glibc would read 32 bits where
// nano-libc reads 64 and the two would disagree for reasons that have nothing
// to do with this library.

#include "nano-libc.h"

int main(void) {
    printf("-- integers --\n");
    printf("[%d] [%d] [%d]\n", 0, 42, -42);
    printf("[%5d] [%-5d] [%05d]\n", 42, 42, 42);
    printf("[%5d] [%-5d] [%05d]\n", -42, -42, -42);
    printf("[%+d] [%+d] [% d]\n", 42, -42, 42);
    printf("[%.5d] [%8.5d] [%-8.5d]\n", 42, 42, 42);
    printf("[%ld] [%ld]\n", 2147483648L, -9223372036854775807L);

    printf("-- bases --\n");
    printf("[%x] [%X] [%o]\n", 48879, 48879, 511);
    printf("[%08x] [%#o]\n", 48879, 511);
    printf("[%03o] [%03o]\n", 7, 200);
    printf("[%lx] [%lu]\n", -1L, -1L);

    printf("-- strings and chars --\n");
    printf("[%s] [%10s] [%-10s]\n", "abc", "abc", "abc");
    printf("[%.2s] [%.10s]\n", "abcdef", "abc");
    printf("[%.*s] [%*d]\n", 4, "abcdefgh", 6, 99);
    printf("[%c] [%3c] [%-3c]\n", 'x', 'y', 'z');
    printf("[%%] [%s]\n", "done");

    printf("-- snprintf return value and truncation --\n");
    char b[8];
    int n = snprintf(b, 8, "%s-%d", "abcdef", 1234);
    printf("n=%d b=[%s]\n", n, b);
    n = snprintf(b, 8, "%d", 42);
    printf("n=%d b=[%s]\n", n, b);

    printf("-- ctype --\n");
    printf("digit  ");
    int i;
    for (i = 32; i < 127; i = i + 1) if (isdigit(i)) printf("%c", i);
    printf("\nalpha  ");
    for (i = 32; i < 127; i = i + 1) if (isalpha(i)) printf("%c", i);
    printf("\nxdigit ");
    for (i = 32; i < 127; i = i + 1) if (isxdigit(i)) printf("%c", i);
    printf("\nspace  ");
    for (i = 1; i < 40; i = i + 1) if (isspace(i)) printf("%d ", i);
    printf("\nlower  ");
    for (i = 'A'; i <= 'E'; i = i + 1) printf("%c", tolower(i));
    printf("\nupper  ");
    for (i = 'a'; i <= 'e'; i = i + 1) printf("%c", toupper(i));
    printf("\n");

    printf("-- string functions --\n");
    char buf[64];
    strcpy(buf, "hello");
    strcat(buf, ", world");
    printf("[%s] len=%ld\n", buf, strlen(buf));
    printf("cmp  %d %d %d\n", strcmp("abc","abc"), strcmp("abc","abd") < 0, strcmp("abd","abc") > 0);
    printf("ncmp %d %d\n", strncmp("abcxx","abcyy",3), strncmp("abcxx","abcyy",4) < 0);
    printf("chr  [%s] [%s]\n", strchr(buf, 'w'), strrchr(buf, 'l'));
    printf("str  [%s] %d\n", strstr(buf, "wor"), strstr(buf, "nope") == NULL);
    char ov[16];
    strcpy(ov, "0123456789");
    memmove(ov + 2, ov, 8);            // overlapping, forwards
    printf("move [%s]\n", ov);
    strcpy(ov, "0123456789");
    memmove(ov, ov + 2, 8);            // overlapping, backwards
    printf("move [%s]\n", ov);
    printf("memcmp %d %d\n", memcmp("abc","abc",3), memcmp("abc","abd",3) < 0);
    printf("atoi %d %d %d\n", atoi("123"), atoi("-45"), atoi("  7x"));

    printf("-- allocator --\n");
    char *p = (char *)malloc(32);
    strcpy(p, "first block");
    long *z = (long *)calloc(6, sizeof(long));
    printf("calloc zeroed %ld %ld %ld\n", z[0], z[3], z[5]);
    char *q = (char *)realloc(p, 64);
    strcat(q, " and more");
    printf("realloc [%s]\n", q);
    // distinct allocations must not overlap
    char *a1 = (char *)malloc(24);
    char *a2 = (char *)malloc(24);
    strcpy(a1, "AAAAAAAAAAAAAAAAAAAAAAA");
    strcpy(a2, "BBBBBBBBBBBBBBBBBBBBBBB");
    printf("distinct %d %d\n", strcmp(a1, "AAAAAAAAAAAAAAAAAAAAAAA") == 0,
                               strcmp(a2, "BBBBBBBBBBBBBBBBBBBBBBB") == 0);
    free(a1);

    printf("-- files --\n");
    FILE *f = fopen("libcheck.tmp", "w");
    if (!f) { printf("fopen for write failed\n"); return 1; }
    fputs("line one\n", f);
    fprintf(f, "line %s %d\n", "two", 2);
    fputc('x', f); fputc('y', f); fputc('\n', f);
    fwrite("raw bytes\n", 1, 10, f);
    fclose(f);

    f = fopen("libcheck.tmp", "r");
    if (!f) { printf("fopen for read failed\n"); return 1; }
    char line[64];
    while (fgets(line, 64, f)) printf("got [%s]", line);
    printf("eof=%d\n", feof(f));
    fclose(f);

    f = fopen("libcheck.tmp", "r");
    int c = fgetc(f);
    ungetc(c, f);
    int c2 = fgetc(f);
    printf("ungetc %d %d\n", c == c2, c);
    long count = 0;
    while (fgetc(f) != EOF) count = count + 1;
    printf("bytes after first = %ld\n", count);
    fclose(f);

    f = fopen("libcheck.tmp", "r");
    char blk[16];
    long got = fread(blk, 1, 8, f);
    blk[8] = 0;
    printf("fread %ld [%s]\n", got, blk);
    fclose(f);

    f = fopen("no/such/path/at/all", "r");
    printf("missing file -> %d\n", f == NULL);

    remove("libcheck.tmp");
    f = fopen("libcheck.tmp", "r");
    printf("after remove -> %d\n", f == NULL);

    printf("-- long output through one fprintf --\n");
    // longer than the 4096-byte stack buffer vfprintf formats into, so it has
    // to notice and re-format rather than truncate
    char big[5000];
    for (i = 0; i < 4999; i = i + 1) big[i] = 'A' + (i % 26);
    big[4999] = 0;
    n = printf("%s\n", big);
    printf("wrote %d\n", n);
    return 0;
}
