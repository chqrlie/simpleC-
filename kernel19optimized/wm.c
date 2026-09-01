// wm.c — the compositor, measured rather than admired.
//
// A window manager that redraws the whole screen whenever anything moves looks
// exactly like one that redraws only what changed. The picture is identical.
// The only difference is how many writes crossed the bus to get there, and that
// is invisible unless you count it.
//
// So this image counts. Every pixel that reaches video memory goes through one
// of two functions in nano-wm.h, both of which increment a counter, and each
// test prints what it cost.
//
// Counting alone would not be enough either. Skipping work you should have done
// is the cheapest way to make a counter look good, and the resulting screen is
// usually still plausible -- one stale rectangle in a corner. So after every
// damage-tracked frame the framebuffer is read back and hashed, then the same
// scene is repainted in full and hashed again, and the two must be equal. Fast
// is only interesting if the picture is also right.
//
// And there is an off switch, wm_no_damage, which forces a full repaint every
// time. Every test that claims damage tracking helped is run again with it set,
// and has to show the cost going back up. A test that passes with the feature
// disabled is not testing the feature -- I got caught by exactly that on the NX
// work, where a jump to a `ret` faulted whether or not the page was executable.

// nano-kernel.h comes first deliberately. It only mirrors console output onto
// the framebuffer if NANO_FB_H is already defined, and here it must not: this
// image reads the framebuffer back and hashes it, so a printf landing in the
// top-left corner would change the very thing being measured. Console text goes
// to the serial line and to VGA text memory, and the screen stays exactly what
// the compositor put there.
#include "nano-kernel.h"
#include "nano-int.h"
#include "nano-fb.h"
#include "nano-mm.h"
#include "nano-wm.h"

long g_fail;

void fail(char *msg) {
    printf("FAIL: %s\n", msg);
    g_fail = g_fail + 1;
}

// Percentage of a full-screen repaint, to one decimal place, without floats.
void print_cost(char *label, long pixels) {
    long full;
    long tenths;
    full = wm_screen_pixels();
    tenths = (pixels * 1000) / full;
    printf("  %s: %d pixels = %d.%d%% of a full repaint\n",
           label, pixels, tenths / 10, tenths % 10);
}

// Fill a window with something that is cheap to draw but not uniform, so a
// blit that lands at the wrong offset changes the checksum instead of being
// absorbed by a flat colour.
void paint_content(long hnd, long seed) {
    long y;
    long w;
    long h;
    w = g_win[hnd].w;
    h = g_win[hnd].h;
    wm_decorate(hnd);
    y = WM_TITLE_H + 4;
    while (y < h - 4) {
        long x;
        x = 4;
        while (x < w - 4) {
            long v;
            v = ((x * 3 + y * 5 + seed * 37) % 96) + 130;
            wm_win_fill(hnd, x, y, 8, 8, rgb(v, (v * 2) % 200 + 40, 200 - v / 2));
            x = x + 12;
        }
        y = y + 12;
    }
    wm_win_text(hnd, 8, h - 14, "backing buffer", rgb(60, 60, 60));
}

// Repaint the whole screen from scratch and return its hash. Used as the
// reference every damage-tracked frame is compared against.
long reference_checksum() {
    long save;
    long sum;
    save = wm_no_damage;
    wm_no_damage = 1;
    wm_present();
    wm_no_damage = save;
    sum = wm_fb_checksum();
    return sum;
}

// The core assertion: do the incremental thing, hash it, then do the whole
// thing, hash that, and require the screen to be identical either way.
void check_matches_full(char *what) {
    long fast;
    long full;
    fast = wm_fb_checksum();
    full = reference_checksum();
    if (fast == full) printf("  %s: screen matches a full repaint (hash %d)\n", what, fast);
    else {
        printf("  incremental hash %d, full repaint hash %d\n", fast, full);
        fail(what);
    }
}

void run_tests() {
    long a; long b; long c; long d;
    long base_pixels;
    long move_pixels;
    long nodmg_pixels;
    long raise_pixels;
    long small_pixels;
    long i;

    printf("FB: %dx%d at %d bpp, pitch %d\n", fb_width, fb_height, fb_bpp, fb_pitch);
    printf("a full repaint is %d pixels\n\n", wm_screen_pixels());

    wm_init(rgb(24, 28, 38));

    // --- 1. four windows, first paint ---
    puts("-- 1. create four windows and paint once --\n");
    a = wm_create(60, 80, 300, 220, "notepad");
    b = wm_create(300, 200, 320, 240, "files");
    c = wm_create(560, 120, 280, 200, "task manager");
    d = wm_create(180, 420, 360, 260, "picture viewer");
    if (a < 0 || b < 0 || c < 0 || d < 0) { fail("could not create four windows"); }
    paint_content(a, 1);
    paint_content(b, 2);
    paint_content(c, 3);
    paint_content(d, 4);

    wm_reset_counters();
    wm_present();
    base_pixels = wm_pixels;
    print_cost("first full paint", base_pixels);
    printf("  %d row copies\n", wm_blits);

    // Painting front to back and subtracting each window from the region that
    // is left means no pixel on screen should be written twice, even where four
    // windows overlap. Anything above the screen size is overdraw.
    if (base_pixels > wm_screen_pixels())
        fail("a full paint wrote more pixels than the screen has");
    else
        printf("  no pixel written twice (%d spare)\n",
               wm_screen_pixels() - base_pixels);

    // --- 2. move one window four pixels ---
    // The interesting case, and the one the whole layer exists for. The damage
    // is the old rectangle plus the new one; everything else on screen is
    // untouched.
    puts("\n-- 2. move a 320x240 window by 4 pixels --\n");
    wm_reset_counters();
    wm_move(b, 304, 204);
    wm_present();
    move_pixels = wm_pixels;
    print_cost("after the move", move_pixels);
    check_matches_full("move");

    if (move_pixels >= base_pixels) fail("moving one window cost as much as painting everything");
    if (move_pixels > wm_screen_pixels() / 3)
        fail("a small move touched more than a third of the screen");

    // --- 3. the same move with damage tracking switched off ---
    // If this does not cost noticeably more, then test 2 proved nothing.
    puts("\n-- 3. the same move, damage tracking disabled --\n");
    wm_no_damage = 1;
    wm_reset_counters();
    wm_move(b, 300, 200);
    wm_present();
    nodmg_pixels = wm_pixels;
    wm_no_damage = 0;
    print_cost("with the feature off", nodmg_pixels);
    if (nodmg_pixels <= move_pixels)
        fail("disabling damage tracking did not make the frame more expensive");
    else
        printf("  damage tracking saved %d pixels on this frame (%dx less work)\n",
               nodmg_pixels - move_pixels, nodmg_pixels / move_pixels);

    // Put it back where test 2 left it, so later hashes are comparable.
    wm_move(b, 304, 204);
    wm_present();

    // --- 4. a tiny update inside one window ---
    // A blinking cursor, a clock ticking. This should cost almost nothing.
    puts("\n-- 4. repaint an 80x16 strip inside one window --\n");
    wm_win_fill(c, 8, 40, 80, 16, rgb(220, 60, 60));
    wm_reset_counters();
    wm_invalidate(c, 8, 40, 80, 16);
    wm_present();
    small_pixels = wm_pixels;
    print_cost("80x16 update", small_pixels);
    if (small_pixels != 80 * 16)
        printf("  (expected exactly %d)\n", 80 * 16);
    if (small_pixels > 80 * 16)
        fail("an 80x16 update wrote more than 80x16 pixels");
    check_matches_full("small update");

    // --- 5. occlusion ---
    // Raising a window must not repaint the ones it does not cover, and the
    // result still has to be pixel-identical to a full repaint.
    puts("\n-- 5. raise a window that others overlap --\n");
    wm_reset_counters();
    wm_raise(a);
    wm_present();
    raise_pixels = wm_pixels;
    print_cost("raise", raise_pixels);
    check_matches_full("raise");
    if (raise_pixels > wm_screen_pixels() / 4)
        fail("raising one window repainted more than a quarter of the screen");

    // --- 6. hiding a window exposes what was behind it ---
    puts("\n-- 6. hide a window, then show it again --\n");
    wm_reset_counters();
    wm_set_visible(d, 0);
    wm_present();
    print_cost("hide", wm_pixels);
    check_matches_full("hide");

    wm_reset_counters();
    wm_set_visible(d, 1);
    wm_present();
    print_cost("show", wm_pixels);
    check_matches_full("show");

    // --- 7. the damage list overflowing ---
    // More separate rectangles than the list holds. The right answer is to give
    // up and repaint everything, because dropping one leaves a stale patch that
    // nothing will ever correct.
    puts("\n-- 7. overflow the damage list --\n");
    i = 0;
    while (i < WM_MAXDMG + 8) {
        wm_win_fill(a, 4 + (i % 8) * 20, 20 + (i / 8) * 20, 8, 8, rgb(255, 200, 0));
        wm_invalidate(a, 4 + (i % 8) * 20, 20 + (i / 8) * 20, 8, 8);
        i = i + 1;
    }
    // Read the flag before presenting, because presenting clears it. The first
    // version of this check printed it afterwards and so could only ever say
    // "handled" -- a test that cannot fail is not a test, and I would rather
    // find that here than have it sit in the suite looking green.
    {
        long overflowed;
        overflowed = g_dmg_overflow;
        if (g_ndmg > WM_MAXDMG) fail("damage list grew past its bound");
        if (!overflowed) fail("more rectangles than the list holds did not set the overflow flag");
        else puts("  overflow detected, falling back to a full repaint\n");
        wm_reset_counters();
        wm_present();
        print_cost("after overflow", wm_pixels);
        check_matches_full("overflow");
    }

    // --- 8. drag across the screen ---
    // Forty frames of movement, to show the cost is per-frame and stays flat.
    puts("\n-- 8. drag a window 40 steps across the desktop --\n");
    wm_reset_counters();
    i = 0;
    while (i < 40) {
        wm_move(c, 560 - i * 8, 120 + i * 4);
        wm_present();
        i = i + 1;
    }
    printf("  40 frames cost %d pixels total, %d per frame\n",
           wm_pixels, wm_pixels / 40);
    printf("  40 full repaints would have cost %d\n", wm_screen_pixels() * 40);
    print_cost("one dragged frame", wm_pixels / 40);
    check_matches_full("drag");

    // --- 9. what it all cost in memory ---
    printf("\nheap: %d pages mapped, %d bytes free\n", heap_pages, heap_bytes_free());

    if (g_fail) printf("\n%d CHECKS FAILED\n", g_fail);
    else puts("\nPASS: the compositor repaints only what changed, and the screen is identical either way\n");

    puts("\nWMTEST DONE\n");
}

int main() {
    serial_init();
    g_fail = 0;

    puts("\nnano-os: a compositing window manager\n");

    interrupts_init(100);
    if (!fb_init(1024, 768)) { puts("fb_init failed\n"); for (;;) { } }
    if (!mm_init())          { puts("mm_init failed\n"); for (;;) { } }
    mm_protect_null();

    run_tests();
    for (;;) { }
    return 0;
}
