// nano-nolibc.h — works with nano_cc's simple __asm__ passthrough
#ifndef NANO_NOLIBC_H
#define NANO_NOLIBC_H

// The syscall layer, the string/memory routines and the stdarg macros now live
// in nano-base.h, shared with nano-libc.h. Everything below is what makes this
// header the SMALL one: a bare puts, an integer printer, and a printf that
// handles %d %x %c %s and %%.
//
// If you want a real C library — FILE, fopen, malloc, a full formatter —
// include nano-libc.h INSTEAD of this file. Note that this puts() does not
// append a newline and C's does; nano-libc.h follows C.
#include "nano-base.h"

void puts(const char *s) { write(1, s, strlen(s)); }

void print_int(long n) {
    char buf[24]; int i = 22; buf[23] = 0;
    if (n < 0) { write(1, "-", 1); n = -n; }
    if (n == 0) { write(1, "0", 1); return; }
    while (n > 0 && i > 0) { buf[i--] = '0' + (n % 10); n /= 10; }
    write(1, &buf[i+1], 22 - i);
}

// --- stdarg, on top of nano_cc's variadic built-ins ---
#define va_list         long
#define va_start(ap, l) __builtin_va_start(ap)
#define va_arg(ap, t)   __builtin_va_arg(ap)
#define va_end(ap)      __builtin_va_end(ap)

// --- printf: supports %d %x %c %s and %% ---
void _putc(char c) { write(1, &c, 1); }

void _put_uint(long n, int base) {
    char buf[32]; int i; char *digits;
    digits = "0123456789abcdef";
    if (n == 0) { _putc('0'); return; }
    i = 0;
    while (n > 0) { buf[i] = digits[n % base]; i = i + 1; n = n / base; }
    while (i > 0) { i = i - 1; _putc(buf[i]); }
}

void printf(char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    while (*fmt) {
        if (*fmt == '%') {
            fmt = fmt + 1;
            if (*fmt == 'd') {
                long v; v = va_arg(ap, int);
                if (v < 0) { _putc('-'); v = -v; }
                _put_uint(v, 10);
            } else if (*fmt == 'x') {
                _put_uint(va_arg(ap, int), 16);
            } else if (*fmt == 's') {
                char *s; s = (char *)va_arg(ap, char *);
                puts(s);
            } else if (*fmt == 'c') {
                _putc(va_arg(ap, int));
            } else if (*fmt == '%') {
                _putc('%');
            } else {
                _putc('%'); _putc(*fmt);
            }
        } else {
            _putc(*fmt);
        }
        fmt = fmt + 1;
    }
    va_end(ap);
}

#endif