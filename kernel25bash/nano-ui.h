// nano-ui.h — immediate-mode widgets: buttons, checkboxes, sliders and text
// fields, in about four hundred lines and with no callbacks anywhere.
//
// Why immediate mode, specifically:
//
// Every retained-mode toolkit is built on callbacks. You create a widget, set
// `widget->on_click = my_handler`, and the toolkit calls you back later.
// nano_cc has no function pointers -- `long (*f)(long)` is a parse error -- so
// that model cannot be written down in this language at all. Not "harder", not
// "needs a shim": there is no way to say "call this function later".
//
// Immediate mode does not need to. A button is a function that draws itself
// and returns whether it was clicked:
//
//     if (ui_button(&ui, "OK")) { ...do the thing here, inline... }
//
// Nothing is registered, nothing is stored, nothing calls back. That is why
// microui is fifteen hundred lines and LVGL is over a hundred thousand, and it
// is why this is the one GUI architecture that fits the compiler we have.
//
// The cost is that the whole panel is rebuilt every frame. That is fine here
// and it is worth being clear about why: rebuilding happens into the window's
// backing buffer, which is ordinary RAM. Nothing reaches the screen until the
// compositor is told a rectangle is damaged. So the expensive thing -- writes
// across the PCI bus to video memory -- is still driven by what actually
// changed, not by how the API is shaped.
//
// That is the whole trick of this file: an immediate-mode interface with
// retained damage tracking underneath it. Each widget compares its visual
// state against last frame's and only invalidates when it differs, so a frame
// in which nothing changed costs zero pixels on screen.
//
// Requires nano-wm.h (drawing and invalidation) and nano-font.h.

#ifndef NANO_UI_H
#define NANO_UI_H

// The number of widgets whose appearance is remembered between frames. This is
// the ONLY retained state in the file.
#define UI_MAXID   256

#define UI_ROW_H   20
#define UI_PAD     4
#define UI_BOX     12          // checkbox side, slider handle width
#define UI_NOSTATE (0 - 1)     // "never drawn", so the first frame always draws
#define UI_MENU_NONE (0 - 1)   // no dropdown is open

struct Ui {
    long win;                  // the window being drawn into
    long mx;                   // pointer, in WINDOW coordinates, not screen
    long my;
    long inside;               // is the pointer over this window at all
    long mdown;                // button currently held
    long mpressed;             // a press EDGE happened this frame
    long mreleased;            // a release edge happened this frame
    long key;                  // one keystroke this frame, or 0

    // The three-part identity every immediate-mode UI needs.
    //   hot    -- the widget under the pointer right now
    //   active -- the widget the pointer went DOWN on, and still owns it
    //   focus  -- the widget receiving keystrokes
    // Keeping `active` separate from `hot` is what makes press-drag-off-
    // release correctly not a click, and what stops a second widget lighting
    // up while the first still owns the button.
    long hot;
    long active;
    long focus;

    // Which top-level menu is open, or UI_MENU_NONE. State, because a
    // dropdown that stays open across frames is state and there is one of it.
    long menu_open;
    // Set when a click has been consumed by something that sits ON TOP of the
    // normal widgets -- dismissing a dropdown must not also press the button
    // that happened to be underneath it.
    long swallow;
    // Scroll position of the list box. One list per panel is enough for the
    // editor and the file manager; a second concurrent list would need this
    // keyed by id, and that is the moment to change it rather than now.
    long list_top;
    // First visible ROW of the icon grid.
    long grid_top;

    long id;                   // auto-id counter, reset every frame
    long ox;                   // panel origin, window coordinates
    long oy;
    long pw;                   // panel width
    long x;                    // layout cursor
    long y;
    long cols;                 // widgets remaining in the current row
    long colw;
    long rowy;                 // y to return to when the row ends

    // The rectangle of the widget currently being built. It lives here rather
    // than being threaded through every helper because nano_cc takes at most
    // SIX call arguments, and (ui, id, state, x, y, w, h) is seven. Carrying
    // the rect on the context is the shape the limit pushes you towards, and
    // it is the better shape anyway -- there is now exactly one place that
    // decides where a widget is, so a helper cannot disagree with it.
    long cw;
    long ch;

    long invalidations;        // widgets that pushed damage this frame
    long drawn;                // widgets drawn into the buffer this frame
    long skipped;              // ...and the ones that did not have to be

    // THE RECTANGLE SOMETHING ELSE HAS PAINTED OVER since these widgets were
    // last drawn, in the coordinates of window `ovwin`. A widget whose state
    // is unchanged is already correct in the backing buffer and does not need
    // redrawing -- unless a 3D viewport has just been rendered underneath it,
    // which is exactly what a HUD sits on top of. The application says where
    // that happened; ui_overpaint is the whole of the contract.
    long ovwin;
    long ovx0;
    long ovy0;
    long ovx1;
    long ovy1;

    // Draw every widget every frame whether or not it needs it -- the old
    // behaviour, kept so a test can run a scene both ways and hash the window.
    long always;

    long fg;
    long bg;
    long panel;
    long accent;
    long edge;
};

// Last frame's visual state, one per widget id. Not the widget's VALUE -- the
// value lives in the caller's own variable, which is the point of immediate
// mode -- but a number that changes whenever the widget would look different.
long g_ui_last[UI_MAXID];

void ui_forget_all() {
    long i;
    i = 0;
    while (i < UI_MAXID) { g_ui_last[i] = UI_NOSTATE; i = i + 1; }
}

// ---------- frame ----------

// Point the layout at a window and a panel origin, and convert the pointer
// from SCREEN coordinates into that window's, once. Doing that per widget is
// the kind of duplication that ends with one widget using the wrong space and
// being unclickable in a way that looks like a hit-testing bug.
//
// Continue the same frame in a DIFFERENT window. The id counter is not reset,
// which is what makes it safe: two windows drawn in one frame get one shared,
// continuous id space, so no widget in the panel can collide with a widget in
// the viewport. Calling ui_begin twice instead would give them both ids
// starting at zero and a hover in one window would light up the other.
void ui_window(struct Ui *ui, long win, long x, long y, long w) {
    ui->win = win;
    ui->ox = x;
    ui->oy = y;
    ui->pw = w;
    ui->x = x;
    ui->y = y;
    ui->cols = 0;
    ui->colw = w;

    ui->mx = g_mouse_x - g_win[win].x;
    ui->my = g_mouse_y - g_win[win].y;
    ui->inside = (ui->mx >= 0 && ui->my >= 0 &&
                  ui->mx < g_win[win].w && ui->my < g_win[win].h);
#ifdef NANO_WMIN_H
    // With more than one window on screen, "the pointer is within this
    // window's rectangle" is not the same question as "the pointer is over
    // this window". Two overlapping windows both answer yes to the first, and
    // a button hidden underneath another window is then still clickable
    // through it. wmin already walks the z-order front to back; ask it.
    if (ui->inside && wm_hit_win(g_mouse_x, g_mouse_y) != win) ui->inside = 0;
#endif
}

// Start a frame: reset the id counter and the per-frame counters, then point
// the layout at `win`.
void ui_begin(struct Ui *ui, long win, long x, long y, long w) {
    ui->id = 0;
    ui->hot = -1;
    ui->invalidations = 0;
    ui->drawn = 0;
    ui->skipped = 0;
    // Nothing is known to have been painted over until the caller says so,
    // and it must say so every frame: an overpaint rectangle that outlived
    // its frame would be a widget redrawing forever for a reason that has
    // already gone away.
    ui->ovwin = -1;
    ui->ovx0 = 0; ui->ovy0 = 0; ui->ovx1 = -1; ui->ovy1 = -1;
    ui_window(ui, win, x, y, w);
    // Cleared every frame: it only ever suppresses the click that dismissed
    // a dropdown, and only for that frame.
    ui->swallow = 0;
}

// "Something has drawn over this rectangle of this window since the last ui
// pass." Called with the box a renderer just wrote -- gl_flush's damage box,
// in window coordinates -- so the widgets standing on that ground know to
// stand up again.
void ui_overpaint(struct Ui *ui, long win, long x, long y, long w, long h) {
    ui->ovwin = win;
    ui->ovx0 = x;
    ui->ovy0 = y;
    ui->ovx1 = x + w - 1;
    ui->ovy1 = y + h - 1;
}

// End the frame and consume the edges.
//
// A press edge is true for exactly one frame. Leaving it set means the next
// frame sees the same click again, and a button fires twice per press -- which
// looks like a hardware fault and is not.
void ui_end(struct Ui *ui) {
    if (ui->mreleased) ui->active = -1;
    ui->mpressed = 0;
    ui->mreleased = 0;
    ui->key = 0;
}

// Feed the frame from the window manager's state. `pressed` and `released` are
// edges the caller worked out from consecutive mouse events.
void ui_input(struct Ui *ui, long down, long pressed, long released, long key) {
    ui->mdown = down;
    if (pressed) ui->mpressed = 1;
    if (released) ui->mreleased = 1;
    if (key) ui->key = key;
}

// ---------- layout ----------

// The next `n` widgets share one row, each an equal fraction of the width.
void ui_row(struct Ui *ui, long n) {
    if (n < 1) n = 1;
    ui->cols = n;
    ui->colw = (ui->pw - (n - 1) * UI_PAD) / n;
    ui->rowy = ui->y;
    ui->x = ui->ox;
}

// Claim the next widget rectangle. Returns its width; x and y are ui->x/ui->y.
long ui_slot(struct Ui *ui) {
    long w;
    if (ui->cols > 0) w = ui->colw;
    else              w = ui->pw;
    ui->cw = w;
    ui->ch = UI_ROW_H;
    return w;
}

void ui_advance_h(struct Ui *ui, long w, long h) {
    if (ui->cols > 1) {
        ui->cols = ui->cols - 1;
        ui->x = ui->x + w + UI_PAD;
    } else {
        ui->cols = 0;
        ui->x = ui->ox;
        ui->y = ui->y + h + UI_PAD;
    }
}

void ui_advance(struct Ui *ui, long w) { ui_advance_h(ui, w, UI_ROW_H); }

// Restart the layout somewhere else in the same window WITHOUT resetting the
// id counter. That distinction is the whole point: ui_begin resets ids, so
// calling it twice in a frame gives two widgets the same identity and they
// share a hover state at a distance. This is how a panel gets drawn on top of
// a 3D viewport in the same frame.
void ui_move_to(struct Ui *ui, long x, long y, long w) {
    ui->ox = x;
    ui->x = x;
    ui->y = y;
    ui->pw = w;
    ui->cols = 0;
    ui->colw = w;
}

// ---------- identity ----------

// Widgets are identified by the order they are called in, which is the normal
// immediate-mode scheme and has one sharp edge: if the SET of widgets drawn
// changes between frames -- an `if` that hides one -- every widget after it
// shifts identity, and a button can inherit the pressed state of the one that
// used to be there.
//
// The fix is to give the conditional widget an explicit id with ui_id(), so
// the numbering does not depend on whether it was drawn. There is a test for
// exactly this.
long ui_next_id(struct Ui *ui) {
    long i;
    i = ui->id;
    ui->id = ui->id + 1;
    return i;
}

void ui_id(struct Ui *ui, long id) { ui->id = id; }

long ui_hit(struct Ui *ui, long x, long y, long w, long h) {
    if (!ui->inside) return 0;
    return ui->mx >= x && ui->mx < x + w && ui->my >= y && ui->my < y + h;
}

// ---------- damage ----------
//
// Called by every widget after it has drawn itself into the backing buffer.
// `state` is any number that differs whenever the widget would LOOK different.
// If it matches last frame, nothing is invalidated and nothing reaches the
// screen -- the buffer was redrawn with identical pixels, so the screen is
// already correct.
// Does the widget currently being built overlap the rectangle something else
// painted over? Its rect is ui->x, ui->y, ui->cw, ui->ch -- claimed by
// ui_slot, which is why ui_paint is called after it and not before.
long ui_over(struct Ui *ui) {
    if (ui->ovwin != ui->win) return 0;
    if (ui->ovx1 < ui->ovx0) return 0;
    if (ui->x + ui->cw - 1 < ui->ovx0) return 0;
    if (ui->ovx1 < ui->x) return 0;
    if (ui->y + ui->ch - 1 < ui->ovy0) return 0;
    if (ui->ovy1 < ui->y) return 0;
    return 1;
}

// SHOULD THIS WIDGET DRAW AT ALL?
//
// Called BEFORE the widget draws, with the number that changes whenever it
// would look different. It answers two separate questions in one place:
//
//   - has it changed? then draw it and tell the compositor.
//   - has it not? then the buffer already holds the right pixels, so draw
//     nothing at all -- unless something has painted over them, which only
//     the application can know and only ui_overpaint can say.
//
// This used to be ui_track, called AFTER drawing, and it skipped only the
// invalidation. The widget was redrawn every frame regardless, identically,
// into a buffer nobody would read: about 600 us a frame for seven widgets in
// the textured demo, which is 6% of a core at a hundred frames a second spent
// writing pixels that were already there.
long ui_paint(struct Ui *ui, long id, long state) {
    long changed;
    changed = 1;
    if (id >= 0 && id < UI_MAXID) {
        // An untracked id cannot be compared, so it must always be drawn and
        // always pushed. Failing the other way would leave a widget that
        // never updates.
        changed = (g_ui_last[id] != state);
        g_ui_last[id] = state;
    }
    if (changed) {
        wm_invalidate(ui->win, ui->x, ui->y, ui->cw, ui->ch);
        ui->invalidations = ui->invalidations + 1;
        ui->drawn = ui->drawn + 1;
        return 1;
    }
    // Unchanged, but standing on ground that has just been repainted. Draw,
    // and do NOT invalidate: whoever painted over it has already damaged this
    // rectangle, which is how the widget came to need redrawing.
    if (ui->always || ui_over(ui)) {
        ui->drawn = ui->drawn + 1;
        return 1;
    }
    ui->skipped = ui->skipped + 1;
    return 0;
}

// ---------- drawing helpers ----------

// Filled rectangle with a border in the theme's edge colour. Six arguments
// exactly, which is the ceiling; a seventh for the border colour would not
// compile, so the two widgets that want a different border draw it themselves.
void ui_box(struct Ui *ui, long x, long y, long w, long h, long fill) {
    wm_win_fill(ui->win, x, y, w, h, fill);
    wm_win_frame(ui->win, x, y, w, h, ui->edge);
}

long ui_strlen(char *s) {
    long n;
    n = 0;
    while (*s) { n = n + 1; s = s + 1; }
    return n;
}

// djb2 over the CONTENTS of a string.
//
// Widgets that show text used to remember the pointer instead, which is
// cheaper and wrong in two ways that both really happen:
//
//   - Two identical literals at two call sites are two different addresses in
//     a compiler that does not pool them, so the same word is "new text" every
//     frame and the widget repaints forever. That is exactly what it did: a
//     label reading "frustum" cost 2,400 pixels a frame while claiming to be
//     idle, and the frame it sat in was already repainting a 3D viewport, so
//     the extra rectangle merged into the viewport's damage and was invisible
//     in the total. It only showed up in a frame where nothing else moved.
//
//   - A label formatted into a scratch buffer keeps ONE address whatever it
//     says, so it would go the other way and never repaint at all.
//
// Hashing the bytes costs a few nanoseconds per widget per frame and is right
// in both directions.
long ui_hash_str(char *s) {
    long h;
    h = 5381;
    while (*s) { h = ((h * 33) + (*s & 255)) & 0xFFFFFFF; s = s + 1; }
    return h;
}

// Left-aligned text, drawn one glyph at a time and STOPPED at `maxw` pixels.
//
// A widget that draws outside its own rectangle is a widget that lies to the
// compositor. ui_paint invalidates the widget's rect and nothing else, so
// anything painted beyond it lands in the backing buffer and is never pushed
// to the screen -- the buffer and the screen then disagree forever. Since
// K24c it is worse than that: the neighbour it spilled onto believes its own
// pixels are still intact and will not redraw them.
//
// wm_win_text has no width limit; it clips to the WINDOW, which is far too
// late. Thirty-one characters in a two-hundred-pixel field spilled over the
// progress bar below it, and the only thing that noticed was hashing the
// framebuffer against a full repaint.
void ui_text_clip(struct Ui *ui, long x, long y, long h, char *s, long maxw) {
    long n;
    long vis;
    long i;
    long ty;
    n = ui_strlen(s);
    vis = maxw / FONT_W;
    if (vis < 0) vis = 0;
    if (n > vis) n = vis;
    ty = y + (h - FONT_H) / 2;
    i = 0;
    while (i < n) {
        wm_win_glyph(ui->win, x + i * FONT_W, ty, s[i] & 255, ui->fg);
        i = i + 1;
    }
}

// Text centred in the current widget rectangle, truncated rather than spilled.
void ui_text_mid(struct Ui *ui, char *s, long fg) {
    long n;
    long tx;
    long ty;
    long vis;
    long i;
    n = ui_strlen(s);
    vis = (ui->cw - 4) / FONT_W;
    if (vis < 0) vis = 0;
    if (n > vis) n = vis;
    tx = ui->x + (ui->cw - n * FONT_W) / 2;
    if (tx < ui->x + 2) tx = ui->x + 2;
    ty = ui->y + (ui->ch - FONT_H) / 2;
    i = 0;
    while (i < n) {
        wm_win_glyph(ui->win, tx + i * FONT_W, ty, s[i] & 255, fg);
        i = i + 1;
    }
}

// ---------- widgets ----------

void ui_label(struct Ui *ui, char *s) {
    long w;
    w = ui_slot(ui);
    // A label has no state of its own, but it still has to be tracked: the
    // first frame must draw it, and after that it changes only when its text
    // does. Which means hashing the text, not the pointer -- see ui_hash_str.
    if (ui_paint(ui, ui_next_id(ui), ui_hash_str(s))) {
        wm_win_fill(ui->win, ui->x, ui->y, w, UI_ROW_H, ui->panel);
        ui_text_clip(ui, ui->x + 2, ui->y, UI_ROW_H, s, w - 4);
    }
    ui_advance(ui, w);
}

// Returns 1 on a completed click: pressed on this widget AND released while
// still over it.
//
// The press-drag-off-release case must NOT count. That is the entire reason
// `active` exists separately from `hot`, and it is the behaviour a user relies
// on to change their mind after pressing a button.
long ui_button(struct Ui *ui, char *s) {
    long id;
    long w;
    long hot;
    long act;
    long clicked;
    long fill;

    id = ui_next_id(ui);
    w = ui_slot(ui);
    hot = ui_hit(ui, ui->x, ui->y, w, UI_ROW_H);
    clicked = 0;

    if (hot) ui->hot = id;
    if (hot && ui->mpressed && ui->active < 0) {
        ui->active = id;
        ui->focus = id;
    }
    act = (ui->active == id);
    if (act && ui->mreleased && hot) clicked = 1;

    if (act && ui->mdown) fill = ui->accent;
    else if (hot)         fill = ui->edge;
    else                  fill = ui->bg;

    // The caption is part of the visual state. A button whose label is
    // swapped -- "Play" to "Pause" -- looks different and must repaint, and
    // hot/pressed alone cannot tell.
    if (ui_paint(ui, id, (ui_hash_str(s) << 2) + (hot ? 2 : 0) +
                         ((act && ui->mdown) ? 1 : 0))) {
        ui_box(ui, ui->x, ui->y, w, UI_ROW_H, fill);
        ui_text_mid(ui, s, (act && ui->mdown) ? rgb(255, 255, 255) : ui->fg);
    }
    ui_advance(ui, w);
    return clicked;
}

// Toggles *v. Returns 1 if it changed this frame.
long ui_checkbox(struct Ui *ui, char *s, long *v) {
    long id;
    long w;
    long hot;
    long changed;
    long bx;
    long by;

    id = ui_next_id(ui);
    w = ui_slot(ui);
    hot = ui_hit(ui, ui->x, ui->y, w, UI_ROW_H);
    changed = 0;

    if (hot) ui->hot = id;
    if (hot && ui->mpressed && ui->active < 0) { ui->active = id; ui->focus = id; }
    if (ui->active == id && ui->mreleased && hot) {
        *v = !*v;
        changed = 1;
    }

    bx = ui->x;
    by = ui->y + (UI_ROW_H - UI_BOX) / 2;
    if (ui_paint(ui, id, (ui_hash_str(s) << 2) + (hot ? 2 : 0) + (*v ? 1 : 0))) {
    wm_win_fill(ui->win, ui->x, ui->y, w, UI_ROW_H, ui->panel);
    ui_box(ui, bx, by, UI_BOX, UI_BOX, hot ? ui->edge : ui->bg);
    if (*v) {
        // A tick, drawn as two strokes rather than a glyph, so it does not
        // depend on the font having one.
        long i;
        i = 0;
        while (i < 4) {
            wm_win_pixel(ui->win, bx + 2 + i, by + 5 + i, ui->accent);
            wm_win_pixel(ui->win, bx + 2 + i, by + 6 + i, ui->accent);
            i = i + 1;
        }
        i = 0;
        while (i < 6) {
            wm_win_pixel(ui->win, bx + 5 + i, by + 8 - i, ui->accent);
            wm_win_pixel(ui->win, bx + 5 + i, by + 9 - i, ui->accent);
            i = i + 1;
        }
    }
    ui_text_clip(ui, bx + UI_BOX + 6, ui->y, UI_ROW_H, s, w - UI_BOX - 8);
    }
    ui_advance(ui, w);
    return changed;
}

// An integer slider. Returns 1 if *v changed this frame.
//
// Integer on purpose, not as a limitation: nano_cc has no floats, and a slider
// that reports 0..100 is what a caller wants anyway. The position is worked out
// with one multiply and one divide, in that order -- dividing first would throw
// away every value below the step size.
long ui_slider(struct Ui *ui, long *v, long lo, long hi) {
    long id;
    long w;
    long hot;
    long act;
    long changed;
    long track;
    long hx;
    long old;

    id = ui_next_id(ui);
    w = ui_slot(ui);
    hot = ui_hit(ui, ui->x, ui->y, w, UI_ROW_H);
    changed = 0;
    old = *v;
    if (hi <= lo) hi = lo + 1;

    if (hot) ui->hot = id;
    if (hot && ui->mpressed && ui->active < 0) { ui->active = id; ui->focus = id; }
    act = (ui->active == id);

    track = w - UI_BOX;
    if (track < 1) track = 1;

    // While this slider owns the pointer it keeps tracking, even if the
    // pointer has left the widget. Dropping out the moment the cursor strays a
    // pixel above the track is the single most irritating slider bug there is.
    if (act && ui->mdown) {
        long rel;
        rel = ui->mx - ui->x - UI_BOX / 2;
        if (rel < 0) rel = 0;
        if (rel > track) rel = track;
        *v = lo + (rel * (hi - lo) + track / 2) / track;
        if (*v < lo) *v = lo;
        if (*v > hi) *v = hi;
        if (*v != old) changed = 1;
    }

    hx = ui->x + ((*v - lo) * track) / (hi - lo);

    if (ui_paint(ui, id, (*v << 2) + (hot ? 2 : 0) +
                         ((act && ui->mdown) ? 1 : 0))) {
        wm_win_fill(ui->win, ui->x, ui->y, w, UI_ROW_H, ui->panel);
        ui_box(ui, ui->x, ui->y + UI_ROW_H / 2 - 2, w, 4, ui->bg);
        ui_box(ui, hx, ui->y + 2, UI_BOX, UI_ROW_H - 4,
               (act && ui->mdown) ? ui->accent : (hot ? ui->edge : ui->bg));
    }
    ui_advance(ui, w);
    return changed;
}

// A single-line text field. `buf` holds a NUL-terminated string of at most
// cap-1 characters. Returns 1 if it was edited this frame.
long ui_text(struct Ui *ui, char *buf, long cap) {
    long id;
    long w;
    long hot;
    long focused;
    long changed;
    long n;
    long hash;
    long i;

    id = ui_next_id(ui);
    w = ui_slot(ui);
    hot = ui_hit(ui, ui->x, ui->y, w, UI_ROW_H);
    changed = 0;

    if (hot) ui->hot = id;
    // Clicking a field focuses it; clicking anywhere else must UNfocus it, or
    // two fields both take the same keystroke.
    if (ui->mpressed) {
        // `ui->active < 0` for the same reason as everywhere else: a widget
        // that already owns the pointer keeps it. Without it, dragging a
        // slider across a text field hands the field the focus mid-drag.
        if (hot && ui->active < 0) { ui->focus = id; ui->active = id; }
        else if (!hot && ui->focus == id) ui->focus = -1;
    }
    focused = (ui->focus == id);

    n = 0;
    while (buf[n]) n = n + 1;

    if (focused && ui->key) {
        long k;
        k = ui->key;
        if (k == '\b') {
            if (n > 0) { n = n - 1; buf[n] = 0; changed = 1; }
        } else if (k >= 32 && k <= 126) {
            if (n < cap - 1) { buf[n] = k; n = n + 1; buf[n] = 0; changed = 1; }
        }
    }

    // The contents are part of the visual state, so they have to be in the
    // hash. Using only the length would miss a character being replaced.
    hash = 5381;
    i = 0;
    while (i < n) { hash = ((hash * 33) + (buf[i] & 255)) & 0xFFFFFFF; i = i + 1; }

    if (ui_paint(ui, id, (hash << 3) + (focused ? 4 : 0) + (hot ? 2 : 0))) {
        wm_win_fill(ui->win, ui->x, ui->y, w, UI_ROW_H,
                    focused ? rgb(255, 255, 255) : ui->bg);
        wm_win_frame(ui->win, ui->x, ui->y, w, UI_ROW_H,
                     focused ? ui->accent : ui->edge);
        // Longer than the box: show the TAIL, because that is where the caret
        // is and where the characters being typed appear. Showing the head
        // instead gives a field that stops responding visibly once it is full.
        {
            long vis;
            long off;
            vis = (w - 6) / FONT_W;
            if (vis < 0) vis = 0;
            off = 0;
            if (n > vis) off = n - vis;
            ui_text_clip(ui, ui->x + 3, ui->y, UI_ROW_H, buf + off, w - 6);
            if (focused)
                wm_win_fill(ui->win, ui->x + 3 + (n - off) * FONT_W, ui->y + 3,
                            1, UI_ROW_H - 6, ui->fg);
        }
    }
    ui_advance(ui, w);
    return changed;
}

// Read-only bar. Useful on its own and the simplest possible check that
// tracking works: it changes only when its value does.
void ui_progress(struct Ui *ui, long v, long lo, long hi) {
    long id;
    long w;
    long fill;

    id = ui_next_id(ui);
    w = ui_slot(ui);
    if (hi <= lo) hi = lo + 1;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    fill = ((v - lo) * (w - 2)) / (hi - lo);

    if (ui_paint(ui, id, v)) {
        ui_box(ui, ui->x, ui->y, w, UI_ROW_H, ui->bg);
        if (fill > 0) wm_win_fill(ui->win, ui->x + 1, ui->y + 1, fill,
                                  UI_ROW_H - 2, ui->accent);
    }
    ui_advance(ui, w);
}

// ---------- the 3D viewport widget ----------
//
// The one widget that draws none of its own interior. It claims a rectangle,
// draws a one-pixel border, and hands the inside to whoever is rendering --
// which for this project means gl_bind(&ctx, win, v.x, v.y, v.w, v.h).
//
// It is a widget in every other respect: it takes hover, it takes focus, it
// owns the pointer while dragged, and it reports the pointer motion and the
// keystrokes that arrive while it has focus. That is what makes "orbit with
// the mouse, walk with the keys" work without the application hit-testing
// anything, and what lets other widgets sit ON TOP of it -- draw the 3D first,
// draw the panel afterwards, and the buffer holds the composite.
//
// Damage: the border is tracked like any other widget, so it costs nothing
// while the focus state is unchanged. The interior is not tracked at all,
// because the renderer already reports the exact box of pixels it wrote. A
// widget that invalidated its whole viewport every frame would throw away
// precisely the saving the last two milestones were about.

struct GlView {
    long x;                    // interior, in WINDOW coordinates
    long y;
    long w;
    long h;
    long hot;
    long active;               // owns the pointer -- a drag continues outside
    long focused;
    long mx;                   // pointer in viewport coordinates, or -1
    long my;
    long dx;                   // pointer motion this frame, while dragging
    long dy;
    long key;                  // a keystroke, only when focused
    long clicked;
    long px;                   // internal: last pointer position
    long py;
    long seen;                 // internal: has px/py ever been set
};

void ui_glview_init(struct GlView *v) {
    v->hot = 0; v->active = 0; v->focused = 0;
    v->mx = -1; v->my = -1;
    v->dx = 0; v->dy = 0;
    v->key = 0; v->clicked = 0;
    v->px = 0; v->py = 0; v->seen = 0;
}

long ui_glview(struct Ui *ui, struct GlView *v, long h) {
    long id;
    long w;
    long hot;
    long act;

    id = ui_next_id(ui);
    w = ui_slot(ui);
    ui->ch = h;
    hot = ui_hit(ui, ui->x, ui->y, w, h);

    v->x = ui->x + 1;
    v->y = ui->y + 1;
    v->w = w - 2;
    v->h = h - 2;
    v->clicked = 0;
    v->dx = 0;
    v->dy = 0;
    v->key = 0;

    if (hot) ui->hot = id;
    // `ui->active < 0` matters more here than on any other widget. A viewport
    // fills its whole rectangle, so a widget drawn ON TOP of it is inside it
    // too, and without this guard the viewport takes the pointer out from
    // under the button the user actually pressed -- the button is painted, is
    // highlighted, and does nothing.
    //
    // The guard is necessary and not sufficient: the overlay must also be
    // asked BEFORE the viewport. Immediate mode ties input order to draw
    // order, and this is the one place they have to differ -- the overlay is
    // painted last and must be offered the pointer first. See glapi.c's
    // event loop, where the HUD is built between the render and this call.
    if (ui->mpressed) {
        if (hot && ui->active < 0) { ui->active = id; ui->focus = id; }
        else if (!hot && ui->focus == id) ui->focus = -1;
    }
    act = (ui->active == id);
    if (act && ui->mreleased && hot) v->clicked = 1;

    v->hot = hot;
    v->active = act;
    v->focused = (ui->focus == id);

    if (hot) { v->mx = ui->mx - v->x; v->my = ui->my - v->y; }
    else     { v->mx = -1; v->my = -1; }

    // Motion is measured from the previous frame's pointer position, and only
    // while this view owns the pointer. Measuring it from the press point
    // instead gives an accelerating drag; measuring it every frame regardless
    // of ownership makes the camera swing when the user drags a slider past.
    if (act && ui->mdown) {
        if (v->seen) { v->dx = ui->mx - v->px; v->dy = ui->my - v->py; }
        v->px = ui->mx;
        v->py = ui->my;
        v->seen = 1;
    } else {
        v->seen = 0;
    }

    if (v->focused && ui->key) v->key = ui->key;

    // The border only; the interior belongs to whoever is rendering into it.
    // Its own state is hover and focus, so it repaints when those change --
    // and also whenever the scene has been rendered inside it, because a
    // viewport that has just been repainted is exactly the rectangle
    // ui_overpaint describes. That costs the outline, about a thousand
    // pixels, and it is the honest answer: the widget's rect includes the
    // interior, so it cannot claim the ground under it is untouched.
    if (ui_paint(ui, id, (hot ? 2 : 0) + (v->focused ? 1 : 0)))
        wm_win_frame(ui->win, ui->x, ui->y, w, h,
                     v->focused ? ui->accent : (hot ? ui->fg : ui->edge));
    ui_advance_h(ui, w, h);
    return act && ui->mdown;
}

// ---------- setup ----------

// ============================================================
// a text buffer you can edit
// ============================================================
//
// A flat array of bytes with newlines in it, and a caret that is an OFFSET
// into it. Not a list of lines.
//
// The line-list version is the tempting one -- it makes "go to line 40" free.
// It also makes every edit that crosses a line boundary a splice of two data
// structures, and makes the buffer disagree with the bytes you save. A flat
// array is what the file IS, so saving is one fs_write and loading is one
// fs_read, and there is no second representation to drift.
//
// The cost is that finding line starts means scanning. That is paid once per
// frame in ed_layout, over the visible window only, and the visible window is
// about forty lines.

#define ED_CAP    16384        // one buffer's bytes
#define ED_MAXLN  2048         // line starts cached per layout

struct Edit {
    char *buf;                 // caller-owned, ED_CAP bytes
    long len;                  // bytes in use, not counting the terminator
    long caret;                // offset of the insertion point, 0..len
    long top;                  // first visible line
    long dirty;                // edited since the last save
    long rows;                 // visible rows, set by the widget each frame
    long cols;
    // Show a line-number gutter. Off by default, because a widget that
    // changes its own text area depending on a flag the caller did not set
    // is a surprise.
    long nums;
    // Line starts, recomputed each frame. line[i] is the offset of the first
    // byte of line i; nlines is how many there are. Always at least one line,
    // because an empty buffer still has a place to type.
    long line[ED_MAXLN];
    long nlines;
};

void ed_init(struct Edit *e, char *storage) {
    long i;
    e->buf = storage;
    e->buf[0] = 0;
    e->len = 0;
    e->caret = 0;
    e->top = 0;
    e->dirty = 0;
    e->rows = 1;
    e->cols = 1;
    e->nums = 0;
    e->nlines = 1;
    i = 0;
    while (i < ED_MAXLN) { e->line[i] = 0; i = i + 1; }
}

// Recompute the line table. Cheap and total, rather than incremental: an
// incremental version has to be right about every edit that inserts or
// removes a newline, and being wrong there means the caret lands on the wrong
// line in a way that looks like a rendering bug.
void ed_layout(struct Edit *e) {
    long i;
    long n;
    n = 0;
    e->line[n] = 0;
    n = 1;
    i = 0;
    while (i < e->len && n < ED_MAXLN) {
        if (e->buf[i] == '\n') { e->line[n] = i + 1; n = n + 1; }
        i = i + 1;
    }
    e->nlines = n;
}

// Which line an offset falls on. Linear, because the table is small and a
// binary search here would be the third place that has to agree about what a
// line start means.
long ed_line_of(struct Edit *e, long off) {
    long i;
    i = 0;
    while (i + 1 < e->nlines && e->line[i + 1] <= off) i = i + 1;
    return i;
}

long ed_line_len(struct Edit *e, long ln) {
    long start;
    long end;
    if (ln < 0 || ln >= e->nlines) return 0;
    start = e->line[ln];
    if (ln + 1 < e->nlines) end = e->line[ln + 1] - 1;   // minus the newline
    else end = e->len;
    if (end < start) end = start;
    return end - start;
}

long ed_col_of(struct Edit *e, long off) {
    return off - e->line[ed_line_of(e, off)];
}

void ed_insert(struct Edit *e, long ch) {
    long i;
    if (e->len + 1 >= ED_CAP) return;
    i = e->len;
    while (i > e->caret) { e->buf[i] = e->buf[i - 1]; i = i - 1; }
    e->buf[e->caret] = ch;
    e->len = e->len + 1;
    e->caret = e->caret + 1;
    e->buf[e->len] = 0;
    e->dirty = 1;
    ed_layout(e);
}

// Delete the byte AT `at`. Backspace and Delete are the same operation on
// different offsets, which is why there is one of these and not two.
void ed_delete_at(struct Edit *e, long at) {
    long i;
    if (at < 0 || at >= e->len) return;
    i = at;
    while (i < e->len - 1) { e->buf[i] = e->buf[i + 1]; i = i + 1; }
    e->len = e->len - 1;
    e->buf[e->len] = 0;
    if (e->caret > at) e->caret = e->caret - 1;
    e->dirty = 1;
    ed_layout(e);
}

// Vertical movement keeps the COLUMN, clamped to the target line's length --
// which is what every editor does and what makes arrowing down a ragged file
// feel right rather than snapping to column zero.
void ed_move_vert(struct Edit *e, long delta) {
    long ln;
    long col;
    long want;
    ln = ed_line_of(e, e->caret);
    col = e->caret - e->line[ln];
    want = ln + delta;
    if (want < 0) want = 0;
    if (want >= e->nlines) want = e->nlines - 1;
    if (col > ed_line_len(e, want)) col = ed_line_len(e, want);
    e->caret = e->line[want] + col;
}

// Keep the caret on screen. Called after every movement rather than inside
// each one, so there is a single place that decides what "visible" means.
void ed_scroll_to_caret(struct Edit *e) {
    long ln;
    ln = ed_line_of(e, e->caret);
    if (ln < e->top) e->top = ln;
    if (e->rows > 0 && ln >= e->top + e->rows) e->top = ln - e->rows + 1;
    if (e->top < 0) e->top = 0;
}

// Apply one keystroke. Returns 1 if the buffer or caret changed.
long ed_key(struct Edit *e, long k) {
    long before;
    before = e->caret;
    if (k == 0) return 0;

    if (k == KEY_LEFT)  { if (e->caret > 0) e->caret = e->caret - 1; }
    else if (k == KEY_RIGHT) { if (e->caret < e->len) e->caret = e->caret + 1; }
    else if (k == KEY_UP)    ed_move_vert(e, 0 - 1);
    else if (k == KEY_DOWN)  ed_move_vert(e, 1);
    else if (k == KEY_PGUP)  ed_move_vert(e, 0 - e->rows);
    else if (k == KEY_PGDN)  ed_move_vert(e, e->rows);
    else if (k == KEY_HOME)  e->caret = e->line[ed_line_of(e, e->caret)];
    else if (k == KEY_END) {
        long ln;
        ln = ed_line_of(e, e->caret);
        e->caret = e->line[ln] + ed_line_len(e, ln);
    }
    else if (k == KEY_DEL)   { ed_delete_at(e, e->caret); ed_scroll_to_caret(e); return 1; }
    else if (k == '\b')      { if (e->caret > 0) { ed_delete_at(e, e->caret - 1); ed_scroll_to_caret(e); return 1; } }
    else if (k == '\n' || k == '\r') { ed_insert(e, '\n'); ed_scroll_to_caret(e); return 1; }
    else if (k == '\t')      { ed_insert(e, ' '); ed_insert(e, ' '); ed_scroll_to_caret(e); return 1; }
    else if (k >= 32 && k <= 126) { ed_insert(e, k); ed_scroll_to_caret(e); return 1; }
    else return 0;

    ed_scroll_to_caret(e);
    return e->caret != before;
}

// ============================================================
// scrollbars
// ============================================================
//
// Not a widget the caller places -- a part of every widget that scrolls. A
// scrollbar you have to remember to add is one that is missing from whichever
// view got written last, and the caller would have to duplicate the
// arithmetic that decides where the thumb goes, which is the arithmetic most
// likely to disagree with the view beside it.
//
// So ui_edit, ui_list and ui_icongrid each reserve UI_SB_W down their right
// edge and call these two. Nothing else has to know.

#define UI_SB_W 10

// The scroll state, as a struct rather than three loose arguments.
//
// nano_cc stops at six call arguments, and a scrollbar wants a window rect
// (four) plus top/total/visible (three) plus the context and an id -- nine.
// Bundling the three that always travel together is the fix, and it is the
// better shape anyway: a view's scroll position, its extent and its height
// are one thing and get passed around as one.
struct Scroll {
    long top;        // first visible unit
    long total;      // units there are
    long visible;    // units that fit
};

// Where the thumb sits. Through globals because nano_cc has no out-parameters
// worth the name and both callers want both numbers.
long g_sb_y;
long g_sb_h;

void ui_scroll_geom(long y, long h, struct Scroll *sc) {
    long th;
    long top; long total; long visible;
    top = sc->top; total = sc->total; visible = sc->visible;
    if (total <= visible || total <= 0) { g_sb_y = y; g_sb_h = h; return; }
    // Proportional, with a floor: a thumb for a 4000-line file would be one
    // pixel tall and impossible to grab.
    th = (h * visible) / total;
    if (th < 12) th = 12;
    if (th > h) th = h;
    g_sb_h = th;
    g_sb_y = y + ((h - th) * top) / (total - visible);
    if (g_sb_y < y) g_sb_y = y;
    if (g_sb_y + th > y + h) g_sb_y = y + h - th;
}

void ui_scroll_draw(struct Ui *ui, long x, long y, long h, struct Scroll *sc) {
    long w;
    w = UI_SB_W;
    wm_win_fill(ui->win, x, y, w, h, rgb(232, 232, 238));
    wm_win_frame(ui->win, x, y, w, h, ui->edge);
    if (sc->total <= sc->visible) return;  // nothing to scroll: track only
    ui_scroll_geom(y, h, sc);
    wm_win_fill(ui->win, x + 2, g_sb_y + 1, w - 4, g_sb_h - 2, rgb(150, 150, 165));
    wm_win_frame(ui->win, x + 2, g_sb_y + 1, w - 4, g_sb_h - 2, ui->edge);
}

// Handle a press or a drag on the track. Returns the new `top`.
//
// Dragging uses ui->active, the same ownership rule as the slider: once the
// bar owns the pointer it keeps it, so sliding off the side mid-drag does not
// hand the pointer to whatever is next to it.
long ui_scroll_input(struct Ui *ui, long id, long x, long y, long h,
                     struct Scroll *sc) {
    long on;
    long top;
    long total;
    long visible;
    top = sc->top; total = sc->total; visible = sc->visible;
    if (total <= visible) return top;
    on = ui_hit(ui, x, y, UI_SB_W, h);
    if (on && ui->mpressed && ui->active < 0) ui->active = id;
    if (ui->active == id && ui->mdown) {
        long want;
        long th;
        ui_scroll_geom(y, h, sc);
        th = g_sb_h;
        // Centre the thumb on the pointer, then convert back to a line.
        want = ui->my - y - th / 2;
        if (want < 0) want = 0;
        if (want > h - th) want = h - th;
        if (h - th > 0) top = (want * (total - visible)) / (h - th);
        else top = 0;
        if (top < 0) top = 0;
        if (top > total - visible) top = total - visible;
    }
    return top;
}

// How wide the line-number gutter is, in pixels, for a buffer of this many
// lines. Sized to the LARGEST number it will show rather than to a constant:
// a 4-digit gutter on a 12-line file wastes a fifth of a narrow window, and a
// fixed 3-digit one starts overlapping the text at line 1000.
long ui_gutter_w(struct Edit *e) {
    long digits;
    long n;
    if (!e->nums) return 0;
    digits = 1;
    n = e->nlines;
    while (n >= 10) { n = n / 10; digits = digits + 1; }
    if (digits < 2) digits = 2;
    return digits * FONT_W + 6;
}

// The editing area. Returns 1 if this frame changed the buffer or the caret.
//
// The state hash is the whole reason this is cheap: it folds the caret, the
// scroll position, the length and a hash of the VISIBLE text, so a frame in
// which nothing moved repaints nothing -- the same rule as every other widget
// here, and the reason an idle editor costs no pixels.
long ui_edit(struct Ui *ui, struct Edit *e, long h) {
    long id;
    long w;
    long hot;
    long focused;
    long changed;
    long hash;
    long i;
    long r;

    id = ui_next_id(ui);
    w = ui_slot(ui);
    hot = ui_hit(ui, ui->x, ui->y, w, h);
    changed = 0;

    if (hot) ui->hot = id;
    if (ui->mpressed) {
        if (hot && ui->active < 0) { ui->focus = id; ui->active = id; }
        else if (!hot && ui->focus == id) ui->focus = -1;
    }
    focused = (ui->focus == id);

    e->rows = (h - 4) / FONT_H;
    // Minus the scrollbar: the text stops where the bar starts, or the last
    // column of every long line is drawn underneath it.
    e->cols = (w - 6 - UI_SB_W - ui_gutter_w(e)) / FONT_W;
    if (e->rows < 1) e->rows = 1;
    if (e->cols < 1) e->cols = 1;

    // Clicking inside puts the caret where the click was, which is the one
    // piece of mouse editing worth having before selection exists.
    if (hot && ui->mpressed && ui->mx < ui->x + w - UI_SB_W) {
        long row;
        long col;
        long ln;
        row = (ui->my - ui->y - 2) / FONT_H;
        col = (ui->mx - ui->x - 3 - ui_gutter_w(e)) / FONT_W;
        if (row < 0) row = 0;
        if (col < 0) col = 0;
        ln = e->top + row;
        if (ln >= e->nlines) ln = e->nlines - 1;
        if (col > ed_line_len(e, ln)) col = ed_line_len(e, ln);
        e->caret = e->line[ln] + col;
        changed = 1;
    }

    if (focused && ui->key) {
        if (ed_key(e, ui->key)) changed = 1;
    }

    {
        struct Scroll sc;
        long nt;
        sc.top = e->top; sc.total = e->nlines; sc.visible = e->rows;
        nt = ui_scroll_input(ui, id + 900, ui->x + w - UI_SB_W, ui->y, h, &sc);
        if (nt != e->top) { e->top = nt; changed = 1; }
    }

    hash = 5381;
    hash = ((hash * 33) + e->caret) & 0xFFFFFFF;
    hash = ((hash * 33) + e->top) & 0xFFFFFFF;
    hash = ((hash * 33) + e->len) & 0xFFFFFFF;
    hash = ((hash * 33) + e->nlines) & 0xFFFFFFF;
    hash = ((hash * 33) + e->nums) & 0xFFFFFFF;
    r = 0;
    while (r < e->rows && e->top + r < e->nlines) {
        long ln;
        long st;
        long n;
        ln = e->top + r;
        st = e->line[ln];
        n = ed_line_len(e, ln);
        if (n > e->cols) n = e->cols;
        i = 0;
        while (i < n) { hash = ((hash * 33) + (e->buf[st + i] & 255)) & 0xFFFFFFF; i = i + 1; }
        r = r + 1;
    }

    if (ui_paint(ui, id, (hash << 2) + (focused ? 2 : 0))) {
        long gw;
        gw = ui_gutter_w(e);
        wm_win_fill(ui->win, ui->x, ui->y, w, h, rgb(255, 255, 255));
        if (gw) {
            wm_win_fill(ui->win, ui->x, ui->y, gw, h, rgb(242, 242, 246));
            wm_win_fill(ui->win, ui->x + gw - 1, ui->y, 1, h, ui->edge);
        }
        wm_win_frame(ui->win, ui->x, ui->y, w, h,
                     focused ? ui->accent : ui->edge);
        r = 0;
        while (r < e->rows && e->top + r < e->nlines) {
            long ln;
            long st;
            long n;
            char save;
            ln = e->top + r;
            st = e->line[ln];
            n = ed_line_len(e, ln);
            if (n > e->cols) n = e->cols;
            // Draw the line by terminating it in place and putting the byte
            // back. The buffer is one array with no per-line terminators, so
            // there is nothing else to hand a string-drawing call -- and
            // copying each line into a scratch buffer would be a second copy
            // of the text that can disagree with the first.
            save = e->buf[st + n];
            e->buf[st + n] = 0;
            ui_text_clip(ui, ui->x + 3 + gw,
                         ui->y + 2 + r * FONT_H - (UI_ROW_H - FONT_H) / 2,
                         UI_ROW_H, e->buf + st, w - 6 - gw);
            e->buf[st + n] = save;
            if (gw) {
                // Right-aligned, the way every editor does it, so the digits
                // line up against the text instead of against the margin.
                char num[12];
                long v;
                long d;
                long k;
                v = ln + 1;
                d = 0;
                while (v > 0 && d < 11) { num[d] = '0' + (v % 10); v = v / 10; d = d + 1; }
                if (d == 0) { num[0] = '0'; d = 1; }
                k = 0;
                while (k < d) {
                    wm_win_glyph(ui->win, ui->x + 3 + (d - 1 - k) * FONT_W,
                                 ui->y + 2 + r * FONT_H, num[k],
                                 rgb(150, 150, 165));
                    k = k + 1;
                }
            }
            r = r + 1;
        }
        if (focused) {
            long cl;
            long cc;
            cl = ed_line_of(e, e->caret);
            cc = e->caret - e->line[cl];
            if (cc > e->cols) cc = e->cols;
            if (cl >= e->top && cl < e->top + e->rows)
                wm_win_fill(ui->win, ui->x + 3 + gw + cc * FONT_W,
                            ui->y + 2 + (cl - e->top) * FONT_H, 1, FONT_H, ui->fg);
        }
        {
            struct Scroll sc;
            sc.top = e->top; sc.total = e->nlines; sc.visible = e->rows;
            ui_scroll_draw(ui, ui->x + w - UI_SB_W, ui->y, h, &sc);
        }
    }
    ui_advance_h(ui, w, h);
    return changed;
}

// ============================================================
// a menu bar
// ============================================================
//
// NO FUNCTION POINTERS, so a menu cannot be a list of callbacks. It is a
// table of strings plus a table saying which menu each item belongs to, and
// the widget returns the index of whatever was chosen. The caller switches on
// it -- the same shape as the display-list opcodes and the syscall
// dispatcher, for the same reason.
//
// The open menu is remembered in the Ui rather than by the caller, because a
// dropdown that is open across frames is state and there is exactly one of it.

// Returns the chosen item index, or -1. `tops` are the bar labels, `items`
// the entries, `owner[i]` the bar index item i belongs to.
long ui_menubar(struct Ui *ui, char **tops, long ntops,
                char **items, long *owner, long nitems) {
    long i;
    long x;
    long chosen;
    long id;
    long barh;

    chosen = UI_MENU_NONE;
    barh = UI_ROW_H;
    id = ui_next_id(ui);

    // The bar itself.
    x = ui->ox;
    i = 0;
    while (i < ntops) {
        long tw;
        long hot;
        tw = (ui_strlen(tops[i]) + 2) * FONT_W;
        hot = ui_hit(ui, x, ui->y, tw, barh);
        if (hot && ui->mpressed) {
            // Clicking the open menu's own title closes it. Without this the
            // only way to dismiss a menu is to pick something from it.
            if (ui->menu_open == i) ui->menu_open = UI_MENU_NONE;
            else ui->menu_open = i;
        }
        if (ui_paint(ui, id + i, (ui->menu_open == i ? 2 : 0) + (hot ? 1 : 0))) {
            wm_win_fill(ui->win, x, ui->y, tw, barh,
                        ui->menu_open == i ? ui->accent : ui->bg);
            wm_win_text(ui->win, x + FONT_W, ui->y + (barh - FONT_H) / 2,
                        tops[i], ui->menu_open == i ? rgb(255,255,255) : ui->fg);
        }
        x = x + tw;
        i = i + 1;
    }

    // The open dropdown, drawn UNDER the bar and over whatever is beneath it.
    if (ui->menu_open != UI_MENU_NONE) {
        long dy;
        long dw;
        long n;
        long ox;
        // Where this menu's title starts, so the dropdown lines up with it.
        ox = ui->ox;
        i = 0;
        while (i < ui->menu_open) { ox = ox + (ui_strlen(tops[i]) + 2) * FONT_W; i = i + 1; }

        dw = 0;
        n = 0;
        i = 0;
        while (i < nitems) {
            if (owner[i] == ui->menu_open) {
                long tw;
                tw = (ui_strlen(items[i]) + 3) * FONT_W;
                if (tw > dw) dw = tw;
                n = n + 1;
            }
            i = i + 1;
        }

        dy = ui->y + barh;
        // The panel is repainted every frame it is open. It sits on top of
        // other widgets, so it cannot use the skip-if-unchanged path: the
        // thing underneath does not know it is covered, and the first frame
        // after it closes is the only chance anyone has to repaint it.
        wm_win_fill(ui->win, ox, dy, dw, n * UI_ROW_H, ui->bg);
        wm_win_frame(ui->win, ox, dy, dw, n * UI_ROW_H, ui->edge);
        wm_invalidate(ui->win, ox, dy, dw, n * UI_ROW_H);

        {
            long row;
            row = 0;
            i = 0;
            while (i < nitems) {
                if (owner[i] == ui->menu_open) {
                    long iy;
                    long hot;
                    iy = dy + row * UI_ROW_H;
                    hot = ui_hit(ui, ox, iy, dw, UI_ROW_H);
                    if (hot) {
                        wm_win_fill(ui->win, ox + 1, iy + 1, dw - 2, UI_ROW_H - 2,
                                    ui->accent);
                    }
                    wm_win_text(ui->win, ox + FONT_W, iy + (UI_ROW_H - FONT_H) / 2,
                                items[i], hot ? rgb(255,255,255) : ui->fg);
                    if (hot && ui->mpressed) {
                        chosen = i;
                        ui->menu_open = UI_MENU_NONE;
                        // The click is spent. Widgets drawn after this one
                        // must not also see it, or choosing "Open" from a
                        // menu that overlaps a button presses the button too.
                        ui->swallow = 1;
                    }
                    row = row + 1;
                }
                i = i + 1;
            }
        }

        // A click anywhere else closes it, and must NOT also reach the widget
        // underneath -- otherwise dismissing a menu presses whatever happened
        // to be behind it.
        if (ui->mpressed && chosen == UI_MENU_NONE) {
            if (!ui_hit(ui, ox, dy, dw, n * UI_ROW_H) &&
                !ui_hit(ui, ui->ox, ui->y, ui->pw, barh)) {
                ui->menu_open = UI_MENU_NONE;
                ui->swallow = 1;
            }
        }
        // One place, at the end, rather than at each of the two sites that
        // set it -- so there is no path that sets swallow and forgets.
        if (ui->swallow) ui->mpressed = 0;
    }

    ui->id = id + ntops;
    ui_advance_h(ui, ui->pw, barh);
    return chosen;
}

// ============================================================
// a list box
// ============================================================
//
// This IS the open/save dialog. Give it a block of names and it shows them,
// scrolls them and returns the index of the one clicked, or -1.
//
// Names come as one flat char array of NUL-terminated strings plus a count,
// rather than an array of pointers, because that is the shape fs_readdir
// fills and converting between the two is a copy that can go stale.

long ui_list_name_at(char *names, long i) {
    long off;
    long n;
    off = 0;
    n = 0;
    while (n < i) {
        while (names[off]) off = off + 1;
        off = off + 1;
        n = n + 1;
    }
    return off;
}

// `sel` is in/out: the highlighted row. Returns the index ACTIVATED by a
// click, or -1. Highlighting and activating are different events -- arrowing
// through a list must not open every file it passes over.
long ui_list(struct Ui *ui, char *names, long count, long *sel, long h) {
    long id;
    long w;
    long hot;
    long focused;
    long rows;
    long i;
    long hash;
    long activated;

    id = ui_next_id(ui);
    w = ui_slot(ui);
    hot = ui_hit(ui, ui->x, ui->y, w, h);
    activated = 0 - 1;
    rows = (h - 4) / UI_ROW_H;
    if (rows < 1) rows = 1;

    if (hot) ui->hot = id;
    if (ui->mpressed) {
        if (hot && ui->active < 0) { ui->focus = id; ui->active = id; }
        else if (!hot && ui->focus == id) ui->focus = -1;
    }
    focused = (ui->focus == id);

    if (*sel < 0) *sel = 0;
    if (*sel >= count) *sel = count - 1;

    // Scroll so the selection is visible, using the same rule as the editor.
    if (*sel < ui->list_top) ui->list_top = *sel;
    if (*sel >= ui->list_top + rows) ui->list_top = *sel - rows + 1;
    if (ui->list_top < 0) ui->list_top = 0;

    if (focused && ui->key) {
        if (ui->key == KEY_UP && *sel > 0) *sel = *sel - 1;
        else if (ui->key == KEY_DOWN && *sel < count - 1) *sel = *sel + 1;
        else if (ui->key == KEY_HOME) *sel = 0;
        else if (ui->key == KEY_END) *sel = count - 1;
        else if (ui->key == '\n' || ui->key == '\r') activated = *sel;
        if (*sel < ui->list_top) ui->list_top = *sel;
        if (*sel >= ui->list_top + rows) ui->list_top = *sel - rows + 1;
    }

    // Not over the scrollbar: a click on the bar scrolls, it does not pick
    // whatever row happens to be beside the thumb.
    if (hot && ui->mpressed && ui->mx < ui->x + w - UI_SB_W) {
        long row;
        row = (ui->my - ui->y - 2) / UI_ROW_H;
        if (row >= 0 && ui->list_top + row < count) {
            *sel = ui->list_top + row;
            activated = *sel;
        }
    }
    {
        struct Scroll sc;
        sc.top = ui->list_top; sc.total = count; sc.visible = rows;
        ui->list_top = ui_scroll_input(ui, id + 910, ui->x + w - UI_SB_W,
                                       ui->y, h, &sc);
    }

    hash = 5381;
    hash = ((hash * 33) + *sel) & 0xFFFFFFF;
    hash = ((hash * 33) + ui->list_top) & 0xFFFFFFF;
    hash = ((hash * 33) + count) & 0xFFFFFFF;
    i = 0;
    while (i < count) {
        long off;
        off = ui_list_name_at(names, i);
        while (names[off]) { hash = ((hash * 33) + (names[off] & 255)) & 0xFFFFFFF; off = off + 1; }
        i = i + 1;
    }

    if (ui_paint(ui, id, (hash << 2) + (focused ? 2 : 0))) {
        wm_win_fill(ui->win, ui->x, ui->y, w, h, rgb(255, 255, 255));
        wm_win_frame(ui->win, ui->x, ui->y, w, h, focused ? ui->accent : ui->edge);
        i = 0;
        while (i < rows && ui->list_top + i < count) {
            long idx;
            long iy;
            long off;
            idx = ui->list_top + i;
            iy = ui->y + 2 + i * UI_ROW_H;
            off = ui_list_name_at(names, idx);
            if (idx == *sel)
                wm_win_fill(ui->win, ui->x + 1, iy, w - 2 - UI_SB_W, UI_ROW_H,
                            ui->accent);
            ui_text_clip(ui, ui->x + 3, iy, UI_ROW_H, names + off,
                         w - 6 - UI_SB_W);
            i = i + 1;
        }
        {
            struct Scroll sc;
            sc.top = ui->list_top; sc.total = count; sc.visible = rows;
            ui_scroll_draw(ui, ui->x + w - UI_SB_W, ui->y, h, &sc);
        }
    }
    ui_advance_h(ui, w, h);
    return activated;
}

// ============================================================
// a tab strip
// ============================================================
//
// Returns the tab clicked, or -1. The close boxes return their tab index
// NEGATED minus two, so one return value carries both events without an
// out-parameter -- -2 is "close tab 0", -3 is "close tab 1". Ugly but it
// keeps the call site to one switch, and nano_cc has no out-parameters worth
// the name.

#define UI_TAB_CLOSE(i) (0 - 2 - (i))

long ui_tabs(struct Ui *ui, char *names, long count, long active) {
    long id;
    long x;
    long i;
    long result;
    long h;

    id = ui_next_id(ui);
    result = 0 - 1;
    h = UI_ROW_H;
    x = ui->x;

    i = 0;
    while (i < count) {
        long off;
        long tw;
        long hot;
        long cx;
        off = ui_list_name_at(names, i);
        tw = (ui_strlen(names + off) + 4) * FONT_W;
        hot = ui_hit(ui, x, ui->y, tw, h);
        cx = x + tw - FONT_W - 2;

        if (hot && ui->mpressed) {
            // The close box is a hit test inside a hit test, and it has to be
            // checked FIRST or clicking the x just switches to the tab.
            if (ui->mx >= cx && ui->mx < cx + FONT_W) result = UI_TAB_CLOSE(i);
            else result = i;
        }

        if (ui_paint(ui, id + i, (i == active ? 4 : 0) + (hot ? 2 : 0) +
                                  (ui_strlen(names + off) & 1))) {
            wm_win_fill(ui->win, x, ui->y, tw, h,
                        i == active ? rgb(255, 255, 255) : ui->bg);
            wm_win_frame(ui->win, x, ui->y, tw, h, ui->edge);
            ui_text_clip(ui, x + FONT_W, ui->y, h, names + off, tw - 2 * FONT_W);
            wm_win_text(ui->win, cx, ui->y + (h - FONT_H) / 2, "x", ui->edge);
        }
        x = x + tw;
        i = i + 1;
    }

    ui->id = id + count;
    ui_advance_h(ui, ui->pw, h);
    return result;
}

// ============================================================
// icons, and a grid of them
// ============================================================
//
// The icons are DRAWN, not loaded. A 32x32 bitmap per file type would be
// about 4KB of table each, and there is no image loader in this OS yet -- so
// a folder is a rectangle with a tab on it and a document is a rectangle with
// a folded corner and some lines, built from wm_win_fill. Crude, and it costs
// nothing to ship and nothing to store.
//
// They are also drawn at a size the caller picks rather than at one fixed
// size, because a file manager and a file picker want different ones and
// scaling a bitmap without a resampler looks worse than redrawing a shape.

// 64 wide, not 48. At 48 and an 8px font the label fits five characters, so
// "readme.txt" renders as "readm" and two files whose names differ after the
// fifth character are indistinguishable on screen -- which is worse than a
// wider grid.
#define ICON_W   64
#define ICON_H   52          // icon box plus one row of label

// Rows of icons visible and rows there are, worked out once per frame from
// the width. Globals because they are derived, not state.
long g_grid_vis;
long g_grid_rows;

// A folder: a body, and a tab along the top-left of it.
void ui_icon_folder(struct Ui *ui, long x, long y, long w, long h, long open) {
    long tabw;
    long tabh;
    long body;
    tabw = w / 2;
    tabh = h / 5;
    body = y + tabh;
    wm_win_fill(ui->win, x, y, tabw, tabh, rgb(196, 160, 64));
    wm_win_fill(ui->win, x, body, w, h - tabh, rgb(228, 190, 86));
    wm_win_frame(ui->win, x, body, w, h - tabh, rgb(120, 96, 32));
    // An open folder gets a lighter mouth, so "which one am I in" is visible
    // without reading the label.
    if (open) wm_win_fill(ui->win, x + 3, body + 3, w - 6, h - tabh - 6,
                          rgb(245, 220, 150));
}

// A document: a page with the top-right corner folded off, and three lines of
// pretend text.
void ui_icon_file(struct Ui *ui, long x, long y, long w, long h) {
    long fold;
    long i;
    fold = w / 3;
    wm_win_fill(ui->win, x, y, w, h, rgb(248, 248, 252));
    wm_win_frame(ui->win, x, y, w, h, rgb(130, 130, 145));
    // The fold: a triangle cut out of the top right, drawn as rows so that no
    // diagonal-line routine is needed.
    i = 0;
    while (i < fold) {
        wm_win_fill(ui->win, x + w - fold + i, y + i, fold - i, 1, ui->bg);
        i = i + 1;
    }
    i = 0;
    while (i < 3) {
        wm_win_fill(ui->win, x + 4, y + h / 2 + i * 5, w - 10, 2,
                    rgb(170, 170, 185));
        i = i + 1;
    }
}

// A grid of icons. Returns the index CLICKED, or -1; `sel` is in/out.
//
// Selection and activation are separate, as in the list box: clicking picks,
// and the caller decides what a second click on an already-selected item
// means. There is no double-click timer in this machine and inventing one
// here would put a clock in a widget.
long ui_icongrid(struct Ui *ui, char *names, long *kinds, long count,
                 long *sel, long h) {
    long id;
    long w;
    long cols;
    long i;
    long hash;
    long clicked;
    long hot;

    id = ui_next_id(ui);
    w = ui_slot(ui);
    hot = ui_hit(ui, ui->x, ui->y, w, h);
    clicked = 0 - 1;
    cols = (w - UI_SB_W) / ICON_W;
    if (cols < 1) cols = 1;
    // Rows of icons that fit, and rows there are -- the units this grid
    // scrolls in. A grid scrolls by ROW, not by icon, or dragging the bar
    // shuffles items sideways.
    {
        long vis;
        long tot;
        vis = h / ICON_H;
        if (vis < 1) vis = 1;
        tot = (count + cols - 1) / cols;
        g_grid_vis = vis;
        g_grid_rows = tot;
    }

    if (hot) ui->hot = id;
    if (ui->mpressed) {
        if (hot && ui->active < 0) { ui->focus = id; ui->active = id; }
        else if (!hot && ui->focus == id) ui->focus = -1;
    }

    if (*sel < 0) *sel = 0;
    if (*sel >= count) *sel = count - 1;

    if (hot && ui->mpressed && ui->mx < ui->x + w - UI_SB_W) {
        long cx;
        long cy;
        long idx;
        cx = (ui->mx - ui->x) / ICON_W;
        cy = (ui->my - ui->y) / ICON_H + ui->grid_top;
        idx = cy * cols + cx;
        if (cx >= 0 && cx < cols && cy >= 0 && idx >= 0 && idx < count) {
            *sel = idx;
            clicked = idx;
        }
    }

    if (ui->focus == id && ui->key) {
        if (ui->key == KEY_LEFT && *sel > 0) *sel = *sel - 1;
        else if (ui->key == KEY_RIGHT && *sel < count - 1) *sel = *sel + 1;
        else if (ui->key == KEY_UP && *sel - cols >= 0) *sel = *sel - cols;
        else if (ui->key == KEY_DOWN && *sel + cols < count) *sel = *sel + cols;
        else if (ui->key == '\n' || ui->key == '\r') clicked = *sel;
        // Follow the selection, the same rule the list box and the editor use.
        if (*sel / cols < ui->grid_top) ui->grid_top = *sel / cols;
        if (*sel / cols >= ui->grid_top + g_grid_vis)
            ui->grid_top = *sel / cols - g_grid_vis + 1;
    }
    {
        struct Scroll sc;
        sc.top = ui->grid_top; sc.total = g_grid_rows; sc.visible = g_grid_vis;
        ui->grid_top = ui_scroll_input(ui, id + 920, ui->x + w - UI_SB_W,
                                       ui->y, h, &sc);
    }
    if (ui->grid_top < 0) ui->grid_top = 0;

    hash = 5381;
    hash = ((hash * 33) + *sel) & 0xFFFFFFF;
    hash = ((hash * 33) + count) & 0xFFFFFFF;
    hash = ((hash * 33) + ui->grid_top) & 0xFFFFFFF;
    i = 0;
    while (i < count) {
        long off;
        hash = ((hash * 33) + kinds[i]) & 0xFFFFFFF;
        off = ui_list_name_at(names, i);
        while (names[off]) { hash = ((hash * 33) + (names[off] & 255)) & 0xFFFFFFF; off = off + 1; }
        i = i + 1;
    }

    if (ui_paint(ui, id, hash)) {
        wm_win_fill(ui->win, ui->x, ui->y, w, h, rgb(255, 255, 255));
        wm_win_frame(ui->win, ui->x, ui->y, w, h, ui->edge);
        i = 0;
        while (i < count) {
            long cx;
            long cy;
            long ix;
            long iy;
            long off;
            cx = i % cols;
            cy = i / cols - ui->grid_top;
            if (cy < 0) { i = i + 1; continue; }
            ix = ui->x + cx * ICON_W;
            iy = ui->y + cy * ICON_H;
            if (iy + ICON_H > ui->y + h) { i = count; continue; }   // clipped
            if (i == *sel)
                wm_win_fill(ui->win, ix, iy, ICON_W - 2, ICON_H - 2, ui->accent);
            if (kinds[i]) ui_icon_folder(ui, ix + 20, iy + 4, 24, 22, 0);
            else          ui_icon_file(ui, ix + 22, iy + 4, 20, 24);
            off = ui_list_name_at(names, i);
            ui_text_clip(ui, ix + 2, iy + 30, FONT_H + 4, names + off, ICON_W - 6);
            i = i + 1;
        }
        {
            struct Scroll sc;
            sc.top = ui->grid_top; sc.total = g_grid_rows; sc.visible = g_grid_vis;
            ui_scroll_draw(ui, ui->x + w - UI_SB_W, ui->y, h, &sc);
        }
    }
    ui_advance_h(ui, w, h);
    return clicked;
}

void ui_init(struct Ui *ui) {
    ui->menu_open = UI_MENU_NONE;
    ui->swallow = 0;
    ui->list_top = 0;
    ui->grid_top = 0;
    ui->always = 0;
    ui->skipped = 0;
    ui->ovwin = -1;
    ui->ovx0 = 0; ui->ovy0 = 0; ui->ovx1 = -1; ui->ovy1 = -1;
    ui->hot = -1;
    ui->active = -1;
    ui->focus = -1;
    ui->mdown = 0;
    ui->mpressed = 0;
    ui->mreleased = 0;
    ui->key = 0;
    ui->fg = rgb(30, 34, 40);
    ui->bg = rgb(226, 229, 234);
    ui->panel = rgb(240, 240, 240);
    ui->accent = rgb(40, 100, 190);
    ui->edge = rgb(150, 156, 165);
    ui_forget_all();
}

#endif
