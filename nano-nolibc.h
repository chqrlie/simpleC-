// nano-nolibc.h — works with nano_cc's simple __asm__ passthrough
#ifndef NANO_NOLIBC_H
#define NANO_NOLIBC_H

#ifdef __GNUC__
#define FILE xFILE
#undef strlen
#undef memcpy
#undef memset
#undef strcmp
#undef stdin
#undef stdout
#undef stderr
#undef NULL
#undef EOF
#undef islower
#undef isupper
#undef isalpha
#undef isdigit
#undef isxdigit
#undef isalnum
#undef isblank
#undef isspace
#undef tolower
#undef toupper
#endif

#define _Noreturn

// stdbool.h (should make these keywords)
#ifndef true
typedef unsigned char bool;
#define true 1
#define false 0
#endif

// stddef.h
#ifndef NULL
#define NULL  ((void*)0)
#endif
typedef unsigned long size_t;
typedef long ssize_t;

// stdarg.h on top of nano_cc's variadic built-ins ---
#ifndef va_start
#define va_list         long
#define va_start(ap, l) __builtin_va_start(ap)
#define va_arg(ap, t)   ((t)__builtin_va_arg(ap))
#define va_copy(a1, a2) ((a1) = (a2))
#define va_end(ap)      __builtin_va_end(ap)
#endif

// ctype.h
// should use a byte table
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isalpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isxdigit(int c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
int isalnum(int c) { return isdigit(c) || isalpha(c); }
int isblank(int c) { return c == ' ' || c == '\t'; }
int isspace(int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
int tolower(int c) { return isupper(c) ? c + ('a' - 'A') : c; }
int toupper(int c) { return islower(c) ? c - ('a' - 'A') : c; }

// errno.h
int errno;
enum {
    EPERM    = 1,  /* Operation not permitted */
    ENOENT   = 2,  /* No such file or directory */
    ESRCH    = 3,  /* No such process */
    EINTR    = 4,  /* Interrupted system call */
    EIO      = 5,  /* I/O error */
    ENXIO    = 6,  /* No such device or address */
    E2BIG    = 7,  /* Argument list too long */
    ENOEXEC  = 8,  /* Exec format error */
    EBADF    = 9,  /* Bad file number */
    ECHILD  = 10,  /* No child processes */
    EAGAIN  = 11,  /* Try again */
    ENOMEM  = 12,  /* Out of memory */
    EACCES  = 13,  /* Permission denied */
    EFAULT  = 14,  /* Bad address */
    ENOTBLK = 15,  /* Block device required */
    EBUSY   = 16,  /* Device or resource busy */
    EEXIST  = 17,  /* File exists */
    EXDEV   = 18,  /* Cross-device link */
    ENODEV  = 19,  /* No such device */
    ENOTDIR = 20,  /* Not a directory */
    EISDIR  = 21,  /* Is a directory */
    EINVAL  = 22,  /* Invalid argument */
    ENFILE  = 23,  /* File table overflow */
    EMFILE  = 24,  /* Too many open files */
    ENOTTY  = 25,  /* Not a typewriter */
    ETXTBSY = 26,  /* Text file busy */
    EFBIG   = 27,  /* File too large */
    ENOSPC  = 28,  /* No space left on device */
    ESPIPE  = 29,  /* Illegal seek */
    EROFS   = 30,  /* Read-only file system */
    EMLINK  = 31,  /* Too many links */
    EPIPE   = 32,  /* Broken pipe */
    EDOM    = 33,  /* Math argument out of domain of func */
    ERANGE  = 34,  /* Math result not representable */
};

// string.h
void *memcpy(void *d, const void *s, size_t n) {
    char *a = (char*)d; const char *b = (const char*)s;
    while (n--) *a++ = *b++; return d;
}
void *memset(void *d, int c, size_t n) {
    char *a = (char*)d; while (n--) *a++ = (char)c; return d;
}
int memcmp(const void *p1, const void *p2, size_t n) {
    const char *a = (const char*)p1; const char *b = (const char*)p2;
    for (; n--; a++, b++) if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
char *strchr(const char *s, int c) { while (*s != (char)c) if (!*s++) return NULL; return (char*)s; }
char *strcpy(char *d, const char *s) { for (size_t i = 0; d[i] = s[i]; i++); return d; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

#ifdef __GNUC__
#include <unistd.h>
#include <fcntl.h>
#else
long __syscall(long n, ...);

// --- Syscall numbers ---
enum {
    SYS_read  = 0,
    SYS_write = 1,
    SYS_open  = 2,
    SYS_close = 3,
    SYS_mmap  = 9,
    SYS_exit  = 60,
    SYS_creat = 85,
    SYS_gettimeofday = 96,
};

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_BINARY 0

// --- POSIX wrappers ---
ssize_t read(int fd, void *buf, size_t len) {
    for (;;) {
        ssize_t n = __syscall(SYS_read, fd, buf, len);
        if (n >= 0 || errno != EINTR) return n;
    }
}
ssize_t write(int fd, const void *buf, size_t len) {
    for (;;) {
        ssize_t n = __syscall(SYS_write, fd, buf, len);
        if (n >= 0 || errno != EINTR) return n;
    }
}
int creat(const char *path, int mode) {
    return __syscall(SYS_creat, path, mode);
}
int open(const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    return __syscall(SYS_open, path, flags, mode);
}
int close(int fd) {
    return __syscall(SYS_close, fd);
}
int fflush(struct FILE *fp);    // should use an atexit function table
_Noreturn void exit(int code) {
    fflush(NULL);
    __syscall(SYS_exit, code);
    while(1);
}

#if SYSTEM_DARWIN
typedef long time_t;
typedef int suseconds_t;
#else
// Same for Linux, FreeBSD and OpenBSD
typedef long time_t;
typedef long suseconds_t;
#endif

struct timeval { time_t tv_sec; suseconds_t tv_usec; };
struct timezone { int tz_minuteswest; int tz_dsttime; };

int gettimeofday(struct timeval *tv, struct timezone *tz) {
    return __syscall(SYS_gettimeofday, tv, tz);
}
#endif

typedef struct FILE { int hd; char *dest; size_t pos, cap; } FILE;
#define NFILE 10
FILE _iofb[NFILE] = {{ 0 }, { 1 }, { 2 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }, { -1 }};
#define stdin  &_iofb[0]
#define stdout &_iofb[1]
#define stderr &_iofb[2]
#define EOF   (-1)

int fclose(FILE *fp) { close(fp->hd); fp->hd = -1; }
int fflush(FILE *fp) { return 0; }
FILE *fopen(const char *filename, char *mode) {
    for (int i = 0; i < NFILE; i++) {
        FILE *fp = &_iofb[i];
        if (fp->hd < 0) {
            fp->hd = open(filename, *mode == 'w' ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY, 0777);
            if (fp->hd >= 0) return fp;
            break;
        }
    }
    return NULL;
}

int fgetc(FILE *fp) { char b; return read(fp->hd, &b, 1) == 1 ? b & 255 : EOF; }
char *fgets(char *buf, size_t n, FILE *fp) {
    size_t i = 0;
    int c;
    while (i + 1 < n && ((c = fgetc(fp)) != EOF)) buf[i++] = (char)c;
    if (i < size) buf[i] = 0;
    if (c == EOF && i == 0) return NULL;
    return buf;
}
size_t fread(void *p, size_t size, size_t nmemb, FILE *fp) {
    size_t len = size * nmemb;  // should check overflow
    ssize_t slen = read(fp->hd, p, len);
    if (slen < 0) return 0;
    return ((size_t)slen < len) ? (size_t)slen / size : nmemb;
}

int fputc(int c, FILE *fp) {
    if (fp->dest) {
        if (fp->pos < fp->cap) return (unsigned char)(fp->dest[fp->pos++] = (char)c);
    } else {
        unsigned char b = (unsigned char)c;
        return write(fp->hd, &b, 1) == 1 ? b : EOF;
    }
}
size_t fwrite(const void *p, size_t size, size_t nmemb, FILE *fp) {
    if (!nmemb) return 0;
    size_t len = size * nmemb, wlen = len;  // should check overflow
    if (fp->dest) {
        if (wlen > fp->cap - fp->pos) wlen = fp->cap - fp->pos;
        memcpy(fp->dest + fp->pos, p, wlen);
        fp->pos += wlen;
    } else {
        ssize_t slen = write(fp->hd, p, len);
        if (slen < 0) return 0;
        wlen = (size_t)slen;
    }
    return (wlen < len) ? wlen / size : nmemb;
}

int fputs(const char *s, FILE *fp) {
    size_t len = strlen(s);
    if (fp->dest) {
        if (len > fp->cap - fp->pos) len = fp->cap - fp->pos;
        memcpy(fp->dest + fp->pos, s, len);
        return fp->pos += len;
    } else {
        return write(fp->hd, s, len);
    }
}

int puts(const char *s) { fputs(s, stdout); return fputc('\n', stdout); }

// --- printf: supports %d %x %c %s and %% ---
size_t ltoa(char *dest, long n) {
    char buf[24]; char *p = &buf[sizeof(buf)];
    *--p = 0;
    if (n == 0) {
        *--p = '0';
    } else if (n < 0) {
        while (n) { *--p = '0' - n % 10; n = n / 10; }
        *--p = '-';
    } else {
        while (n) { *--p = '0' + n % 10; n = n / 10; }
    }
    size_t size = &buf[sizeof(buf)] - p;
    memcpy(dest, p, size);
    return size - 1;
}

size_t ultoa(char *dest, unsigned long n, int base) {
    char buf[72]; char *p = &buf[sizeof(buf)];
    const char *digits = "0123456789abcdefghijklmnopqrstuvwxyz";    // share digit string
    *--p = 0;
    if (n == 0) *--p = '0';
    if (base >= 2 && base <= 36) {
        while (n > 0) { *--p = digits[__lmod(n, base)]; n = __ldiv(n, base); }
    } else {
        errno = ERANGE;
    }
    size_t size = &buf[sizeof(buf)] - p;
    memcpy(dest, p, size);
    return size - 1;
}

void _puts(const char *s) { fputs(s, stdout); }
void _print_int(long n) {
    char buf[24]; size_t len = ltoa(buf, n);
    fwrite(buf, 1, len, stdout);
}

int vfprintf(FILE *fp, const char *fmt, va_list ap) {
    char buf[72];
    const char *q = fmt;
    size_t total = 0;
    for (;;) {
        if (*fmt && (*fmt != '%' || !fmt[1])) { fmt++; continue; }
        size_t len = fmt - q;
        fwrite(q, 1, len, fp);
        total += len;
        q = fmt;
        if (!*fmt) return (int)total;
        char *s;
        int base = 10;
        int mod = 0;
        fmt++;  // past '%'
    again:
        switch (*fmt++) {
        case 'd':
        case 'u':            goto out_num;
        case 'l': case 'z': case 't': mod++; goto again;
        case 'x': base = 16; goto out_num;
        case 'o': base = 8;  goto out_num;
        case 'b': base = 2;  goto out_num;
        case 's':
            s = va_arg(ap, char *);
            if (!s) s = "(null)";
            len = strlen(s);
            goto out_str;
        case 'c':
            fputc(va_arg(ap, int), fp);
            total++;
            q = fmt;
            continue;
        case '%':  q = fmt - 1; continue;
        case '\0': fmt--;       continue;
        default:                continue;
        }
    out_num:
        long n = mod ? va_arg(ap, long) : va_arg(ap, int);
        len = fmt[-1] == 'd' ? ltoa(buf, n) : ultoa(buf, n, base);
        s = buf;
    out_str:
        fwrite(s, 1, len, fp);
        total += len;
        q = fmt;
    }
}

int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap); return n;
}

int fprintf(FILE *fp, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(fp, fmt, ap);
    va_end(ap); return n;
}

int snprintf(char *dest, size_t size, const char *fmt, ...) {
    FILE f; f.hd = -1; f.dest = dest; f.pos = 0; f.cap = size;
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(&f, fmt, ap);
    va_end(ap);
    if ((size_t)n < size) dest[n] = '\0';
    else if (size) dest[size - 1] = '\0';
    return n;
}

// sys_err.c
int sys_nerr = 35;
const char *sys_errlist[] = {
    "Success",
    "Operation not permitted",
    "No such file or directory",
    "No such process",
    "Interrupted system call",
    "I/O error",
    "No such device or address",
    "Argument list too long",
    "Exec format error",
    "Bad file number",
    "No child processes",
    "Try again",
    "Out of memory",
    "Permission denied",
    "Bad address",
    "Block device required",
    "Device or resource busy",
    "File exists",
    "Cross-device link",
    "No such device",
    "Not a directory",
    "Is a directory",
    "Invalid argument",
    "File table overflow",
    "Too many open files",
    "Not a typewriter",
    "Text file busy",
    "File too large",
    "No space left on device",
    "Illegal seek",
    "Read-only file system",
    "Too many links",
    "Broken pipe",
    "Math argument out of domain of func",
    "Math result not representable",
};
static char errbuf[20];
const char *strerror(int errnum) {
    if (errnum >= 0 && errnum < sys_nerr) return sys_errlist[errnum];
    snprintf(errbuf, "Error %d", errnum);
    return errbuf;
}
void perror(const char *s) {
    int errnum = errno;
    if (s && *s) fprintf(stderr, "%s: ", s);
    fprintf(stderr, "%s\n", strerror(errnum));
}

#endif // NANO_NOLIBC_H
