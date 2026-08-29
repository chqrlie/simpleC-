// os-base.h — nano-base.h, with nanoOS underneath instead of Linux.
//
// This is the whole trick behind running the compiler inside the OS. The
// compiler is written against a hosted C library; nano-libc.h supplies that
// library in nano_cc's own subset, and it reaches the outside world through
// exactly one file: nano-base.h. Swap that file and the same libc, byte for
// byte, sits on a different kernel.
//
// So the build copies THIS file into place as nano-base.h, and nano-libc.h and
// simpleC++.c are used completely unmodified. Everything above the line is
// shared with the Linux build; everything below it is 60 lines.
//
// What actually differs:
//
//   * The trap. Linux: `syscall`, numbers from the x86-64 ABI. Here: int $0x80
//     through syscall4 in ustart.s, numbers from kernel/nano-proc.h.
//
//   * open(). Linux takes a flags word; nanoOS takes the same word but only
//     looks at O_CREAT and O_TRUNC, because there is no permission model to
//     honour a mode with and nothing else in the word means anything yet.
//
//   * brk(). Deliberately the Linux shape -- absolute address in, new break
//     out, address 0 meaning "just tell me where it is" -- because that is
//     what nano-libc.h's allocator was written against, and matching it here
//     is cheaper and less error-prone than a special case up there.
//
// Everything else in this file is character-for-character the pure-C half of
// nano-base.h. It is duplicated rather than included because nano-base.h is one
// file with no seam in it, and cutting a seam through it to share a few string
// functions would leave the Linux build carrying an #ifdef for a kernel it
// never runs on.

#ifndef NANO_BASE_H
#define NANO_BASE_H

#define NULL 0
#define EOF  (-1)

// --- string / memory (pure C) ---
long strlen(const char *s) { long n=0; while(s[n]) n++; return n; }
void *memcpy(void *d, const void *s, long n) {
    char *a=(char*)d; const char *b=(const char*)s;
    while(n--) *a++=*b++; return d;
}
void *memset(void *d, int c, long n) {
    char *a=(char*)d; while(n--) *a++=(char)c; return d;
}
void *memmove(void *d, const void *s, long n) {
    char *a=(char*)d; const char *b=(const char*)s;
    if (a == b || n <= 0) return d;
    if (a < b) { while(n--) *a++=*b++; return d; }
    a = a + n; b = b + n;
    while(n--) { a--; b--; *a = *b; }
    return d;
}
int memcmp(const void *p, const void *q, long n) {
    const char *a=(const char*)p; const char *b=(const char*)q;
    while (n--) {
        int d = (*a & 255) - (*b & 255);
        if (d) return d;
        a++; b++;
    }
    return 0;
}
int strcmp(const char *a, const char *b) {
    while(*a && *a==*b){a++;b++;} return (*a & 255)-(*b & 255);
}
int strncmp(const char *a, const char *b, long n) {
    while (n > 0) {
        int d = (*a & 255) - (*b & 255);
        if (d || !*a) return d;
        a++; b++; n--;
    }
    return 0;
}
char *strcpy(char *d, const char *s) { char *r=d; while((*d++=*s++)); return r; }
char *strncpy(char *d, const char *s, long n) {
    char *r=d;
    while (n > 0 && *s) { *d++ = *s++; n--; }
    while (n > 0) { *d++ = 0; n--; }
    return r;
}
char *strcat(char *d, const char *s) {
    char *r=d; while(*d) d++; while((*d++=*s++)); return r;
}
char *strchr(const char *s, int c) {
    while (*s) { if ((*s & 255) == (c & 255)) return (char *)s; s++; }
    if ((c & 255) == 0) return (char *)s;
    return NULL;
}
char *strrchr(const char *s, int c) {
    char *last = NULL;
    while (*s) { if ((*s & 255) == (c & 255)) last = (char *)s; s++; }
    if ((c & 255) == 0) return (char *)s;
    return last;
}
char *strstr(const char *h, const char *n) {
    long ln = strlen(n);
    if (ln == 0) return (char *)h;
    while (*h) {
        if (!strncmp(h, n, ln)) return (char *)h;
        h++;
    }
    return NULL;
}

// --- stdarg, on top of nano_cc's variadic built-ins ---
#define va_list         long
#define va_start(ap, l) __builtin_va_start(ap)
#define va_arg(ap, t)   __builtin_va_arg(ap)
#define va_end(ap)      __builtin_va_end(ap)

// =====================================================================
// Below here is the part that knows it is running on nanoOS.
// =====================================================================

// The trap itself lives in ustart.s, which every user program links against:
// number in rax, three arguments in rdi/rsi/rdx, `int $0x80`, result in rax.
// No inline asm is needed here at all, which is the nicest difference from the
// Linux side -- there the trampoline has to pass arguments through globals
// because nano_cc's __asm__ has no operand constraints.
extern long syscall4(long nr, long a, long b, long c);

// These must match the SYS_* defines in kernel/nano-proc.h. They are written
// out in both places rather than shared, because the kernel does not want to
// include a user header, and a mismatch here fails the way mismatches always
// do -- silently doing the wrong call rather than refusing.
#define SYS_exit    0
#define SYS_write   1
#define SYS_read    2
#define SYS_open    3
#define SYS_close   4
#define SYS_lseek   5
#define SYS_fsize   6
#define SYS_sbrk    7
#define SYS_getpid  8
#define SYS_yield   9
#define SYS_ticks  10
#define SYS_unlink 11
#define SYS_brk    12

static inline long syscall1(long n, long a1)                     { return syscall4(n, a1, 0, 0); }
static inline long syscall2(long n, long a1, long a2)            { return syscall4(n, a1, a2, 0); }
static inline long syscall3(long n, long a1, long a2, long a3)   { return syscall4(n, a1, a2, a3); }

// nanoOS returns -1 for a failure, not -errno, so errno only ever says "1".
// Kept because nano-libc.h checks the return value, never errno -- and a
// caller that DID read errno would at least see a nonzero number rather than
// stale success. Widening this into real error codes is its own change.
int errno;
long _sysret(long r) {
    if (r < 0 && r > 0 - 4096) { errno = 0 - r; return -1; }
    return r;
}

// --- open() flags ---
// The values are Linux's, so fopen() in nano-libc.h needs no edit. The kernel
// looks at O_CREAT and O_TRUNC and ignores the rest; there is no read-only
// enforcement yet, and pretending otherwise by rejecting O_WRONLY reads here
// would be a check that protects nothing.
#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    64
#define O_TRUNC    512
#define O_APPEND   1024

// --- POSIX wrappers ---
long read(int fd, void *buf, long len) {
    return _sysret(syscall3(SYS_read, fd, (long)buf, len));
}
long write(int fd, const char *buf, long len) {
    return _sysret(syscall3(SYS_write, fd, (long)buf, len));
}
int open(const char *path, int flags, ...) {
    long mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return (int)_sysret(syscall3(SYS_open, (long)path, flags, mode));
}
int close(int fd) { return (int)_sysret(syscall1(SYS_close, fd)); }
long lseek(int fd, long off, int whence) {
    return _sysret(syscall3(SYS_lseek, fd, off, whence));
}
int unlink(const char *path) {
    return (int)_sysret(syscall1(SYS_unlink, (long)path));
}
void exit(int code) {
    syscall1(SYS_exit, code);
    while(1);
}

#endif
