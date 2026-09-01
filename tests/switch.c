// switch.c — demonstrates the switch / case / default statement.
//
//   ./nano_cc switch.c switch.s
//   gcc -nostdlib -no-pie switch.s -o switch_prog
//   ./switch_prog
//
// Shows: basic dispatch, the default arm, intentional fall-through (no break),
// break, a switch nested inside another switch, and a switch inside a loop
// (where break leaves the switch but continue belongs to the loop).
#include "nano-nolibc.h"

// Classic dispatch: one arm per value, plus a default.
char *day_name(int d) {
    switch (d) {
    case 1: return "Mon";
    case 2: return "Tue";
    case 3: return "Wed";
    case 4: return "Thu";
    case 5: return "Fri";
    default: return "weekend";
    }
}

// Fall-through: 1 and 2 share the "few" arm because 1 has no break.
char *bucket(int n) {
    char *r;
    r = "many";
    switch (n) {
    case 0:
        r = "none";
        break;
    case 1:
    case 2:
        r = "few";
        break;
    case 3:
        r = "some";
        break;
    }
    return r;
}

int main() {
    printf("day 1 = %s\n", day_name(1));
    printf("day 5 = %s\n", day_name(5));
    printf("day 9 = %s\n", day_name(9));

    printf("bucket 0 = %s\n", bucket(0));
    printf("bucket 1 = %s\n", bucket(1));   // falls through to case 2
    printf("bucket 2 = %s\n", bucket(2));
    printf("bucket 3 = %s\n", bucket(3));
    printf("bucket 7 = %s\n", bucket(7));   // no case, no default -> "many"

    // switch inside a loop: break ends the switch, the loop keeps going;
    // continue belongs to the loop and skips the tail print.
    int i;
    int total;
    total = 0;
    for (i = 0; i < 6; i++) {
        switch (i) {
        case 2:
            continue;            // skip i == 2 entirely
        case 4:
            total = total + 100; // and don't add the +1 tail for i == 4
            break;
        default:
            total = total + 1;
            break;
        }
        total = total + 1;       // reached for every i except 2 (continue) ...
    }                            // ... note i==4 breaks the switch then runs this
    printf("loop total = %d\n", total);

    // nested switch: the inner cases keep their own labels.
    int a;
    int b;
    a = 1;
    b = 2;
    switch (a) {
    case 1:
        switch (b) {
        case 2:
            printf("nested a=1 b=2\n");
            break;
        default:
            printf("nested a=1 b=other\n");
            break;
        }
        break;
    default:
        printf("nested a=other\n");
        break;
    }
    return 0;
}
