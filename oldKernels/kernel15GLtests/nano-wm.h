// nano-wm.h — a compositing window manager: backing buffers, damage rectangles,
// and a present step that touches only the pixels that actually changed.
//
// The reason this exists as its own layer, rather than everything drawing
// straight to the screen the way nano-fb.h does, is arithmetic. Every pixel put
// on screen is an `mmio_write32` across the PCI bus to video memory. At
// 1024x768 a full repaint is 786,432 of them. Doing that because a 4-pixel-wide
// strip of one window moved is the difference between a machine that feels
// alive and one that does not.
//
// So each window owns a buffer in ordinary RAM. Programs draw into that as fast
// as memory allows and nothing reaches the screen. Then they say which
// rectangle changed, and `wm_present` copies out only those rectangles, clipped
// to whichever windows are visible inside them.
//
// TinyGL cannot do this job -- it is a triangle rasterizer with no concept of a
// damaged region; ZB_clear clears the whole buffer and ZB_copyFrameBuffer
// copies the whole buffer. This layer is what would eventually hand TinyGL a
// window's backing buffer to render into, and then blit that one rectangle.
//
// Everything here is integer. There is no floating point in nano_cc and none is
// needed: a compositor is coordinates, widths, and copies.
//
// Requires nano-fb.h (for fb_base/fb_pitch and the font) and nano-mm.h (kmalloc).

#ifndef NANO_WM_H
#define NANO_WM_H

#define WM_MAXWIN   16
#define WM_MAXDMG   32

// Title bar and border, in pixels.
#define WM_TITLE_H  16
#define WM_BORDER   2

struct Rect { long x; long y; long w; long h; };

struct Win {
    long used;
    long x; long y; long w; long h;     // outer rectangle, screen coordinates
    long *pix;                          // w*h pixels, 0x00RRGGBB, one per long
    char *title;
    long visible;
    long bg;
    long accent;                        // title bar colour when focused
    long dim;                           // title bar colour when not focused
    long kind;                          // what the input layer should do with it
    long tag;                           // index into whatever `kind` implies
    long fixed;                         // 1 = cannot be dragged or closed
};

// Window kinds. There are no function pointers in nano_cc, so a window cannot
// carry its own key handler; it carries a number and the input layer switches
// on it. Less flexible than a vtable and, at four kinds, considerably less
// code.
#define WM_KIND_PLAIN 0
#define WM_KIND_TERM  1

struct Win  g_win[WM_MAXWIN];
long        g_order[WM_MAXWIN];         // window handles, back to front
long        g_nwin;

struct Rect g_dmg[WM_MAXDMG];
long        g_ndmg;
long        g_dmg_overflow;             // list filled up; next present is full

long wm_bg;                             // desktop colour
long wm_pixels;                         // framebuffer pixels written since reset
long wm_blits;                          // row-copies issued since reset

// The off switch. With this set, every present repaints the whole screen and
// the damage list is ignored. It exists so the tests can prove that damage
// tracking is doing something -- a "fast" path is easy to believe in when the
// only evidence is that the picture looks right.
long wm_no_damage;

// ---------- rectangles ----------

// Clip r to the box (cx,cy,cw,ch), in place. Returns 0 if nothing is left.
// Takes a pointer rather than four in/out longs because nano_cc allows six call
// arguments and this would want eight.
long rect_clip(struct Rect *r, long cx, long cy, long cw, long ch) {
    long x1; long y1; long cx1; long cy1;
    x1  = r->x + r->w;   y1  = r->y + r->h;
    cx1 = cx + cw;       cy1 = cy + ch;
    if (r->x < cx) r->x = cx;
    if (r->y < cy) r->y = cy;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    r->w = x1 - r->x;
    r->h = y1 - r->y;
    if (r->w <= 0 || r->h <= 0) { r->w = 0; r->h = 0; return 0; }
    return 1;
}

long rect_area(struct Rect *r) { return r->w * r->h; }

// Does the box (x,y,w,h) completely contain r?
long rect_covers(long x, long y, long w, long h, struct Rect *r) {
    if (x > r->x) return 0;
    if (y > r->y) return 0;
    if (x + w < r->x + r->w) return 0;
    if (y + h < r->y + r->h) return 0;
    return 1;
}

// ---------- damage ----------

void wm_damage_all() {
    g_ndmg = 0;
    g_dmg[0].x = 0; g_dmg[0].y = 0;
    g_dmg[0].w = fb_width; g_dmg[0].h = fb_height;
    g_ndmg = 1;
    g_dmg_overflow = 0;
}

// Add a damaged rectangle.
//
// Two rectangles that overlap are merged into their bounding box. That can
// enlarge the damaged area -- two 10x10 squares at opposite corners of the
// screen would merge into the whole screen -- so it only merges when the
// bounding box is no worse than the two rectangles separately. Otherwise the
// "optimisation" quietly costs more than it saves, which is the sort of thing
// that never shows up as a bug, only as a machine that is slower than it
// should be.
void wm_damage(long x, long y, long w, long h) {
    struct Rect r;
    long i;

    r.x = x; r.y = y; r.w = w; r.h = h;
    if (!rect_clip(&r, 0, 0, fb_width, fb_height)) return;

    i = 0;
    while (i < g_ndmg) {
        long ux; long uy; long ux1; long uy1;
        long merged;
        ux  = g_dmg[i].x; if (r.x < ux) ux = r.x;
        uy  = g_dmg[i].y; if (r.y < uy) uy = r.y;
        ux1 = g_dmg[i].x + g_dmg[i].w; if (r.x + r.w > ux1) ux1 = r.x + r.w;
        uy1 = g_dmg[i].y + g_dmg[i].h; if (r.y + r.h > uy1) uy1 = r.y + r.h;
        merged = (ux1 - ux) * (uy1 - uy);
        if (merged <= rect_area(&g_dmg[i]) + rect_area(&r)) {
            g_dmg[i].x = ux; g_dmg[i].y = uy;
            g_dmg[i].w = ux1 - ux; g_dmg[i].h = uy1 - uy;
            return;
        }
        i = i + 1;
    }

    if (g_ndmg >= WM_MAXDMG) {
        // Out of slots. Repainting everything is correct and slow; dropping a
        // rectangle is fast and wrong, and the wrongness is invisible until
        // somebody notices a stale patch of screen.
        g_dmg_overflow = 1;
        return;
    }
    g_dmg[g_ndmg].x = r.x; g_dmg[g_ndmg].y = r.y;
    g_dmg[g_ndmg].w = r.w; g_dmg[g_ndmg].h = r.h;
    g_ndmg = g_ndmg + 1;
}

// ---------- counted writes to the screen ----------
// Every pixel that reaches video memory goes through one of these two, so the
// counter cannot drift away from the truth.

void wm_row_fill(long fx, long fy, long n, long colour) {
    long addr; long i;
    if (n <= 0) return;
    addr = fb_base + fy * fb_pitch + fx * 4;
    i = 0;
    while (i < n) { mmio_write32(addr + i * 4, colour); i = i + 1; }
    wm_pixels = wm_pixels + n;
    wm_blits = wm_blits + 1;
}

void wm_row_copy(long fx, long fy, long *src, long n) {
    long addr; long i;
    if (n <= 0) return;
    addr = fb_base + fy * fb_pitch + fx * 4;
    i = 0;
    while (i < n) { mmio_write32(addr + i * 4, src[i]); i = i + 1; }
    wm_pixels = wm_pixels + n;
    wm_blits = wm_blits + 1;
}

void wm_fill_rect(struct Rect *r, long colour) {
    long y;
    y = 0;
    while (y < r->h) { wm_row_fill(r->x, r->y + y, r->w, colour); y = y + 1; }
}

// ---------- windows ----------

long wm_alloc_slot() {
    long i;
    i = 0;
    while (i < WM_MAXWIN) { if (!g_win[i].used) return i; i = i + 1; }
    return -1;
}

// Returns a handle, or -1. The backing buffer is one long per pixel: nano_cc
// has no 32-bit integer type, so packing four bytes per pixel would mean doing
// the shifting by hand on every access. Eight bytes a pixel costs memory and
// buys simple code, and at these window sizes memory is the thing we have.
long wm_create(long x, long y, long w, long h, char *title) {
    long i;
    long *buf;
    if (w <= 0 || h <= 0) return -1;
    i = wm_alloc_slot();
    if (i < 0) return -1;
    if (g_nwin >= WM_MAXWIN) return -1;
    buf = (long *)kmalloc(w * h * 8);
    if (!buf) return -1;

    g_win[i].used = 1;
    g_win[i].x = x; g_win[i].y = y; g_win[i].w = w; g_win[i].h = h;
    g_win[i].pix = buf;
    g_win[i].title = title;
    g_win[i].visible = 1;
    g_win[i].bg = rgb(240, 240, 240);
    g_win[i].accent = rgb(40, 80, 160);
    g_win[i].dim = rgb(110, 118, 130);
    g_win[i].kind = WM_KIND_PLAIN;
    g_win[i].tag = 0;
    g_win[i].fixed = 0;

    g_order[g_nwin] = i;
    g_nwin = g_nwin + 1;
    wm_damage(x, y, w, h);
    return i;
}

// ---------- drawing into a window's own buffer ----------
// None of this touches the screen. It is plain memory, so it is fast and it can
// happen as often as a program likes without costing a single bus write.

void wm_win_fill(long hnd, long x, long y, long w, long h, long colour) {
    struct Rect r;
    long yy;
    long *p;
    r.x = x; r.y = y; r.w = w; r.h = h;
    if (!rect_clip(&r, 0, 0, g_win[hnd].w, g_win[hnd].h)) return;
    p = g_win[hnd].pix;
    yy = 0;
    while (yy < r.h) {
        long xx;
        long row;
        row = (r.y + yy) * g_win[hnd].w;
        xx = 0;
        while (xx < r.w) { p[row + r.x + xx] = colour; xx = xx + 1; }
        yy = yy + 1;
    }
}

void wm_win_pixel(long hnd, long x, long y, long colour) {
    if (x < 0 || y < 0 || x >= g_win[hnd].w || y >= g_win[hnd].h) return;
    g_win[hnd].pix[y * g_win[hnd].w + x] = colour;
}

void wm_win_frame(long hnd, long x, long y, long w, long h, long colour) {
    wm_win_fill(hnd, x, y, w, 1, colour);
    wm_win_fill(hnd, x, y + h - 1, w, 1, colour);
    wm_win_fill(hnd, x, y, 1, h, colour);
    wm_win_fill(hnd, x + w - 1, y, 1, h, colour);
}

// 8x8 glyphs from nano-font.h, drawn into the window buffer.
void wm_win_glyph(long hnd, long px, long py, long ch, long fg) {
    long row; long base;
    if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
    base = (ch - FONT_FIRST) * FONT_H;
    row = 0;
    while (row < FONT_H) {
        long bits; long col;
        bits = g_font[base + row] & 255;
        col = 0;
        while (col < FONT_W) {
            if ((bits >> col) & 1) wm_win_pixel(hnd, px + col, py + row, fg);
            col = col + 1;
        }
        row = row + 1;
    }
}

void wm_win_text(long hnd, long px, long py, char *s, long fg) {
    long x;
    x = px;
    while (*s) { wm_win_glyph(hnd, x, py, *s, fg); x = x + FONT_W; s = s + 1; }
}

// ---------- focus and the title bar ----------

// The focused window is the one keystrokes go to. It is deliberately NOT the
// same thing as the front window: a window can be raised without taking focus,
// and keeping the two ideas separate now is cheaper than untangling them later.
long g_focus;

// The close button, a square at the right-hand end of the title bar.
#define WM_CLOSE_SZ  (WM_TITLE_H - 6)
#define WM_CLOSE_PAD 4

long wm_close_x(long hnd) { return g_win[hnd].w - WM_CLOSE_SZ - WM_CLOSE_PAD; }
long wm_close_y() { return (WM_TITLE_H - WM_CLOSE_SZ) / 2; }

// Draw only the title bar. Separate from wm_decorate because focus changes on
// every click, and redrawing a window's entire client area to recolour a
// 16-pixel strip is exactly the kind of waste the rest of this file exists to
// avoid.
void wm_titlebar(long hnd) {
    long bar;
    long cx;
    long cy;
    long i;
    long focused;

    focused = (g_focus == hnd);
    bar = focused ? g_win[hnd].accent : g_win[hnd].dim;
    wm_win_fill(hnd, 0, 0, g_win[hnd].w, WM_TITLE_H, bar);
    if (g_win[hnd].title)
        wm_win_text(hnd, 4, 4, g_win[hnd].title,
                    focused ? rgb(255, 255, 255) : rgb(225, 228, 232));

    if (!g_win[hnd].fixed) {
        cx = wm_close_x(hnd);
        cy = wm_close_y();
        wm_win_fill(hnd, cx, cy, WM_CLOSE_SZ, WM_CLOSE_SZ, rgb(200, 70, 70));
        wm_win_frame(hnd, cx, cy, WM_CLOSE_SZ, WM_CLOSE_SZ, rgb(90, 20, 20));
        i = 2;
        while (i < WM_CLOSE_SZ - 2) {
            wm_win_pixel(hnd, cx + i, cy + i, rgb(255, 235, 235));
            wm_win_pixel(hnd, cx + WM_CLOSE_SZ - 1 - i, cy + i, rgb(255, 235, 235));
            i = i + 1;
        }
    }
    wm_win_fill(hnd, 0, WM_TITLE_H - 1, g_win[hnd].w, 1, rgb(20, 20, 20));
}

// Border, title bar, and a client area cleared to the window's background.
void wm_decorate(long hnd) {
    wm_win_fill(hnd, 0, 0, g_win[hnd].w, g_win[hnd].h, g_win[hnd].bg);
    wm_titlebar(hnd);
    wm_win_frame(hnd, 0, 0, g_win[hnd].w, g_win[hnd].h, rgb(20, 20, 20));
}

long wm_client_x() { return WM_BORDER; }
long wm_client_y() { return WM_TITLE_H; }
long wm_client_w(long hnd) { return g_win[hnd].w - WM_BORDER * 2; }
long wm_client_h(long hnd) { return g_win[hnd].h - WM_TITLE_H - WM_BORDER; }

// ---------- the region still waiting to be painted ----------
//
// Damage tracking alone is not enough. Painting a damaged rectangle back to
// front is always correct, but where two windows overlap the lower one is
// written and then written over, and the desktop background underneath is
// written first and covered entirely. A "full repaint" of four overlapping
// windows measured 137% of the screen: three hundred thousand pixels pushed
// across the bus purely to be hidden.
//
// So the paint goes front to back instead, carrying a region -- a list of
// rectangles -- of what is still unpainted. Each window draws only where the
// region still says nothing has been drawn, then subtracts itself from it.
// Whatever survives to the end is desktop. Every pixel is written exactly once,
// and windows behind a covering window are never touched at all.

#define WM_MAXREG 64

struct Rect g_reg[WM_MAXREG];
struct Rect g_reg2[WM_MAXREG];
long g_nreg;

void region_reset(struct Rect *r) {
    g_reg[0].x = r->x; g_reg[0].y = r->y;
    g_reg[0].w = r->w; g_reg[0].h = r->h;
    g_nreg = 1;
}

void region_push(struct Rect *dst, long *n, long x, long y, long w, long h) {
    if (w <= 0 || h <= 0) return;
    if (*n >= WM_MAXREG) return;
    dst[*n].x = x; dst[*n].y = y; dst[*n].w = w; dst[*n].h = h;
    *n = *n + 1;
}

// Remove the box from the region, splitting each overlapped rectangle into the
// up-to-four strips that survive it.
//
// If the split would need more rectangles than the region can hold, the
// original is kept whole. That over-paints -- some pixels get written twice --
// which is the safe direction to fail in. Dropping the rectangle instead would
// leave a patch of screen that no later frame ever repairs.
void region_subtract(long bx, long by, long bw, long bh) {
    long i;
    long n2;
    long bx1; long by1;

    bx1 = bx + bw;
    by1 = by + bh;
    n2 = 0;
    i = 0;
    while (i < g_nreg) {
        long rx; long ry; long rw; long rh;
        long rx1; long ry1;
        rx = g_reg[i].x; ry = g_reg[i].y; rw = g_reg[i].w; rh = g_reg[i].h;
        rx1 = rx + rw; ry1 = ry + rh;

        if (bx >= rx1 || bx1 <= rx || by >= ry1 || by1 <= ry) {
            region_push(g_reg2, &n2, rx, ry, rw, rh);      // no overlap
        } else if (n2 + 4 > WM_MAXREG) {
            region_push(g_reg2, &n2, rx, ry, rw, rh);      // no room to split
        } else {
            long iy; long iy1; long ix; long ix1;
            iy  = ry;  if (by  > iy)  iy  = by;
            iy1 = ry1; if (by1 < iy1) iy1 = by1;
            ix  = rx;  if (bx  > ix)  ix  = bx;
            ix1 = rx1; if (bx1 < ix1) ix1 = bx1;
            region_push(g_reg2, &n2, rx, ry, rw, iy - ry);           // above
            region_push(g_reg2, &n2, rx, iy1, rw, ry1 - iy1);        // below
            region_push(g_reg2, &n2, rx, iy, ix - rx, iy1 - iy);     // left
            region_push(g_reg2, &n2, ix1, iy, rx1 - ix1, iy1 - iy);  // right
        }
        i = i + 1;
    }

    i = 0;
    while (i < n2) {
        g_reg[i].x = g_reg2[i].x; g_reg[i].y = g_reg2[i].y;
        g_reg[i].w = g_reg2[i].w; g_reg[i].h = g_reg2[i].h;
        i = i + 1;
    }
    g_nreg = n2;
}

// ---------- the pointer ----------
//
// The cursor is the one thing on screen that belongs to no window. That is not
// a detail, it is the whole design decision: if the cursor were composited into
// a window's backing buffer it would be captured by that window's content,
// smeared across it during a drag, and left behind whenever the pointer crossed
// a boundary. So it is drawn straight to the framebuffer AFTER the compositor
// has finished, and it is removed by telling the compositor that the rectangle
// it occupied is damaged. The compositor then repaints what was underneath
// without ever knowing why.
//
// The cost of that is two small rectangles per frame -- the one being vacated
// and the one being entered -- against 786,432 pixels for a full repaint. A
// cursor is the thing that moves most often on a desktop, so getting this wrong
// undoes every saving the compositor makes.

#define CUR_W 12
#define CUR_H 19

// '.' transparent, 'X' outline, '#' fill. A mask rather than a colour array,
// because a pointer that is a solid rectangle is unusable over text.
char *g_cursor_bits =
    "X..........."
    "XX.........."
    "X#X........."
    "X##X........"
    "X###X......."
    "X####X......"
    "X#####X....."
    "X######X...."
    "X#######X..."
    "X########X.."
    "X#########X."
    "X##########X"
    "X#####XXXXXX"
    "X###X#X....."
    "X##X.X#X...."
    "XX...X#X...."
    "X.....X#X..."
    "......X#X..."
    ".......XX...";

long g_cur_on;          // is there a pointer at all
long g_cur_x;           // where it should be
long g_cur_y;
long g_cur_px;          // where it actually is on screen right now
long g_cur_py;
long g_cur_painted;     // ...and whether it is on screen at all
long g_cur_pixels;      // pixels the pointer itself has cost since a reset

void wm_cursor_show(long on) {
    if (g_cur_on == on) return;
    g_cur_on = on;
    if (!on && g_cur_painted) wm_damage(g_cur_px, g_cur_py, CUR_W, CUR_H);
}

// Ask for the pointer to be somewhere. Nothing reaches the screen until the
// next present, so a burst of mouse packets between two frames costs one move,
// not one per packet.
void wm_cursor_move(long x, long y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > fb_width - 1) x = fb_width - 1;
    if (y > fb_height - 1) y = fb_height - 1;
    g_cur_x = x;
    g_cur_y = y;
}

void wm_cursor_draw() {
    long y;
    if (!g_cur_on) return;
    y = 0;
    while (y < CUR_H) {
        long sy;
        sy = g_cur_y + y;
        if (sy >= 0 && sy < fb_height) {
            long x;
            long wrote;
            wrote = 0;
            x = 0;
            while (x < CUR_W) {
                long ch;
                long sx;
                ch = g_cursor_bits[y * CUR_W + x];
                sx = g_cur_x + x;
                if (ch != '.' && sx >= 0 && sx < fb_width) {
                    long col;
                    col = (ch == 'X') ? rgb(0, 0, 0) : rgb(255, 255, 255);
                    mmio_write32(fb_base + sy * fb_pitch + sx * 4, col);
                    wrote = wrote + 1;
                }
                x = x + 1;
            }
            // Counted through the same totals as everything else, so the cost
            // of the pointer cannot hide from the measurements.
            if (wrote) {
                wm_pixels = wm_pixels + wrote;
                g_cur_pixels = g_cur_pixels + wrote;
                wm_blits = wm_blits + 1;
            }
        }
        y = y + 1;
    }
    g_cur_px = g_cur_x;
    g_cur_py = g_cur_y;
    g_cur_painted = 1;
}

// ---------- the present step ----------

// Copy the part of window `hnd` that lies inside r to the screen.
void wm_blit(long hnd, struct Rect *r) {
    struct Rect c;
    long y;
    c.x = r->x; c.y = r->y; c.w = r->w; c.h = r->h;
    if (!rect_clip(&c, g_win[hnd].x, g_win[hnd].y, g_win[hnd].w, g_win[hnd].h))
        return;
    y = 0;
    while (y < c.h) {
        long sy; long sx;
        sy = (c.y + y) - g_win[hnd].y;
        sx = c.x - g_win[hnd].x;
        wm_row_copy(c.x, c.y + y, g_win[hnd].pix + sy * g_win[hnd].w + sx, c.w);
        y = y + 1;
    }
}

// Paint one damaged rectangle, front to back, subtracting as it goes.
//
// Dragging a window over a busy desktop costs the window's own area rather than
// the area of everything underneath it, and the windows behind a covering
// window are never read, never blitted, and never considered again.
void wm_paint_rect(struct Rect *r) {
    long i;

    region_reset(r);

    i = g_nwin - 1;
    while (i >= 0) {
        long hnd;
        hnd = g_order[i];
        if (g_win[hnd].used && g_win[hnd].visible) {
            long k;
            k = 0;
            while (k < g_nreg) { wm_blit(hnd, &g_reg[k]); k = k + 1; }
            region_subtract(g_win[hnd].x, g_win[hnd].y,
                            g_win[hnd].w, g_win[hnd].h);
            if (g_nreg == 0) return;      // nothing of this rectangle is left
        }
        i = i - 1;
    }

    // Anything the windows did not claim is desktop.
    i = 0;
    while (i < g_nreg) { wm_fill_rect(&g_reg[i], wm_bg); i = i + 1; }
}

void wm_present() {
    long i;

    // Erase the pointer by damaging where it is. The compositor has no idea a
    // pointer exists; it just repaints that rectangle from the windows and the
    // desktop, which is exactly what "erase" means. Doing it this way rather
    // than saving and restoring the pixels underneath also means the pointer
    // cannot resurrect a stale copy of a window that has since moved.
    if (g_cur_painted) {
        wm_damage(g_cur_px, g_cur_py, CUR_W, CUR_H);
        g_cur_painted = 0;
    }

    if (wm_no_damage || g_dmg_overflow) wm_damage_all();
    i = 0;
    while (i < g_ndmg) { wm_paint_rect(&g_dmg[i]); i = i + 1; }
    g_ndmg = 0;
    g_dmg_overflow = 0;

    // Last, on top of everything, and outside the damage system entirely.
    wm_cursor_draw();
}

// ---------- window operations that generate damage ----------

void wm_move(long hnd, long nx, long ny) {
    if (!g_win[hnd].used) return;
    wm_damage(g_win[hnd].x, g_win[hnd].y, g_win[hnd].w, g_win[hnd].h);
    g_win[hnd].x = nx;
    g_win[hnd].y = ny;
    wm_damage(nx, ny, g_win[hnd].w, g_win[hnd].h);
}

void wm_raise(long hnd) {
    long i; long at;
    at = -1;
    i = 0;
    while (i < g_nwin) { if (g_order[i] == hnd) at = i; i = i + 1; }
    if (at < 0 || at == g_nwin - 1) return;
    i = at;
    while (i < g_nwin - 1) { g_order[i] = g_order[i + 1]; i = i + 1; }
    g_order[g_nwin - 1] = hnd;
    wm_damage(g_win[hnd].x, g_win[hnd].y, g_win[hnd].w, g_win[hnd].h);
}

void wm_set_visible(long hnd, long on) {
    if (!g_win[hnd].used) return;
    if (g_win[hnd].visible == on) return;
    g_win[hnd].visible = on;
    wm_damage(g_win[hnd].x, g_win[hnd].y, g_win[hnd].w, g_win[hnd].h);
}

// The client area changed; tell the compositor which part, in window
// coordinates. This is the call an application makes, and getting it wrong is
// what leaves stale pixels on screen.
void wm_invalidate(long hnd, long x, long y, long w, long h) {
    if (!g_win[hnd].used) return;
    wm_damage(g_win[hnd].x + x, g_win[hnd].y + y, w, h);
}

// Move focus. Both title bars are redrawn and both are invalidated -- the one
// losing focus as well as the one gaining it. Repainting only the new one is
// the obvious bug here, and it leaves two windows both looking active, which
// is worse than neither looking active because it is a confident lie about
// where the next keystroke will land.
void wm_set_focus(long hnd) {
    long old;
    if (g_focus == hnd) return;
    old = g_focus;
    g_focus = hnd;
    if (old >= 0 && old < WM_MAXWIN && g_win[old].used) {
        wm_titlebar(old);
        wm_invalidate(old, 0, 0, g_win[old].w, WM_TITLE_H);
    }
    if (hnd >= 0 && hnd < WM_MAXWIN && g_win[hnd].used) {
        wm_titlebar(hnd);
        wm_invalidate(hnd, 0, 0, g_win[hnd].w, WM_TITLE_H);
    }
}

void wm_destroy(long hnd) {
    long i;
    long at;
    if (hnd < 0 || hnd >= WM_MAXWIN) return;
    if (!g_win[hnd].used) return;

    // Damage before the window stops existing, not after: the rectangle to
    // repaint is read from fields that are about to be cleared.
    wm_damage(g_win[hnd].x, g_win[hnd].y, g_win[hnd].w, g_win[hnd].h);

    at = -1;
    i = 0;
    while (i < g_nwin) { if (g_order[i] == hnd) at = i; i = i + 1; }
    if (at >= 0) {
        i = at;
        while (i < g_nwin - 1) { g_order[i] = g_order[i + 1]; i = i + 1; }
        g_nwin = g_nwin - 1;
    }

    if (g_win[hnd].pix) kfree((void *)g_win[hnd].pix);
    g_win[hnd].pix = 0;
    g_win[hnd].used = 0;
    g_win[hnd].visible = 0;

    // Focus cannot stay on a window that is gone. Hand it to whatever is now
    // in front, or to nothing.
    if (g_focus == hnd) {
        g_focus = -1;
        if (g_nwin > 0) wm_set_focus(g_order[g_nwin - 1]);
    }
}

// ---------- measurement ----------

void wm_reset_counters() { wm_pixels = 0; wm_blits = 0; g_cur_pixels = 0; }

// A rolling hash of everything on screen, read back from video memory. This is
// the check that matters: a compositor that skips work it should have done
// still produces a plausible picture, and only a comparison against a full
// repaint will say so. Masked to 52 bits because nano_cc has no unsigned type
// and a hash that goes negative is harder to read in a log than one that does
// not.
long wm_fb_checksum() {
    long y; long acc;
    acc = 5381;
    y = 0;
    while (y < fb_height) {
        long x; long row;
        row = fb_base + y * fb_pitch;
        x = 0;
        while (x < fb_width) {
            acc = ((acc * 33) + mmio_read32(row + x * 4)) & 0xFFFFFFFFFFFFF;
            x = x + 1;
        }
        y = y + 1;
    }
    return acc;
}

long wm_screen_pixels() { return fb_width * fb_height; }

void wm_init(long bg) {
    long i;
    i = 0;
    while (i < WM_MAXWIN) { g_win[i].used = 0; g_order[i] = 0; i = i + 1; }
    g_nwin = 0;
    g_ndmg = 0;
    g_dmg_overflow = 0;
    wm_bg = bg;
    wm_no_damage = 0;
    g_focus = -1;
    g_cur_on = 0;
    g_cur_painted = 0;
    g_cur_x = fb_width / 2;
    g_cur_y = fb_height / 2;
    g_cur_px = g_cur_x;
    g_cur_py = g_cur_y;
    g_cur_pixels = 0;
    wm_reset_counters();
    wm_damage_all();
}

#endif
