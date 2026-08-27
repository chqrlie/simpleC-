// initializers.c — brace initialisers, local and global, scalar and aggregate.
//
// Every value printed here is checked against what gcc produces for the same
// source, so a wrong offset or a missed zero-fill shows up as a diff rather
// than as a plausible-looking number.

#include "nano-nolibc.h"

struct Point { long x; long y; };
struct Line  { struct Point a; struct Point b; long tag; };
// char pad[3] is deliberate: an array must align like its ELEMENT, so pad
// occupies 3 bytes and z still lands on an 8-byte boundary after it.
struct Box   { char tag[8]; long n; long grid[2][2]; char pad[3]; long z; };

// --- globals ---------------------------------------------------------------
long  g_scalar          = 7;
long  g_expr            = 3 * 4 + 2;          // folded at compile time
long  g_arr[4]          = { 10, 20, 30, 40 };
long  g_partial[5]      = { 1, 2 };           // rest must be zero
long  g_inferred[]      = { 5, 6, 7 };        // length from the initialiser
char  g_text[8]         = "abc";              // rest must be zero
char  g_inferred_text[] = "hello";
char *g_ptr             = "pointed-to";
char *g_ptrs[3]         = { "one", "two", "three" };
struct Point g_pt       = { 11, 22 };
struct Line  g_line     = { { 1, 2 }, { 3, 4 }, 99 };
long  g_nested[2][3]    = { { 1, 2, 3 }, { 4, 5, 6 } };
long  g_zero[3]         = { 0 };
long  g_uninit[3];                            // stays in .bss
struct Box g_box        = { "hi", 5, { {1,2},{3,4} }, "ab", 77 };

static long sum(long *p, long n) {
    long t = 0;
    long i = 0;
    while (i < n) { t = t + p[i]; i = i + 1; }
    return t;
}

int main(void) {
    printf("-- globals --\n");
    printf("scalar   %d\n", g_scalar);
    printf("expr     %d\n", g_expr);
    printf("arr      %d %d %d %d  sum=%d\n",
           g_arr[0], g_arr[1], g_arr[2], g_arr[3], sum(g_arr, 4));
    printf("partial  %d %d %d %d %d\n",
           g_partial[0], g_partial[1], g_partial[2], g_partial[3], g_partial[4]);
    printf("inferred %d %d %d  sum=%d\n",
           g_inferred[0], g_inferred[1], g_inferred[2], sum(g_inferred, 3));
    printf("text     [%s] pad=%d %d\n", g_text, g_text[3], g_text[7]);
    printf("inftext  [%s]\n", g_inferred_text);
    printf("ptr      [%s]\n", g_ptr);
    printf("ptrs     [%s] [%s] [%s]\n", g_ptrs[0], g_ptrs[1], g_ptrs[2]);
    printf("point    %d %d\n", g_pt.x, g_pt.y);
    printf("line     %d %d %d %d tag=%d\n",
           g_line.a.x, g_line.a.y, g_line.b.x, g_line.b.y, g_line.tag);
    printf("nested   %d %d %d / ", g_nested[0][0], g_nested[0][1], g_nested[0][2]);
    printf("%d %d %d\n",          g_nested[1][0], g_nested[1][1], g_nested[1][2]);
    printf("zero     %d %d %d\n", g_zero[0], g_zero[1], g_zero[2]);
    printf("uninit   %d %d %d\n", g_uninit[0], g_uninit[1], g_uninit[2]);
    printf("box      [%s] %d %d %d\n", g_box.tag, g_box.n, g_box.grid[0][0], g_box.grid[1][1]);
    printf("box      [%s] %d\n", g_box.pad, g_box.z);

    printf("-- locals --\n");
    long l_arr[4] = { 10, 20, 30, 40 };
    long l_partial[5] = { 1, 2 };
    long l_inferred[] = { 5, 6, 7 };
    char l_text[8] = "abc";
    char l_inferred_text[] = "hello";
    char *l_ptr = "local-pointed-to";
    char *l_ptrs[3] = { "un", "deux", "trois" };
    struct Point l_pt = { 11, 22 };
    struct Line l_line = { { 1, 2 }, { 3, 4 }, 99 };
    long l_nested[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
    char l_big[200] = { 0 };            // large enough to take the loop path
    long l_scalar = 7;

    printf("arr      %d %d %d %d  sum=%d\n",
           l_arr[0], l_arr[1], l_arr[2], l_arr[3], sum(l_arr, 4));
    printf("partial  %d %d %d %d %d\n",
           l_partial[0], l_partial[1], l_partial[2], l_partial[3], l_partial[4]);
    printf("inferred %d %d %d  sum=%d\n",
           l_inferred[0], l_inferred[1], l_inferred[2], sum(l_inferred, 3));
    printf("text     [%s] pad=%d %d\n", l_text, l_text[3], l_text[7]);
    printf("inftext  [%s]\n", l_inferred_text);
    printf("ptr      [%s]\n", l_ptr);
    printf("ptrs     [%s] [%s] [%s]\n", l_ptrs[0], l_ptrs[1], l_ptrs[2]);
    printf("point    %d %d\n", l_pt.x, l_pt.y);
    printf("line     %d %d %d %d tag=%d\n",
           l_line.a.x, l_line.a.y, l_line.b.x, l_line.b.y, l_line.tag);
    printf("nested   %d %d %d / ", l_nested[0][0], l_nested[0][1], l_nested[0][2]);
    printf("%d %d %d\n",          l_nested[1][0], l_nested[1][1], l_nested[1][2]);
    printf("scalar   %d\n", l_scalar);
    struct Box l_box = { "yo", 6, { {7,8},{9,10} }, "cd", 88 };
    printf("box      [%s] %d %d %d\n", l_box.tag, l_box.n, l_box.grid[0][0], l_box.grid[1][1]);
    printf("box      [%s] %d\n", l_box.pad, l_box.z);

    long big_nonzero = 0;
    long i = 0;
    while (i < 200) { big_nonzero = big_nonzero + l_big[i]; i = i + 1; }
    printf("big      all-zero=%d\n", big_nonzero == 0);

    // writing through them must still work
    l_arr[2] = 99;
    g_arr[2] = 88;
    printf("stores   %d %d\n", l_arr[2], g_arr[2]);
    return 0;
}
