// ui.c — immediate-mode widgets, and the measurement that says whether the
// idea actually works.
//
// The claim nano-ui.h makes is not "here are some buttons". It is that an
// immediate-mode interface -- which by construction rebuilds every widget
// every frame -- can sit on top of a damage-tracking compositor without
// throwing away what the compositor is for.
//
// That claim is falsifiable, so this image falsifies it or does not. The
// central test is two identical frames in a row: the second one must push
// ZERO pixels to the screen. Everything was recomputed, every widget was
// redrawn into the backing buffer, and nothing crossed the bus. If that number
// is not zero then immediate mode has quietly turned every frame into a full
// repaint and the whole approach is wrong.
//
// The behavioural tests are the ones that are easy to get subtly wrong and
// impossible to notice by looking: a button that fires when you press it and
// drag away, two text fields that both take the same keystroke, a slider that
// lets go the moment the pointer strays a pixel above the track.
//
// nano-kernel.h first, as in wm.c and wmin.c -- it only mirrors console output
// onto the framebuffer if NANO_FB_H is already defined, and here it must not,
// because this image reads the framebuffer back and hashes it.
#include "nano-kernel.h"
#include "nano-fb.h"
#include "nano-mouse.h"
#include "nano-int.h"
#include "nano-mm.h"
#include "nano-wm.h"
#include "nano-wmin.h"
#include "nano-term.h"
#include "nano-ui.h"

long g_fail;

void fail(char *msg) {
    printf("FAIL: %s\n", msg);
    g_fail = g_fail + 1;
}

void expect_true(char *what, long got) {
    if (got) printf("  ok  %s\n", what);
    else fail(what);
}

void expect(char *what, long got, long want) {
    if (got == want) printf("  ok  %s = %d\n", what, got);
    else {
        printf("  got %d, wanted %d\n", got, want);
        fail(what);
    }
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

// ---------- the panel under test ----------

#define PX  (WM_BORDER + 8)
#define PY  (WM_TITLE_H + 8)
#define PW  200
#define ROW (UI_ROW_H + UI_PAD)

struct Ui g_ui;
long g_w;                   // the window the panel lives in

long g_ok;                  // what each widget returned last frame
long g_cancel;
long g_check_changed;
long g_slider_changed;
long g_text_edited;

long g_check;               // the caller's own values -- immediate mode keeps
long g_slider;              // no copy of these, which is the point
char g_buf[32];

// Widget ids are the call order: 0 label, 1 OK, 2 Cancel, 3 checkbox,
// 4 slider, 5 text, 6 progress.
void panel() {
    ui_label(&g_ui, "settings");
    g_ok             = ui_button(&g_ui, "OK");
    g_cancel         = ui_button(&g_ui, "Cancel");
    g_check_changed  = ui_checkbox(&g_ui, "wireframe", &g_check);
    g_slider_changed = ui_slider(&g_ui, &g_slider, 0, 100);
    g_text_edited    = ui_text(&g_ui, g_buf, 32);
    ui_progress(&g_ui, g_slider, 0, 100);
}

// Screen coordinates of the centre of widget row `i`.
long row_cy(long i) { return g_win[g_w].y + PY + i * ROW + UI_ROW_H / 2; }
long row_cx() { return g_win[g_w].x + PX + PW / 2; }
long row_left() { return g_win[g_w].x + PX + 2; }
long row_right() { return g_win[g_w].x + PX + PW - 2; }

// One frame, with the pointer somewhere and the button up or down. The edges
// are worked out here exactly as the real event loop works them out, from the
// previous frame's button state.
long g_prev_down;

void frame_key(long sx, long sy, long down, long key) {
    long pressed;
    long released;
    pressed  = down && !g_prev_down;
    released = !down && g_prev_down;
    g_prev_down = down;

    mouse_warp(sx, sy);
    wm_cursor_move(sx, sy);

    ui_begin(&g_ui, g_w, PX, PY, PW);
    ui_input(&g_ui, down, pressed, released, key);
    panel();
    ui_end(&g_ui);
    wm_present();
}

void frame(long sx, long sy, long down) { frame_key(sx, sy, down, 0); }

// The same frame, but telling the interface that something else has painted
// over one row of it since last time. This is what an application does after
// rendering into a viewport with widgets on top of it.
long g_over_y;
long g_over_on;

// A hash of one rectangle of a window's BACKING BUFFER -- not the screen.
// check_matches_full cannot see the fault this is for: it repaints the screen
// from the buffer and compares, so a widget that has been scribbled over in
// the buffer and never redrawn agrees with itself perfectly. The only way to
// catch it is to remember what the row looked like before.
long win_rect_hash(long win, long x, long y, long w, long h) {
    long hash;
    long j;
    hash = 5381;
    j = 0;
    while (j < h) {
        long i;
        i = 0;
        while (i < w) {
            hash = ((hash * 33) + g_win[win].pix[(y + j) * g_win[win].w + x + i])
                   & 0xFFFFFFF;
            i = i + 1;
        }
        j = j + 1;
    }
    return hash;
}

void frame_over(long sx, long sy) {
    g_prev_down = 0;
    mouse_warp(sx, sy);
    wm_cursor_move(sx, sy);
    ui_begin(&g_ui, g_w, PX, PY, PW);
    if (g_over_on) ui_overpaint(&g_ui, g_w, PX, g_over_y, PW, UI_ROW_H);
    ui_input(&g_ui, 0, 0, 0, 0);
    panel();
    ui_end(&g_ui);
    wm_present();
}

// A frame with the edges stated outright rather than derived. Needed for the
// cases a well-behaved mouse cannot produce but a real one does: a second
// press with no release in between, which is what a dropped release event
// looks like from up here.
void frame_raw(long sx, long sy, long down, long pressed, long released) {
    g_prev_down = down;
    mouse_warp(sx, sy);
    wm_cursor_move(sx, sy);
    ui_begin(&g_ui, g_w, PX, PY, PW);
    ui_input(&g_ui, down, pressed, released, 0);
    panel();
    ui_end(&g_ui);
    wm_present();
}

void build_panel_window() {
    wm_init(rgb(24, 28, 38));
    wmin_init();
    ui_init(&g_ui);
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);

    g_w = wm_create(120, 100, PW + 16 + WM_BORDER, PY + 7 * ROW + 8, "settings");
    wm_decorate(g_w);
    wm_set_focus(g_w);
    g_prev_down = 0;
    g_check = 0;
    g_slider = 0;
    g_buf[0] = 0;
    wm_present();
}

// ============================================================
// 1. a button
// ============================================================
void test_button() {
    puts("-- 1. a button, and the click that should not count --\n");

    build_panel_window();
    wm_cursor_show(1);

    // Away from everything.
    frame(g_win[g_w].x + 400, g_win[g_w].y + 400, 0);
    expect("nothing is hot", g_ui.hot, -1);
    expect("nothing is active", g_ui.active, -1);

    frame(row_cx(), row_cy(1), 0);
    expect("hovering OK makes it hot", g_ui.hot, 1);
    expect("...but not active", g_ui.active, -1);
    expect("...and not clicked", g_ok, 0);

    frame(row_cx(), row_cy(1), 1);
    expect("pressing makes it active", g_ui.active, 1);
    expect("a press alone is not a click", g_ok, 0);

    frame(row_cx(), row_cy(1), 0);
    expect("releasing on it IS a click", g_ok, 1);
    expect("and the button is released", g_ui.active, -1);

    frame(row_cx(), row_cy(1), 0);
    expect("the click does not repeat next frame", g_ok, 0);
    // And nothing may still own the pointer. A press edge that is never
    // consumed leaves `active` set forever, and the only visible symptom is
    // that a hovered button looks pressed -- which no other check here reads.
    expect("nothing is active while merely hovering", g_ui.active, -1);

    // The one that matters. Press, change your mind, drag off, release.
    // This is the entire reason `active` is a separate thing from `hot`.
    puts("\n  press, drag off, release:\n");
    frame(row_cx(), row_cy(1), 1);
    expect("pressed on OK", g_ui.active, 1);
    frame(g_win[g_w].x + 400, g_win[g_w].y + 400, 1);
    expect("dragged away, still owned by OK", g_ui.active, 1);
    expect("nothing is hot now", g_ui.hot, -1);
    frame(g_win[g_w].x + 400, g_win[g_w].y + 400, 0);
    expect("released elsewhere: NOT a click", g_ok, 0);

    // And while one widget owns the pointer, hovering another must not steal
    // it. Otherwise dragging across a row of buttons presses all of them.
    puts("\n  a second widget cannot steal an active pointer:\n");
    frame(row_cx(), row_cy(1), 1);
    frame(row_cx(), row_cy(2), 1);
    expect("still owned by OK", g_ui.active, 1);
    expect("Cancel did not fire", g_cancel, 0);
    frame(row_cx(), row_cy(2), 0);
    expect("releasing over Cancel clicks neither", g_ok + g_cancel, 0);

    // The above never actually produced a second press EDGE -- holding the
    // button down across two frames gives one press and no more -- so it did
    // not exercise the guard it is named after. This does: a press arriving
    // while another widget already owns the pointer, which is exactly what a
    // dropped release event looks like.
    puts("\n  a second press edge while a widget already owns the pointer:\n");
    frame(row_cx(), row_cy(1), 1);
    expect("OK owns the pointer", g_ui.active, 1);
    frame_raw(row_cx(), row_cy(2), 1, 1, 0);
    expect("a second press does not transfer ownership", g_ui.active, 1);
    expect("and Cancel still did not fire", g_cancel, 0);
    frame_raw(row_cx(), row_cy(2), 0, 0, 1);

    check_matches_full("after the button tests");
}

// ============================================================
// 2. checkbox and slider
// ============================================================
void click_row(long i) {
    frame(row_cx(), row_cy(i), 0);
    frame(row_cx(), row_cy(i), 1);
    frame(row_cx(), row_cy(i), 0);
}

void test_checkbox_slider() {
    puts("\n-- 2. a checkbox and an integer slider --\n");

    expect("the checkbox starts clear", g_check, 0);
    click_row(3);
    expect("clicking it sets it", g_check, 1);
    expect("and it reported the change", g_check_changed, 1);
    click_row(3);
    expect("clicking again clears it", g_check, 0);

    // The value lives in the caller's variable. Immediate mode keeps no copy,
    // so writing to it out of band must simply work.
    g_check = 1;
    frame(row_cx(), row_cy(0), 0);
    expect("a value changed behind its back still shows", g_check, 1);
    g_check = 0;

    puts("\n  the slider:\n");
    // Press at the far right of the track.
    frame(row_right(), row_cy(4), 0);
    frame(row_right(), row_cy(4), 1);
    expect("dragged to the right end", g_slider, 100);
    frame(row_left(), row_cy(4), 1);
    expect("dragged to the left end", g_slider, 0);

    // Past both ends. A slider that can be pushed outside its range is a
    // slider that hands the caller a number it promised not to.
    frame(g_win[g_w].x - 200, row_cy(4), 1);
    expect("dragged far past the left end, clamped", g_slider, 0);
    frame(g_win[g_w].x + 900, row_cy(4), 1);
    expect("dragged far past the right end, clamped", g_slider, 100);

    // Still held, but the pointer has left the widget vertically. It must keep
    // tracking. Letting go the moment the cursor strays a pixel above the
    // track is the single most irritating slider bug there is.
    frame(row_left(), g_win[g_w].y - 60, 1);
    expect("still tracking with the pointer far above it", g_slider, 0);
    frame(row_right(), g_win[g_w].y - 60, 1);
    expect("...and back to the right", g_slider, 100);
    frame(row_right(), g_win[g_w].y - 60, 0);

    // Released. Now moving over it must do nothing at all.
    frame(row_left(), row_cy(4), 0);
    expect("moving over a released slider does not move it", g_slider, 100);

    check_matches_full("after the checkbox and slider");
}

// ============================================================
// 3. a text field, and where keystrokes go
// ============================================================
long buf_eq(char *a, char *b) {
    while (*a && *b) { if (*a != *b) return 0; a = a + 1; b = b + 1; }
    return *a == *b;
}

void type_into(char *s) {
    while (*s) { frame_key(row_cx(), row_cy(5), 0, *s); s = s + 1; }
}

void test_text() {
    puts("\n-- 3. a text field --\n");

    // Focus it by clicking.
    frame(row_cx(), row_cy(5), 0);
    frame(row_cx(), row_cy(5), 1);
    frame(row_cx(), row_cy(5), 0);
    expect("clicking the field focuses it", g_ui.focus, 5);

    type_into("hello");
    if (!buf_eq(g_buf, "hello")) fail("typing did not reach the field");
    else puts("  ok  typed into the focused field\n");

    frame_key(row_cx(), row_cy(5), 0, '\b');
    frame_key(row_cx(), row_cy(5), 0, '\b');
    if (!buf_eq(g_buf, "hel")) fail("backspace did not shorten the field");
    else puts("  ok  backspace\n");

    // Clicking elsewhere must UNfocus. If focus is only ever set and never
    // cleared, the field keeps eating keystrokes that belong to nothing.
    frame(row_cx(), row_cy(1), 0);
    frame(row_cx(), row_cy(1), 1);
    frame(row_cx(), row_cy(1), 0);
    if (g_ui.focus == 5) fail("clicking a button left the text field focused");
    else puts("  ok  clicking elsewhere unfocuses the field\n");

    frame_key(row_cx(), row_cy(1), 0, 'X');
    if (!buf_eq(g_buf, "hel")) fail("an unfocused field still received a key");
    else puts("  ok  an unfocused field receives nothing\n");

    // That only proved the BUTTON took focus for itself. The branch that
    // actually clears focus is the one for a click that lands on something
    // which takes no focus at all -- a label, or bare panel. Without it,
    // clicking dead space leaves the field quietly eating every keystroke.
    frame(row_cx(), row_cy(5), 0);
    frame(row_cx(), row_cy(5), 1);
    frame(row_cx(), row_cy(5), 0);
    expect("focused again", g_ui.focus, 5);
    click_row(0);                       // the label: it never takes focus
    if (g_ui.focus == 5) fail("clicking dead space left the text field focused");
    else puts("  ok  clicking something that takes no focus clears it\n");
    frame_key(row_cx(), row_cy(0), 0, 'Q');
    if (!buf_eq(g_buf, "hel")) fail("the field kept receiving keys after dead-space click");
    else puts("  ok  and it receives nothing afterwards\n");

    // The capacity bound. Off by one here overruns the caller's buffer, which
    // in a kernel means corrupting whatever is next to it.
    frame(row_cx(), row_cy(5), 0);
    frame(row_cx(), row_cy(5), 1);
    frame(row_cx(), row_cy(5), 0);
    {
        long i;
        i = 0;
        while (i < 80) { frame_key(row_cx(), row_cy(5), 0, 'a'); i = i + 1; }
    }
    {
        long n;
        n = 0;
        while (g_buf[n]) n = n + 1;
        expect("80 characters into a 32-byte field stops at cap-1", n, 31);
        expect("...and it is still terminated", g_buf[31], 0);
    }

    check_matches_full("after the text field");
}

// ============================================================
// 4. the claim: immediate mode without a full repaint
// ============================================================
void test_damage() {
    long idle;
    long hover;

    puts("\n-- 4. the claim: rebuild everything, repaint almost nothing --\n");

    // Measured with the pointer hidden. The pointer is redrawn on every frame
    // -- that is what a pointer is -- and leaving it in would fold a constant
    // into a measurement of something else. K13 taught me that one.
    wm_cursor_show(0);
    frame(row_cx(), row_cy(0), 0);
    wm_present();

    // Two identical frames. The second rebuilt every widget from scratch.
    wm_reset_counters();
    frame(row_cx(), row_cy(0), 0);
    idle = wm_pixels;
    printf("  an unchanged frame: %d widgets drawn into the buffer, "
           "%d invalidated, %d pixels to the screen\n",
           g_ui.drawn, g_ui.invalidations, idle);
    // K24c: it used to rebuild all seven into the buffer and invalidate none
    // of them, which was half the saving. The pixels were already right, so
    // writing them again was work with no reader -- 600 us a frame in the
    // textured demo. Now an unchanged widget does not draw either.
    expect("no widget was redrawn", g_ui.drawn, 0);
    expect("all seven were skipped", g_ui.skipped, 7);
    expect("none of them was invalidated", g_ui.invalidations, 0);
    expect("and nothing crossed the bus", idle, 0);

    // ...which is only safe while nothing else writes into the same buffer.
    // Scribble over one widget's row, say so, and it must come back -- and
    // the screen must then match a full repaint, which is the check that
    // would catch a widget wrongly deciding it was still intact.
    {
        long before;
        long after;
        g_over_y = PY + 2 * ROW;
        before = win_rect_hash(g_w, PX, g_over_y, PW, UI_ROW_H);
        wm_win_fill(g_w, PX, g_over_y, PW, UI_ROW_H, rgb(255, 0, 0));
        wm_invalidate(g_w, PX, g_over_y, PW, UI_ROW_H);
        expect_true("the scribble really did change the row",
                    win_rect_hash(g_w, PX, g_over_y, PW, UI_ROW_H) != before);

        g_over_on = 1;
        wm_reset_counters();
        frame_over(row_cx(), row_cy(0));
        g_over_on = 0;
        after = win_rect_hash(g_w, PX, g_over_y, PW, UI_ROW_H);

        printf("  one row painted over: %d redrawn, %d skipped, %d invalidated\n",
               g_ui.drawn, g_ui.skipped, g_ui.invalidations);
        expect("the widget that was painted over redrew", g_ui.drawn, 1);
        expect("...and the other six did not", g_ui.skipped, 6);
        expect("...and it did not invalidate: the scribble already did",
               g_ui.invalidations, 0);
        // THE CHECK THAT MATTERS. The counters say a widget drew; this says
        // the row is back to the pixels it had before the scribble.
        expect_true("...and the row is pixel-for-pixel what it was", after == before);
        check_matches_full("after something painted over a widget");
    }

    // Now move the pointer onto one button. Exactly one widget changes
    // appearance, so exactly one rectangle should be pushed.
    wm_reset_counters();
    frame(row_cx(), row_cy(1), 0);
    hover = wm_pixels;
    printf("  hovering one button: %d invalidated, %d pixels\n",
           g_ui.invalidations, hover);
    expect("one widget invalidated", g_ui.invalidations, 1);
    expect("costing exactly its own rectangle", hover, PW * UI_ROW_H);

    // Moving off it changes that one back, and nothing else.
    wm_reset_counters();
    frame(row_cx(), row_cy(0), 0);
    expect("moving off invalidates the same one", g_ui.invalidations, 1);
    expect("and costs the same", wm_pixels, PW * UI_ROW_H);

    // A character REPLACED, not added or removed. The string is the same
    // length, so a widget hash built from the length alone would call this
    // unchanged and leave the old text on screen forever.
    g_buf[30] = 'z';
    wm_reset_counters();
    frame(row_cx(), row_cy(0), 0);
    expect("replacing one character redraws the field", g_ui.invalidations, 1);

    // A hundred frames of nothing happening.
    wm_reset_counters();
    {
        long i;
        i = 0;
        while (i < 100) { frame(row_cx(), row_cy(0), 0); i = i + 1; }
    }
    printf("  100 idle frames: %d widgets rebuilt, %d pixels to the screen\n",
           g_ui.drawn, wm_pixels);
    expect("a hundred idle frames cost nothing", wm_pixels, 0);
    printf("  (a full repaint each frame would have been %d)\n",
           wm_screen_pixels() * 100);

    wm_cursor_show(1);
    wm_present();
    check_matches_full("after the damage measurements");
}

// ============================================================
// 5. the auto-id hazard, pinned down
// ============================================================
long g_show_extra;
long g_p2_ok;

// Widgets are identified by call ORDER. If the set of widgets drawn changes
// between frames, everything after the change shifts identity -- and a button
// can inherit the pressed state of whatever used to occupy its number.
//
// This is not a bug to be fixed, it is the cost of not having a widget tree.
// It is pinned here so that it is documented behaviour rather than something
// discovered later by a button that fires on its own.
void panel_auto() {
    if (g_show_extra) ui_button(&g_ui, "extra");
    g_p2_ok = ui_button(&g_ui, "OK");
}

void panel_explicit() {
    if (g_show_extra) { ui_id(&g_ui, 20); ui_button(&g_ui, "extra"); }
    ui_id(&g_ui, 21);
    g_p2_ok = ui_button(&g_ui, "OK");
}

long id_of_ok(long explicit_ids, long show) {
    g_show_extra = show;
    ui_begin(&g_ui, g_w, PX, PY, PW);
    ui_input(&g_ui, 0, 0, 0, 0);
    if (explicit_ids) panel_explicit(); else panel_auto();
    ui_end(&g_ui);
    // ui->id has advanced past the last widget, so the id OK was given is one
    // less than wherever the counter ended up.
    return g_ui.id - 1;
}

void test_ids() {
    puts("\n-- 5. what identifies a widget --\n");

    build_panel_window();
    wm_cursor_show(0);

    expect("with the optional widget hidden, OK is id 0", id_of_ok(0, 0), 0);
    expect("with it shown, OK becomes id 1", id_of_ok(0, 1), 1);
    puts("  ...which is the hazard: OK inherited another widget's number\n");

    expect("with an explicit id and the widget hidden", id_of_ok(1, 0), 21);
    expect("with an explicit id and the widget shown", id_of_ok(1, 1), 21);
    puts("  ok  an explicit id survives the set of widgets changing\n");

    wm_cursor_show(1);
}

// ============================================================
// the desktop that is left behind
// ============================================================
long g_live_term;
long g_live_count;

void build_desktop() {
    long ti;

    wm_init(rgb(24, 28, 38));
    wmin_init();
    term_init();
    ui_init(&g_ui);
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);

    g_w = wm_create(90, 90, PW + 16 + WM_BORDER, PY + 7 * ROW + 8, "settings");
    wm_decorate(g_w);
    g_check = 1;
    g_slider = 35;
    g_buf[0] = 'n'; g_buf[1] = 'a'; g_buf[2] = 'm'; g_buf[3] = 'e'; g_buf[4] = 0;

    ti = term_create(400, 330, 56, 22, "console");
    g_live_term = ti;
    if (ti >= 0) {
        term_puts(ti, "nano-os K14 -- immediate-mode widgets\n");
        term_puts(ti, "no callbacks: nano_cc has no function pointers.\n");
        term_puts(ti, "click the buttons; every frame rebuilds them all\n");
        term_puts(ti, "and an unchanged frame costs 0 screen pixels.\n\n");
        term_prompt(ti);
        term_flush(ti);
        wm_set_focus(g_term[ti].win);
    }

    wm_cursor_show(1);
    mouse_warp(fb_width / 2, fb_height / 2);
    wm_cursor_move(g_mouse_x, g_mouse_y);
    g_prev_down = 0;
    wm_present();
}

// The live loop. The UI frame runs on every pass; the compositor is what
// decides whether anything reaches the screen.
void event_loop() {
    for (;;) {
        struct MEvent e;
        long down;
        long pressed;
        long released;
        long key;
        char c;

        pressed = 0;
        released = 0;
        key = 0;
        down = g_prev_down;

        while (mouse_pop(&e)) {
            wm_input_mouse(e.x, e.y, e.btn);
            down = e.btn & 1;
            if (down && !g_prev_down) pressed = 1;
            if (!down && g_prev_down) released = 1;
            g_prev_down = down;
        }

        for (;;) {
            c = kbd_getchar_nb();
            if (c == 0) break;
            // A keystroke goes to the focused WINDOW first. Only if that is
            // not a terminal does the widget panel get a look at it.
            if (!wm_input_key(c)) key = c;
        }

        ui_begin(&g_ui, g_w, PX, PY, PW);
        ui_input(&g_ui, down, pressed, released, key);
        if (g_win[g_w].used) {
            ui_label(&g_ui, "settings");
            if (ui_button(&g_ui, "count up")) {
                g_live_count = g_live_count + 1;
                if (g_live_term >= 0 && g_term[g_live_term].used) {
                    term_puts(g_live_term, "clicked ");
                    term_putnum(g_live_term, g_live_count);
                    term_putc(g_live_term, '\n');
                    term_prompt(g_live_term);
                    term_flush(g_live_term);
                }
            }
            if (ui_button(&g_ui, "reset")) g_live_count = 0;
            ui_checkbox(&g_ui, "wireframe", &g_check);
            ui_slider(&g_ui, &g_slider, 0, 100);
            ui_text(&g_ui, g_buf, 32);
            ui_progress(&g_ui, g_slider, 0, 100);
        }
        ui_end(&g_ui);

        wm_present();
        cpu_idle();
    }
}

void run_tests() {
    printf("FB: %dx%d at %d bpp\n", fb_width, fb_height, fb_bpp);
    printf("a full repaint is %d pixels\n", wm_screen_pixels());
    printf("PS/2 mouse: %s\n\n", g_mouse_present ? "present" : "NOT DETECTED");

    test_button();
    test_checkbox_slider();
    test_text();
    test_damage();
    test_ids();

    printf("\nheap: %d pages mapped, %d bytes free\n", heap_pages, heap_bytes_free());

    if (g_fail) printf("\n%d CHECKS FAILED\n", g_fail);
    else puts("\nPASS: immediate-mode widgets, and an unchanged frame costs nothing\n");

    puts("\nUITEST DONE\n");
}

int main() {
    serial_init();
    g_fail = 0;

    puts("\nnano-os: immediate-mode widgets with no callbacks\n");

    if (!fb_init(1024, 768)) { puts("fb_init failed\n"); for (;;) { } }
    if (!mm_init())          { puts("mm_init failed\n"); for (;;) { } }
    mm_protect_null();

    kbd_init();
    interrupts_init(100);

    run_tests();

    build_desktop();
    puts("desktop up; the machine is now interactive\n");
    event_loop();
    return 0;
}
