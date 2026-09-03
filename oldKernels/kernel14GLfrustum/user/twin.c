// twin.c — the isolation test, run as two processes at once.
//
// Both copies are the same binary, so g_pattern is at the SAME virtual address
// in both. Each fills it with a different byte and then keeps checking it while
// the scheduler moves back and forth between them.
//
// If the two processes shared an address space, the second one to start would
// overwrite the first one's buffer and both would report thousands of wrong
// bytes. Zero wrong, from both, at the same address, is the whole claim.
//
// The counting is deliberate too: it reports how many bytes were wrong rather
// than "ok", because a test that can only say ok cannot distinguish "isolated"
// from "never ran".

#include "nano-user.h"

#define N 4096

char g_pattern[N];

int main(int argc, char **argv) {
    long id;
    long i;
    long round;
    long bad;
    char mine;

    id = argc > 1 ? uatol(argv[1]) : 1;

    // 'A' for the first twin, 'B' for the second: visible in a memory dump,
    // and different enough that a partial overwrite still shows up.
    mine = (char)(64 + id);

    i = 0;
    while (i < N) { g_pattern[i] = mine; i = i + 1; }

    printf("twin %d: pid %d, buffer at 0x%x, filled with '%c'\n",
           id, getpid(), (long)g_pattern, mine);

    bad = 0;
    round = 0;
    while (round < 30) {
        i = 0;
        while (i < N) {
            if (g_pattern[i] != mine) bad = bad + 1;
            i = i + 1;
        }
        // Give the other twin the CPU while our buffer sits there. Without
        // this the two might never actually overlap in time and the test would
        // pass by accident.
        yield();
        round = round + 1;
    }

    printf("twin %d: %d of %d bytes wrong after %d rounds\n",
           id, bad, N * round, round);
    return bad;
}
