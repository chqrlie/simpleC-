// linebench.c — where does the time in a line actually go?
//
// The question: is drawing a line faster if you halve the stepping and plot
// from both ends, or write it out as spans the way a glyph blit does?
//
// The honest answer is a measurement, so this is one. Four ways of drawing the
// same line, each timed on the ACPI power-management timer at 3.579545 MHz --
// a free-running counter that does not care what the CPU is doing, which is
// the same clock K2 used to prove the idle loop was really idle.
//
//   gl_line        Bresenham, one gl_put per pixel     -- the baseline
//   gl_line_sym    stepped from both ends at once      -- half the loop
//   gl_line_clip   bounds-checked ONCE, direct stores  -- no per-pixel guard
//   gl_line_span   runs batched into horizontal spans  -- the "blit" version
//
// CORRECTNESS COMES FIRST AND IT IS NOT A FORMALITY. A faster line that is one
// pixel different is not a faster line, it is a different line. Every variant
// is required to produce a bit-identical framebuffer to gl_line over a fan of
// several hundred lines at every angle. One of them does not, and that is the
// most interesting result in the file.
//
// nano-kernel.h first: this image reads the framebuffer back and must not have
// console output mirrored onto it.
#include "nano-kernel.h"
#include "nano-fb.h"
#include "nano-acpi.h"
#include "nano-int.h"
#include "nano-mm.h"
#include "nano-wm.h"
#include "nano-gl.h"

long g_fail;

void fail(char *msg) {
    printf("FAIL: %s\n", msg);
    g_fail = g_fail + 1;
}

void expect(char *what, long got, long want) {
    if (got == want) printf("  ok  %s = %d\n", what, got);
    else {
        printf("  got %d, wanted %d\n", got, want);
        fail(what);
    }
}

void expect_true(char *what, long got) {
    if (got) printf("  ok  %s\n", what);
    else fail(what);
}

#define VPX  (WM_BORDER + 4)
#define VPY  (WM_TITLE_H + 4)
#define VPW  320
#define VPH  240

struct GLCtx g_gl;
long g_win3d;


// ---------- the two variants that were measured and rejected ----------
//
// They live here rather than in nano-gl.h because that is where the evidence
// belongs: they are not shipped, they are the reason the shipped one looks the
// way it does.

// 1. SYMMETRIC. A Bresenham line looks symmetric about its midpoint, so the
//    loop should be able to run half as many times and plot from both ends at
//    once. That halves the branches; it does not touch the stores.
//
//    It is also not actually symmetric, which the pixel comparison below is
//    what proves. Walking the error from the far end makes different rounding
//    choices wherever the error term ties, so the two halves meet with a kink.
void gl_line_sym(struct GLCtx *c, long x0, long y0, long x1, long y1, long colour) {
    long dx; long dy; long sx; long sy;
    long ea; long eb;
    long ax; long ay; long bx; long by;
    long n;
    long i;

    dx = x1 - x0; if (dx < 0) dx = 0 - dx;
    dy = y1 - y0; if (dy < 0) dy = 0 - dy;
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    n = dx > dy ? dx : dy;

    ax = x0; ay = y0; ea = dx - dy;
    bx = x1; by = y1; eb = dx - dy;

    i = 0;
    while (i <= n / 2) {
        long e2;
        gl_put(c, ax, ay, colour);
        if (i != n - i) gl_put(c, bx, by, colour);
        e2 = ea * 2;
        if (e2 > (0 - dy)) { ea = ea - dy; ax = ax + sx; }
        if (e2 < dx)       { ea = ea + dx; ay = ay + sy; }
        e2 = eb * 2;
        if (e2 > (0 - dy)) { eb = eb - dy; bx = bx - sx; }
        if (e2 < dx)       { eb = eb + dx; by = by - sy; }
        i = i + 1;
    }
}

// 2. SPANS -- the "draw it like a glyph blit" version. On a shallow line the
//    pixels come in horizontal runs, so count the run and fill it, and the
//    innermost loop has no error term in it at all.
//
//    The run boundaries are taken FROM the Bresenham decisions rather than
//    from a closed form, so the pixels are identical by construction.
void gl_line_span(struct GLCtx *c, long x0, long y0, long x1, long y1, long colour) {
    long dx; long dy; long sx; long sy; long err;
    long *pix;
    long stride;
    long ox;
    long oy;
    long n;
    long run;

    if (!gl_line_inside(c, x0, y0, x1, y1)) { gl_line_slow(c, x0, y0, x1, y1, colour); return; }

    gl_mark(c, x0, y0);
    gl_mark(c, x1, y1);
    pix = g_win[c->win].pix;
    stride = g_win[c->win].w;
    ox = c->vx;
    oy = c->vy;

    dx = x1 - x0; if (dx < 0) dx = 0 - dx;
    dy = y1 - y0; if (dy < 0) dy = 0 - dy;
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx - dy;
    n = (dx > dy ? dx : dy) + 1;
    c->pixels = c->pixels + n;

    if (dx < dy) {
        for (;;) {
            long e2;
            pix[(oy + y0) * stride + ox + x0] = colour;
            if (x0 == x1 && y0 == y1) return;
            e2 = err * 2;
            if (e2 > (0 - dy)) { err = err - dy; x0 = x0 + sx; }
            if (e2 < dx)       { err = err + dx; y0 = y0 + sy; }
        }
    }

    run = x0;
    for (;;) {
        long e2;
        long px;
        long py;
        long a;
        long b;

        px = x0;
        py = y0;
        if (x0 == x1 && y0 == y1) {
            a = run; b = x0;
            if (a > b) { long t; t = a; a = b; b = t; }
            {
                long row;
                row = (oy + y0) * stride + ox;
                while (a <= b) { pix[row + a] = colour; a = a + 1; }
            }
            return;
        }
        e2 = err * 2;
        if (e2 > (0 - dy)) { err = err - dy; x0 = x0 + sx; }
        if (e2 < dx)       { err = err + dx; y0 = y0 + sy; }
        if (y0 != py) {
            a = run; b = px;
            if (a > b) { long t; t = a; a = b; b = t; }
            {
                long row;
                row = (oy + py) * stride + ox;
                while (a <= b) { pix[row + a] = colour; a = a + 1; }
            }
            run = x0;
        }
    }
}

// ---------- the fan of lines every variant is asked to draw ----------
//
// A fan from the centre out to every point on the border. That covers every
// slope, both signs, the axis-aligned cases and the exact diagonals -- the
// four places where a line routine most often disagrees with another one.
#define NLINES 512
long g_lx[NLINES];
long g_ly[NLINES];
long g_n;

void build_fan() {
    long i;
    long step;
    g_n = 0;
    // Walk the border of the viewport, one endpoint every few pixels.
    step = 5;
    i = 0;
    while (i < VPW && g_n < NLINES) { g_lx[g_n] = i; g_ly[g_n] = 0; g_n = g_n + 1; i = i + step; }
    i = 0;
    while (i < VPH && g_n < NLINES) { g_lx[g_n] = VPW - 1; g_ly[g_n] = i; g_n = g_n + 1; i = i + step; }
    i = VPW - 1;
    while (i >= 0 && g_n < NLINES) { g_lx[g_n] = i; g_ly[g_n] = VPH - 1; g_n = g_n + 1; i = i - step; }
    i = VPH - 1;
    while (i >= 0 && g_n < NLINES) { g_lx[g_n] = 0; g_ly[g_n] = i; g_n = g_n + 1; i = i - step; }
}

// `which` selects the variant, because nano_cc has no function pointers --
// which is the same constraint that decided the whole widget architecture in
// K14. A switch on a number is what that leaves, and it is honest: the cost of
// the switch is one compare per LINE, not per pixel, so it does not pollute
// the measurement.
void draw_fan(long which) {
    long i;
    i = 0;
    while (i < g_n) {
        if (which == 0) gl_line_slow(&g_gl, VPW / 2, VPH / 2, g_lx[i], g_ly[i], rgb(90, 200, 255));
        if (which == 1) gl_line_sym(&g_gl, VPW / 2, VPH / 2, g_lx[i], g_ly[i], rgb(90, 200, 255));
        if (which == 2) gl_line(&g_gl, VPW / 2, VPH / 2, g_lx[i], g_ly[i], rgb(90, 200, 255));
        if (which == 3) gl_line_span(&g_gl, VPW / 2, VPH / 2, g_lx[i], g_ly[i], rgb(90, 200, 255));
        i = i + 1;
    }
}

long win_hash() {
    long h;
    long j;
    h = 5381;
    j = 0;
    while (j < VPH) {
        long i;
        i = 0;
        while (i < VPW) {
            h = ((h * 33) + g_win[g_win3d].pix[(VPY + j) * g_win[g_win3d].w + VPX + i])
                & 0xFFFFFFFF;
            i = i + 1;
        }
        j = j + 1;
    }
    return h;
}

// How many pixels of the viewport differ between the buffer and a saved copy.
long *g_ref;

void save_ref() {
    long j;
    j = 0;
    while (j < VPH) {
        long i;
        i = 0;
        while (i < VPW) {
            g_ref[j * VPW + i] =
                g_win[g_win3d].pix[(VPY + j) * g_win[g_win3d].w + VPX + i];
            i = i + 1;
        }
        j = j + 1;
    }
}

long diff_from_ref() {
    long n;
    long j;
    n = 0;
    j = 0;
    while (j < VPH) {
        long i;
        i = 0;
        while (i < VPW) {
            if (g_ref[j * VPW + i] !=
                g_win[g_win3d].pix[(VPY + j) * g_win[g_win3d].w + VPX + i]) n = n + 1;
            i = i + 1;
        }
        j = j + 1;
    }
    return n;
}

char *name(long which) {
    if (which == 0) return "gl_line_slow (baseline, gl_put per pixel)";
    if (which == 1) return "gl_line_sym  (both ends at once)";
    if (which == 2) return "gl_line      (checked once, direct stores)";
    return "gl_line_span (runs batched into spans)";
}

// ---------- correctness ----------

long g_hash[4];
long g_pixels[4];
long g_diff[4];

void test_identical() {
    long w;

    puts("-- 1. do they draw the same line? --\n");

    w = 0;
    while (w < 4) {
        gl_clear(&g_gl);
        g_gl.pixels = 0;
        draw_fan(w);
        g_hash[w] = win_hash();
        g_pixels[w] = g_gl.pixels;
        if (w == 0) { save_ref(); g_diff[0] = 0; }
        else g_diff[w] = diff_from_ref();
        printf("  %s\n", name(w));
        printf("      %d pixels written, %d differ from the baseline\n",
               g_pixels[w], g_diff[w]);
        w = w + 1;
    }

    expect_true("the baseline drew a lot of pixels", g_pixels[0] > 20000);
    expect("gl_line (the fast path) is bit-identical to gl_line_slow", g_diff[2], 0);
    expect("gl_line_span is bit-identical to gl_line_slow", g_diff[3], 0);

    // gl_line_sym is the one being asked about, and it gets its own verdict
    // rather than a pass or a fail: whether stepping from both ends gives the
    // same pixels is exactly the question, and the answer is worth printing
    // whichever way it comes out.
    if (g_diff[1] == 0) {
        puts("  ok  gl_line_sym is bit-identical to gl_line_slow too\n");
    } else {
        printf("  NOTE gl_line_sym differs from gl_line at %d pixels "
               "(%d per mille of what it draws)\n",
               g_diff[1], g_pixels[1] ? g_diff[1] * 1000 / g_pixels[1] : 0);
        puts("       Bresenham is not symmetric: walking the error from the far\n");
        puts("       end makes different rounding choices where it ties, so the\n");
        puts("       two halves meet with a kink. That is why this is measured\n");
        puts("       and not assumed.\n");
    }
}

// ---------- timing ----------

#define REPEATS 6

long time_variant(long which, long rounds) {
    long a;
    long b;
    long r;
    a = pm_timer_read();
    r = 0;
    while (r < rounds) { draw_fan(which); r = r + 1; }
    b = pm_timer_read();
    return pm_timer_delta(a, b);
}

// Microseconds, from the timer's own frequency. Done in this order --
// multiply, then divide -- because the other order throws the answer away.
long to_us(long ticks) { return (ticks * 1000000) / PM_TMR_HZ; }

void test_speed() {
    long w;
    long best[4];
    long rounds;

    puts("\n-- 2. how long do they take? --\n");
    printf("  %d lines per round, timed on the ACPI PM timer at %d Hz\n",
           g_n, PM_TMR_HZ);
    printf("  best of %d rounds, because a preempted run is slow for a reason\n",
           REPEATS);
    printf("  that has nothing to do with the code\n\n");

    rounds = 4;

    // Warm the cache and the branch predictors once, discarded.
    gl_clear(&g_gl);
    draw_fan(0);

    w = 0;
    while (w < 4) {
        long r;
        best[w] = 0;
        r = 0;
        while (r < REPEATS) {
            long t;
            gl_clear(&g_gl);
            t = time_variant(w, rounds);
            if (best[w] == 0 || t < best[w]) best[w] = t;
            r = r + 1;
        }
        w = w + 1;
    }

    w = 0;
    while (w < 4) {
        long us;
        long per;
        us = to_us(best[w]);
        // Per line, in nanoseconds, so the numbers are readable.
        per = (us * 1000) / (g_n * rounds);
        printf("  %s\n", name(w));
        printf("      %d us for %d lines   (%d ns per line, %d%% of baseline)\n",
               us, g_n * rounds, per,
               best[0] ? (best[w] * 100) / best[0] : 0);
        w = w + 1;
    }

    puts("\n  what that says:\n");
    printf("    stepping from both ends:   %d%% of the baseline\n",
           best[0] ? (best[1] * 100) / best[0] : 0);
    printf("    checking bounds once:      %d%% of the baseline\n",
           best[0] ? (best[2] * 100) / best[0] : 0);
    printf("    batching runs into spans:  %d%% of the baseline\n",
           best[0] ? (best[3] * 100) / best[0] : 0);

    // Two PROPERTIES, not three guesses about the ordering. The first draft
    // of this asserted that the span version would be at least as fast as the
    // hoisted one, which is not a property of anything -- it was my prediction,
    // and it was wrong, and the test failed on a correct measurement. Again.
    //
    // What is actually claimed: hoisting the per-pixel guard is a win, and
    // batching runs is a win over the baseline. Which of the two wins by more
    // is a fact about this machine, and it gets PRINTED, not asserted.
    expect_true("checking the bounds once beats checking them per pixel",
                best[2] < best[0]);
    expect_true("batching runs into spans also beats the baseline",
                best[3] < best[0]);
    if (best[1] < best[0])
        puts("  NOTE stepping from both ends was faster here\n");
    else
        printf("  NOTE stepping from both ends was SLOWER: %d%% of the baseline. "
               "It halves the iterations and does two error updates in each, so "
               "the arithmetic is unchanged and the bookkeeping is doubled\n",
               best[0] ? (best[1] * 100) / best[0] : 0);
}

// ---------- where the time actually is ----------
//
// The variants above change three things at once between them. This pulls the
// one suspected of dominating out on its own: the same number of stores, with
// and without gl_put's guard around each.
void test_where() {
    long a;
    long b;
    long bare;
    long guarded;
    long i;
    long j;
    long rounds;

    puts("\n-- 3. the same stores, with and without the guard --\n");

    rounds = 20;

    gl_clear(&g_gl);
    a = pm_timer_read();
    {
        long r;
        r = 0;
        while (r < rounds) {
            j = 0;
            while (j < VPH) {
                i = 0;
                while (i < VPW) {
                    g_win[g_win3d].pix[(VPY + j) * g_win[g_win3d].w + VPX + i] = 7;
                    i = i + 1;
                }
                j = j + 1;
            }
            r = r + 1;
        }
    }
    b = pm_timer_read();
    bare = pm_timer_delta(a, b);

    gl_clear(&g_gl);
    a = pm_timer_read();
    {
        long r;
        r = 0;
        while (r < rounds) {
            j = 0;
            while (j < VPH) {
                i = 0;
                while (i < VPW) { gl_put(&g_gl, i, j, 7); i = i + 1; }
                j = j + 1;
            }
            r = r + 1;
        }
    }
    b = pm_timer_read();
    guarded = pm_timer_delta(a, b);

    printf("  %d pixels stored directly: %d us\n",
           VPW * VPH * rounds, to_us(bare));
    printf("  the same through gl_put:   %d us  (%dx)\n",
           to_us(guarded), bare ? guarded / bare : 0);
    expect_true("the per-pixel guard costs more than the store it guards",
                guarded > bare * 2);
}

void build_window() {
    wm_init(rgb(24, 28, 38));
    g_win3d = wm_create(60, 50, VPW + WM_BORDER * 2 + 8,
                        VPH + WM_TITLE_H + WM_BORDER + 8, "lines");
    wm_decorate(g_win3d);
    if (!gl_bind(&g_gl, g_win3d, VPX, VPY, VPW, VPH)) fail("gl_bind failed");
    wm_present();
}

void run_tests() {
    printf("FB: %dx%d at %d bpp\n", fb_width, fb_height, fb_bpp);
    printf("viewport %dx%d\n\n", VPW, VPH);

    if (!acpi_pm_tmr) {
        puts("no ACPI PM timer -- cannot time anything honestly\n");
        fail("the PM timer");
        puts("\nLINEBENCHTEST DONE\n");
        return;
    }
    printf("PM timer at port 0x%x, %d bits\n\n",
           acpi_pm_tmr, acpi_pm_tmr_32 ? 32 : 24);

    g_ref = (long *)kmalloc(VPW * VPH * 8);
    if (!g_ref) { fail("the comparison buffer"); return; }

    build_fan();
    printf("%d lines in the fan, from the centre to every point on the border\n\n",
           g_n);

    build_window();

    test_identical();
    test_speed();
    test_where();

    printf("\nheap: %d pages mapped, %d bytes free\n", heap_pages, heap_bytes_free());

    if (g_fail) printf("\n%d CHECKS FAILED\n", g_fail);
    else puts("\nPASS: measured, and the answer is not where it looked\n");

    puts("\nLINEBENCHTEST DONE\n");
}

int main() {
    serial_init();
    g_fail = 0;

    puts("\nnano-os: four ways to draw a line, timed\n");

    if (!fb_init(1024, 768)) { puts("fb_init failed\n"); for (;;) { } }
    if (!mm_init())          { puts("mm_init failed\n"); for (;;) { } }

    // acpi_init BEFORE mm_protect_null. The pointer to the EBDA, where the
    // RSDP is looked for, lives at physical 0x40E -- inside the page that
    // mm_protect_null unmaps. K5 hit this exact fault and left a note about
    // it; the note was right.
    if (!acpi_init()) puts("acpi_init found nothing\n");
    mm_protect_null();

    interrupts_init(100);

    run_tests();

    // Leave the fan on screen.
    gl_clear(&g_gl);
    draw_fan(3);
    gl_flush(&g_gl);
    wm_present();
    for (;;) cpu_idle();
    return 0;
}
