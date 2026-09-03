// wingl.c — a graphical program for the OS to compile, assemble and run.
//
// This is the other half of what K8 and K9 built. The machine already had a
// compiler and an assembler inside it; what it did not have was a way for a
// program it produced to put anything on the screen. Now it does, through five
// syscalls, and this is a program that uses them.
//
// It draws a rotating wireframe cube. Everything below is computed by this
// program, in its own address space, with its own arithmetic:
//
//   - a sine table, built at startup from a lookup of 91 whole degrees
//   - a 3x3 rotation, in 16.16 fixed point
//   - the perspective divide
//   - Bresenham's line algorithm
//
// The kernel's renderer is not involved and could not be: a process cannot
// call a kernel function. What crosses the boundary is a WINDOW HANDLE and a
// pointer to this program's own pixels, one blit at a time.
//
// It is written for the `cc --minimal --nasm --bss` path, which is a narrower
// world than the ordinary one. Two consequences shape the file:
//
//   _start MUST BE THE FIRST FUNCTION. The assembler makes the first byte it
//   emits the entry point and nano_cc emits functions in source order, so
//   anything above this is what the machine would start executing.
//
//   THERE IS NO C LIBRARY. Not a small one -- none. The syscall trampolines
//   pass their arguments through globals because nano_cc's __asm__ has no
//   operand constraints: the body is handed to the assembler verbatim.

void _start() {
    __asm__(
        "call main\n"
        "mov rdi, rax\n"
        "mov rax, 0\n"         // SYS_EXIT
        "int 0x80\n"
    );
}

long _n, _a1, _a2, _a3, _a4, _a5, _ret;

// Three arguments, for the calls that predate windows.
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

// Five, which is what the window calls need. r10 and r8 for the fourth and
// fifth, matching the kernel's dispatcher -- and matching Linux, which picked
// r10 for the same reason: `syscall` destroys rcx.
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
#define SYS_YIELD      9
#define SYS_TICKS      10
#define SYS_WINOPEN    13
#define SYS_WINBLIT    14
#define SYS_WINPRESENT 15
#define SYS_WINPOLL    16
#define SYS_WINCLOSE   17

long slen(char *s) { long n; n = 0; while (s[n]) n = n + 1; return n; }
void say(char *s) { sys3(SYS_WRITE, 1, (long)s, slen(s)); }

// ---------- the program's own arithmetic ----------

#define FRAC 16
#define ONE  65536

long fxmul(long a, long b) {
    long p;
    p = a * b;
    if (p < 0) return 0 - ((0 - p + 32768) >> FRAC);
    return (p + 32768) >> FRAC;
}

long fxdiv(long a, long b) {
    if (b == 0) return 0;
    return (a << FRAC) / b;
}

// sin for whole degrees 0..90, in 16.16. Written out rather than computed:
// there is no maths library here, and a series expansion would be more code
// than the table it replaces.
long sintab[91];

void build_sin() {
    long i;
    long x;
    // sin(d) by the small-angle recurrence, corrected. Rather than trust a
    // recurrence to stay on the circle for ninety steps, each entry is built
    // from a rational approximation good to a few parts in 65536:
    //   sin(d) ~ (4d(180-d)) / (40500 - d(180-d))     -- Bhaskara I, 7th c.
    // Its worst error over 0..90 degrees is about 0.0016, which is a hundred
    // units of 1/65536 -- invisible in a cube twenty pixels across, and it
    // costs four multiplies instead of a hundred stored constants.
    i = 0;
    while (i <= 90) {
        x = i * (180 - i);
        sintab[i] = (4 * x * ONE) / (40500 - x);
        i = i + 1;
    }
    sintab[0] = 0;
    sintab[90] = ONE;
}

long isin(long deg) {
    long q;
    long i;
    deg = deg % 360;
    if (deg < 0) deg = deg + 360;
    q = deg / 90;
    i = deg % 90;
    if (q == 0) return sintab[i];
    if (q == 1) return sintab[90 - i];
    if (q == 2) return 0 - sintab[i];
    return 0 - sintab[90 - i];
}

long icos(long deg) { return isin(deg + 90); }

// ---------- the window and the frame buffer this program owns ----------

#define W 160
#define H 120

long *fb;                       // W*H pixels, from sbrk -- this program's own
long hnd;

void plot(long x, long y, long c) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    fb[y * W + x] = c;
}

void line(long x0, long y0, long x1, long y1, long c) {
    long dx; long dy; long sx; long sy; long err;
    dx = x1 - x0; if (dx < 0) dx = 0 - dx;
    dy = y1 - y0; if (dy < 0) dy = 0 - dy;
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx - dy;
    for (;;) {
        long e2;
        plot(x0, y0, c);
        if (x0 == x1 && y0 == y1) return;
        e2 = err * 2;
        if (e2 > (0 - dy)) { err = err - dy; x0 = x0 + sx; }
        if (e2 < dx)       { err = err + dx; y0 = y0 + sy; }
    }
}

long cubex[8];
long cubey[8];
long cubez[8];
long edges[24];
long px[8];
long py[8];

void setc(long i, long x, long y, long z) {
    cubex[i] = x * ONE; cubey[i] = y * ONE; cubez[i] = z * ONE;
}

void sete(long i, long a, long b) { edges[i * 2] = a; edges[i * 2 + 1] = b; }

void build_cube() {
    setc(0, -1, -1, -1); setc(1, 1, -1, -1); setc(2, 1, 1, -1); setc(3, -1, 1, -1);
    setc(4, -1, -1,  1); setc(5, 1, -1,  1); setc(6, 1, 1,  1); setc(7, -1, 1,  1);
    sete(0, 0, 1); sete(1, 1, 2); sete(2, 2, 3); sete(3, 3, 0);
    sete(4, 4, 5); sete(5, 5, 6); sete(6, 6, 7); sete(7, 7, 4);
    sete(8, 0, 4); sete(9, 1, 5); sete(10, 2, 6); sete(11, 3, 7);
}

// Rotate about Y then X, project, and cache the screen position of every
// corner. Doing the eight corners once and then drawing twelve edges from the
// cache is the difference between eight transforms a frame and twenty-four.
void transform(long ay, long ax) {
    long cy; long sy; long cx; long sx;
    long i;
    cy = icos(ay); sy = isin(ay);
    cx = icos(ax); sx = isin(ax);
    i = 0;
    while (i < 8) {
        long x; long y; long z; long t;
        x = cubex[i]; y = cubey[i]; z = cubez[i];
        t = fxmul(x, cy) + fxmul(z, sy);
        z = fxmul(z, cy) - fxmul(x, sy);
        x = t;
        t = fxmul(y, cx) - fxmul(z, sx);
        z = fxmul(z, cx) + fxmul(y, sx);
        y = t;
        z = z + 5 * ONE;                        // push it in front of the eye
        if (z < ONE / 2) z = ONE / 2;
        px[i] = W / 2 + ((fxdiv(x, z) * 130) >> FRAC);
        py[i] = H / 2 - ((fxdiv(y, z) * 130) >> FRAC);
        i = i + 1;
    }
}

void clear(long c) {
    long i;
    i = 0;
    while (i < W * H) { fb[i] = c; i = i + 1; }
}

char msg[64];

int main(int argc, char **argv) {
    long frame;
    long poll[8];
    long spin;

    say("wingl: a program this machine compiled, asking for a window\n");

    build_sin();
    build_cube();

    // The frame buffer is this program's own memory, from its own heap. The
    // kernel never hands out a pointer into the window; it copies.
    fb = (long *)sys3(SYS_SBRK, W * H * 8, 0, 0);
    if (!fb) { say("sbrk failed\n"); return 1; }

    msg[0] = 'c'; msg[1] = 'u'; msg[2] = 'b'; msg[3] = 'e'; msg[4] = 0;
    hnd = sys5(SYS_WINOPEN, 420, 90, W + 4, H + 18, (long)msg);
    if (hnd < 0) { say("no window\n"); return 2; }
    say("got a window handle\n");

    frame = 0;
    spin = 0;
    while (frame < 60) {
        long i;
        long shade;

        // Has somebody closed it? A program whose window has gone would
        // otherwise spin forever blitting into nothing.
        if (!sys3(SYS_WINPOLL, hnd, (long)poll, 0)) break;

        clear(0x00101828);

        // A horizon, so the cube is obviously in a scene rather than floating
        // in a void, and so a blit that lands in the wrong place is obvious.
        i = 0;
        while (i < W) { plot(i, H - 20, 0x00304050); i = i + 1; }

        transform(spin, spin * 2 / 3);
        shade = 0x0060D0FF;
        i = 0;
        while (i < 12) {
            long a;
            long b;
            a = edges[i * 2];
            b = edges[i * 2 + 1];
            line(px[a], py[a], px[b], py[b], shade);
            i = i + 1;
        }

        // A cursor dot, so the poll's answer is visible on screen too.
        if (poll[0] >= 0 && poll[0] < W && poll[1] >= 0 && poll[1] < H) {
            plot(poll[0], poll[1], 0x00FF4040);
            plot(poll[0] + 1, poll[1], 0x00FF4040);
            plot(poll[0], poll[1] + 1, 0x00FF4040);
        }

        sys5(SYS_WINBLIT, hnd, (long)fb, W, H, 0);
        sys3(SYS_WINPRESENT, hnd, 0, 0);

        spin = (spin + 6) % 360;
        frame = frame + 1;
        sys3(SYS_YIELD, 0, 0, 0);
    }

    say("wingl: done, closing the window\n");
    sys3(SYS_WINCLOSE, hnd, 0, 0);
    return 7;
}
