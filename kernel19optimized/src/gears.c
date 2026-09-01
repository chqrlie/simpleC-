// gears.c — the gears, as a PROCESS the machine compiles and runs.
//
// kernel/gears.c is the same program as a kernel image, where calling the
// renderer is just a function call. This one is on the other side of the
// syscall boundary: it is compiled inside the OS by nano_cc, loaded by the ELF
// loader into its own address space, and it cannot call a kernel function at
// all.
//
// So it does not. It LINKS THE RENDERER INTO ITSELF. nano-gl.h and
// nano-glapi.h are compiled into this program, told to leave the window
// manager out with GL_NO_WM, and pointed at pixels this program allocated with
// sbrk. What crosses the boundary is still only a window handle and a blit --
// exactly the wingl.c arrangement from K18, with a real GL behind it instead
// of a hand-written line drawer.
//
// Three things follow from that, and all three are the point:
//
//   THE RENDERER IS THE SAME CODE. Not a port, not a subset -- the same two
//   headers the kernel images use. gl_bind_buf is the only entry point that
//   differs, and it exists because a process has no kmalloc and no window
//   backing buffer to be given.
//
//   THE DEPTH BUFFER IS THIS PROGRAM'S. w*h longs from sbrk. A kernel that
//   handed one out would be handing out a mapping, and there are no shared
//   mappings here.
//
//   MOUSE AND KEYBOARD COME THROUGH ONE POLL. SYS_WINPOLL returns the pointer
//   in client coordinates, the buttons, one keystroke, and the client size.
//   The keystroke arrives only when this window has focus, which is what stops
//   a background program eating the keys meant for whatever you are looking
//   at.
//
// Written for `cc --minimal --nasm --bss`, which is a narrower world than the
// ordinary one:
//
//   _start MUST BE THE FIRST FUNCTION. The assembler makes the first byte it
//   emits the entry point and nano_cc emits functions in source order.
//
//   THERE IS NO C LIBRARY. The syscall trampolines pass their arguments
//   through globals because nano_cc's __asm__ has no operand constraints.

void _start() {
    __asm__(
        "call main\n"
        "mov rdi, rax\n"
        "mov rax, 0\n"         // SYS_EXIT
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
#define SYS_YIELD      9
#define SYS_TICKS      10
#define SYS_WINOPEN    13
#define SYS_WINBLIT    14
#define SYS_WINPRESENT 15
#define SYS_WINPOLL    16
#define SYS_WINCLOSE   17

long slen(char *s) { long n; n = 0; while (s[n]) n = n + 1; return n; }
void say(char *s) { sys3(SYS_WRITE, 1, (long)s, slen(s)); }

void sayn(long v) {
    char buf[24];
    long i;
    long neg;
    neg = v < 0;
    if (neg) v = 0 - v;
    i = 23;
    buf[i] = 0;
    if (v == 0) { i = i - 1; buf[i] = '0'; }
    while (v > 0) { i = i - 1; buf[i] = '0' + v % 10; v = v / 10; }
    if (neg) { i = i - 1; buf[i] = '-'; }
    say(&buf[i]);
}

// ---------- the renderer, compiled into this program ----------
//
// GL_NO_WM leaves out gl_bind, which is the only thing in nano-gl.h that
// needs the window manager and the kernel heap. rgb() is three lines and is
// supplied here rather than dragging in nano-fb.h for it.

long rgb(long r, long g, long b) {
    return ((r & 255) << 16) | ((g & 255) << 8) | (b & 255);
}

// GL_NO_WM leaves out the window binding; GL_NO_FRUSTUM leaves out the
// frustum-culling helpers, which this program never calls.
//
// The second one is not tidiness. The compiler inside the OS has a ceiling on
// how much source it will get through, and this translation unit -- gears.c
// plus both renderer headers -- sits just under it. Adding roughly seven
// hundred tokens of ANY code pushes it over, and what happens then is not an
// error message: cc goes into lex() and never comes out, and the machine stops
// scheduling. Verified by adding a dummy function of that size to the
// unmodified header, which fails identically.
//
// So the headroom is bought back by not lexing what this program does not use.
// Excluded by the preprocessor, the tokens never reach the lexer at all.
#define GL_NO_WM
#define GL_NO_FRUSTUM
#define GL_NO_CAMERA
#include "nano-gl.h"
#include "nano-glapi.h"

// ---------- the window ----------

#define W 260
#define H 200

long *fb;
long *zb;
long hnd;
char title[8];

struct GLCtx gl;

// ---------- the gear, identical to the kernel image's ----------

long g_r1;
long g_r2;

void gear(long inner_radius, long outer_radius, long width, long teeth,
          long tooth_depth) {
    long i;
    long r0;
    long r1;
    long r2;
    long angle;
    long da;
    long hw;
    long full;

    r0 = inner_radius;
    r1 = outer_radius - tooth_depth / 2;
    r2 = outer_radius + tooth_depth / 2;
    g_r1 = r1;
    g_r2 = r2;

    full = 360 * GL_ONE;
    da = full / teeth / 4;
    hw = width / 2;

    glNormal3x(&g_gls, 0, 0, GL_ONE);

    glBegin(&g_gls, GL_QUAD_STRIP);
    i = 0;
    while (i <= teeth) {
        angle = i * full / teeth;
        glVertex3x(&g_gls, fx_mul(r0, gl_cos_fx(angle)), fx_mul(r0, gl_sin_fx(angle)), hw);
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle)), fx_mul(r1, gl_sin_fx(angle)), hw);
        glVertex3x(&g_gls, fx_mul(r0, gl_cos_fx(angle)), fx_mul(r0, gl_sin_fx(angle)), hw);
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                           fx_mul(r1, gl_sin_fx(angle + 3 * da)), hw);
        i = i + 1;
    }
    glEnd(&g_gls);

    glBegin(&g_gls, GL_QUADS);
    i = 0;
    while (i < teeth) {
        angle = i * full / teeth;
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle)), fx_mul(r1, gl_sin_fx(angle)), hw);
        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + da)),
                           fx_mul(r2, gl_sin_fx(angle + da)), hw);
        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + 2 * da)),
                           fx_mul(r2, gl_sin_fx(angle + 2 * da)), hw);
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                           fx_mul(r1, gl_sin_fx(angle + 3 * da)), hw);
        i = i + 1;
    }
    glEnd(&g_gls);

    glNormal3x(&g_gls, 0, 0, 0 - GL_ONE);

    glBegin(&g_gls, GL_QUAD_STRIP);
    i = 0;
    while (i <= teeth) {
        angle = i * full / teeth;
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle)), fx_mul(r1, gl_sin_fx(angle)), 0 - hw);
        glVertex3x(&g_gls, fx_mul(r0, gl_cos_fx(angle)), fx_mul(r0, gl_sin_fx(angle)), 0 - hw);
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                           fx_mul(r1, gl_sin_fx(angle + 3 * da)), 0 - hw);
        glVertex3x(&g_gls, fx_mul(r0, gl_cos_fx(angle)), fx_mul(r0, gl_sin_fx(angle)), 0 - hw);
        i = i + 1;
    }
    glEnd(&g_gls);

    glBegin(&g_gls, GL_QUADS);
    i = 0;
    while (i < teeth) {
        angle = i * full / teeth;
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                           fx_mul(r1, gl_sin_fx(angle + 3 * da)), 0 - hw);
        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + 2 * da)),
                           fx_mul(r2, gl_sin_fx(angle + 2 * da)), 0 - hw);
        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + da)),
                           fx_mul(r2, gl_sin_fx(angle + da)), 0 - hw);
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle)), fx_mul(r1, gl_sin_fx(angle)), 0 - hw);
        i = i + 1;
    }
    glEnd(&g_gls);

    glBegin(&g_gls, GL_QUAD_STRIP);
    i = 0;
    while (i < teeth) {
        long u;
        long v;
        long len;

        angle = i * full / teeth;

        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle)), fx_mul(r1, gl_sin_fx(angle)), hw);
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle)), fx_mul(r1, gl_sin_fx(angle)), 0 - hw);

        u = fx_mul(r2, gl_cos_fx(angle + da)) - fx_mul(r1, gl_cos_fx(angle));
        v = fx_mul(r2, gl_sin_fx(angle + da)) - fx_mul(r1, gl_sin_fx(angle));
        len = fx_sqrt(fx_mul(u, u) + fx_mul(v, v));
        if (len) { u = fx_div(u, len); v = fx_div(v, len); }
        glNormal3x(&g_gls, v, 0 - u, 0);

        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + da)),
                           fx_mul(r2, gl_sin_fx(angle + da)), hw);
        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + da)),
                           fx_mul(r2, gl_sin_fx(angle + da)), 0 - hw);

        glNormal3x(&g_gls, gl_cos_fx(angle), gl_sin_fx(angle), 0);

        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + 2 * da)),
                           fx_mul(r2, gl_sin_fx(angle + 2 * da)), hw);
        glVertex3x(&g_gls, fx_mul(r2, gl_cos_fx(angle + 2 * da)),
                           fx_mul(r2, gl_sin_fx(angle + 2 * da)), 0 - hw);

        u = fx_mul(r1, gl_cos_fx(angle + 3 * da)) - fx_mul(r2, gl_cos_fx(angle + 2 * da));
        v = fx_mul(r1, gl_sin_fx(angle + 3 * da)) - fx_mul(r2, gl_sin_fx(angle + 2 * da));
        len = fx_sqrt(fx_mul(u, u) + fx_mul(v, v));
        if (len) { u = fx_div(u, len); v = fx_div(v, len); }
        glNormal3x(&g_gls, v, 0 - u, 0);

        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                           fx_mul(r1, gl_sin_fx(angle + 3 * da)), hw);
        glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(angle + 3 * da)),
                           fx_mul(r1, gl_sin_fx(angle + 3 * da)), 0 - hw);

        glNormal3x(&g_gls, gl_cos_fx(angle), gl_sin_fx(angle), 0);
        i = i + 1;
    }
    glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(0)), fx_mul(r1, gl_sin_fx(0)), hw);
    glVertex3x(&g_gls, fx_mul(r1, gl_cos_fx(0)), fx_mul(r1, gl_sin_fx(0)), 0 - hw);
    glEnd(&g_gls);

    glBegin(&g_gls, GL_QUAD_STRIP);
    i = 0;
    while (i <= teeth) {
        angle = i * full / teeth;
        glNormal3x(&g_gls, 0 - gl_cos_fx(angle), 0 - gl_sin_fx(angle), 0);
        glVertex3x(&g_gls, fx_mul(r0, gl_cos_fx(angle)), fx_mul(r0, gl_sin_fx(angle)), 0 - hw);
        glVertex3x(&g_gls, fx_mul(r0, gl_cos_fx(angle)), fx_mul(r0, gl_sin_fx(angle)), hw);
        i = i + 1;
    }
    glEnd(&g_gls);
}

long gear1;
long gear2;
long gear3;

long view_rotx;
long view_roty;
long view_rotz;
long dist;

void setup_projection() {
    long h;
    h = (H * GL_ONE) / W;
    gl.far = 60 * GL_ONE;
    glMatrixMode(&g_gls, GL_PROJECTION);
    glLoadIdentity(&g_gls);
    glFrustumx(&g_gls, 0 - GL_ONE, GL_ONE, 0 - h, h, 5 * GL_ONE);
    glMatrixMode(&g_gls, GL_MODELVIEW);
    glLoadIdentity(&g_gls);
    glTranslatex(&g_gls, 0, 0, dist);
}

void build_gears() {
    gear1 = glGenList();
    glNewList(&g_gls, gear1, GL_COMPILE);
    glMaterialx(&g_gls, GL_FRONT, GL_AMBIENT_AND_DIFFUSE,
                (GL_ONE * 8) / 10, GL_ONE / 10, 0);
    gear(GL_ONE, 4 * GL_ONE, GL_ONE, 20, (GL_ONE * 7) / 10);
    glEndList(&g_gls);

    gear2 = glGenList();
    glNewList(&g_gls, gear2, GL_COMPILE);
    glMaterialx(&g_gls, GL_FRONT, GL_AMBIENT_AND_DIFFUSE,
                0, (GL_ONE * 8) / 10, (GL_ONE * 2) / 10);
    gear(GL_ONE / 2, 2 * GL_ONE, 2 * GL_ONE, 10, (GL_ONE * 7) / 10);
    glEndList(&g_gls);

    gear3 = glGenList();
    glNewList(&g_gls, gear3, GL_COMPILE);
    glMaterialx(&g_gls, GL_FRONT, GL_AMBIENT_AND_DIFFUSE,
                (GL_ONE * 2) / 10, (GL_ONE * 2) / 10, GL_ONE);
    gear((GL_ONE * 13) / 10, 2 * GL_ONE, GL_ONE / 2, 10, (GL_ONE * 7) / 10);
    glEndList(&g_gls);
}

void draw_frame(long angle) {
    gl_clear(&gl);
    setup_projection();

    glPushMatrix(&g_gls);
    glRotatex(&g_gls, view_rotx, GL_ONE, 0, 0);
    glRotatex(&g_gls, view_roty, 0, GL_ONE, 0);
    glRotatex(&g_gls, view_rotz, 0, 0, GL_ONE);

    glPushMatrix(&g_gls);
    glTranslatex(&g_gls, 0 - 3 * GL_ONE, 0 - 2 * GL_ONE, 0);
    glRotatex(&g_gls, angle, 0, 0, GL_ONE);
    glCallList(&g_gls, gear1);
    glPopMatrix(&g_gls);

    glPushMatrix(&g_gls);
    glTranslatex(&g_gls, (31 * GL_ONE) / 10, 0 - 2 * GL_ONE, 0);
    glRotatex(&g_gls, 0 - 2 * angle - 9 * GL_ONE, 0, 0, GL_ONE);
    glCallList(&g_gls, gear2);
    glPopMatrix(&g_gls);

    glPushMatrix(&g_gls);
    glTranslatex(&g_gls, 0 - (31 * GL_ONE) / 10, (42 * GL_ONE) / 10, 0);
    glRotatex(&g_gls, 0 - 2 * angle - 25 * GL_ONE, 0, 0, GL_ONE);
    glCallList(&g_gls, gear3);
    glPopMatrix(&g_gls);

    glPopMatrix(&g_gls);
}

// ---------- input ----------
//
// The whole point of the milestone as far as this program is concerned. Drag
// with the left button to turn the view, the arrow keys and w/a/s/d do the
// same from the keyboard, z and Z roll, +/- move the camera, and q or Escape
// quits. One poll returns all of it.

long last_mx;
long last_my;
long dragging;
long keys_seen;
long drags_seen;

// Stage timings, in timer ticks. Accumulated over the whole run rather than
// per frame, because one tick is 10ms and a single frame does not resolve.
long t_draw;
long t_blit;
long t_present;

// Returns 0 when the program should stop.
long handle_input(long *poll) {
    long mx;
    long my;
    long btn;
    long key;

    mx = poll[0];
    my = poll[1];
    btn = poll[2];
    key = poll[3];

    if (btn & 1) {
        if (dragging) {
            // A third of a degree per pixel, in 16.16, so a seven-pixel drag
            // is not rounded to two whole degrees and made to stutter.
            view_roty = view_roty + (mx - last_mx) * (GL_ONE / 3);
            view_rotx = view_rotx + (my - last_my) * (GL_ONE / 3);
            if (mx != last_mx || my != last_my) drags_seen = drags_seen + 1;
        }
        dragging = 1;
        last_mx = mx;
        last_my = my;
    } else {
        dragging = 0;
    }

    if (key) {
        keys_seen = keys_seen + 1;
        if (key == 'q' || key == 27) return 0;
        if (key == 'w') view_rotx = view_rotx + 5 * GL_ONE;
        if (key == 's') view_rotx = view_rotx - 5 * GL_ONE;
        if (key == 'a') view_roty = view_roty + 5 * GL_ONE;
        if (key == 'd') view_roty = view_roty - 5 * GL_ONE;
        if (key == 'z') view_rotz = view_rotz + 5 * GL_ONE;
        if (key == 'Z') view_rotz = view_rotz - 5 * GL_ONE;
        if (key == '+' || key == '=') dist = dist - 2 * GL_ONE;
        if (key == '-') dist = dist + 2 * GL_ONE;
        if (dist < 12 * GL_ONE) dist = 12 * GL_ONE;
        if (dist > 55 * GL_ONE) dist = 55 * GL_ONE;
    }

    return 1;
}

int main(int argc, char **argv) {
    long frame;
    long angle;
    long poll[8];
    long limit;
    long t0;
    long t1;

    say("gears: a program this machine compiled, with the renderer inside it\n");

    // Pixels and depth buffer, both from this program's own heap. A kernel
    // that handed either of them out would be handing out a mapping.
    fb = (long *)sys3(SYS_SBRK, W * H * 8, 0, 0);
    if (!fb) { say("sbrk failed for the framebuffer\n"); return 1; }
    zb = (long *)sys3(SYS_SBRK, W * H * 8, 0, 0);
    if (!zb) { say("sbrk failed for the depth buffer\n"); return 2; }

    if (!gl_bind_buf(&gl, fb, W, H, zb)) { say("gl_bind_buf failed\n"); return 3; }
    gl_state_init(&g_gls, &gl);

    glLightDirx(&g_gls, 5 * GL_ONE, 5 * GL_ONE, 10 * GL_ONE);
    glFrontFace(&g_gls, GL_CCW);
    glEnable(&g_gls, GL_CULL_FACE);
    glEnable(&g_gls, GL_LIGHTING);
    glEnable(&g_gls, GL_DEPTH_TEST);

    view_rotx = 20 * GL_ONE;
    view_roty = 30 * GL_ONE;
    view_rotz = 0;
    dist = 40 * GL_ONE;

    build_gears();
    say("built three gears into display lists, ");
    sayn(glListSize(gear1) + glListSize(gear2) + glListSize(gear3));
    say(" commands\n");

    title[0] = 'g'; title[1] = 'e'; title[2] = 'a'; title[3] = 'r';
    title[4] = 's'; title[5] = 0;
    hnd = sys5(SYS_WINOPEN, 380, 70, W + 4, H + 18, (long)title);
    if (hnd < 0) { say("no window\n"); return 4; }

    // argv[1], if the shell gave one, is how many frames to draw. Without it
    // the program runs until its window is closed or somebody presses q --
    // which is what you want when it is on the desktop, and not what you want
    // when a test is driving it.
    limit = 0;
    if (argc > 1) {
        long i;
        i = 0;
        limit = 0;
        while (argv[1][i] >= '0' && argv[1][i] <= '9') {
            limit = limit * 10 + (argv[1][i] - '0');
            i = i + 1;
        }
    }
    // Forty is enough to prove it animates and cheap enough that a test
    // harness can wait for it. Software rendering nine hundred triangles into
    // 52,000 pixels is not fast on an emulated machine, and the first version
    // of this defaulted to 240 and outlived the harness that was watching it.
    if (limit <= 0) limit = 40;

    frame = 0;
    angle = 0;
    keys_seen = 0;
    drags_seen = 0;
    t0 = sys3(SYS_TICKS, 0, 0, 0);

    while (frame < limit) {
        long ta;
        long tb;

        if (!sys3(SYS_WINPOLL, hnd, (long)poll, 0)) break;
        if (!handle_input(poll)) break;

        // Three stages, timed separately. Where the time goes is not obvious
        // from reading the code -- the rasteriser and the blit both touch
        // 52,000 pixels -- and a profile that names the stage is the only way
        // to optimise the one that matters instead of the one that looks slow.
        ta = sys3(SYS_TICKS, 0, 0, 0);
        draw_frame(angle);
        tb = sys3(SYS_TICKS, 0, 0, 0);
        t_draw = t_draw + (tb - ta);

        ta = tb;
        sys5(SYS_WINBLIT, hnd, (long)fb, W, H, 0);
        tb = sys3(SYS_TICKS, 0, 0, 0);
        t_blit = t_blit + (tb - ta);

        ta = tb;
        sys3(SYS_WINPRESENT, hnd, 0, 0);
        tb = sys3(SYS_TICKS, 0, 0, 0);
        t_present = t_present + (tb - ta);

        angle = angle + 2 * GL_ONE;
        if (angle >= 360 * GL_ONE) angle = angle - 360 * GL_ONE;
        frame = frame + 1;
        sys3(SYS_YIELD, 0, 0, 0);
    }

    t1 = sys3(SYS_TICKS, 0, 0, 0);

    say("gears: ");
    sayn(frame);
    say(" frames in ");
    sayn(t1 - t0);
    say(" ticks, ");
    sayn(g_gls.tris);
    say(" triangles, ");
    sayn(keys_seen);
    say(" keys, ");
    sayn(drags_seen);
    say(" drags\n");

    say("gears: draw ");
    sayn(t_draw);
    say(" blit ");
    sayn(t_blit);
    say(" present ");
    sayn(t_present);
    say(" ticks\n");

    sys3(SYS_WINCLOSE, hnd, 0, 0);

    // A bit mask, as winbad does: 0 means every one of these held.
    {
        long bad;
        bad = 0;
        if (frame == 0) bad = bad + 1;
        if (g_gls.tris == 0) bad = bad + 2;
        if (g_gls.overflow != 0) bad = bad + 4;
        if (g_list_over != 0) bad = bad + 8;
        if (glListSize(gear1) != 648) bad = bad + 16;
        return bad;
    }
}
