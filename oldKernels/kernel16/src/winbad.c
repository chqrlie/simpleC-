// winbad.c — a program that misbehaves on purpose, and reports what the
// kernel said.
//
// Every check here is made from the WRONG SIDE of the syscall boundary, which
// is the only side that matters. A kernel test that calls its own clipping
// code and finds it clips proves that the function works; this proves that a
// process cannot get past it.
//
// Same constraints as src/wingl.c: _start first, no C library, syscall
// arguments through globals because nano_cc's __asm__ has no operand
// constraints.
//
// The exit code is a bit mask, one bit per check that FAILED, so a single
// number says which ones and 0 means all of them passed.

void _start() {
    __asm__(
        "call main\n"
        "mov rdi, rax\n"
        "mov rax, 0\n"
        "int 0x80\n"
    );
}

long _n, _a1, _a2, _a3, _a4, _a5, _ret;

void _do_syscall3() {
    __asm__(
        "mov rax, [rip + _n]\n"
        "mov rdi, [rip + _a1]\n"
        "mov rsi, [rip + _a2]\n"
        "mov rdx, [rip + _a3]\n"
        "int 0x80\n"
        "mov [rip + _ret], rax\n"
    );
}

void _do_syscall5() {
    __asm__(
        "mov rax, [rip + _n]\n"
        "mov rdi, [rip + _a1]\n"
        "mov rsi, [rip + _a2]\n"
        "mov rdx, [rip + _a3]\n"
        "mov r10, [rip + _a4]\n"
        "mov r8,  [rip + _a5]\n"
        "int 0x80\n"
        "mov [rip + _ret], rax\n"
    );
}

long sys3(long n, long a, long b, long c) {
    _n = n; _a1 = a; _a2 = b; _a3 = c;
    _do_syscall3();
    return _ret;
}

long sys5(long n, long a, long b, long c, long d, long e) {
    _n = n; _a1 = a; _a2 = b; _a3 = c; _a4 = d; _a5 = e;
    _do_syscall5();
    return _ret;
}

#define SYS_WRITE      1
#define SYS_SBRK       7
#define SYS_WINOPEN    13
#define SYS_WINBLIT    14
#define SYS_WINPRESENT 15
#define SYS_WINPOLL    16
#define SYS_WINCLOSE   17

long slen(char *s) { long n; n = 0; while (s[n]) n = n + 1; return n; }
void say(char *s) { sys3(SYS_WRITE, 1, (long)s, slen(s)); }

#define W 40
#define H 30

long *fb;
char title[8];
long poll[8];

int main(int argc, char **argv) {
    long hnd;
    long bad;
    long n;

    bad = 0;
    say("winbad: testing the boundary from the wrong side of it\n");

    fb = (long *)sys3(SYS_SBRK, W * H * 8, 0, 0);
    if (!fb) { say("sbrk failed\n"); return 1048576; }
    { long i; i = 0; while (i < W * H) { fb[i] = 0x00FF00FF; i = i + 1; } }

    title[0] = 'b'; title[1] = 'a'; title[2] = 'd'; title[3] = 0;
    hnd = sys5(SYS_WINOPEN, 600, 300, 120, 90, (long)title);
    if (hnd < 0) { say("no window\n"); return 2097152; }

    // 1. A window smaller than the minimum is refused rather than created and
    //    then found to be unusable.
    if (sys5(SYS_WINOPEN, 10, 10, 4, 4, (long)title) >= 0) { say("tiny window allowed\n"); bad = bad + 1; }

    // 2. Blitting into a window this process does not own. Handle 0 belongs to
    //    the kernel, which opened it before any process existed.
    if (sys5(SYS_WINBLIT, 0, (long)fb, W, H, 0) != -1) { say("blitted into a kernel window\n"); bad = bad + 2; }

    // 3. A handle that was never a window at all.
    if (sys5(SYS_WINBLIT, 999, (long)fb, W, H, 0) != -1) { say("blitted into handle 999\n"); bad = bad + 4; }
    if (sys5(SYS_WINBLIT, 0 - 1, (long)fb, W, H, 0) != -1) { say("blitted into handle -1\n"); bad = bad + 8; }

    // 4. A blit that runs off the right and bottom edges. It must succeed and
    //    report FEWER pixels than it was given, which is the only observable
    //    proof from out here that it was clipped rather than trusted.
    n = sys5(SYS_WINBLIT, hnd, (long)fb, W, H, 0);
    if (n != W * H) { say("an in-bounds blit was clipped\n"); bad = bad + 16; }
    // The offset is worked out from the client size the kernel reported, not
    // from the window size this program asked for. How much of a window is
    // border and title bar is the window manager's business.
    sys3(SYS_WINPOLL, hnd, (long)poll, 0);
    n = sys5(SYS_WINBLIT, hnd, (long)fb, W, H,
             (poll[5] - 5) * poll[4] + (poll[4] - 5));
    if (n <= 0) { say("an overhanging blit copied nothing at all\n"); bad = bad + 32; }
    if (n >= W * H) { say("an overhanging blit was NOT clipped\n"); bad = bad + 64; }
    if (poll[4] <= 0 || poll[5] <= 0) { say("the client size came back nonsense\n"); bad = bad + 4096; }

    // 5. A null pixel pointer, and a nonsense size. Refused, not dereferenced.
    if (sys5(SYS_WINBLIT, hnd, 0, W, H, 0) != -1) { say("blitted from a null pointer\n"); bad = bad + 128; }
    if (sys5(SYS_WINBLIT, hnd, (long)fb, 0, 0, 0) != -1) { say("a zero-sized blit was accepted\n"); bad = bad + 256; }

    // 6. Polling somebody else's window says "gone", not "here it is".
    if (sys3(SYS_WINPOLL, 0, (long)poll, 0) != 0) { say("polled a kernel window\n"); bad = bad + 512; }

    // 7. After closing, this process's own handle is gone too.
    sys3(SYS_WINPRESENT, hnd, 0, 0);
    sys3(SYS_WINCLOSE, hnd, 0, 0);
    if (sys3(SYS_WINPOLL, hnd, (long)poll, 0) != 0) { say("polled a closed window\n"); bad = bad + 1024; }
    if (sys5(SYS_WINBLIT, hnd, (long)fb, W, H, 0) != -1) { say("blitted into a closed window\n"); bad = bad + 2048; }

    if (bad == 0) say("winbad: the boundary held\n");
    return bad;
}
