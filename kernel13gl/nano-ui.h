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
#define UI_MAXID   128

#define UI_ROW_H   20
#define UI_PAD     4
#define UI_BOX     12          // checkbox side, slider handle width
#define UI_NOSTATE (0 - 1)     // "never drawn", so the first frame always draws

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

// Start a panel at (x,y) in window coordinates, `w` wide.
//
// The pointer arrives in SCREEN coordinates and is converted here, once. Doing
// it per widget is the kind of duplication that ends with one widget using the
// wrong space and being unclickable in a way that looks like a hit-testing bug.
void ui_begin(struct Ui *ui, long win, long x, long y, long w) {
    ui->win = win;
    ui->ox = x;
    ui->oy = y;
    ui->pw = w;
    ui->x = x;
    ui->y = y;
    ui->cols = 0;
    ui->colw = w;
    ui->id = 0;
    ui->hot = -1;
    ui->invalidations = 0;
    ui->drawn = 0;

    ui->mx = g_mouse_x - g_win[win].x;
    ui->my = g_mouse_y - g_win[win].y;
    ui->inside = (ui->mx >= 0 && ui->my >= 0 &&
                  ui->mx < g_win[win].w && ui->my < g_win[win].h);
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

void ui_advance(struct Ui *ui, long w) {
    if (ui->cols > 1) {
        ui->cols = ui->cols - 1;
        ui->x = ui->x + w + UI_PAD;
    } else {
        ui->cols = 0;
        ui->x = ui->ox;
        ui->y = ui->y + UI_ROW_H + UI_PAD;
    }
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
long ui_track(struct Ui *ui, long id, long state) {
    if (id < 0 || id >= UI_MAXID) {
        // Untracked ids cannot be compared, so they must always be pushed.
        // Failing the other way would leave a widget that never updates.
        wm_invalidate(ui->win, ui->x, ui->y, ui->cw, ui->ch);
        ui->invalidations = ui->invalidations + 1;
        return 1;
    }
    if (g_ui_last[id] == state) return 0;
    g_ui_last[id] = state;
    wm_invalidate(ui->win, ui->x, ui->y, ui->cw, ui->ch);
    ui->invalidations = ui->invalidations + 1;
    return 1;
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

// Left-aligned text, drawn one glyph at a time and STOPPED at `maxw` pixels.
//
// A widget that draws outside its own rectangle is a widget that lies to the
// compositor. ui_track invalidates the widget's rect and nothing else, so
// anything painted beyond it lands in the backing buffer and is never pushed
// to the screen -- the buffer and the screen then disagree forever.
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
    // first frame must draw it, and after that it never changes. Hashing the
    // pointer means a label whose TEXT is swapped redraws.
    wm_win_fill(ui->win, ui->x, ui->y, w, UI_ROW_H, ui->panel);
    ui_text_clip(ui, ui->x + 2, ui->y, UI_ROW_H, s, w - 4);
    ui->drawn = ui->drawn + 1;
    ui_track(ui, ui_next_id(ui), (long)s);
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

    ui_box(ui, ui->x, ui->y, w, UI_ROW_H, fill);
    ui_text_mid(ui, s, (act && ui->mdown) ? rgb(255, 255, 255) : ui->fg);
    ui->drawn = ui->drawn + 1;

    ui_track(ui, id, (hot ? 2 : 0) + ((act && ui->mdown) ? 1 : 0));
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
    ui->drawn = ui->drawn + 1;

    ui_track(ui, id, (hot ? 2 : 0) + (*v ? 1 : 0));
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

    wm_win_fill(ui->win, ui->x, ui->y, w, UI_ROW_H, ui->panel);
    ui_box(ui, ui->x, ui->y + UI_ROW_H / 2 - 2, w, 4, ui->bg);
    ui_box(ui, hx, ui->y + 2, UI_BOX, UI_ROW_H - 4,
           (act && ui->mdown) ? ui->accent : (hot ? ui->edge : ui->bg));
    ui->drawn = ui->drawn + 1;

    ui_track(ui, id, (*v << 2) + (hot ? 2 : 0) + ((act && ui->mdown) ? 1 : 0));
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
        if (hot) { ui->focus = id; ui->active = id; }
        else if (ui->focus == id) ui->focus = -1;
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

    wm_win_fill(ui->win, ui->x, ui->y, w, UI_ROW_H,
                focused ? rgb(255, 255, 255) : ui->bg);
    wm_win_frame(ui->win, ui->x, ui->y, w, UI_ROW_H,
                 focused ? ui->accent : ui->edge);
    // Longer than the box: show the TAIL, because that is where the caret is
    // and where the characters being typed appear. Showing the head instead
    // gives a field that stops responding visibly once it is full.
    {
        long vis;
        long off;
        vis = (w - 6) / FONT_W;
        if (vis < 0) vis = 0;
        off = 0;
        if (n > vis) off = n - vis;
        ui_text_clip(ui, ui->x + 3, ui->y, UI_ROW_H, buf + off, w - 6);
        if (focused)
            wm_win_fill(ui->win, ui->x + 3 + (n - off) * FONT_W, ui->y + 3, 1,
                        UI_ROW_H - 6, ui->fg);
    }
    ui->drawn = ui->drawn + 1;

    // The contents are part of the visual state, so they have to be in the
    // hash. Using only the length would miss a character being replaced.
    hash = 5381;
    i = 0;
    while (i < n) { hash = ((hash * 33) + (buf[i] & 255)) & 0xFFFFFFF; i = i + 1; }
    ui_track(ui, id, (hash << 3) + (focused ? 4 : 0) + (hot ? 2 : 0));
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

    ui_box(ui, ui->x, ui->y, w, UI_ROW_H, ui->bg);
    if (fill > 0) wm_win_fill(ui->win, ui->x + 1, ui->y + 1, fill, UI_ROW_H - 2,
                              ui->accent);
    ui->drawn = ui->drawn + 1;
    ui_track(ui, id, v);
    ui_advance(ui, w);
}

// ---------- setup ----------

void ui_init(struct Ui *ui) {
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
