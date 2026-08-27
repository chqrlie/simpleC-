// typedefs.c — typedef and enum.
//
// Same discipline as initializers.c: every value printed here is checked
// against gcc compiling this same source, so a mis-scoped name or an
// auto-increment that restarts in the wrong place shows up as a diff.
//
// Deliberately no sizeof of anything containing `int` or an enum: nano_cc
// makes both 8 bytes and gcc makes them 4, which is a known and documented
// difference, not the thing under test here.

#include "nano-nolibc.h"

// --- typedef of a builtin, and a typedef of a typedef ----------------------
typedef long i64;
typedef i64  word;
typedef char *string;
typedef char line[8];              // typedef'd array type

// --- tagged struct typedef -------------------------------------------------
typedef struct Pt { i64 x; i64 y; } Pt;

// --- self-referential: the name is registered while the struct is still
//     incomplete, and the definition below fills in that same object --------
typedef struct Node Node;
struct Node { i64 v; Node *next; };

// --- anonymous struct typedef ----------------------------------------------
typedef struct { i64 a; i64 b; } Pair;

// --- enums -----------------------------------------------------------------
enum { RED, GREEN, BLUE };                     // implicit 0,1,2
enum { A = 5, B, C = 1 << 4, D, E = -3, F };   // resumes after each explicit value
enum Color { CYAN = 100, MAGENTA, YELLOW };    // tagged, usable as a type name
enum { NSLOT = 4 };                            // the classic array-size idiom

// an enumerator inside a struct, and an enum-typed member
struct Cell { enum Color hue; i64 weight; };

// --- globals typed through typedefs and sized by enumerators ---------------
Pt    g_pt      = { 3, 4 };
Pair  g_pair    = { 11, 22 };
i64   g_slots[NSLOT] = { 1, 2, 3, 4 };         // enumerator as an array length
i64   g_scaled[NSLOT * 2];                     // constant expression over one
word  g_word    = C + D;                       // 16 + 17
string g_names[3] = { "alpha", "beta", "gamma" };
line  g_line    = "abcdefg";
Node  g_tail    = { 99, 0 };
Node  g_head    = { 42, &g_tail };
enum Color g_hue = YELLOW;

// --- typedefs in parameter and return position -----------------------------
i64 pt_dot(Pt *a, Pt *b) { return a->x * b->x + a->y * b->y; }
i64 node_sum(Node *n) { i64 s = 0; while (n) { s = s + n->v; n = n->next; } return s; }

string hue_name(enum Color c) {
    switch (c) {                       // enumerators as case labels
        case CYAN:    return "cyan";
        case MAGENTA: return "magenta";
        case YELLOW:  return "yellow";
    }
    return "?";
}

int main() {
    printf("-- enum values --\n");
    printf("RED GREEN BLUE   = %d %d %d\n", RED, GREEN, BLUE);
    printf("A B C            = %d %d %d\n", A, B, C);
    printf("D E F            = %d %d %d\n", D, E, F);
    printf("CYAN MAG YELLOW  = %d %d %d\n", CYAN, MAGENTA, YELLOW);
    printf("NSLOT            = %d\n", NSLOT);

    printf("-- enums in expressions --\n");
    printf("BLUE + C         = %d\n", BLUE + C);
    printf("ternary on enum  = %d\n", GREEN < BLUE ? A : B);
    printf("enum as index    = %d\n", g_slots[GREEN]);

    printf("-- typedef'd globals --\n");
    printf("g_pt             = %d %d\n", g_pt.x, g_pt.y);
    printf("g_pair           = %d %d\n", g_pair.a, g_pair.b);
    printf("g_word (C + D)   = %d\n", g_word);
    printf("g_slots          = %d %d %d %d\n",
           g_slots[0], g_slots[1], g_slots[2], g_slots[3]);
    printf("g_scaled zeroed  = %d %d %d\n", g_scaled[0], g_scaled[4], g_scaled[7]);
    printf("g_names          = %s %s %s\n", g_names[0], g_names[1], g_names[2]);
    printf("g_line           = %s\n", g_line);
    printf("g_hue            = %d (%s)\n", g_hue, hue_name(g_hue));

    printf("-- self-referential typedef --\n");
    printf("node_sum         = %d\n", node_sum(&g_head));
    printf("g_head.next->v   = %d\n", g_head.next->v);

    printf("-- typedef'd locals --\n");
    Pt p;
    p.x = 6; p.y = 7;
    Pt q = { 2, 5 };
    printf("pt_dot(p,q)      = %d\n", pt_dot(&p, &q));

    Pair r = { 8, 9 };
    printf("local Pair       = %d %d\n", r.a + r.b, r.b - r.a);

    word w = 5;
    i64 acc = 0;
    for (i64 i = 0; i < NSLOT; i = i + 1) acc = acc + g_slots[i] * w;
    printf("acc              = %d\n", acc);

    line buf;
    buf[0] = 'h'; buf[1] = 'i'; buf[2] = 0;
    printf("line buf         = %s\n", buf);

    string s = "via a typedef'd pointer";
    printf("string s         = %s\n", s);

    Node local = { 7, &g_head };
    printf("local node chain = %d\n", node_sum(&local));

    printf("-- typedef through pointers and arrays --\n");
    Pt pts[3];
    for (i64 i = 0; i < 3; i = i + 1) { pts[i].x = i; pts[i].y = i * i; }
    printf("pts              = %d %d %d\n", pts[0].y, pts[1].y, pts[2].y);
    Pt *pp = &g_pt;
    printf("pp->x pp->y      = %d %d\n", pp->x, pp->y);

    printf("-- casts and sizeof through typedefs --\n");
    printf("sizeof(i64)      = %d\n", sizeof(i64));
    printf("sizeof(Pt)       = %d\n", sizeof(Pt));
    printf("sizeof(Pair)     = %d\n", sizeof(Pair));
    printf("sizeof(Node)     = %d\n", sizeof(Node));
    printf("sizeof(line)     = %d\n", sizeof(line));
    printf("sizeof(string)   = %d\n", sizeof(string));
    printf("cast via typedef = %d\n", (i64)('A' + 1));

    printf("-- block-scope typedef --\n");
    {
        typedef i64 counter;
        counter n = 0;
        for (counter i = 1; i <= 4; i = i + 1) n = n + i;
        printf("block typedef    = %d\n", n);
    }

    printf("-- enum in a struct --\n");
    struct Cell c;
    c.hue = MAGENTA; c.weight = 12;
    printf("cell             = %d %d (%s)\n", c.hue, c.weight, hue_name(c.hue));

    printf("-- switch over an enum --\n");
    printf("%s %s %s\n", hue_name(CYAN), hue_name(MAGENTA), hue_name(YELLOW));

    return 0;
}
