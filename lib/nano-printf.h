// --- vfprintf / snprintf: supports everything except FP ---

#ifdef NANO_LIBC_H
#define nano_FILE FILE
#define put1(c, fp)  putc(c, fp)
#else
#include <stdarg.h>
#include <stdbool.h>
typedef struct nano_FILE { size_t pos, cap, size; unsigned char *buf; } nano_FILE;
#define put1(c, fp)  ((fp->pos < fp->cap) ? fp->buf[fp->pos++] = (unsigned char)(c) : EOF)
ssize_t __fwrite(const void *p, size_t len, nano_FILE *fp) {
    size_t nw = fp->cap - fp->pos; if (nw > len) nw = len;
    memcpy(fp->buf + fp->pos, p, nw); fp->pos += nw;
    return nw;
}
#endif

char *_cvulong(char *p, unsigned long n, int shft, char cc) {
    if (!shft) while (n) { *--p = '0' + n % 10; n = n / 10; }
    else {
        const char *digits = "0123456789ABCDEF";
        unsigned long msk = (1 << shft) - 1;
        while (n) { *--p = digits[n & msk] | (cc & 0x20); n >>= shft; }
    }
    return p;
}

int vfprintf(nano_FILE *fp, const char *fmt, va_list ap) {
    size_t total = 0;
    const char *q = fmt;
    for (;;) {
        if (*fmt && *fmt != '%') { fmt++; continue; }
        int len = fmt - q;
        __fwrite(q, len, fp); total += len;
        q = fmt;
        if (!*fmt) return (int)total;
        char buf[72], *e, *s = e = buf + sizeof(buf) - 8;
        unsigned long n;
        fmt++;  // past '%'
#ifndef SMALL
        switch (*fmt) {
        case 'd':;
            int n1 = va_arg(ap, int); n = n1;  // force sign extension
            if (n1 < 0) { total++; put1('-', fp); n = -n; }
            do { *--s = '0' + n % 10; } while (n /= 10);
            total += e - s;
            while (s < e) put1(*s++, fp);
            q = ++fmt; continue;
        }
#endif
        unsigned long mask = 0xffffffff, sbit = 0;
        unsigned char cc, pref[2], sign = 0;
        bool minus = false, zero = false, val64 = false;
        int width = 0, shft = 0, zeroes = 0, plen = 0, prec = -1;
    again:;
        switch (cc = *fmt++) {
        case 'o': plen >>= 1; shft -= 1; // 3
        case 'x': case 'X':   shft += 3; // 4
        case 'b': case 'B':   shft += 1; pref[1] = cc; sign = 0; goto get_num; // 1
        case 'u': plen = 0; sign = 0; goto get_num;
        case 'd': case 'i': plen = 0; sbit = (mask >> 1) + 1;
        get_num:
            n = val64 ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
        has_num:
            if (n & sbit) { sign = '-'; n = -n; };
            if (!(n &= mask)) plen &= !prec;  // no prefix except #.0o
            s = _cvulong(e, n, shft, cc); len = e - s;
            if (prec < 0) prec = 1; else zero = false;
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
            len = (int)strnlen(s, prec);
        out_str:
            if ((width -= plen + zeroes + len) < 0) width = 0;
            total += width + plen + zeroes + len;
            if (!minus) while (width --> 0) put1(' ', fp);
            for (int i = 0; i < plen; i++) put1(pref[i], fp);
            while (zeroes --> 0) put1('0', fp);
            __fwrite(s, len, fp);
            while (width --> 0) put1(' ', fp);
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
            width = cc - '0'; while ((cc = *fmt - '0') < 10) { fmt++; width = width * 10 + cc; } goto again;
        case '*': width = va_arg(ap, int); if (width < 0) { minus = true; width = -width; } goto again;
        case '.': if (*fmt == '*') { fmt++; prec = va_arg(ap, int); goto again; }
            prec = 0; while ((cc = *fmt - '0') < 10) { fmt++; prec = prec * 10 + cc; } goto again;
        case '-': minus = true; goto again;
        case ' ': sign |= ' ';  goto again; // '+' has priority over ' '
        case '+': sign = '+';   goto again;
        case '#': *pref = '0'; plen = 2; goto again;
        //case 'm': // print the error message for errno. Print the error name or number if `#`
        //s = ((unsigned)errno < (unsigned)sys_nerr) ? sys_errlist[errno] : NULL; goto has_s;
        }
    }
}

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
