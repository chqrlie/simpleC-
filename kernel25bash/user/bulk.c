// bulk.c — write a long, self-checking stream to stdout.
//
// The 86-byte `hello | cat` test proves a pipe carries bytes, but it never
// fills one: the buffer is 4096 and nothing ever waits. This program writes
// BULK_TOTAL bytes, which is several times the buffer, so the writer runs out
// of space and has to yield to the reader and then pick up where it stopped.
// That is the path that distinguishes a pipe from a queue that happened to be
// big enough, and it is also the only thing that exercises the ring buffer
// wrapping past the end of its storage.
//
// The content is not filler. Every line is numbered, so a reader can tell
// "some bytes went missing" from "the bytes arrived out of order" and from
// "a wrap dropped a chunk" -- three failures that all look like a short file
// if the payload is the same character repeated.

#include "nano-user.h"

#define BULK_LINES 2000

char g_line[64];

// The digits of n, written backwards into buf and then reversed. No printf
// here: this program's whole job is to be the thing under test, so it does
// not depend on the libc formatting that the rest of the system uses.
long put_num(char *buf, long n) {
    char tmp[16];
    long i;
    long len;
    i = 0;
    if (n == 0) { tmp[i] = '0'; i = 1; }
    while (n > 0) { tmp[i] = '0' + (n % 10); n = n / 10; i = i + 1; }
    len = i;
    i = 0;
    while (i < len) { buf[i] = tmp[len - 1 - i]; i = i + 1; }
    return len;
}

int main(int argc, char **argv) {
    long i;
    long n;

    i = 0;
    while (i < BULK_LINES) {
        n = 0;
        g_line[n] = 'l'; n = n + 1;
        g_line[n] = 'i'; n = n + 1;
        g_line[n] = 'n'; n = n + 1;
        g_line[n] = 'e'; n = n + 1;
        g_line[n] = ' '; n = n + 1;
        n = n + put_num(g_line + n, i);
        g_line[n] = '\n'; n = n + 1;
        // One write per line, so the producer crosses the buffer boundary at
        // an offset that is not a multiple of anything convenient.
        write(1, g_line, n);
        i = i + 1;
    }
    return 0;
}
