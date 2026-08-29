// nano-user.h — the user side of the syscall boundary.
//
// Programs that include this are compiled by nano_cc with --kernel (which only
// means "do not emit the Linux _start stub") and linked against ustart.s, which
// supplies _start and the one instruction C cannot express: int $0x80.
//
// Nothing here touches hardware, reads a kernel global, or calls a kernel
// function. That is the whole point of the exercise: every line below goes
// through a numbered call and a register convention, and the kernel could
// change completely underneath it without this file noticing.

#ifndef NANO_USER_H
#define NANO_USER_H

// nr in rax, arguments in rdi/rsi/rdx, result in rax. Three arguments is as
// wide as any call here needs; a fourth would mean touching r10, since rcx is
// destroyed by `syscall` on real hardware and this stays close to that shape.
extern long syscall4(long nr, long a, long b, long c);

// These numbers are the interface. They match the SYS_* defines in
// kernel/nano-proc.h; if the two ever disagree the calls silently do the wrong
// thing rather than failing, which is why they are written out in both places
// rather than shared through an include the kernel would not want.
#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_READ   2
#define SYS_OPEN   3
#define SYS_CLOSE  4
#define SYS_SEEK   5
#define SYS_SIZE   6
#define SYS_SBRK   7
#define SYS_GETPID 8
#define SYS_YIELD  9
#define SYS_TICKS  10
#define SYS_UNLINK 11

void exit_(long code)                       { syscall4(SYS_EXIT, code, 0, 0); }
long write(long fd, char *buf, long n)      { return syscall4(SYS_WRITE, fd, (long)buf, n); }
long read(long fd, char *buf, long n)       { return syscall4(SYS_READ, fd, (long)buf, n); }
long open(char *path, long create)          { return syscall4(SYS_OPEN, (long)path, create, 0); }
long close(long fd)                         { return syscall4(SYS_CLOSE, fd, 0, 0); }
long seek(long fd, long pos)                { return syscall4(SYS_SEEK, fd, pos, 0); }
long fsize(long fd)                         { return syscall4(SYS_SIZE, fd, 0, 0); }
long sbrk(long delta)                       { return syscall4(SYS_SBRK, delta, 0, 0); }
long getpid()                               { return syscall4(SYS_GETPID, 0, 0, 0); }
void yield()                                { syscall4(SYS_YIELD, 0, 0, 0); }
long ticks()                                { return syscall4(SYS_TICKS, 0, 0, 0); }
long unlink(char *path)                     { return syscall4(SYS_UNLINK, (long)path, 0, 0); }

// ---------- the small amount of libc a program needs ----------

long ustrlen(char *s) { long n; n = 0; while (s[n]) n = n + 1; return n; }

void *umemset(void *d, int c, long n) {
    char *p; long i;
    p = (char *)d; i = 0;
    while (i < n) { p[i] = c; i = i + 1; }
    return d;
}

int ustrcmp(char *a, char *b) {
    long i;
    i = 0;
    while (a[i] && a[i] == b[i]) i = i + 1;
    return (a[i] & 255) - (b[i] & 255);
}

// One write() per printf rather than one per character. A syscall costs an
// interrupt, a full register save and a scheduler check; a 60-character line
// printed a byte at a time is sixty of those, and on a preemptive kernel the
// output of two processes interleaves mid-word. Buffering makes each line
// atomic as well as cheap.
#define UBUF 512
char g_ubuf[UBUF];
long g_ulen;

void uflush() {
    if (g_ulen) write(1, g_ubuf, g_ulen);
    g_ulen = 0;
}

void uputc(int c) {
    if (g_ulen >= UBUF) uflush();
    g_ubuf[g_ulen] = c;
    g_ulen = g_ulen + 1;
    if (c == '\n') uflush();
}

void puts(char *s) { while (*s) { uputc(*s); s = s + 1; } uflush(); }

void _uput_uint(long n, int base) {
    char buf[32]; int i; char *digits;
    digits = "0123456789abcdef";
    if (n == 0) { uputc('0'); return; }
    i = 0;
    while (n > 0) { buf[i] = digits[n % base]; i = i + 1; n = n / base; }
    while (i > 0) { i = i - 1; uputc(buf[i]); }
}

#define va_list         long
#define va_start(ap, l) __builtin_va_start(ap)
#define va_arg(ap, t)   __builtin_va_arg(ap)
#define va_end(ap)      __builtin_va_end(ap)

// %d %x %c %s %% only -- the same deliberate subset as the kernel's, and the
// same warning applies: a field width is printed literally and then the next
// conversion reads the wrong argument.
void printf(char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    while (*fmt) {
        if (*fmt == '%') {
            fmt = fmt + 1;
            if (*fmt == 'd') {
                long v; v = va_arg(ap, int);
                if (v < 0) { uputc('-'); v = -v; }
                _uput_uint(v, 10);
            } else if (*fmt == 'x') {
                _uput_uint(va_arg(ap, int), 16);
            } else if (*fmt == 's') {
                char *s; s = (char *)va_arg(ap, char *);
                while (*s) { uputc(*s); s = s + 1; }
            } else if (*fmt == 'c') {
                uputc(va_arg(ap, int));
            } else if (*fmt == '%') {
                uputc('%');
            } else {
                uputc('%'); uputc(*fmt);
            }
        } else {
            uputc(*fmt);
        }
        fmt = fmt + 1;
    }
    va_end(ap);
    uflush();
}

#endif
