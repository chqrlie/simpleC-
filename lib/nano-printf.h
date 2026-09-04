// --- vfprintf / snprintf: supports everything except FP ---

#ifdef NANO_LIBC_H
#define nano_FILE FILE
#define put1(c, fp)  putc(c, fp)
#else
#include <stdarg.h>
#include <stdbool.h>
#pragma GCC diagnostic ignored "-Wpragmas"
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
#pragma GCC diagnostic ignored "-Wreserved-identifier"
#pragma GCC diagnostic ignored "-Wswitch-default"
#pragma GCC diagnostic ignored "-Wunsafe-buffer-usage"
#if __has_attribute(__fallthrough__)
#define fallthrough  __attribute__((__fallthrough__))
#else
#define fallthrough  do {} while (0)  /* fallthrough */
#endif
typedef struct nano_FILE { size_t pos, cap, size; unsigned char *buf; } nano_FILE;
#define put1(c, fp)  ((fp->pos < fp->cap) ? fp->buf[fp->pos++] = (unsigned char)(c) : EOF)
static ssize_t __fwrite(const void *p, size_t len, nano_FILE *fp) {
    size_t nw = fp->cap - fp->pos; if (nw > len) nw = len;
    memcpy(fp->buf + fp->pos, p, nw); fp->pos += nw;
    return (ssize_t)nw;
}
#endif

static char *_cvulong(char *p, unsigned long n, unsigned char shft, unsigned char cc) {
    if (!shft) while (n) { *--p = n % 10 + '0'; n = n / 10; }
    else {
        const char *digits = "0123456789ABCDEF";
        unsigned long msk = (1 << shft) - 1;
        while (n) { *--p = digits[n & msk] | (cc & 0x20); n >>= shft; }
    }
    return p;
}

int vfprintf(nano_FILE *fp, const char *fmt, va_list ap);
int vfprintf(nano_FILE *fp, const char *fmt, va_list ap) {
    unsigned total = 0;
    const char *q = fmt;
    for (;;) {
        if (*fmt && *fmt != '%') { fmt++; continue; }
        unsigned len = (unsigned)(fmt - q);
        __fwrite(q, len, fp); total += len;
        q = fmt;
        if (!*fmt) return (int)total;
        char buf[72], *e, *s = e = buf + sizeof(buf) - 8;
        unsigned long n;
        fmt++;  // past '%'
#ifndef SMALL
        switch (*fmt) {
        case 'd':;
            int n1 = va_arg(ap, int); n = (unsigned long)(long)n1;  // force sign extension
            if (n1 < 0) { total++; put1('-', fp); n = -n; }
            do { *--s = n % 10 + '0'; } while (n /= 10);
            total += (unsigned)(e - s);
            while (s < e) put1(*s++, fp);
            q = ++fmt; continue;
        }
#endif
        unsigned long mask = 0xffffffff, sbit = 0;
        unsigned char cc, pref[2], sign = 0, shft = 0;
        bool minus = false, zero = false, val64 = false;
        unsigned int width = 0, zeroes = 0, plen = 0, prec = -1U;
    again:;
        switch (cc = (unsigned char)*fmt++) {
        case 'o': plen >>= 1; shft -= 1; fallthrough; // 3
        case 'x': case 'X':   shft += 3; fallthrough; // 4
        case 'b': case 'B':   shft += 1; pref[1] = cc; sign = 0; goto get_num; // 1
        case 'u': plen = 0; sign = 0; goto get_num;
        case 'd': case 'i': plen = 0; sbit = (mask >> 1) + 1;
        get_num:
            n = val64 ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
        has_num:
            if (n & sbit) { sign = '-'; n = -n; }
            if (!(n &= mask)) plen &= !prec;  // no prefix except #.0o
            s = _cvulong(e, n, shft, cc); len = (unsigned)(e - s);
            if ((int)prec < 0) prec = 1; else zero = false;
            if (prec > len) { zeroes = prec - len; plen &= 2; }
            if (sign) { *pref = sign; plen = 1; }
            if (zero && !minus && width > len + plen) zeroes = width - len - plen;
            goto out_str;
        case 'n':
            s = va_arg(ap, char *);
            while (mask) { *s++ = (unsigned char)total & 255; mask >>= 8; } continue;
        //case 'C': val64 = true; // Unix standard
        case 'c':
            // could support utf-8 conversion if val64 and escape encode if #
            *s = (char)va_arg(ap, int); len = 1; plen = 0;
            goto out_str;
        //case 'S': val64 = true; // Unix standard
        case 's':
            s = va_arg(ap, char *);
            if (!s) s = "(null)";
            len = (unsigned)strnlen(s, prec);
        out_str:
            if ((int)(width -= plen + zeroes + len) < 0) width = 0;
            total += width + plen + zeroes + len;
            if (!minus) while (width) { put1(' ', fp); width--; }
            for (unsigned i = 0; i < plen; i++) put1(pref[i], fp);
            while (zeroes) { put1('0', fp); zeroes--; }
            __fwrite(s, len, fp);
            while (width) { put1(' ', fp); width--; }
            q = fmt;
            continue;
        case 'p':
            if (!(n = (unsigned long)va_arg(ap, void *))) { s = "(nil)"; len = 5; goto out_str; }
            shft = 4; *pref = '0'; pref[1] = 'x'; plen = 2; goto has_num;
        case '%':  q = fmt - 1; continue;
        case '\0': fmt--;       continue;
        case 'I': goto again; // internationalized form: ignored
        //case 'q': // 4.4 BSD extension for 'll'
        case 'l': case 'z': case 'j': case 't': val64 = true; mask = -1UL; goto again;
        case 'h': mask = 0xffff; if (*fmt == 'h') { mask = 0xff; fmt++; } goto again;
        case '0': zero = true; goto again;
        case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
            width = cc - '0'; while ((cc = (unsigned char)(*fmt - '0')) < 10) { fmt++; width = width * 10 + cc; } goto again;
        case '*': width = (unsigned)va_arg(ap, int); if ((int)width < 0) { minus = true; width = -width; } goto again;
        case '.': if (*fmt == '*') { fmt++; prec = (unsigned)va_arg(ap, int); goto again; }
            prec = 0; while ((cc = (unsigned char)(*fmt - '0')) < 10) { fmt++; prec = prec * 10 + cc; } goto again;
        case '-': minus = true; goto again;
        case ' ': sign |= ' ';  goto again; // '+' has priority over ' '
        case '+': sign = '+';   goto again;
        case '#': *pref = '0'; plen = 2; goto again;
        //case 'm': // print the error message for errno. Print the error name or number if `#`
        //s = ((unsigned)errno < (unsigned)sys_nerr) ? sys_errlist[errno] : NULL; goto has_s;
        }
    }
}

int snprintf(char *buf, size_t size, const char *fmt, ...) attr_printf(3, 4);
int snprintf(char *buf, size_t size, const char *fmt, ...) {
    nano_FILE f; memset(&f, 0, sizeof(f)); f.buf = (void*)buf; f.cap = f.size = size;
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(&f, fmt, ap);
    va_end(ap);
    if ((size_t)n < size) buf[n] = '\0';
    else if (size) buf[size - 1] = '\0';
    return n;
}

#undef nano_FILE
#undef put1
