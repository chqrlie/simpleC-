// nano-nolibc.h — very minimal runtime to run some tests
#ifndef NANO_NOLIBC_H
#define NANO_NOLIBC_H

#ifdef __GNUC__

#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#define attr_printf(a, b)  __attribute__((format(printf, a, b)))
#if __has_attribute(__fallthrough__)
#define fallthrough  __attribute__((__fallthrough__))
#else
#define fallthrough  do {} while (0)  /* fallthrough */
#endif

#else

#define attr_printf(a, b)
#define fallthrough

typedef unsigned char bool;
#define true 1
#define false 0

#define NULL  ((void*)0)

// errno.h
thread_local int errno;
enum {  // Linux error codes
    EPERM = 1, ENOENT, ESRCH, EINTR, EIO, ENXIO, E2BIG, ENOEXEC, EBADF,
    ECHILD, EAGAIN, ENOMEM, EACCES, EFAULT, ENOTBLK, EBUSY, EEXIST, EXDEV,
    ENODEV, ENOTDIR, EISDIR, EINVAL, ENFILE, EMFILE, ENOTTY, ETXTBSY, EFBIG,
    ENOSPC, ESPIPE, EROFS, EMLINK, EPIPE, EDOM, ERANGE,
};

size_t strlen(const char *s) { size_t i = 0; while (s[i]) i++; return i; }

// --- Syscall numbers ---
enum {
    SYS_read  = 0,
    SYS_write = 1,
    SYS_open  = 2,
    SYS_close = 3,
    SYS_mmap  = 9,
    SYS_exit  = 60,
};

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
int open(const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    return __syscall(SYS_open, path, flags, mode);
}
int close(int fd) { return __syscall(SYS_close, fd); }

_Noreturn void exit(int code) {
    __syscall(SYS_exit, code);
    for(;;);
}
#endif

// ancillary functions used in the examples
void _putc(char c) { write(1, &c, 1); }
void _puts(const char *s) { write(1, s, strlen(s)); }
void print_int(long n) {
    char buf[24]; unsigned long u = n;
    if (n < 0) u = -u;
    char *p = buf + sizeof(buf);
    *--p = 0;
    do { *--p = u % 10 + '0'; u = u / 10; } while (u);
    if (n < 0) *--p = '-';
    _puts(p);
}
void print(const char *str, long n) { if (str) _puts(str); print_int(n); }
void println(const char *str, long n) { print(str, n); _putc('\n'); }
#endif
