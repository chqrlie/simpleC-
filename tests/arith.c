// arith.c — multiply, divide and modulo, against gcc.
//
//   sh gcc-check.sh arith.c
//
// This exists because of `--minimal`. That mode targets an assembler with no
// imul and no idiv, so nano_cc synthesises both: multiply is a shift-add loop
// over the bits of the multiplier, divide is a restoring division that shifts
// the dividend a bit at a time. Both were made faster -- the multiply now
// negates a negative multiplier instead of walking its sign bits, and the
// divide skips the leading zeros of the dividend instead of always taking 64
// steps -- and both changes are the kind that work for every number you happen
// to try and then fail on the sign of a remainder.
//
// So the numbers are not chosen to be interesting. Every pair from a table that
// deliberately contains both signs, zero, one, powers of two, values either
// side of the 32-bit boundary, and the extremes of a long, is run through *, /
// and %, and the answer is whatever gcc says. 34 x 34 pairs, three operators.
//
// The one pair left out is LONG_MIN / -1, whose quotient is not representable;
// C calls it undefined and gcc traps on it, so there is no reference answer to
// compare against.
//
// Output goes through printf("%s") and a hand-rolled decimal conversion rather
// than %d, because nano_cc has one integer type and it is 64-bit, while gcc's
// %d is an int -- comparing those would be comparing the format strings.
#include "nano-nolibc.h"

char g_buf[64];

// Decimal, into g_buf, returning the start. Negation is done on the digits
// rather than on the value, so the most negative long -- whose absolute value
// is not a long -- comes out right instead of coming out as itself.
char *dec(long v) {
    long i;
    long neg;
    i = 63;
    g_buf[i] = 0;
    neg = 0;
    if (v < 0) neg = 1;
    if (v == 0) { i = i - 1; g_buf[i] = '0'; }
    while (v != 0) {
        long d;
        d = v % 10;
        if (d < 0) d = 0 - d;
        i = i - 1;
        g_buf[i] = '0' + d;
        v = v / 10;
    }
    if (neg) { i = i - 1; g_buf[i] = '-'; }
    return &g_buf[i];
}

void show(long v) {
    printf("%s", dec(v));
}

long vals[34];

void fill() {
    vals[0]  = 0;
    vals[1]  = 1;
    vals[2]  = 0 - 1;
    vals[3]  = 2;
    vals[4]  = 0 - 2;
    vals[5]  = 3;
    vals[6]  = 0 - 3;
    vals[7]  = 7;
    vals[8]  = 0 - 7;
    vals[9]  = 10;
    vals[10] = 0 - 10;
    vals[11] = 16;
    vals[12] = 0 - 16;
    vals[13] = 100;
    vals[14] = 0 - 100;
    vals[15] = 255;
    vals[16] = 0 - 255;
    vals[17] = 1000;
    vals[18] = 0 - 1000;
    vals[19] = 65536;
    vals[20] = 0 - 65536;
    vals[21] = 123456789;
    vals[22] = 0 - 123456789;
    vals[23] = 2147483647;                    // the 32-bit boundary, either side
    vals[24] = 0 - 2147483647;
    vals[25] = 2147483648;
    vals[26] = 0 - 2147483648;
    vals[27] = 4294967296;
    vals[28] = 0 - 4294967296;
    vals[29] = 1099511627776;                 // 2^40
    vals[30] = 0 - 1099511627776;
    vals[31] = 9223372036854775807;           // LONG_MAX
    vals[32] = 0 - 9223372036854775807;
    vals[33] = (0 - 9223372036854775807) - 1; // LONG_MIN
}

int main() {
    long i;
    long j;

    fill();

    printf("-- multiply --\n");
    i = 0;
    while (i < 34) {
        j = 0;
        while (j < 34) {
            show(vals[i]); printf(" * "); show(vals[j]); printf(" = ");
            show(vals[i] * vals[j]); printf("\n");
            j = j + 1;
        }
        i = i + 1;
    }

    printf("-- divide and modulo --\n");
    i = 0;
    while (i < 34) {
        j = 0;
        while (j < 34) {
            long skip;
            // A zero divisor is a fault under gcc, and the most negative long
            // over -1 has no representable quotient. Neither has a reference
            // answer, so neither is a test.
            skip = 0;
            if (vals[j] == 0) skip = 1;
            if (vals[i] == vals[33] && vals[j] == (0 - 1)) skip = 1;
            if (!skip) {
                show(vals[i]); printf(" / "); show(vals[j]); printf(" = ");
                show(vals[i] / vals[j]);
                printf("  rem ");
                show(vals[i] % vals[j]);
                printf("\n");
                // The identity C promises: (a/b)*b + a%b == a. It is checked
                // rather than assumed because a quotient and a remainder can
                // each look plausible while disagreeing with each other, and
                // that is exactly what a sign bug looks like.
                if ((vals[i] / vals[j]) * vals[j] + (vals[i] % vals[j]) != vals[i]) {
                    printf("  IDENTITY BROKEN\n");
                }
            }
            j = j + 1;
        }
        i = i + 1;
    }

    printf("done\n");
    exit(0);
}
