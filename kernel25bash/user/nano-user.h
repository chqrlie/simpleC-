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
// Five arguments, which only SYS_WINOPEN needs.
extern long syscall6(long nr, long a, long b, long c, long d, long e);

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
#define SYS_BRK    12
#define SYS_WINOPEN    13
#define SYS_WINBLIT    14
#define SYS_WINPRESENT 15
#define SYS_WINPOLL    16
#define SYS_WINCLOSE   17
#define SYS_NAP        18
#define SYS_TRUNCATE   19
#define SYS_SYNC       20
#define SYS_MKDIR      21
#define SYS_RENAME     22
#define SYS_READDIR    23
#define SYS_ISDIR      24
#define SYS_SPAWN      25
#define SYS_WAIT       26
#define SYS_PIPE       27

// The non-character keys, as delivered by SYS_WINPOLL in out[3].
//
// These are interface numbers in exactly the way the SYS_* values above are,
// and they are written out here for the same stated reason: the kernel's
// nano-int.h defines them for its own use, and a shared include would drag
// the kernel's interrupt handling into every program. They must match
// nano-int.h; uitest asserts the kernel half and apptest asserts that an
// arrow pressed at a PROCESS arrives as KEY_LEFT and not as a letter, which
// is the only check that can catch the two drifting apart.
#define KEY_UP     0x100
#define KEY_DOWN   0x101
#define KEY_LEFT   0x102
#define KEY_RIGHT  0x103
#define KEY_HOME   0x104
#define KEY_END    0x105
#define KEY_PGUP   0x106
#define KEY_PGDN   0x107
#define KEY_DEL    0x108
#define KEY_INS    0x109

// open() flags. Linux's values, because the fuller C library in nano-libc.h
// speaks them and one set of numbers is better than two.
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  64
#define O_TRUNC  512

void exit_(long code)                       { syscall4(SYS_EXIT, code, 0, 0); }
long write(long fd, char *buf, long n)      { return syscall4(SYS_WRITE, fd, (long)buf, n); }
long read(long fd, char *buf, long n)       { return syscall4(SYS_READ, fd, (long)buf, n); }
long open(char *path, long flags)           { return syscall4(SYS_OPEN, (long)path, flags, 0); }
long close(long fd)                         { return syscall4(SYS_CLOSE, fd, 0, 0); }
long seek(long fd, long pos)                { return syscall4(SYS_SEEK, fd, pos, 0); }
long fsize(long fd)                         { return syscall4(SYS_SIZE, fd, 0, 0); }
long sbrk(long delta)                       { return syscall4(SYS_SBRK, delta, 0, 0); }
long getpid()                               { return syscall4(SYS_GETPID, 0, 0, 0); }
void yield()                                { syscall4(SYS_YIELD, 0, 0, 0); }
long ticks()                                { return syscall4(SYS_TICKS, 0, 0, 0); }
long unlink(char *path)                     { return syscall4(SYS_UNLINK, (long)path, 0, 0); }
void nap(long ms)                           { syscall4(SYS_NAP, ms, 0, 0); }

// An editor's Save is "make the file be exactly these bytes", which is a
// truncate and then a write -- without the truncate, saving something shorter
// leaves the tail of the longer version behind it.
long ftruncate_(char *path)                 { return syscall4(SYS_TRUNCATE, (long)path, 0, 0); }
// Blocks written, or -1 if this filesystem is not on a disk at all. A caller
// that cares about durability has to tell that apart from "wrote nothing".
long fsync_()                               { return syscall4(SYS_SYNC, 0, 0, 0); }
long mkdir_(char *path)                     { return syscall4(SYS_MKDIR, (long)path, 0, 0); }
long rename_(char *from, char *to)          { return syscall4(SYS_RENAME, (long)from, (long)to, 0); }
// (path, index, name_out[64]) -> the entry's INODE number, or 0 if there is
// no entry at that index. NOT a boolean -- test for > 0.
long readdir_(char *path, long i, char *nm) { return syscall4(SYS_READDIR, (long)path, i, (long)nm); }
// 1 if the path is a directory, 0 if it is a file, -1 if it does not exist.
// Cannot be inferred from readdir_: calling that on a file reads the file's
// own bytes as directory entries and answers from whatever it finds.
long isdir_(char *path)                     { return syscall4(SYS_ISDIR, (long)path, 0, 0); }

// ---------- starting other programs ----------
//
// spawn(path, argc, argv, outfd, infd) -> pid, or 0 if it could not start.
//
// outfd and infd are descriptors of YOURS to give the child as its stdout and
// stdin; -1 means "inherit the console". That is the whole redirection
// mechanism -- `prog > file` opens the file and passes that fd.
long spawn(char *path, long argc, char **argv, long outfd, long infd) {
    return syscall6(SYS_SPAWN, (long)path, argc, (long)argv, outfd, infd);
}
// The child's exit code once it has finished, or -1 while it is still
// running. Does NOT block -- poll it.
long waitpid_(long pid)                     { return syscall4(SYS_WAIT, pid, 0, 0); }
// out[0] becomes the read end, out[1] the write end. Reading an empty pipe
// blocks until there is data or every writer has closed; writing a full one
// blocks until the reader drains it.
long pipe_(long *out)                       { return syscall4(SYS_PIPE, (long)out, 0, 0); }

// ---------- windows ----------
long win_open(long x, long y, long w, long h, char *title) {
    return syscall6(SYS_WINOPEN, x, y, w, h, (long)title);
}
// Copy a w*h pixel buffer into the window's client area at `off`, which is a
// LINEAR offset -- oy * client_width + ox -- not a pair. Checked against the
// kernel rather than assumed: it computes ox = off % cw and oy = off / cw.
long win_blit(long hnd, long *pix, long w, long h, long off) {
    return syscall6(SYS_WINBLIT, hnd, (long)pix, w, h, off);
}
long win_present(long hnd)                  { return syscall4(SYS_WINPRESENT, hnd, 0, 0); }
// out[6]: mouse x, mouse y, buttons, one key, client width, client height.
long win_poll(long hnd, long *out)          { return syscall4(SYS_WINPOLL, hnd, (long)out, 0); }
long win_close(long hnd)                    { return syscall4(SYS_WINCLOSE, hnd, 0, 0); }

// ---------- the small amount of libc a program needs ----------

long ustrlen(char *s) { long n; n = 0; while (s[n]) n = n + 1; return n; }

void *umemset(void *d, int c, long n) {
    char *p; long i;
    p = (char *)d; i = 0;
    while (i < n) { p[i] = c; i = i + 1; }
    return d;
}

// argv arrives as text. Deliberately returns 0 for anything that is not a
// number rather than reporting an error: every caller here is a test program
// choosing a mode, and none of them has anywhere to report to.
long uatol(char *s) {
    long v;
    long sign;
    v = 0;
    sign = 1;
    if (*s == '-') { sign = -1; s = s + 1; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s = s + 1; }
    return sign * v;
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
