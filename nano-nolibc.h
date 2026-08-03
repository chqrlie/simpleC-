// nano-nolibc.h — works with nano_cc's simple __asm__ passthrough
#ifndef NANO_NOLIBC_H
#define NANO_NOLIBC_H

// --- String functions (pure C) ---
long strlen(const char *s) { long n=0; while(s[n]) n++; return n; }
void *memcpy(void *d, const void *s, long n) {
    char *a=(char*)d; const char *b=(const char*)s;
    while(n--) *a++=*b++; return d;
}
void *memset(void *d, int c, long n) {
    char *a=(char*)d; while(n--) *a++=(char)c; return d;
}
int strcmp(const char *a, const char *b) {
    while(*a && *a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b;
}

// --- Syscall trampoline ---
// All syscalls go through this one naked asm function.
// Args are passed via globals to avoid needing extended asm syntax.
long _a1, _a2, _a3, _a4, _a5, _a6, _n, _ret;

void _do_syscall() {
    // nano_cc passes the __asm__ body straight through to the assembler
    // (GNU as, .intel_syntax noprefix). Globals are addressed RIP-relative.
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

static inline long syscall1(long n, long a1) {
    _n=n; _a1=a1; _do_syscall(); return _ret;
}
static inline long syscall2(long n, long a1, long a2) {
    _n=n; _a1=a1; _a2=a2; _do_syscall(); return _ret;
}
static inline long syscall3(long n, long a1, long a2, long a3) {
    _n=n; _a1=a1; _a2=a2; _a3=a3; _do_syscall(); return _ret;
}

// --- Syscall numbers ---
#define SYS_read   0
#define SYS_write  1
#define SYS_open   2
#define SYS_close  3
#define SYS_mmap   9
#define SYS_exit   60

// --- POSIX wrappers ---
long write(int fd, const char *buf, long len) {
    return syscall3(SYS_write, fd, (long)buf, len);
}
int  open(const char *path, int flags) {
    return syscall2(SYS_open, (long)path, flags);
}
int  close(int fd) {
    return syscall1(SYS_close, fd);
}
void exit(int code) {
    syscall1(SYS_exit, code);
    while(1);
}

// --- Minimal printf (just %d and %s) ---
void puts(const char *s) { write(1, s, strlen(s)); }

void print_int(long n) {
    char buf[24]; int i = 22; buf[23] = 0;
    if (n < 0) { write(1, "-", 1); n = -n; }
    if (n == 0) { write(1, "0", 1); return; }
    while (n > 0 && i > 0) { buf[i--] = '0' + (n % 10); n /= 10; }
    write(1, &buf[i+1], 22 - i);
}

#endif