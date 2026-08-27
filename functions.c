// functions.c — array parameters and function return types.
//
// Both of these were silently wrong before rather than rejected, which is why
// they get their own checked file:
//
//   * `long m[4][3]` as a PARAMETER is `long (*m)[3]`. Only the outermost
//     dimension decays; the inner one has to survive or m[r] would not know
//     how far to step.
//   * Every call used to be typed `long` regardless of what the function
//     returned, so `p->field` on a call result had nothing to look in and
//     pointer arithmetic on a `char *` result stepped 8 bytes instead of 1.
//
// Checked against gcc compiling this same source.

#include "nano-nolibc.h"

struct Item { long id; char *tag; };

// --- array parameters ------------------------------------------------------
long row_sum(long m[4][3], long r) {
    long s = 0, j;
    for (j = 0; j < 3; j = j + 1) s = s + m[r][j];
    return s;
}
long cell(long m[][3], long r, long c) { return m[r][c]; }   // omitted first dim
long total(long a[], long n) {
    long s = 0, i;
    for (i = 0; i < n; i = i + 1) s = s + a[i];
    return s;
}
void show3(char names[3][8]) { printf("%s|%s|%s\n", names[0], names[1], names[2]); }

// --- return types ----------------------------------------------------------
char *pick_name(long i) {
    if (i == 0) return "alpha";
    if (i == 1) return "beta";
    return "gamma";
}
struct Item g_items[3];
struct Item *item_at(long i) { return &g_items[i]; }
long *slot_of(long a[], long i) { return &a[i]; }
char first_char(char *s) { return s[0]; }

int main() {
    long m[4][3] = { {1,2,3}, {4,5,6}, {7,8,9}, {10,11,12} };
    printf("row_sum   = %d %d %d %d\n", row_sum(m,0), row_sum(m,1), row_sum(m,2), row_sum(m,3));
    printf("cell      = %d %d %d\n", cell(m,0,0), cell(m,2,1), cell(m,3,2));

    long a[5] = { 2, 4, 6, 8, 10 };
    printf("total     = %d\n", total(a, 5));

    char names[3][8] = { "ann", "bob", "cid" };
    show3(names);

    // a char* return, then indexed and stepped as a char pointer
    printf("names     = %s %s %s\n", pick_name(0), pick_name(1), pick_name(2));
    printf("2nd chars = %c %c %c\n", pick_name(0)[1], pick_name(1)[1], pick_name(2)[1]);
    printf("stepped   = %s\n", pick_name(2) + 2);

    // a struct* return, then dereferenced through -> straight off the call
    g_items[0].id = 10; g_items[0].tag = "ten";
    g_items[1].id = 20; g_items[1].tag = "twenty";
    g_items[2].id = 30; g_items[2].tag = "thirty";
    printf("items     = %d %s\n", item_at(1)->id, item_at(1)->tag);
    printf("items     = %d %s\n", item_at(2)->id, item_at(2)->tag);

    // a long* return, written through
    *slot_of(a, 3) = 99;
    printf("after set = %d %d\n", a[3], total(a, 5));

    printf("first     = %c %c\n", first_char("Xylophone"), first_char("quay"));
    return 0;
}
