// nano-nolibc.h — very minimal runtime to run some tests
#ifndef NANO_NOLIBC_H
#define NANO_NOLIBC_H

typedef unsigned char bool;
#define true 1
#define false 0

typedef long va_list;
#define va_start(ap, l)  __builtin_va_start(ap, l)
#define va_arg(ap, t)    ((t)__builtin_va_arg(ap))
#define va_copy(a1, a2)  ((a1) = (a2))
#define va_end(ap)       __builtin_va_end(ap)

#define NULL  ((void*)0)
typedef unsigned long size_t;
typedef long ssize_t;

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
long __syscall(long n, ...);

// --- POSIX wrappers ---
ssize_t _read(int fd, void *buf, size_t len) {
    for (;;) {
        ssize_t n = __syscall(SYS_read, fd, buf, len);
        if (n >= 0 || errno != EINTR) return n;
    }
}
ssize_t _write(int fd, const void *buf, size_t len) {
    for (;;) {
        ssize_t n = __syscall(SYS_write, fd, buf, len);
        if (n >= 0 || errno != EINTR) return n;
    }
}
int _open(const char *path, int flags, ...) {
    va_list ap; va_start(ap, flags); int mode = va_arg(ap, int); va_end(ap);
    return __syscall(SYS_open, path, flags, mode);
}
int _close(int fd) { return __syscall(SYS_close, fd); }

_Noreturn void _exit(int code) {
    __syscall(SYS_exit, code);
    for(;;);
}

// ancillary functions used in the examples
void _putc(char c) { _write(1, &c, 1); }
void _puts(const char *s) { _write(1, s, strlen(s)); }
void _print_int(long n) {
    char buf[24]; unsigned long u = n;
    if (n < 0) u = -u;
    char *p = buf + sizeof(buf);
    *--p = 0;
    do { *--p = '0' + u % 10; u = u / 10; } while (u);
    if (n < 0) *--p = '-';
    _puts(p);
}
#endif
