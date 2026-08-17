// nano-nolibc.h — header-only libc for the nano_cc compiler
// x86_64 Linux syscall convention:
//   rax = syscall number, rdi/rsi/rdx/r10/r8/r9 = args
//   syscall instruction, return in rax

#ifndef NANO_NOLIBC_H
#define NANO_NOLIBC_H

// ===== SYSCALL NUMBERS (x86_64 Linux) =====
#define SYS_read        0
#define SYS_write       1
#define SYS_open        2
#define SYS_close       3
#define SYS_mmap        9
#define SYS_munmap      11
#define SYS_brk         12
#define SYS_exit        60
#define SYS_ioctl       16

// ===== RAW SYSCALL WRAPPERS =====
// Our compiler's __asm__ is a simple passthrough, so we use
// a naked function style: move args into regs, syscall, read rax.

static inline long syscall0(long n) {
    long ret;
    __asm__("mov rax, %0" : : "i"(n));  // we'll use a simpler approach below
    return ret;
}

// Since our asm is simple passthrough, we write syscall wrappers
// as inline asm blocks that follow the ABI manually.

static inline long sys_write(int fd, const char *buf, long count) {
    long ret;
    __asm__(
        "mov rax, 1\n"          // SYS_write
        "mov rdi, %1\n"         // fd
        "mov rsi, %2\n"         // buf
        "mov rdx, %3\n"         // count
        "syscall\n"
        "mov %0, rax\n"
        : "=r"(ret) : "r"((long)fd), "r"(buf), "r"(count)
    );
    return ret;
}
#endif
