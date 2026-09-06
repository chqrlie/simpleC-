#ifndef LIB_SYS_SYSCALL_H
#define LIB_SYS_SYSCALL_H
// --- Syscall numbers ---
enum {
    SYS_read  = 0,
    SYS_write = 1,
    SYS_open  = 2,
    SYS_close = 3,
    SYS_mmap  = 9,
    SYS_ioctl = 16,
    SYS_exit  = 60,
    SYS_creat = 85,
    SYS_gettimeofday = 96,
    SYS_clock_gettime = 228,
};
#endif
