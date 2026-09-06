// nano-libc.h — C library functions used by the compiler
#ifndef NANO_NOLIBC_H
#ifndef NANO_LIBC_H
#define NANO_LIBC_H

#define attr_printf(a, b)
#define fallthrough

#include <sys/syscall.h>
#include <unistd.h>

_Noreturn void _exit(int code) {
#ifdef LIB_STDIO_H
    fflush(NULL);
#endif
    __syscall(SYS_exit, code);
    while(1);
}

#ifdef LIB_STDIO_H

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define _IOABF   3
#define _IOREAD  1
#define _IOWRITE 2

FILE _iob[NFILE] = {
    { 0, _IOFBF, _IOREAD,  false, BUFSIZ },
    { 1, _IOABF, _IOWRITE, false, BUFSIZ },
    { 2, _IONBF, _IOWRITE, false, 0 },
};

//#define _check_fp(fp) if (!(fp) || (fp) < _iob || (fp) >= &_iob[NFILE]) { errno = EINVAL; return -1; }
#define _check_fp(fp) if (!(fp)) { errno = EINVAL; return -1; }
//#define _check_fp(fp)

FILE *fopen(const char *filename, char *mode) {
    for (FILE *fp = _iob; fp < _iob + NFILE; fp++) {
        if (!fp->flags) {
            int hd, omode; unsigned char flags;
            if      (*mode == 'r') { flags = _IOREAD;  omode = O_RDONLY; }
            else if (*mode == 'w') { flags = _IOWRITE; omode = O_WRONLY | O_CREAT | O_TRUNC; }
            else                   { errno = EINVAL; return NULL; }
            if ((hd = open(filename, omode, 0777)) < 0) return NULL;
            fp->hd = hd; fp->flags = flags; fp->size = BUFSIZ;
            return fp;
        }
    }
    errno = EMFILE; return NULL;
}
int setvbuf(FILE *fp, char *buf, int mode, size_t size) {
    if (!(fp->flags & (_IOREAD|_IOWRITE)) || fflush(fp)) return -1;
    size_t cap = 0;
    switch (mode) {
    case _IOFBF: cap = size; break;
    case _IOLBF:             break;
    case _IONBF: size = 0;   break;
    default: errno = EINVAL; return -1;
    }
    if (fp->alloc) { free(fp->buf); fp->alloc = false; }
    fp->buf = buf; fp->size = size;
    fp->bmode = (unsigned char)mode;
    fp->cap = cap;
    return 0;
}
// reading
int _filbuf(FILE *fp) {
    if (!(fp->flags & _IOREAD)) return EOF;
    if (fp->bmode != _IONBF) {
        if (!fp->buf) {
            if ((fp->buf = malloc(fp->size))) fp->alloc = true;
            else fp->bmode = _IONBF;
        }
        if (fp->buf) {
            ssize_t rsz = read(fp->hd, fp->buf, fp->size); if (rsz <= 0) return EOF;
            fp->pos = 0; fp->len = (size_t)rsz; return fp->buf[fp->pos++];
        }
    }
    char b;
    if (read(fp->hd, &b, 1) == 1) return b & 255;
    return EOF;
}
int fgetc(FILE *fp) { return getc(fp); }
char *fgets(char *buf, size_t n, FILE *fp) {
    size_t i = 0;
    int c;
    while (i + 1 < n && ((c = getc(fp)) != EOF)) buf[i++] = (char)c;
    if (i < size) buf[i] = 0;
    if (c == EOF && i == 0) return NULL;
    return buf;
}
// try and read exactly len bytes in memory
size_t __fread(void *pv, size_t len, FILE *fp) {
    size_t nread = 0;
    unsigned char *p = pv;
    while (len) {
        if (fp->pos < fp->len) {
            size_t n = fp->len - fp->pos;
            if (n > len) n = len;
            memcpy(p, fp->buf + fp->pos, n);
            p += n; nread += n; fp->pos += n;
            if (!(len -= n)) break;
        }
        if (len <= fp->size && fp->buf) {
            if (_filbuf(fp) < 0) break;
            fp->pos = 0; // unget the first byte
        } else {
            ssize_t rsz = read(fp->hd, p, len);
            if (rsz <= 0) break;
            p += rsz; nread += rsz;
            if (!(len -= rsz)) break;
        }
    }
    return nread;
}
size_t fread(void *p, size_t size, size_t nmemb, FILE *fp) {
    //XXX: overflow should be an error
    //size_t len = ((size | nmemb) > 1 && nmemb > SIZE_MAX / size) ? SIZE_MAX : size * nmemb;
    size_t len = size * nmemb;
    size_t rlen = __fread(p, len, fp);
    if (rlen == len) return nmemb; else return rlen / size;
}
int fputc(int c, FILE *fp) { return putc(c, fp); }

int fflush(FILE *fp) {
    FILE *fp1 = fp + 1;
    if (!fp) { fp = _iob; fp1 = fp + NFILE; }
    int rc = 0;
    for (; fp < fp1; fp++) {
        size_t len = fp->pos;
        unsigned char *p = fp->buf;
        if ((fp->flags & _IOWRITE) && p && len) {
            fp->pos = 0;
            while (len) {
                ssize_t wsz = write(fp->hd, p, len);
                if (wsz <= 0) {
                    if (p > fp->buf) memcpy(fp->buf, p, len);
                    fp->pos = len;
                    rc = -1; break;
                }
                p += wsz; len -= wsz;
            }
        }
    }
    return rc;
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
    const unsigned char *p = pv;
    size_t nw = fp->cap - fp->pos; if (nw > len) nw = len;
    if (nw) {
        memcpy(fp->buf + fp->pos, p, nw);
        p += nw; fp->pos += nw;
        len -= nw;
    }
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

int fclose(FILE *fp) {
    _check_fp(fp);
    int rc = fflush(fp);
    if (fp->alloc) free(fp->buf);
    if (fp->flags & (_IOREAD|_IOWRITE)) rc |= close(fp->hd);
    memset(fp, 0, sizeof(*fp));
    return rc;
}

size_t fwrite(const void *p, size_t size, size_t nmemb, FILE *fp) {
    size_t len = size * nmemb;  // should check overflow
    ssize_t wlen = __fwrite(p, len, fp);
    if (wlen < 0) return 0;
    if ((size_t)wlen == len) return nmemb; else return wlen / size;
}

// return the number of bytes written or EOF on error
int fputs(const char *s, FILE *fp) {
    return (int)__fwrite(s, strlen(s), fp);
}

// return the number of bytes written or EOF on error
int puts(const char *s) {
    int len = (int)__fwrite(s, strlen(s), stdout);
    if (putc('\n', stdout) < 0) return EOF;
    return len + 1;
}

#include "nano-printf.h"

#endif

#ifdef LIB_STDLIB_H

#include <ctype.h>

// stdlib.h
static int _xdigit(int d) {
    if (d >= '0' && d <= '9') return d - '0';
    if ((d |= 0x20) >= 'a' && d <= 'z') return d - 'a' + 10;
    return 255;
}
long strtol(const char *s, char **endp, int base) {
    int sign = 1, d;
    long n = 0;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    if (!base) {
        base = 10;
        if (*s == '0') {
            base = 8;
            switch (s[1] | 0x20) {
            case 'b': base -= 14;
            case 'x': base += 8;
            case 'o': s += 2; break;
            }
        }
    }
    while ((d = _xdigit((unsigned char)*s)) < base) { s++; n = n * base + sign * d; }
    if (endp) *endp = (char*)s;
    return n;
}
int atoi(const char *s) { return (int)strtol(s, 0, 0); }

_Noreturn void exit(int code) { _exit(code); }

#include "nano-malloc.h"
#endif

#ifdef LIB_STRING_H
// string.h
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *a = d; const unsigned char *b = s;
    while (n--) *a++ = *b++; return d;
}
void *memset(void *d, int c, size_t n) {
    unsigned char *a = d; while (n--) *a++ = (unsigned char)c; return d;
}
int memcmp(const void *p1, const void *p2, size_t n) {
    const unsigned char *a = p1, *b = p2;
    for (; n--; a++, b++) { if (*a == *b) continue; return *a - *b; }
    return 0;
}
size_t strlen(const char *s) { size_t i = 0; while (s[i]) i++; return i; }
size_t strnlen(const char *s, size_t n) { size_t i = 0; while (i < n && s[i]) i++; return i; }
char *strchr(const char *s, int c) { while (*s != (char)c) if (!*s++) return NULL; return (char*)s; }
char *strrchr(const char *s, int c) { char *e = NULL; while (*s) if (*s == (char)c) e = (char*)s; return e; }
char *strcpy(char *d, const char *s) { for (size_t i = 0; d[i] = s[i]; i++); return d; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
#endif

#ifdef LIB_TIME_H
#include <sys/syscall.h>
#include <unistd.h>
int clock_gettime(clockid_t clock_id, struct timespec *tp) {
    return __syscall(SYS_clock_gettime, clock_id, tp);
}
clock_t clock(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts)) return -1;
    //if (ts.tv_sec > LONG_MAX / 1000000 || ts.tv_nsec / 1000 > LONG_MAX - 1000000 * ts.tv_sec) return -1;
    return (clock_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif

#ifdef LIB_SYS_TIME_H
#include <sys/syscall.h>
#include <unistd.h>
int gettimeofday(struct timeval *tv, struct timezone *tz) {
    return __syscall(SYS_gettimeofday, tv, tz);
}
#endif

#ifdef LIB_FCNTL_H
#include <sys/syscall.h>
#include <unistd.h>
#include <stdarg.h>
int creat(const char *path, int mode) {
    return __syscall(SYS_creat, path, mode);
}
int open(const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags);
    int mode = va_arg(ap, int);
    va_end(ap);
    return __syscall(SYS_open, path, flags, mode);
}
#endif

#ifdef LIB_UNISTD_H
#include <sys/syscall.h>
#include <stdarg.h>
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
int close(int fd) { return __syscall(SYS_close, fd); }
int ioctl(int fd, int cmd, ...) {
    va_list ap; va_start(ap, flags);
    unsigned long arg1 = va_arg(ap, unsigned long);
    unsigned long arg2 = va_arg(ap, unsigned long);
    unsigned long arg3 = va_arg(ap, unsigned long);
    //unsigned long arg4 = va_arg(ap, unsigned long);
    va_end(ap);
    return __syscall(SYS_ioctl, fd, cmd, arg1, arg2, arg3 /*, arg4*/);
}

struct winsize {
    unsigned short ws_row;     // rows, in characters
    unsigned short ws_col;     // columns, in characters
    unsigned short ws_xpixel;  // horizontal size, pixels
    unsigned short ws_ypixel;  // vertical size, pixels
};
enum { TIOCGWINSZ = 0x5413 };

int isatty(int fd) {
    struct winsize wsz;
    int r = __syscall(SYS_ioctl, fd, TIOCGWINSZ, &wsz);
    if (r == 0) return 1;
    if (errno != EBADF) errno = ENOTTY;
    return 0;
}
#endif

#ifdef LIB_ERRNO_H
// sys_err.c
thread_local int errno;
const int sys_nerr = 35;
const char * const sys_errlist[] = {
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
thread_local static char errbuf[20];
const char *strerror(int errnum) {
    if (errnum >= 0 && errnum < sys_nerr) return sys_errlist[errnum];
    snprintf(errbuf, sizeof(errbuf), "Error %d", errnum);
    return errbuf;
}
void perror(const char *s) {
    int errnum = errno;
    if (s && *s) fprintf(stderr, "%s: ", s);
    fprintf(stderr, "%s\n", strerror(errnum));
}
#endif
#endif
#endif
