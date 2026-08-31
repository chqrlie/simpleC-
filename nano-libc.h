// nano-libc.h — a freestanding C library for nano_cc, on raw Linux syscalls.
//
// This is the library half of self-hosting. simpleC++.c is written against a
// hosted <stdio.h>/<stdlib.h>/<string.h>/<ctype.h>/<stdarg.h>; this file
// provides that surface with no glibc underneath, and is itself written in the
// subset nano_cc compiles.
//
// Include this INSTEAD of nano-nolibc.h — both define printf and puts, and
// nano-nolibc.h's puts deliberately does not append a newline. This one
// follows C.
//
// Design notes, because two of these are the kind of thing that bites later:
//
//   * OUTPUT IS UNBUFFERED between calls. Each fputs/fputc/fprintf issues one
//     write(). That costs a syscall per call — ~90k for a full compiler run,
//     about a tenth of a second — and in exchange there is no flush ordering
//     to get wrong and no way to lose output on an abrupt exit. Input IS
//     buffered, because reading a source file a byte at a time is a syscall
//     per byte and that genuinely matters.
//
//   * free() is a no-op and the allocator only ever moves forward. A compiler
//     allocates a parse tree and exits; reclaiming it buys nothing. Each block
//     does carry its size, so realloc() is real. Say so rather than pretending
//     otherwise: this allocator is wrong for a long-running program.
//
//   * There is no unsigned type in nano_cc, so %u and %x do their own
//     unsigned division and shifting. See _udiv10.

#ifndef NANO_LIBC_H
#define NANO_LIBC_H

#include "nano-base.h"

typedef long size_t;

// =====================================================================
// stdlib: a bump allocator on brk
// =====================================================================
// Every block is preceded by a 16-byte header holding its usable size, which
// keeps the payload 16-byte aligned and lets realloc know how much to copy.

long _heap_ptr = 0;      // next free byte
long _heap_end = 0;      // current break

long _brk(long addr) { return syscall1(SYS_brk, addr); }

void *malloc(long n) {
    if (n < 1) n = 1;
    n = (n + 15) & (0 - 16);
    long need = n + 16;
    if (_heap_ptr == 0) { _heap_ptr = _brk(0); _heap_end = _heap_ptr; }
    if (_heap_ptr + need > _heap_end) {
        long want = _heap_ptr + need + 4194304;      // grow 4 MiB at a time
        long got = _brk(want);
        if (got < want) return NULL;                 // brk returns the new break
        _heap_end = got;
    }
    long p = _heap_ptr;
    _heap_ptr = _heap_ptr + need;
    long *hdr = (long *)p;
    hdr[0] = n;
    return (void *)(p + 16);
}

void free(void *p) { }        // deliberately nothing; see the header comment

long _blocksize(void *p) {
    long *hdr = (long *)((char *)p - 16);
    return hdr[0];
}

void *calloc(long count, long size) {
    long n = count * size;
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void *realloc(void *p, long n) {
    if (!p) return malloc(n);
    long old = _blocksize(p);
    if (n <= old) return p;
    void *q = malloc(n);
    if (!q) return NULL;
    memcpy(q, p, old);
    return q;
}

int abs(int v) { return v < 0 ? 0 - v : v; }

long atol(const char *s) {
    long sign = 1, v = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return sign * v;
}
int atoi(const char *s) { return (int)atol(s); }

// =====================================================================
// ctype
// =====================================================================
int isdigit(int c)  { return c >= '0' && c <= '9'; }
int islower(int c)  { return c >= 'a' && c <= 'z'; }
int isupper(int c)  { return c >= 'A' && c <= 'Z'; }
int isalpha(int c)  { return islower(c) || isupper(c); }
int isalnum(int c)  { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c)  { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 11 || c == 12; }
int isprint(int c)  { return c >= 32 && c < 127; }
int ispunct(int c)  { return isprint(c) && c != ' ' && !isalnum(c); }
int tolower(int c)  { return isupper(c) ? c + 32 : c; }
int toupper(int c)  { return islower(c) ? c - 32 : c; }

// =====================================================================
// stdio
// =====================================================================
// Only reading is buffered. `buf` is the read-ahead window; `bpos`/`blen`
// index into it. `back` holds a single pushed-back character for ungetc.

#define _NANO_BUFSZ 8192

struct _NanoFile {
    long fd;
    long is_eof;
    long is_err;
    long readable;
    long bpos;
    long blen;
    long back;              // -1 when empty
    char *buf;              // read window, allocated on the first fgetc
};
typedef struct _NanoFile FILE;

// The buffer is a pointer rather than an inline array so these three globals
// stay 64 bytes each. An inline array would put 8 KB of explicit zero bytes in
// the object file for every one of them.
FILE _f_stdin  = { 0, 0, 0, 1, 0, 0, -1, 0 };
FILE _f_stdout = { 1, 0, 0, 0, 0, 0, -1, 0 };
FILE _f_stderr = { 2, 0, 0, 0, 0, 0, -1, 0 };

FILE *stdin  = &_f_stdin;
FILE *stdout = &_f_stdout;
FILE *stderr = &_f_stderr;

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

FILE *fopen(const char *path, const char *mode) {
    int flags = O_RDONLY;
    int readable = 1;
    if (mode[0] == 'w') { flags = O_WRONLY | O_CREAT | O_TRUNC;  readable = 0; }
    else if (mode[0] == 'a') { flags = O_WRONLY | O_CREAT | O_APPEND; readable = 0; }
    if (mode[0] != 'r' && mode[1] == '+') { flags = O_RDWR | O_CREAT | O_TRUNC; readable = 1; }
    else if (mode[0] == 'r' && mode[1] == '+') { flags = O_RDWR; readable = 1; }

    int fd;
    if (flags & O_CREAT) fd = open(path, flags, 420);   // 0644
    else                 fd = open(path, flags);
    if (fd < 0) return NULL;

    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) { close(fd); return NULL; }
    f->fd = fd; f->is_eof = 0; f->is_err = 0; f->readable = readable;
    f->bpos = 0; f->blen = 0; f->back = -1; f->buf = NULL;
    return f;
}

int fclose(FILE *f) {
    if (!f) return EOF;
    int r = close((int)f->fd);
    f->fd = -1;
    return r < 0 ? EOF : 0;
}

long fwrite(const void *p, long size, long count, FILE *f) {
    long n = size * count;
    if (n <= 0) return 0;
    if (write((int)f->fd, (const char *)p, n) < 0) { f->is_err = 1; return 0; }
    return count;
}

int fputs(const char *s, FILE *f) {
    long n = strlen(s);
    if (n && write((int)f->fd, s, n) < 0) { f->is_err = 1; return EOF; }
    return 0;
}

int fputc(int c, FILE *f) {
    char b = (char)c;
    if (write((int)f->fd, &b, 1) < 0) { f->is_err = 1; return EOF; }
    return c & 255;
}
int putc(int c, FILE *f)  { return fputc(c, f); }
int putchar(int c)        { return fputc(c, stdout); }

// C's puts appends a newline. nano-nolibc.h's deliberately does not — that is
// the one place the two headers disagree, so it is worth being loud about.
int puts(const char *s) {
    if (fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout);
}

int fgetc(FILE *f) {
    if (f->back >= 0) { int c = (int)f->back; f->back = -1; return c; }
    if (f->bpos >= f->blen) {
        if (!f->buf) { f->buf = (char *)malloc(_NANO_BUFSZ); if (!f->buf) return EOF; }
        long n = read((int)f->fd, f->buf, _NANO_BUFSZ);
        if (n <= 0) { if (n < 0) f->is_err = 1; f->is_eof = 1; return EOF; }
        f->blen = n; f->bpos = 0;
    }
    int c = f->buf[f->bpos] & 255;
    f->bpos = f->bpos + 1;
    return c;
}
int getc(FILE *f)   { return fgetc(f); }
int getchar(void)   { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
    if (c == EOF) return EOF;
    f->back = c & 255;
    f->is_eof = 0;
    return c & 255;
}

long fread(void *p, long size, long count, FILE *f) {
    char *d = (char *)p;
    long want = size * count, got = 0;
    while (got < want) {
        int c = fgetc(f);
        if (c == EOF) break;
        d[got] = (char)c;
        got = got + 1;
    }
    return size ? got / size : 0;
}

char *fgets(char *dst, int cap, FILE *f) {
    int i = 0;
    if (cap <= 0) return NULL;
    while (i < cap - 1) {
        int c = fgetc(f);
        if (c == EOF) { if (i == 0) return NULL; break; }
        dst[i] = (char)c; i = i + 1;
        if (c == '\n') break;
    }
    dst[i] = 0;
    return dst;
}

int remove(const char *path) { return unlink(path); }

int feof(FILE *f)   { return (int)f->is_eof; }
int ferror(FILE *f) { return (int)f->is_err; }
int fflush(FILE *f) { return 0; }        // nothing is held back to flush

// =====================================================================
// The formatter
// =====================================================================
// One core routine writes into a bounded buffer and returns the length the
// result WOULD have had — the C rule for snprintf, and what lets vfprintf
// notice an overflow instead of silently truncating.

// Unsigned divide by 10 without an unsigned type: halve first so the value is
// certainly positive, divide by 5, then recover the remainder. The correction
// covers the bit lost to the shift.
long _udiv10(long u, long *rem) {
    long q = ((u >> 1) & 0x7fffffffffffffff) / 5;
    long r = u - q * 10;
    if (r > 9) { q = q + 1; r = r - 10; }
    *rem = r;
    return q;
}

struct _Sink { char *dst; long cap; long len; };

void _sink_put(struct _Sink *s, int c) {
    if (s->len < s->cap - 1) s->dst[s->len] = (char)c;
    s->len = s->len + 1;                 // counts past the end, as C requires
}

// One conversion, as a record. This used to be ten separate parameters, which
// nano_cc cannot call -- six is the limit -- and which was unreadable anyway.
struct _Conv {
    long base;
    long is_signed;
    long upper;
    long width;
    long prec;          // -1 for "unset"
    long zeropad;
    long leftalign;
    long plus;          // '+' : always show a sign
    long space;         // ' ' : show a space where '+' would go ('+' wins)
    long alt;           // '#' : 0 prefix for octal, 0x for hex
};

void _sink_num(struct _Sink *s, long v, struct _Conv *c) {
    char tmp[72];
    int n = 0;
    int neg = 0;
    long base = c->base;
    int width = (int)c->width, prec = (int)c->prec;
    int zeropad = (int)c->zeropad, leftalign = (int)c->leftalign, plus = (int)c->plus;
    int is_signed = (int)c->is_signed;
    int space = (int)c->space, alt = (int)c->alt, upper = (int)c->upper;
    char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (is_signed && v < 0) { neg = 1; v = 0 - v; }

    if (v == 0) { tmp[n] = '0'; n = 1; }
    else if (base == 10 && !is_signed) {
        while (v != 0) { long r; v = _udiv10(v, &r); tmp[n] = dig[r]; n = n + 1; }
    } else if (base == 10) {
        while (v != 0) { tmp[n] = dig[v % 10]; n = n + 1; v = v / 10; }
    } else {
        // base 8 and 16 are powers of two, so a shift and a mask work on the
        // raw bit pattern and no unsigned division is needed
        long shift = base == 16 ? 4 : 3;
        long mask = base - 1;
        while (v != 0) { tmp[n] = dig[v & mask]; n = n + 1; v = (v >> shift) & 0x0fffffffffffffff; }
        if (base == 8 && n > 21) n = 22;
    }

    // '+' beats ' ' when both are given; both are ignored for unsigned bases
    int signch = 0;
    if (neg) signch = '-'; else if (plus) signch = '+'; else if (space) signch = ' ';

    // '#': a leading 0 for octal (unless there already is one), 0x/0X for hex
    char pre[2]; int npre = 0;
    if (alt && base == 8 && tmp[n-1] != '0') { pre[0] = '0'; npre = 1; }
    if (alt && base == 16 && !(n == 1 && tmp[0] == '0')) {
        pre[0] = '0'; pre[1] = upper ? 'X' : 'x'; npre = 2;
    }

    int zeros = prec > n ? prec - n : 0;
    int body  = n + zeros + npre + (signch ? 1 : 0);
    int pad   = width > body ? width - body : 0;

    if (!leftalign && !zeropad) { while (pad > 0) { _sink_put(s, ' '); pad = pad - 1; } }
    if (signch) _sink_put(s, signch);
    { int k = 0; while (k < npre) { _sink_put(s, pre[k]); k = k + 1; } }
    if (!leftalign && zeropad && prec < 0) { while (pad > 0) { _sink_put(s, '0'); pad = pad - 1; } }
    while (zeros > 0) { _sink_put(s, '0'); zeros = zeros - 1; }
    while (n > 0) { n = n - 1; _sink_put(s, tmp[n]); }
    while (pad > 0) { _sink_put(s, ' '); pad = pad - 1; }
}

long _vfmt(char *dst, long cap, const char *fmt, va_list ap) {
    struct _Sink s;
    s.dst = dst; s.cap = cap < 1 ? 1 : cap; s.len = 0;
    if (cap < 1) s.cap = 1;

    while (*fmt) {
        if (*fmt != '%') { _sink_put(&s, *fmt); fmt++; continue; }
        fmt++;
        if (*fmt == '%') { _sink_put(&s, '%'); fmt++; continue; }

        int leftalign = 0, zeropad = 0, plus = 0, space = 0, alt = 0;
        while (*fmt == '-' || *fmt == '0' || *fmt == '+' || *fmt == ' ' || *fmt == '#') {
            if (*fmt == '-') leftalign = 1;
            if (*fmt == '0') zeropad = 1;
            if (*fmt == '+') plus = 1;
            if (*fmt == ' ') space = 1;
            if (*fmt == '#') alt = 1;
            fmt++;
        }
        int width = 0;
        if (*fmt == '*') { width = (int)va_arg(ap, int); fmt++;
                           if (width < 0) { leftalign = 1; width = 0 - width; } }
        else while (isdigit(*fmt)) { width = width * 10 + (*fmt - '0'); fmt++; }

        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = (int)va_arg(ap, int); fmt++; }
            else while (isdigit(*fmt)) { prec = prec * 10 + (*fmt - '0'); fmt++; }
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') fmt++;   // all 64-bit here

        int conv = *fmt;
        if (!conv) break;
        fmt++;

        // named `cv`, not `c`: nano_cc gives every local in a function ONE
        // stack slot per name, so a `c` here and the `int c` in the %c branch
        // below would share a slot at two different sizes
        struct _Conv cv;
        cv.base = 10; cv.is_signed = 0; cv.upper = 0;
        cv.width = width; cv.prec = prec;
        cv.zeropad = zeropad; cv.leftalign = leftalign;
        cv.plus = 0; cv.space = 0; cv.alt = alt;

        if (conv == 'd' || conv == 'i') {
            cv.is_signed = 1; cv.plus = plus; cv.space = space; cv.alt = 0;
            _sink_num(&s, va_arg(ap, long), &cv);
        } else if (conv == 'u') {
            _sink_num(&s, va_arg(ap, long), &cv);
        } else if (conv == 'x') {
            cv.base = 16; _sink_num(&s, va_arg(ap, long), &cv);
        } else if (conv == 'X') {
            cv.base = 16; cv.upper = 1; _sink_num(&s, va_arg(ap, long), &cv);
        } else if (conv == 'o') {
            cv.base = 8; _sink_num(&s, va_arg(ap, long), &cv);
        } else if (conv == 'p') {
            _sink_put(&s, '0'); _sink_put(&s, 'x');
            cv.base = 16; cv.width = 0; cv.prec = -1; cv.zeropad = 0;
            cv.leftalign = 0; cv.alt = 0;
            _sink_num(&s, va_arg(ap, long), &cv);
        } else if (conv == 'c') {
            int ch = (int)va_arg(ap, int);
            int pad = width > 1 ? width - 1 : 0;
            if (!leftalign) while (pad-- > 0) _sink_put(&s, ' ');
            _sink_put(&s, ch);
            if (leftalign) while (pad-- > 0) _sink_put(&s, ' ');
        } else if (conv == 's') {
            char *str = (char *)va_arg(ap, char *);
            if (!str) str = "(null)";
            long sn = 0;
            while (str[sn] && (prec < 0 || sn < prec)) sn++;  // %.Ns need not be NUL-terminated
            long spad = width > sn ? width - sn : 0;          // `pad` is an int in the %c branch
            if (!leftalign) { while (spad > 0) { _sink_put(&s, ' '); spad = spad - 1; } }
            long si = 0;
            while (si < sn) { _sink_put(&s, str[si]); si = si + 1; }
            if (leftalign) { while (spad > 0) { _sink_put(&s, ' '); spad = spad - 1; } }
        } else {
            _sink_put(&s, '%'); _sink_put(&s, conv);
        }
    }

    if (cap > 0) {
        long at = s.len < cap - 1 ? s.len : cap - 1;
        dst[at] = 0;
    }
    return s.len;
}

int vsnprintf(char *dst, long cap, const char *fmt, va_list ap) {
    return (int)_vfmt(dst, cap, fmt, ap);
}
int snprintf(char *dst, long cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    long n = _vfmt(dst, cap, fmt, ap);
    va_end(ap);
    return (int)n;
}
int sprintf(char *dst, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    long n = _vfmt(dst, 1000000000, fmt, ap);
    va_end(ap);
    return (int)n;
}

// Format once into a stack buffer, then issue ONE write. If the result did not
// fit, allocate exactly enough and format again rather than truncate — the
// va_list is a plain offset here, so it can simply be reused.
int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char buf[4096];
    va_list ap2 = ap;
    long n = _vfmt(buf, 4096, fmt, ap);
    if (n < 4096) {
        if (n > 0 && write((int)f->fd, buf, n) < 0) { f->is_err = 1; return -1; }
        return (int)n;
    }
    char *big = (char *)malloc(n + 1);
    if (!big) { f->is_err = 1; return -1; }
    _vfmt(big, n + 1, fmt, ap2);
    if (write((int)f->fd, big, n) < 0) { f->is_err = 1; return -1; }
    return (int)n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}
int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}
int vprintf(const char *fmt, va_list ap) { return vfprintf(stdout, fmt, ap); }

// =====================================================================
// errno reporting
// =====================================================================
char *strerror(int e) {
    if (e == 1)  return "Operation not permitted";
    if (e == 2)  return "No such file or directory";
    if (e == 4)  return "Interrupted system call";
    if (e == 5)  return "Input/output error";
    if (e == 9)  return "Bad file descriptor";
    if (e == 12) return "Cannot allocate memory";
    if (e == 13) return "Permission denied";
    if (e == 14) return "Bad address";
    if (e == 17) return "File exists";
    if (e == 20) return "Not a directory";
    if (e == 21) return "Is a directory";
    if (e == 22) return "Invalid argument";
    if (e == 24) return "Too many open files";
    if (e == 27) return "File too large";
    if (e == 28) return "No space left on device";
    if (e == 30) return "Read-only file system";
    if (e == 32) return "Broken pipe";
    return "Unknown error";
}

void perror(const char *m) {
    if (m && *m) { fputs(m, stderr); fputs(": ", stderr); }
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}

#endif
