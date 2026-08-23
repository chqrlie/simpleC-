#include <stdarg.h>

int fflush(FILE *fp) {
    size_t len = fp->pos;
    unsigned char *p = fp->buf;
    if ((fp->flags & _IOWRITE) && p && len) {
        fp->pos = 0;
        while (len) {
            ssize_t wsz = write(fp->hd, p, len);
            if (wsz < 0) {
                if (p > fp->buf) memcpy(fp->buf, p, len);
                fp->pos = len;
                return -1;
            }
            p += wsz; len -= wsz;
        }
    }
    return 0;
}

int _allocbuf(FILE *fp) {
    if (!(fp->buf = malloc(fp->size))) { fp->bmode = _IONBF; return -1; }
    fp->alloc = true;
    if (fp->bmode == _IOABF) fp->bmode = isatty(fp->hd) ? _IOLBF : _IOFBF;
    fp->cap = (fp->bmode == _IOFBF) ? fp->size : 0;
    return 0;
}
// writing
int _flsbuf(int c, FILE *fp) {
    if (fp->pos < fp->size && fp->buf) {       // line buffered case
        fp->buf[fp->pos++] = (unsigned char)c;
        if (c != '\n') return (unsigned char)c;
        return fflush(fp) ? EOF : '\n';
    }
    if (!(fp->flags & _IOWRITE)) return EOF; // XXX: should potentially reallocate memory buffer
    if (fp->bmode != _IONBF) {
        if (!fp->buf) {
            if (_allocbuf(fp)) goto unbuf;
        } else if (fflush(fp)) {
            if (fp->pos >= fp->size) return EOF;
        }
        return fp->buf[fp->pos++] = (unsigned char)c;
    }
unbuf:;
    unsigned char b = (unsigned char)c;
    if (write(fp->hd, &b, 1) == 1) return b;
    return EOF;
}

// write bytes to a stream, return the number of bytes written or -1 on error
ssize_t __fwrite(const void *pv, size_t len, FILE *fp) {
    size_t nw = 0;
    const unsigned char *p = pv;
    if (fp->pos < fp->cap) {
        size_t n = fp->cap - fp->pos;
        if (n > len) n = len;
        memcpy(fp->buf + fp->pos, p, n);
        p += n; nw += n; fp->pos += n;
        len -= n;
    }
    if (fp->bmode != _IOFBF) { while (len-- && putc(*p++, fp) != EOF) nw++; return nw; }//@@@ DEBUG
    while (len) {
        if (fp->pos < fp->size && fp->buf) {
            size_t n = fp->size - fp->pos;
            if (n > len) n = len;
            memcpy(fp->buf + fp->pos, p, n);
            p += n; nw += n; fp->pos += n;
            if (!(len -= n)) { if (fp->bmode == _IOLBF && p[-1] == '\n') fflush(fp); return nw; }
        }
        if (!(fp->flags & _IOWRITE)) return -1; // XXX: should potentially reallocate memory buffer
        if (fp->bmode == _IONBF) break;
        if (!fp->buf) {
            if (_allocbuf(fp)) break;
        } else {
            if (fflush(fp)) return -1;
            if (len >= fp->size) break;
        }
    }
    while (len) {
        ssize_t wsz = write(fp->hd, p, len);
        if (wsz <= 0) break;
        p += wsz; nw += wsz; len -= wsz;
    }
    return nw;
}

char *_cvulong(char *p, unsigned long n, int shft, char cc) {
    if (!shft) while (n) { *--p = '0' + n % 10; n = n / 10; }
    else {
        const char *digits = "0123456789ABCDEF";
        unsigned long msk = (1 << shft) - 1;
        while (n) { *--p = digits[n & msk] | (cc & 0x20); n >>= shft; }
    }
    return p;
}

// --- vfprintf: supports everything except FP ---
int vfprintf(FILE *fp, const char *fmt, va_list ap) {
    size_t total = 0;
    const char *q = fmt;
    for (;;) {
        if (*fmt && *fmt != '%') { fmt++; continue; }
        int len = fmt - q;
        __fwrite(q, len, fp); total += len;
        q = fmt;
        if (!*fmt) return (int)total;
        char buf[72], *s = buf, *e = buf + sizeof(buf);
        unsigned long n, mask = 0xffffffff, sbit = 0;
        unsigned char cc, pref[2], sign = 0;
        bool minus = false, zero = false, val64 = false;
        int width = 0, shft = 0, zeroes = 0, plen = 0, prec = -1;
        fmt++;  // past '%'
    again:;
        switch (cc = *fmt++) {
        case 'o': plen >>= 1; shft -= 1; // 3
        case 'x': case 'X':   shft += 3; // 4
        case 'b': case 'B':   shft += 1; pref[1] = cc; sign = 0; goto has_num; // 1
        case 'u': plen = 0; sign = 0; goto has_num;
        case 'd': case 'i': plen = 0; sbit = (mask >> 1) + 1;
        has_num:
            n = val64 ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
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
            *buf = (char)va_arg(ap, int); len = 1; plen = 0;
            goto out_str;
        //case 'S': val64 = true; // Unix standard
        case 's':
            s = va_arg(ap, char *);
            if (!s) s = "(null)";
            len = (int)strnlen(s, prec);
        out_str:
            if ((width -= plen + zeroes + len) < 0) width = 0;
            total += width + plen + zeroes + len;
            if (!minus) while (width --> 0) putc(' ', fp);
            for (int i = 0; i < plen; i++) putc(pref[i], fp);
            while (zeroes --> 0) putc('0', fp);
            __fwrite(s, len, fp);
            while (width --> 0) putc(' ', fp);
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
    FILE f; memset(&f, 0, sizeof(f)); f.buf = (void*)buf; f.cap = f.size = size;
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(&f, fmt, ap);
    va_end(ap);
    if ((size_t)n < size) buf[n] = '\0';
    else if (size) buf[size - 1] = '\0';
    return n;
}
