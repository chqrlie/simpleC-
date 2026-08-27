// nano-base.h — the freestanding core shared by nano-nolibc.h and nano-libc.h.
//
// Raw Linux syscalls, the handful of string/memory routines everything needs,
// and the stdarg macros that sit on nano_cc's variadic built-ins. No stdio,
// no allocator, no opinion about buffering — those live one level up.
//
// Everything here compiles with nano_cc itself, so it stays inside the
// language subset: no unsigned arithmetic, no function pointers, `int` and
// `long` are both 64-bit.

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
// Copies correctly when the two regions overlap; memcpy does not promise to.
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
//
// LIMIT worth knowing: nano_cc spills the six argument REGISTERS into a save
// area, so va_arg only reaches arguments 1..6 of the call. A variadic function
// with two fixed parameters can therefore take four varargs. Everything is
// read 8 bytes at a time, which is why `int` and `long` behave identically.
#define va_list         long
#define va_start(ap, l) __builtin_va_start(ap)
#define va_arg(ap, t)   __builtin_va_arg(ap)
#define va_end(ap)      __builtin_va_end(ap)

// --- syscall trampoline ---
// All syscalls go through this one naked asm function. Arguments travel via
// globals so the asm body needs no operand constraints — nano_cc passes the
// body straight to the assembler.
//
// NOTE: the asm body is emitted VERBATIM, so these six names must never be
// ones the compiler would rename on the way out (see asm_sym in simpleC++.c).
// `_n`, `_a1`.. are safe; a global called `sp` would not be.
long _a1, _a2, _a3, _a4, _a5, _a6, _n, _ret;

void _do_syscall() {
    __asm__(
        "mov rax, [rip + _n]\n"
        "mov rdi, [rip + _a1]\n"
        "mov rsi, [rip + _a2]\n"
        "mov rdx, [rip + _a3]\n"
        "mov r10, [rip + _a4]\n"
        "mov r8,  [rip + _a5]\n"
        "mov r9,  [rip + _a6]\n"
        "syscall\n"
        "mov [rip + _ret], rax\n"
    );
}

// A failing Linux syscall returns -errno in rax, in the range [-4095, -1].
int errno;
long _sysret(long r) {
    if (r < 0 && r > 0 - 4096) { errno = 0 - r; return -1; }
    return r;
}

static inline long syscall1(long n, long a1) {
    _n=n; _a1=a1; _do_syscall(); return _ret;
}
static inline long syscall2(long n, long a1, long a2) {
    _n=n; _a1=a1; _a2=a2; _do_syscall(); return _ret;
}
static inline long syscall3(long n, long a1, long a2, long a3) {
    _n=n; _a1=a1; _a2=a2; _a3=a3; _do_syscall(); return _ret;
}
// (no syscall6 wrapper: nano_cc allows at most 6 arguments per call, and a
// 6-argument syscall would need seven counting the syscall number. Nothing
// here needs one — the allocator uses brk, not mmap.)

// --- syscall numbers ---
#define SYS_read   0
#define SYS_write  1
#define SYS_open   2
#define SYS_close  3
#define SYS_lseek  8
#define SYS_mmap   9
#define SYS_brk    12
#define SYS_unlink 87
#define SYS_exit   60

// --- open() flags ---
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
// C's open() is variadic: the mode argument exists only when O_CREAT is set,
// and reading it otherwise is undefined. Same rule here, for the same reason —
// with no O_CREAT there is nothing in that vararg slot to read.
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
