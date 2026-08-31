// nano-term.h — a console that lives in a window.
//
// The shell in shell.c writes to a framebuffer that is the whole screen and
// owns all of it. A console in a window cannot: it has to be told how wide it
// is, it has to scroll inside its own rectangle, and above all it has to tell
// the compositor which part of it changed. A terminal that invalidates its
// whole client area every time a character is typed is a terminal that repaints
// 50,000 pixels to draw eight-by-eight of them.
//
// So the terminal keeps two grids: `cells`, what it wants on screen, and
// `shown`, what is actually there. Flushing compares them, redraws only the
// cells that differ, and issues one invalidate covering the bounding box of
// those cells. Typing a character touches one cell. Scrolling touches all of
// them, honestly, because all of them changed.
//
// Requires nano-wm.h and nano-mm.h.

#ifndef NANO_TERM_H
#define NANO_TERM_H

#define TERM_MAX     4
#define TERM_LINEMAX 128

// A cell is wider and taller than the glyph it holds. Row 7 of every glyph in
// nano-font.h is the descender line -- it is where g, j, p, q, y and the
// underscore go -- so packing rows at exactly FONT_H makes those characters
// touch the tops of the line below. Two pixels of leading, one above and one
// below, is the difference between text that is legible and text that is
// merely present.
#define TERM_CELL_W  FONT_W
#define TERM_CELL_H  (FONT_H + 2)
#define TERM_GLYPH_Y 1

// Not a printable character and not 0, so it can never equal a real cell.
// Written into `shown` to force a redraw of a cell whose content did not
// change but whose appearance did -- which is exactly the text cursor.
#define TERM_DIRTY 1

struct Term {
    long used;
    long win;
    long cols;
    long rows;
    long cx;            // cursor, in cells
    long cy;
    long scx;           // where the cursor is currently DRAWN
    long scy;
    char *cells;
    char *shown;
    long fg;
    long bg;
    char *line;         // the line being edited
    long linelen;
    long echo;          // 0 while a test drives it, to keep the log readable
};

struct Term g_term[TERM_MAX];
long g_nterm;
long g_term_cells_drawn;    // cells actually repainted, since a reset

// ---------- small string helpers ----------
// Local rather than from a libc, because there is no libc down here.

long str_eq(char *a, char *b) {
    while (*a && *b) { if (*a != *b) return 0; a = a + 1; b = b + 1; }
    return *a == *b;
}

long str_starts(char *s, char *pre) {
    while (*pre) { if (*s != *pre) return 0; s = s + 1; pre = pre + 1; }
    return 1;
}

// ---------- geometry ----------

long term_win_w(long cols) { return cols * TERM_CELL_W + WM_BORDER * 2; }
long term_win_h(long rows) { return rows * TERM_CELL_H + WM_TITLE_H + WM_BORDER; }

long term_by_win(long hnd) {
    long i;
    i = 0;
    while (i < TERM_MAX) {
        if (g_term[i].used && g_term[i].win == hnd) return i;
        i = i + 1;
    }
    return -1;
}

// ---------- the grid ----------

void term_clear(long ti) {
    long n;
    long i;
    n = g_term[ti].cols * g_term[ti].rows;
    i = 0;
    while (i < n) { g_term[ti].cells[i] = ' '; i = i + 1; }
    g_term[ti].cx = 0;
    g_term[ti].cy = 0;
}

// Move every row up one and blank the last. A memmove by hand: nano_cc has no
// library, and at 49x35 this is 1,715 byte copies, which is nothing next to the
// 13,720 framebuffer writes the repaint costs.
void term_scroll(long ti) {
    long cols;
    long rows;
    long i;
    long n;
    char *c;
    cols = g_term[ti].cols;
    rows = g_term[ti].rows;
    c = g_term[ti].cells;
    n = cols * (rows - 1);
    i = 0;
    while (i < n) { c[i] = c[i + cols]; i = i + 1; }
    while (i < cols * rows) { c[i] = ' '; i = i + 1; }
}

void term_newline(long ti) {
    g_term[ti].cx = 0;
    g_term[ti].cy = g_term[ti].cy + 1;
    if (g_term[ti].cy >= g_term[ti].rows) {
        term_scroll(ti);
        g_term[ti].cy = g_term[ti].rows - 1;
    }
}

void term_putc(long ti, long ch) {
    if (ch == '\n') { term_newline(ti); return; }
    if (ch == '\r') { g_term[ti].cx = 0; return; }
    if (ch == '\b') {
        if (g_term[ti].cx > 0) {
            g_term[ti].cx = g_term[ti].cx - 1;
            g_term[ti].cells[g_term[ti].cy * g_term[ti].cols + g_term[ti].cx] = ' ';
        }
        return;
    }
    if (ch < 32 || ch > 126) return;
    if (g_term[ti].cx >= g_term[ti].cols) term_newline(ti);
    g_term[ti].cells[g_term[ti].cy * g_term[ti].cols + g_term[ti].cx] = ch;
    g_term[ti].cx = g_term[ti].cx + 1;
}

void term_puts(long ti, char *s) {
    while (*s) { term_putc(ti, *s); s = s + 1; }
}

void term_putnum(long ti, long v) {
    char buf[24];
    long n;
    long neg;
    neg = 0;
    if (v < 0) { neg = 1; v = 0 - v; }
    n = 0;
    if (v == 0) { buf[0] = '0'; n = 1; }
    while (v > 0) { buf[n] = '0' + (v % 10); v = v / 10; n = n + 1; }
    if (neg) term_putc(ti, '-');
    while (n > 0) { n = n - 1; term_putc(ti, buf[n]); }
}

// ---------- painting ----------

// One cell into the window's backing buffer. No screen writes happen here; the
// invalidate that follows in term_flush is what eventually costs anything.
void term_draw_cell(long ti, long col, long row, long inverse) {
    long px;
    long py;
    long ch;
    long fg;
    long bg;
    px = WM_BORDER + col * TERM_CELL_W;
    py = WM_TITLE_H + row * TERM_CELL_H;
    ch = g_term[ti].cells[row * g_term[ti].cols + col] & 255;
    fg = g_term[ti].fg;
    bg = g_term[ti].bg;
    if (inverse) { long t; t = fg; fg = bg; bg = t; }
    wm_win_fill(g_term[ti].win, px, py, TERM_CELL_W, TERM_CELL_H, bg);
    if (ch != ' ') wm_win_glyph(g_term[ti].win, px, py + TERM_GLYPH_Y, ch, fg);
    g_term_cells_drawn = g_term_cells_drawn + 1;
}

// Reconcile `shown` with `cells` and tell the compositor about it.
//
// One invalidate covering the bounding box of the changed cells, rather than
// one per cell. Per-cell would be exact, but the damage list holds 32
// rectangles and a scrolled screen produces 1,715 of them, so it would overflow
// into a full repaint anyway -- and pay for building the list first.
void term_flush(long ti) {
    long cols;
    long rows;
    long r;
    long minx; long miny; long maxx; long maxy;

    cols = g_term[ti].cols;
    rows = g_term[ti].rows;

    // The cursor cell's contents may not have changed, but where the cursor is
    // has. Mark both the old and the new position so the block is lifted from
    // one and painted on the other. Marking only the new one leaves a trail of
    // cursors behind, which looks like the terminal is drawing garbage.
    if (g_term[ti].scx != g_term[ti].cx || g_term[ti].scy != g_term[ti].cy) {
        if (g_term[ti].scx >= 0 && g_term[ti].scx < cols &&
            g_term[ti].scy >= 0 && g_term[ti].scy < rows)
            g_term[ti].shown[g_term[ti].scy * cols + g_term[ti].scx] = TERM_DIRTY;
        if (g_term[ti].cx >= 0 && g_term[ti].cx < cols &&
            g_term[ti].cy >= 0 && g_term[ti].cy < rows)
            g_term[ti].shown[g_term[ti].cy * cols + g_term[ti].cx] = TERM_DIRTY;
    }

    minx = cols; miny = rows; maxx = -1; maxy = -1;
    r = 0;
    while (r < rows) {
        long c;
        c = 0;
        while (c < cols) {
            long i;
            i = r * cols + c;
            if (g_term[ti].shown[i] != g_term[ti].cells[i]) {
                long cur;
                cur = (c == g_term[ti].cx && r == g_term[ti].cy);
                term_draw_cell(ti, c, r, cur);
                g_term[ti].shown[i] = g_term[ti].cells[i];
                if (c < minx) minx = c;
                if (c > maxx) maxx = c;
                if (r < miny) miny = r;
                if (r > maxy) maxy = r;
            }
            c = c + 1;
        }
        r = r + 1;
    }

    g_term[ti].scx = g_term[ti].cx;
    g_term[ti].scy = g_term[ti].cy;

    if (maxx < 0) return;                    // nothing changed
    wm_invalidate(g_term[ti].win,
                  WM_BORDER + minx * TERM_CELL_W,
                  WM_TITLE_H + miny * TERM_CELL_H,
                  (maxx - minx + 1) * TERM_CELL_W,
                  (maxy - miny + 1) * TERM_CELL_H);
}

// ---------- the shell inside the window ----------

void term_prompt(long ti) {
    term_puts(ti, "> ");
    g_term[ti].linelen = 0;
}

void term_exec(long ti, char *line) {
    if (line[0] == 0) return;

    if (str_eq(line, "help")) {
        term_puts(ti, "help ver clear ticks mem wins mouse echo close\n");
        return;
    }
    if (str_eq(line, "ver")) {
        term_puts(ti, "nano-os K13, built by nano_cc\n");
        return;
    }
    if (str_eq(line, "clear")) { term_clear(ti); return; }
    if (str_eq(line, "ticks")) {
        term_puts(ti, "ticks "); term_putnum(ti, g_ticks); term_putc(ti, '\n');
        return;
    }
    if (str_eq(line, "mem")) {
        term_puts(ti, "heap "); term_putnum(ti, heap_pages);
        term_puts(ti, " pages, "); term_putnum(ti, heap_bytes_free());
        term_puts(ti, " bytes free\n");
        return;
    }
    if (str_eq(line, "wins")) {
        long i;
        i = 0;
        while (i < g_nwin) {
            long h;
            h = g_order[i];
            term_putnum(ti, i);
            term_puts(ti, ": ");
            term_puts(ti, g_win[h].title ? g_win[h].title : "(none)");
            term_puts(ti, " ");
            term_putnum(ti, g_win[h].x); term_puts(ti, ",");
            term_putnum(ti, g_win[h].y); term_puts(ti, " ");
            term_putnum(ti, g_win[h].w); term_puts(ti, "x");
            term_putnum(ti, g_win[h].h);
            if (h == g_focus) term_puts(ti, " *focus");
            term_putc(ti, '\n');
            i = i + 1;
        }
        return;
    }
    if (str_eq(line, "mouse")) {
        term_puts(ti, "at "); term_putnum(ti, g_mouse_x);
        term_puts(ti, ","); term_putnum(ti, g_mouse_y);
        term_puts(ti, "  packets "); term_putnum(ti, g_mouse_packets);
        term_puts(ti, "  resync "); term_putnum(ti, g_mouse_resync);
        term_putc(ti, '\n');
        return;
    }
    if (str_starts(line, "echo ")) {
        term_puts(ti, line + 5);
        term_putc(ti, '\n');
        return;
    }
    if (str_eq(line, "close")) { wm_destroy(g_term[ti].win); g_term[ti].used = 0; return; }

    term_puts(ti, "unknown: ");
    term_puts(ti, line);
    term_puts(ti, "\n");
}

// One keystroke. Returns 1 if the terminal still exists afterwards -- `close`
// destroys the window out from under the caller, and the caller must not then
// flush a terminal whose window handle has been reused.
long term_key(long ti, long ch) {
    if (ch == '\n') {
        long win;
        win = g_term[ti].win;
        term_putc(ti, '\n');
        g_term[ti].line[g_term[ti].linelen] = 0;
        term_exec(ti, g_term[ti].line);
        if (!g_term[ti].used || !g_win[win].used) return 0;
        term_prompt(ti);
        return 1;
    }
    if (ch == '\b') {
        if (g_term[ti].linelen > 0) {
            g_term[ti].linelen = g_term[ti].linelen - 1;
            term_putc(ti, '\b');
        }
        return 1;
    }
    if (ch < 32 || ch > 126) return 1;
    if (g_term[ti].linelen >= TERM_LINEMAX - 1) return 1;
    g_term[ti].line[g_term[ti].linelen] = ch;
    g_term[ti].linelen = g_term[ti].linelen + 1;
    term_putc(ti, ch);
    return 1;
}

// ---------- creation ----------

// Returns a terminal index, or -1.
long term_create(long x, long y, long cols, long rows, char *title) {
    long ti;
    long hnd;
    long n;

    ti = 0;
    while (ti < TERM_MAX && g_term[ti].used) ti = ti + 1;
    if (ti >= TERM_MAX) return -1;

    hnd = wm_create(x, y, term_win_w(cols), term_win_h(rows), title);
    if (hnd < 0) return -1;

    n = cols * rows;
    g_term[ti].cells = (char *)kmalloc(n);
    g_term[ti].shown = (char *)kmalloc(n);
    g_term[ti].line  = (char *)kmalloc(TERM_LINEMAX);
    if (!g_term[ti].cells || !g_term[ti].shown || !g_term[ti].line) return -1;

    g_term[ti].used = 1;
    g_term[ti].win = hnd;
    g_term[ti].cols = cols;
    g_term[ti].rows = rows;
    g_term[ti].fg = rgb(220, 226, 232);
    g_term[ti].bg = rgb(18, 22, 30);
    g_term[ti].linelen = 0;
    g_term[ti].echo = 1;
    g_term[ti].scx = -1;
    g_term[ti].scy = -1;

    // The window's background is deliberately left as the manager's default
    // rather than being set to the terminal's. Setting it would mean the
    // terminal's blank cells happen to be the right colour without the
    // terminal ever having painted them -- a hidden dependency on wm_decorate
    // having pre-filled the client area in exactly the colour this terminal
    // wanted. It also makes a whole class of bug invisible: a flush that
    // skipped every blank cell would look perfect. The terminal paints all of
    // its own background, and the tests check that none of the window's is
    // left showing.
    g_win[hnd].accent = rgb(30, 90, 70);
    g_win[hnd].kind = WM_KIND_TERM;
    g_win[hnd].tag = ti;

    // `shown` starts as TERM_DIRTY everywhere, not as spaces: the window's
    // backing buffer has just been filled with the background colour and
    // contains no cells at all, so "nothing has changed" would be a lie and the
    // first flush would draw nothing.
    {
        long i;
        i = 0;
        while (i < n) { g_term[ti].shown[i] = TERM_DIRTY; i = i + 1; }
    }
    term_clear(ti);
    wm_decorate(hnd);

    g_nterm = g_nterm + 1;
    return ti;
}

void term_init() {
    long i;
    i = 0;
    while (i < TERM_MAX) { g_term[i].used = 0; i = i + 1; }
    g_nterm = 0;
    g_term_cells_drawn = 0;
}

// ---------- routing ----------

// A keystroke goes to whichever window has focus, and to nothing at all if the
// desktop does. That is the whole point of focus: before this, every key went
// to the one program that happened to be reading the keyboard.
long wm_input_key(long ch) {
    long hnd;
    hnd = g_focus;
    if (hnd < 0 || hnd >= WM_MAXWIN) return 0;
    if (!g_win[hnd].used) return 0;
    if (g_win[hnd].kind != WM_KIND_TERM) return 0;
    {
        long ti;
        ti = g_win[hnd].tag;
        if (ti < 0 || ti >= TERM_MAX || !g_term[ti].used) return 0;
        if (term_key(ti, ch)) term_flush(ti);
        return 1;
    }
}

#endif
