// wmin.c — the mouse, the pointer, and a console in a window.
//
// The hard part of this milestone is not the driver. It is that almost none of
// it can be tested by looking at the screen. A cursor that leaves a trail, a
// packet decoder that is one byte out of step, a click that lands on the window
// behind the one you can see -- all of those produce a picture that is broadly
// plausible, and the only honest way to find them is to assert on numbers.
//
// So the decoder in nano-mouse.h is a pure function of a byte stream, and this
// image feeds it byte streams: negative movements, the overflow bits set, a
// deliberately desynchronised stream. No mouse is moved and no human looks at
// anything.
//
// The pointer is checked the same way the compositor was in K11: do the cheap
// thing, hash the framebuffer, do the expensive thing, hash it again, and
// require the two to be identical. A cursor drawn on top of a compositor is
// precisely the sort of feature that can look right while quietly leaving stale
// pixels behind, because the stale pixels are usually under the cursor.
//
// nano-kernel.h first, as in wm.c: it only mirrors console output onto the
// framebuffer if NANO_FB_H is already defined, and here it must not, because
// this image reads the framebuffer back and hashes it.
#include "nano-kernel.h"
#include "nano-fb.h"
#include "nano-mouse.h"
#include "nano-int.h"
#include "nano-mm.h"
#include "nano-wm.h"
#include "nano-wmin.h"
#include "nano-term.h"

long g_fail;

void fail(char *msg) {
    printf("FAIL: %s\n", msg);
    g_fail = g_fail + 1;
}

// The diagnostic goes on its own line and the word FAIL starts the next one.
// The first version printed "  got 246, wanted 500 -- " and then called fail(),
// which put FAIL in the middle of a line -- and the sabotage script greps for
// it at the start of one, so eight deliberately broken builds were reported as
// passing. The suite was fine; the thing reading the suite was not.
void expect(char *what, long got, long want) {
    if (got == want) printf("  ok  %s = %d\n", what, got);
    else {
        printf("  got %d, wanted %d\n", got, want);
        fail(what);
    }
}

void print_cost(char *label, long pixels) {
    long full;
    long tenths;
    full = wm_screen_pixels();
    tenths = (pixels * 1000) / full;
    printf("  %s: %d pixels = %d.%d%% of a full repaint\n",
           label, pixels, tenths / 10, tenths % 10);
}

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

void check_matches_full(char *what) {
    long fast;
    long full;
    fast = wm_fb_checksum();
    full = reference_checksum();
    if (fast == full) printf("  ok  %s: screen matches a full repaint\n", what);
    else {
        printf("  incremental hash %d, full repaint hash %d\n", fast, full);
        fail(what);
    }
}

// ---------- injecting mouse packets ----------
//
// Exactly the bytes the hardware would produce, fed to exactly the function the
// interrupt handler feeds. dx and dy are in the MOUSE's convention, where a
// positive dy means the mouse moved away from you and the cursor goes up.
void inject_raw(long b0, long b1, long b2) {
    mouse_byte(b0);
    mouse_byte(b1);
    mouse_byte(b2);
}

void inject(long dx, long dy, long btn) {
    long b0;
    b0 = 8 | (btn & 7);
    if (dx < 0) { b0 = b0 | 0x10; dx = dx + 256; }
    if (dy < 0) { b0 = b0 | 0x20; dy = dy + 256; }
    inject_raw(b0, dx & 0xFF, dy & 0xFF);
}

// The same, but in screen terms: positive dy moves the pointer DOWN.
void inject_screen(long dx, long dy, long btn) { inject(dx, 0 - dy, btn); }

// Move the pointer to an absolute place and press/release there. Warping is not
// something a real mouse can do, but a test that has to travel to a coordinate
// by accumulating relative packets is a test about arithmetic, not about
// clicking.
void click_at(long x, long y) {
    mouse_warp(x, y);
    inject(0, 0, 1);            // press
    wm_pump_mouse();
    inject(0, 0, 0);            // release
    wm_pump_mouse();
}

void press_at(long x, long y) {
    mouse_warp(x, y);
    inject(0, 0, 1);
    wm_pump_mouse();
}

void release() {
    inject(0, 0, 0);
    wm_pump_mouse();
}

// Drag the pointer to (x,y) with the button still down, in one step.
void drag_to(long x, long y) {
    mouse_warp(x, y);
    inject(0, 0, 1);
    wm_pump_mouse();
}

// ---------- reading the window's own buffer back ----------
//
// check_matches_full compares an incremental frame against a full repaint of
// the SAME backing buffers, so it cannot see a bug that is in the buffers. If
// wm_set_focus forgot to redraw the window losing focus, both frames would
// agree perfectly on a wrong picture. These two look at the buffer itself.

// A title bar pixel clear of both the title text and the close box.
long titlebar_pixel(long hnd) {
    return g_win[hnd].pix[2 * g_win[hnd].w + g_win[hnd].w / 2];
}

// How many pixels of the client area are still the window manager's background
// -- that is, how much of it the program in the window never painted. For a
// terminal the answer must be zero: it fills every cell, including the blank
// ones. A flush that skipped cells whose contents had not changed would leave
// the manager's grey showing through, and no framebuffer checksum can see that,
// because both the incremental frame and the full repaint are built from the
// same buffer.
long client_unpainted(long hnd) {
    long y;
    long n;
    n = 0;
    y = WM_TITLE_H;
    while (y < g_win[hnd].h - WM_BORDER) {
        long x;
        x = WM_BORDER;
        while (x < g_win[hnd].w - WM_BORDER) {
            if (g_win[hnd].pix[y * g_win[hnd].w + x] == g_win[hnd].bg) n = n + 1;
            x = x + 1;
        }
        y = y + 1;
    }
    return n;
}

// How many cells are drawn in inverse -- which is to say, how many text cursors
// are on screen. There must be exactly one, always.
//
// Column 7 of every glyph in nano-font.h is empty: no byte in the font has bit
// 7 set. So the pixel at the right-hand edge of a cell is that cell's
// BACKGROUND whatever character is in it, which makes it an exact probe for
// whether the cell was drawn inverted.
long term_cursor_blocks(long ti) {
    long hnd;
    long r;
    long n;
    hnd = g_term[ti].win;
    n = 0;
    r = 0;
    while (r < g_term[ti].rows) {
        long c;
        c = 0;
        while (c < g_term[ti].cols) {
            long px;
            long py;
            px = WM_BORDER + c * TERM_CELL_W + TERM_CELL_W - 1;
            py = WM_TITLE_H + r * TERM_CELL_H;
            if (g_win[hnd].pix[py * g_win[hnd].w + px] == g_term[ti].fg) n = n + 1;
            c = c + 1;
        }
        r = r + 1;
    }
    return n;
}

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
}

// ============================================================
// 1. the packet decoder, as a pure function of bytes
// ============================================================
void test_decoder() {
    puts("-- 1. the three-byte packet, decoded from injected bytes --\n");

    mouse_state_reset();
    mouse_bounds(1024, 768);
    mouse_warp(500, 400);

    inject(10, 0, 0);
    expect("dx +10 moves right", g_mouse_x, 510);
    expect("...and not down", g_mouse_y, 400);

    // The one that is wrong in every driver written without a test: dx arrives
    // as an unsigned byte, and the sign lives in a bit of the FIRST byte.
    // Without sign extension 0xF6 is 246, and the cursor jumps right when the
    // mouse moves left.
    inject(-10, 0, 0);
    expect("dx -10 moves back left", g_mouse_x, 500);

    // And the axis flip. The mouse's Y grows away from the user; the screen's
    // grows downward. A driver that copies dy straight through gives a pointer
    // that goes up when you push the mouse forward, which everybody notices
    // and nobody can immediately explain.
    inject(0, 5, 0);
    expect("mouse dy +5 moves the pointer UP", g_mouse_y, 395);
    inject(0, -5, 0);
    expect("mouse dy -5 moves it back down", g_mouse_y, 400);

    inject(0, 0, 1);
    expect("left button down", g_mouse_btn, 1);
    inject(0, 0, 2);
    expect("right button", g_mouse_btn, 2);
    inject(0, 0, 0);
    expect("buttons released", g_mouse_btn, 0);

    // Clamping. The pointer must not be able to leave the screen: every packet
    // is relative, so once it is outside there is no way to bring it back.
    mouse_warp(500, 400);
    inject(-127, 0, 0); inject(-127, 0, 0); inject(-127, 0, 0);
    inject(-127, 0, 0); inject(-127, 0, 0);
    expect("cannot go left of 0", g_mouse_x, 0);
    {
        long i;
        i = 0;
        while (i < 12) { inject(127, -127, 0); i = i + 1; }
    }
    expect("cannot go past the right edge", g_mouse_x, 1023);
    expect("cannot go past the bottom edge", g_mouse_y, 767);

    // The overflow bits. When the mouse itself overflowed, the magnitude byte
    // is meaningless; using it anyway throws the pointer across the screen.
    mouse_warp(500, 400);
    {
        long before;
        before = g_mouse_dropped_ovf;
        inject_raw(8 | 0x40, 200, 200);         // X overflow
        expect("an X-overflow packet is discarded", g_mouse_dropped_ovf, before + 1);
        expect("...and the pointer did not move", g_mouse_x, 500);
        inject_raw(8 | 0x80, 200, 200);         // Y overflow
        expect("a Y-overflow packet is discarded", g_mouse_dropped_ovf, before + 2);
        expect("...and it still did not move", g_mouse_y, 400);
    }

    // Resynchronisation. Byte 0 always has bit 3 set. Feed a stream that is one
    // byte out of step and the driver has to find the boundary again rather
    // than decode nonsense forever.
    puts("\n  a desynchronised stream:\n");
    mouse_state_reset();
    mouse_bounds(1024, 768);
    mouse_warp(500, 400);
    {
        long before;
        before = g_mouse_resync;
        mouse_byte(0x00);       // not a first byte: bit 3 clear
        mouse_byte(0x04);       // nor this
        mouse_byte(0x02);       // nor this
        expect("three bad bytes rejected", g_mouse_resync, before + 3);
        expect("no packet decoded from them", g_mouse_packets, 0);
        inject(7, 0, 0);        // now a good one
        expect("the next real packet still decodes", g_mouse_x, 507);
    }

    // A truncated packet, then a stream of good ones.
    //
    // The packet that straddles the truncation is garbage and there is nothing
    // to be done about that: the protocol carries no length and no marker, so
    // the decoder cannot know two of its three bytes came from a different
    // packet. What matters is that it does not stay wrong. The bit-3 check
    // makes the two orphaned bytes fail to open a packet, and the stream
    // re-locks on the next real first byte.
    //
    // So the assertion is not about where the pointer ends up during the mess.
    // It is that once re-locked, decoding is EXACT -- ten packets of +1 move it
    // by exactly ten. A decoder permanently one byte out of step cannot do
    // that, and neither can one that "recovers" onto the wrong boundary.
    mouse_state_reset();
    mouse_bounds(1024, 768);
    mouse_warp(500, 400);
    {
        long resync_before;
        long settled;
        long i;

        resync_before = g_mouse_resync;
        mouse_byte(0x08);       // open a packet
        mouse_byte(3);          // dx = 3
        // ...and no third byte. The stream simply restarts:
        i = 0;
        while (i < 4) { inject(1, 0, 0); i = i + 1; }

        if (g_mouse_resync <= resync_before)
            fail("the orphaned bytes were accepted as a packet header");
        else
            printf("  ok  the orphaned bytes were rejected (%d resyncs)\n",
                   g_mouse_resync - resync_before);

        settled = g_mouse_x;
        i = 0;
        while (i < 10) { inject(1, 0, 0); i = i + 1; }
        expect("after re-locking, 10 packets of +1 move exactly 10",
               g_mouse_x - settled, 10);
    }

    printf("  totals: %d packets, %d resyncs, %d overflow drops\n",
           g_mouse_packets, g_mouse_resync, g_mouse_dropped_ovf);
}

// ============================================================
// 2. the event queue
// ============================================================
void test_queue() {
    struct MEvent e;
    long n;
    long i;

    puts("\n-- 2. the event queue coalesces motion but never a click --\n");

    mouse_state_reset();
    mouse_bounds(1024, 768);
    mouse_warp(100, 100);
    while (mouse_pop(&e)) { }           // drain the warp

    // Far more motion events than the ring holds. All of them have the same
    // button state, so they must collapse into one.
    i = 0;
    while (i < 200) { inject(1, 0, 0); i = i + 1; }
    n = 0;
    while (mouse_pop(&e)) { n = n + 1; }
    expect("200 motions collapse to one event", n, 1);
    expect("...carrying the final position", e.x, 300);
    expect("nothing was dropped", g_mev_dropped, 0);

    // Now the same volume, but with the button changing every time. These
    // cannot be coalesced and must not be silently lost either.
    i = 0;
    while (i < 20) {
        inject(1, 0, 1);
        inject(1, 0, 0);
        i = i + 1;
    }
    n = 0;
    while (mouse_pop(&e)) { n = n + 1; }
    expect("40 button changes all survive", n, 40);
    expect("still nothing dropped", g_mev_dropped, 0);

    // A CLICK WITH A HAND ON THE END OF IT.
    //
    // The press and the movement just after it carry the same button mask, so
    // the coalescing rule above was entitled to merge them -- and merging them
    // delivers the press at the position the mouse ENDED UP. The event was
    // never lost; it was moved. Nobody holds a mouse still while clicking it,
    // so this is not an edge case, it is every click.
    mouse_state_reset();
    mouse_bounds(1024, 768);
    mouse_warp(400, 300);
    while (mouse_pop(&e)) { }
    inject(0, 0, 1);                                    // press at (400, 300)
    i = 0;
    while (i < 6) { inject(5, 0, 1); i = i + 1; }       // and drift 30 right
    {
        long px;
        long py;
        long seen;
        px = 0 - 1; py = 0 - 1; seen = 0;
        n = 0;
        while (mouse_pop(&e)) {
            n = n + 1;
            if (e.btn == 1 && !seen) { px = e.x; py = e.y; seen = 1; }
        }
        expect("the press is delivered where the button went down", px, 400);
        expect("...and at the y it went down at", py, 300);
        expect("...with the drift after it as a second event", n, 2);
    }
}

// ============================================================
// 3. the pointer on screen
// ============================================================
void test_cursor() {
    long a;
    long b;
    long move_pixels;
    long nodmg_pixels;

    puts("\n-- 3. the pointer: drawn on top, erased by the compositor --\n");

    wm_init(rgb(24, 28, 38));
    a = wm_create(80, 90, 320, 240, "notepad");
    b = wm_create(420, 220, 300, 220, "files");
    paint_content(a, 1);
    paint_content(b, 2);
    wm_set_focus(a);

    wm_cursor_show(1);
    wm_cursor_move(500, 300);
    wm_present();
    check_matches_full("pointer drawn");

    // The measurement this milestone exists for. A pointer is the thing that
    // moves most often on a desktop; if moving it costs a full repaint then
    // every saving K11 made is undone by the mouse.
    wm_reset_counters();
    wm_cursor_move(508, 306);
    wm_present();
    move_pixels = wm_pixels;
    print_cost("moving the pointer 8 pixels", move_pixels);
    printf("  of which the pointer glyph itself was %d\n", g_cur_pixels);
    check_matches_full("pointer moved");

    if (move_pixels > CUR_W * CUR_H * 3)
        fail("moving the pointer cost more than three pointer-sized rectangles");

    // And the control. If this does not cost far more, the number above proved
    // nothing.
    wm_no_damage = 1;
    wm_reset_counters();
    wm_cursor_move(516, 312);
    wm_present();
    nodmg_pixels = wm_pixels;
    wm_no_damage = 0;
    print_cost("the same move, damage tracking off", nodmg_pixels);
    if (nodmg_pixels <= move_pixels * 10)
        fail("disabling damage tracking barely changed the cost of a pointer move");
    else
        printf("  ok  %dx cheaper with damage tracking\n", nodmg_pixels / move_pixels);

    // Drag the pointer across both windows and the desktop, then check that
    // nothing was left behind. This is the failure a save-and-restore cursor
    // gives you: the saved pixels are from before the window moved.
    puts("\n  200 pointer moves across two windows and the desktop:\n");
    wm_reset_counters();
    {
        long i;
        i = 0;
        while (i < 200) {
            wm_cursor_move(60 + i * 3, 100 + i);
            wm_present();
            i = i + 1;
        }
    }
    printf("  200 frames cost %d pixels, %d per frame\n",
           wm_pixels, wm_pixels / 200);
    printf("  200 full repaints would have cost %d\n", wm_screen_pixels() * 200);
    check_matches_full("no trail after 200 moves");

    // The pointer must not be in any window's backing buffer. If it were, the
    // window would carry a copy of it around when dragged. Checked by moving
    // the pointer into a window, then moving the window away and asserting the
    // screen still matches a full repaint -- which it cannot if a cursor was
    // baked into the buffer.
    wm_cursor_move(g_win[a].x + 100, g_win[a].y + 100);
    wm_present();
    wm_move(a, g_win[a].x + 200, g_win[a].y + 40);
    wm_present();
    check_matches_full("window dragged out from under the pointer");

    // A POINTER THAT HAS NOT MOVED MUST NOT BE REDRAWN.
    //
    // wm_present used to lift the pointer and put it back on every single
    // call, whether or not anything had changed. That is invisible in a test
    // and extremely visible on a monitor: the desktop loop at the end of
    // oswin.c presents as fast as the CPU will go, so the pointer was being
    // taken off the screen and put back thousands of times a second, and a
    // 60Hz display caught it absent about as often as it caught it there.
    // Reported as "the mouse was flickering bad", and it was.
    puts("\n  presenting with a still pointer:\n");
    wm_cursor_move(600, 400);
    wm_present();
    wm_reset_counters();
    {
        long i;
        i = 0;
        while (i < 50) { wm_present(); i = i + 1; }
    }
    printf("  50 idle presents cost %d pixels, %d of them the pointer\n",
           wm_pixels, g_cur_pixels);
    // One redraw of the glyph is about 110 pixels, so the old version scored
    // 5,500 here. Anything above zero is the flicker.
    if (g_cur_pixels != 0)
        fail("an idle present repainted a pointer that had not moved");

    // And the control, which is the half that matters: the saving must not be
    // a pointer that has quietly stopped being drawn at all. Repaint the
    // ground underneath it and it MUST come back on top.
    wm_reset_counters();
    wm_damage(600 - 4, 400 - 4, CUR_W + 8, CUR_H + 8);
    wm_present();
    printf("  repainting the ground under it redrew %d pointer pixels\n",
           g_cur_pixels);
    if (g_cur_pixels == 0)
        fail("the pointer was painted over and never put back");
    check_matches_full("pointer back on top after the ground was repainted");

    wm_cursor_show(0);
    wm_present();
    check_matches_full("pointer hidden");
    wm_cursor_show(1);
    wm_present();
    // Named, rather than left for a later section to trip over. Hiding the
    // pointer damages the rectangle it was in, so it is no longer on screen --
    // and the first version of the "do not redraw a pointer that has not
    // moved" rule read the stale "it is already painted" flag, decided there
    // was nothing to do, and left the desktop with no pointer at all. The only
    // thing that caught it was a checksum mismatch sixty lines later, in the
    // console test, which is not where anybody would look.
    check_matches_full("pointer shown again");
}

// ============================================================
// 4. hit testing
// ============================================================
void test_hit() {
    long a;
    long b;

    puts("\n-- 4. hit testing walks the z-order the other way --\n");

    wm_init(rgb(24, 28, 38));
    wmin_init();
    a = wm_create(100, 100, 300, 200, "back");
    b = wm_create(200, 150, 300, 200, "front");
    paint_content(a, 1);
    paint_content(b, 2);
    wm_present();

    expect("a point only in the back window", wm_hit_win(120, 250), a);
    expect("a point only in the front window", wm_hit_win(450, 300), b);

    // The overlap. Both windows contain this point; the answer is the one you
    // can actually see. Painting walks the order back to front and hit testing
    // walks it front to back, and getting that backwards gives clicks that
    // land on whatever is hidden.
    expect("the overlap belongs to the front window", wm_hit_win(250, 200), b);
    expect("the desktop is -1", wm_hit_win(900, 700), -1);

    expect("title bar", wm_hit_part(b, 210, 155), WM_PART_TITLE);
    expect("client area", wm_hit_part(b, 210, 250), WM_PART_CLIENT);
    expect("outside the window", wm_hit_part(b, 10, 10), WM_PART_NONE);

    // The close box is inside the title bar, so it has to be tested first.
    // Testing the title bar first gives a close button that starts a drag.
    {
        long cx;
        long cy;
        cx = g_win[b].x + wm_close_x(b) + 2;
        cy = g_win[b].y + wm_close_y() + 2;
        expect("the close box, not the title bar", wm_hit_part(b, cx, cy), WM_PART_CLOSE);
        expect("just left of it is title bar",
               wm_hit_part(b, g_win[b].x + wm_close_x(b) - 4, cy), WM_PART_TITLE);
    }

    // Raise the back window and the answer in the overlap must change. A hit
    // test that caches anything gets this wrong.
    wm_raise(a);
    expect("after raising, the overlap belongs to the other one",
           wm_hit_win(250, 200), a);
}

// ============================================================
// 5. clicking, dragging, focus, closing
// ============================================================
void test_interaction() {
    long a;
    long b;
    long ox;
    long oy;

    puts("\n-- 5. what a click does --\n");

    wm_init(rgb(24, 28, 38));
    wmin_init();
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);
    a = wm_create(100, 100, 300, 200, "alpha");
    b = wm_create(500, 300, 300, 200, "beta");
    paint_content(a, 1);
    paint_content(b, 2);
    wm_cursor_show(1);
    wm_set_focus(a);
    wm_present();

    // Click-to-raise and focus.
    click_at(600, 400);
    expect("clicking beta focuses it", g_focus, b);
    expect("...and raises it to the front", g_order[g_nwin - 1], b);

    // Both title bars must have been repainted, not just the new one. Read
    // from the backing buffers, because a checksum of the screen against a
    // full repaint of those same buffers agrees either way.
    expect("beta's title bar is lit", titlebar_pixel(b), g_win[b].accent);
    expect("alpha's title bar went dim", titlebar_pixel(a), g_win[a].dim);

    click_at(150, 200);
    expect("clicking alpha focuses it back", g_focus, a);
    expect("...and it is now in front", g_order[g_nwin - 1], a);
    expect("alpha's title bar is lit again", titlebar_pixel(a), g_win[a].accent);
    expect("beta's title bar went dim", titlebar_pixel(b), g_win[b].dim);

    // Clicking the desktop drops focus. Without this, keystrokes keep going to
    // a window the user has visibly clicked away from.
    click_at(950, 720);
    expect("clicking the desktop focuses nothing", g_focus, -1);

    // Dragging by the title bar.
    puts("\n  drag by the title bar:\n");
    ox = g_win[a].x;
    oy = g_win[a].y;
    press_at(ox + 40, oy + 6);          // 40,6 into the title bar
    expect("a press on the title bar starts a drag", g_drag_win, a);
    drag_to(ox + 140, oy + 106);
    expect("the window followed in x", g_win[a].x, ox + 100);
    expect("...and in y", g_win[a].y, oy + 100);
    // The grab point must still be under the pointer. Storing a delta instead
    // of the grab offset makes the window drift away from the cursor over a
    // long drag, and the drift is only visible after many events.
    expect("the grab point is still under the pointer",
           g_mouse_x - g_win[a].x, 40);
    release();
    expect("releasing ends the drag", g_drag_win, -1);
    drag_to(ox + 300, oy + 300);
    expect("moving after release does not move the window", g_win[a].x, ox + 100);

    wm_present();
    check_matches_full("after a drag");

    // Dragging by the client area must NOT move the window.
    puts("\n  a press on the client area:\n");

    // Release first. The step above left the button held down, and a press
    // that is not a press EDGE does nothing at all -- so without this the
    // whole block passed without ever pressing anything. It read green with
    // the code deliberately broken to drag from anywhere in a window, which is
    // the definition of a test that is not testing.
    release();
    ox = g_win[a].x;
    {
        long clicks;
        clicks = g_wmin_clicks;
        press_at(ox + 40, g_win[a].y + 100);
        expect("the press was registered at all", g_wmin_clicks, clicks + 1);
        expect("...but no drag started", g_drag_win, -1);
    }
    drag_to(ox + 200, g_win[a].y + 100);
    expect("the window did not move", g_win[a].x, ox);
    release();

    // The close button.
    puts("\n  the close button:\n");
    {
        long before;
        before = g_nwin;
        click_at(g_win[b].x + wm_close_x(b) + 3, g_win[b].y + wm_close_y() + 3);
        expect("the window is gone", g_nwin, before - 1);
        expect("its slot is free", g_win[b].used, 0);
        expect("the close was counted", g_wmin_closes, 1);
    }
    wm_present();
    check_matches_full("after closing a window");

    // A close must not leave focus pointing at a destroyed window: the next
    // keystroke would be delivered into a freed slot.
    if (g_focus == b) fail("focus stayed on the closed window");
    else printf("  ok  focus moved off the closed window (now %d)\n", g_focus);

    // THE SAME CLICK, DONE THE WAY A PERSON DOES IT.
    //
    // click_at pumps the queue between the press and the release. A hand does
    // not: the press, the drift and the release all arrive between two of the
    // compositor's twenty-millisecond drains. Every close test above was
    // written the convenient way, which is exactly why they were all green
    // while the close button did not work on the desktop -- the harness was
    // draining the queue at a moment no real click ever offers.
    //
    // Twelve pixels of drift is a hand, not a drag. Before the queue kept the
    // position a button went down at, this reported the press on the title bar
    // beside the close box: the window was raised and started dragging instead
    // of closing, which is the reported symptom exactly.
    puts("\n  the close button, clicked by a hand that is not perfectly still:\n");
    {
        long c;
        long drags;
        long cx;
        long cy;
        release();
        c = wm_create(300, 400, 200, 120, "shaky");
        wm_decorate(c);
        drags = g_wmin_drags;
        cx = g_win[c].x + wm_close_x(c) + 3;
        cy = g_win[c].y + wm_close_y() + 3;
        mouse_warp(cx, cy);
        wm_pump_mouse();
        inject(0, 0, 1);            // press, on the close box
        inject(0 - 12, 0, 1);       // the hand moves while the button is down
        inject(0, 0, 0);            // release
        wm_pump_mouse();            // ONE drain, which is all the desktop does
        expect("the window closed anyway", g_win[c].used, 0);
        expect("...and nothing was dragged instead", g_wmin_drags, drags);
        if (g_win[c].used) wm_destroy(c);
        release();
    }
    wm_present();
    check_matches_full("after a shaky close");
}

// ============================================================
// 6. the console
// ============================================================
void type(long ti, char *s) {
    while (*s) { term_key(ti, *s); s = s + 1; }
    term_flush(ti);
}

long term_row_text_eq(long ti, long row, char *want) {
    long c;
    c = 0;
    while (want[c]) {
        if (c >= g_term[ti].cols) return 0;
        if (g_term[ti].cells[row * g_term[ti].cols + c] != want[c]) return 0;
        c = c + 1;
    }
    return 1;
}

void test_console() {
    long ti;
    long one_char;
    long i;

    puts("\n-- 6. a console in a window --\n");

    wm_init(rgb(24, 28, 38));
    wmin_init();
    term_init();
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);

    ti = term_create(120, 120, 56, 24, "console");
    if (ti < 0) { fail("could not create a console"); return; }
    printf("  console is %dx%d cells in a %dx%d window\n",
           g_term[ti].cols, g_term[ti].rows,
           g_win[g_term[ti].win].w, g_win[g_term[ti].win].h);

    wm_set_focus(g_term[ti].win);
    wm_cursor_show(1);
    wm_cursor_move(700, 500);
    term_puts(ti, "nano-os console\n");
    term_prompt(ti);
    term_flush(ti);
    wm_present();
    check_matches_full("console first paint");

    // ...and the terminal painted all of its own client area, blank cells
    // included. Every checksum in this file compares a frame against a repaint
    // of the same backing buffers, so a flush that skipped cells would sail
    // through all of them while leaving the window manager's grey on screen.
    expect("client-area pixels the console never painted",
           client_unpainted(g_term[ti].win), 0);
    expect("exactly one text cursor on screen", term_cursor_blocks(ti), 1);

    // Typing one character must repaint the cells that changed and nothing
    // else. This is the number the two-grid design exists for: without it,
    // every keystroke costs the whole client area.
    //
    // Measured with the pointer hidden, so the figure is the terminal's cost
    // and only the terminal's cost. The pointer is redrawn on every single
    // frame -- that is what a pointer is -- and leaving it in would fold a
    // constant 346 pixels into a measurement of something else. The first
    // version of this test did exactly that and then asserted a bound loose
    // enough to hide it, which is two mistakes covering for each other.
    wm_cursor_show(0);
    wm_present();
    wm_reset_counters();
    g_term_cells_drawn = 0;
    term_key(ti, 'x');
    term_flush(ti);
    wm_present();
    one_char = wm_pixels;
    printf("  typing one character: %d cells redrawn, %d screen pixels\n",
           g_term_cells_drawn, one_char);
    // Two cells: the one gaining the character, and the one the block cursor
    // moved to. Both are 8x8 and they are adjacent, so one 16x8 invalidate.
    expect("cells redrawn for one keystroke", g_term_cells_drawn, 2);
    expect("screen pixels for one keystroke", one_char,
           2 * TERM_CELL_W * TERM_CELL_H);
    check_matches_full("after typing one character");

    // And with the pointer back on, so the constant it adds is on the record
    // rather than buried in a bound.
    wm_cursor_show(1);
    wm_present();
    wm_reset_counters();
    term_key(ti, 'y');
    term_flush(ti);
    wm_present();
    printf("  the same keystroke with the pointer visible: %d pixels"
           " (%d of them the pointer)\n", wm_pixels, g_cur_pixels);

    term_key(ti, '\b');
    term_key(ti, '\b');
    term_flush(ti);
    wm_present();

    // A command.
    type(ti, "ver\n");
    if (!term_row_text_eq(ti, 2, "nano-os K13"))
        fail("the ver command did not print into the console");
    else puts("  ok  a command ran and printed into the window\n");

    type(ti, "echo hello from a window\n");
    if (!term_row_text_eq(ti, 4, "hello from a window"))
        fail("echo did not print its argument");
    else puts("  ok  echo\n");

    type(ti, "nosuchthing\n");
    if (!term_row_text_eq(ti, 6, "unknown: nosuchthing"))
        fail("an unknown command was not reported");
    else puts("  ok  unknown commands are reported\n");

    wm_present();
    check_matches_full("after three commands");

    // Backspace.
    term_key(ti, 'a'); term_key(ti, 'b'); term_key(ti, 'c');
    term_key(ti, '\b'); term_key(ti, '\b');
    term_flush(ti);
    expect("backspace shortens the input line", g_term[ti].linelen, 1);
    term_key(ti, '\b');
    term_flush(ti);
    expect("backspace at the prompt does nothing", g_term[ti].linelen, 0);
    // ...and it must not eat the prompt itself.
    if (!term_row_text_eq(ti, g_term[ti].cy, "> "))
        fail("backspace erased the prompt");
    else puts("  ok  backspace stops at the prompt\n");

    // The text cursor must not leave a trail.
    //
    // The case that catches this is a move where NEITHER the cell being
    // vacated nor the cell being entered changes its contents -- a bare
    // newline on a blank line. Every other move happens to redraw the vacated
    // cell anyway, because that cell is the one that just received a
    // character. Which is why the bug survives casual use and then shows up as
    // "the console is drawing garbage".
    puts("\n  the text cursor:\n");
    term_clear(ti);
    term_flush(ti);
    expect("one cursor block after a clear", term_cursor_blocks(ti), 1);
    term_putc(ti, '\n');
    term_flush(ti);
    expect("still one after a bare newline", term_cursor_blocks(ti), 1);
    i = 0;
    while (i < 5) { term_putc(ti, '\n'); term_flush(ti); i = i + 1; }
    expect("still one after five more", term_cursor_blocks(ti), 1);
    expect("and no unpainted client area", client_unpainted(g_term[ti].win), 0);
    wm_present();
    check_matches_full("after moving the text cursor");

    // Scrolling. Fill past the bottom and check the top line is gone, the
    // content moved up, and the screen still matches a full repaint.
    puts("\n  scrolling:\n");
    term_clear(ti);
    i = 0;
    while (i < g_term[ti].rows + 5) {
        term_puts(ti, "line ");
        term_putnum(ti, i);
        term_putc(ti, '\n');
        i = i + 1;
    }
    term_flush(ti);
    wm_present();
    if (term_row_text_eq(ti, 0, "line 0"))
        fail("the console did not scroll: line 0 is still at the top");
    else if (!term_row_text_eq(ti, 0, "line 6"))
        fail("the console scrolled by the wrong amount");
    else puts("  ok  scrolled by exactly the overflow (line 6 is now at the top)\n");
    check_matches_full("after scrolling");

    // Keyboard routing. A key goes to the focused window and nowhere else.
    puts("\n  keyboard routing:\n");
    {
        long ti2;
        long before;
        ti2 = term_create(560, 320, 34, 16, "second console");
        if (ti2 < 0) { fail("could not create a second console"); return; }
        term_prompt(ti2);
        term_flush(ti2);
        wm_present();

        wm_set_focus(g_term[ti].win);
        before = g_term[ti2].linelen;
        wm_input_key('q');
        wm_input_key('w');
        expect("keys reached the focused console", g_term[ti].linelen, 2);
        expect("...and not the unfocused one", g_term[ti2].linelen, before);

        wm_set_focus(g_term[ti2].win);
        wm_input_key('z');
        expect("after refocusing, the other one receives", g_term[ti2].linelen, before + 1);
        expect("...and the first is unchanged", g_term[ti].linelen, 2);

        // Clicking a console focuses it, so typing follows the mouse.
        click_at(g_win[g_term[ti].win].x + 60, g_win[g_term[ti].win].y + 60);
        expect("clicking a console focuses it", g_focus, g_term[ti].win);
        wm_input_key('!');
        expect("and it receives the next key", g_term[ti].linelen, 3);

        // Focus on the desktop means nobody gets the key -- not "the last
        // window keeps getting them", which is what happens if focus is only
        // ever set and never cleared.
        click_at(980, 740);
        {
            long l1;
            long l2;
            l1 = g_term[ti].linelen;
            l2 = g_term[ti2].linelen;
            wm_input_key('?');
            if (g_term[ti].linelen != l1 || g_term[ti2].linelen != l2)
                fail("a key was delivered while the desktop had focus");
            else puts("  ok  with the desktop focused, keys go nowhere\n");
        }
    }

    term_flush(ti);
    wm_present();
    check_matches_full("console, final");
}

// ============================================================
// 7. what a drag costs
// ============================================================
//
// "dragging a window uses all the CPU" is a rate, not something you can see in
// a frame. The mouse reports about a hundred times a second, the event loop
// presents one frame per report, and if a frame costs a whole tick then a drag
// is the entire machine with nothing left over.
//
// Two numbers, because they have two different causes and two different fixes:
//
//   PIXELS PER FRAME is the damage tracker's answer to "how much work does
//   moving a window ask for". Moving a window by one pixel dirties the union
//   of where it was and where it is, so the floor is one window's area and
//   there is not much to win here.
//
//   TICKS PER FRAME is the blitter's answer to "how long does that work take".
//   This is the one that decides whether a drag is free or fatal, and it is
//   measured, not reasoned about, because the pixel count says nothing about
//   the cost of a pixel.
void test_drag_cost() {
    long a;
    long b;
    long i;
    long t0;
    long t1;
    long px;
    long frames;
    long load;
    long full0;
    long full1;

    puts("\n-- 7. what a drag costs --\n");

    wm_init(rgb(24, 28, 38));
    wmin_init();
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);
    a = wm_create(60, 70, 340, 230, "picture viewer");
    b = wm_create(330, 190, 300, 180, "files");
    paint_content(a, 3);
    paint_content(b, 6);
    wm_cursor_show(1);
    wm_present();

    frames = 400;

    // Grab the title bar and then move one pixel at a time, doing exactly what
    // the event loop does per mouse report: pump the queue, present once.
    // Alternating direction keeps the window in the same place over 400 frames,
    // so this measures a drag and not a journey off the edge of the screen.
    press_at(g_win[a].x + 120, g_win[a].y + WM_TITLE_H / 2);
    expect("the drag started", g_drag_win, a);

    wm_reset_counters();
    t0 = g_ticks;
    i = 0;
    while (i < frames) {
        inject(((i & 1) ? -1 : 1), 0, 1);
        wm_pump_mouse();
        wm_present();
        i = i + 1;
    }
    t1 = g_ticks;
    px = wm_pixels;
    release();
    expect("releasing ended the drag", g_drag_win, -1);

    printf("  %d drag frames: %d pixels, %d per frame, %d blits\n",
           frames, px, px / frames, wm_blits);
    printf("  the window itself is %d pixels, a full screen is %d\n",
           g_win[a].w * g_win[a].h, wm_screen_pixels());
    printf("  %d drag frames took %d ticks at %d Hz\n", frames, t1 - t0, g_hz);

    // A PS/2 mouse reports at 100 samples a second, so a drag that costs
    // (t1-t0) ticks for `frames` frames occupies this share of the machine.
    load = (100 * (t1 - t0) * 100) / (frames * g_hz);
    printf("  => a real drag would use %d%% of the CPU\n", load);
    // Ten per cent, and the bound is chosen so that the version of this that
    // wrote a pixel at a time -- measured at 30% here, and at "all of it" on
    // the client's machine -- fails it.
    if (load > 10) fail("dragging a window costs more than 10% of the CPU");
    else printf("  ok  a drag leaves the machine usable\n");

    check_matches_full("after 400 drag frames");

    // And the blitter on its own, with the damage tracker taken out of the
    // picture entirely: how long does it take to write the whole screen? The
    // drag number above is a consequence of this one, and separating them is
    // what says whether to fix the damage tracker or the pixel loop.
    wm_no_damage = 1;
    wm_reset_counters();
    full0 = g_ticks;
    i = 0;
    while (i < 10) { wm_present(); i = i + 1; }
    full1 = g_ticks;
    wm_no_damage = 0;
    {
        long ticks;
        long perscreen;
        ticks = full1 - full0;
        printf("  10 full repaints (%d pixels) took %d ticks\n", wm_pixels, ticks);
        perscreen = (ticks * 1000) / 10;
        printf("  = %d.%d ticks, or %d ms, per full screen\n",
               perscreen / 1000, (perscreen % 1000) / 100,
               (perscreen * 1000) / (g_hz * 1000));
    }
    wm_present();
}

// ============================================================
// the desktop that is left on screen
// ============================================================
long g_shell_term;

void build_desktop() {
    long a;
    long ti;

    wm_init(rgb(24, 28, 38));
    wmin_init();
    term_init();
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);

    a = wm_create(60, 70, 340, 230, "picture viewer");
    paint_content(a, 3);
    // Deliberately overlapping the other two, so the screenshot shows the
    // z-order and the two title bar colours doing their job.
    {
        long b;
        b = wm_create(330, 190, 300, 180, "files");
        paint_content(b, 6);
    }

    ti = term_create(240, 330, 62, 26, "console");
    g_shell_term = ti;
    if (ti >= 0) {
        term_puts(ti, "nano-os K13 -- compiled by nano_cc\n");
        term_puts(ti, "mouse: PS/2 on IRQ12, three-byte packets\n");
        term_puts(ti, "drag a title bar, click to raise, type here.\n");
        term_puts(ti, "'help' lists the commands.\n\n");
        term_prompt(ti);
        term_flush(ti);
        wm_set_focus(g_term[ti].win);
    }

    wm_cursor_show(1);
    mouse_warp(fb_width / 2, fb_height / 2);
    wm_cursor_move(g_mouse_x, g_mouse_y);
    wm_present();
}

// The live loop. Everything above ran without a human; this is the part a
// human uses.
void event_loop() {
    for (;;) {
        long busy;
        char c;
        busy = wm_pump_mouse();
        for (;;) {
            c = kbd_getchar_nb();
            if (c == 0) break;
            if (wm_input_key(c)) busy = 1;
        }
        if (busy) wm_present();
        // Nothing to do: stop the core until the next interrupt rather than
        // spinning. A desktop that burns a CPU while idle is a desktop with a
        // bug in its event loop.
        cpu_idle();
    }
}

void run_tests() {
    printf("FB: %dx%d at %d bpp, pitch %d\n", fb_width, fb_height, fb_bpp, fb_pitch);
    printf("a full repaint is %d pixels\n", wm_screen_pixels());
    printf("PS/2 mouse: %s\n\n", g_mouse_present ? "present" : "NOT DETECTED");

    test_decoder();
    test_queue();
    test_cursor();
    test_hit();
    test_interaction();
    test_console();
    test_drag_cost();

    printf("\nheap: %d pages mapped, %d bytes free\n", heap_pages, heap_bytes_free());
    printf("mouse: %d packets, %d resyncs, %d dropped events, %d spurious IRQs\n",
           g_mouse_packets, g_mouse_resync, g_mev_dropped, g_spurious);

    if (g_fail) printf("\n%d CHECKS FAILED\n", g_fail);
    else puts("\nPASS: the pointer, the mouse, the clicks and the console all behave\n");

    puts("\nWMINTEST DONE\n");
}

int main() {
    serial_init();
    g_fail = 0;

    puts("\nnano-os: a mouse, a pointer, and a console in a window\n");

    if (!fb_init(1024, 768)) { puts("fb_init failed\n"); for (;;) { } }
    if (!mm_init())          { puts("mm_init failed\n"); for (;;) { } }
    mm_protect_null();

    kbd_init();
    interrupts_init(100);          // IDT, PIC, timer, keyboard, and the mouse

    run_tests();

    // Leave a usable desktop behind rather than a wall of test output, and then
    // hand the machine to whoever is sitting in front of it.
    build_desktop();
    puts("desktop up; the machine is now interactive\n");
    event_loop();
    return 0;
}
