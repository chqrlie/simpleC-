// nano-uiapp.h — what nano-ui.h needs, supplied from user space.
//
// THE POINT OF THIS FILE, because it is not the obvious design.
//
// nano-ui.h draws by calling wm_win_fill, wm_win_frame, wm_win_glyph,
// wm_win_text and wm_win_pixel, and reports damage with wm_invalidate. Those
// are kernel functions that write into a window's backing store. A process
// has no window manager and no g_win[]; it has a buffer of its own and one
// syscall that copies the buffer into a window.
//
// The obvious port is to rewrite those fifty-three call sites against a new
// "surface" abstraction. That produces two copies of the widget code that are
// meant to behave identically and are free not to -- and the widgets carry
// rules (the caret's column memory, the swallow flag, the skip-if-unchanged
// hash) that are exactly the kind of thing that drifts between two copies.
//
// So instead this file DEFINES THOSE NAMES, against a plain pixel buffer.
// nano-ui.h is then included unmodified: the same file, the same lines, the
// same widgets, compiled a second time for a different machine. The only
// thing that differs is what "draw a rectangle" means underneath.
//
// It has to be included BEFORE nano-ui.h and it must not be combined with
// nano-wm.h, which defines the same names for the kernel.

#ifndef NANO_UIAPP_H
#define NANO_UIAPP_H

#include "nano-user.h"
#include "nano-font.h"

// ---------- the surface ----------
//
// One window's worth of pixels, owned by the process. `g_sfc_w` is the
// stride: nano-ui.h's coordinates are all client-relative, which is what
// makes them portable here at all.
#define SFC_MAXW 1024
#define SFC_MAXH 768

long *g_sfc;                   // the pixel buffer, caller-supplied
long  g_sfc_w;
long  g_sfc_h;

// Damage, accumulated by wm_invalidate and consumed by the app when it
// decides how much of the buffer to blit. The compositor's job, done by the
// program, because in a process there is no compositor.
long g_dmg_x0; long g_dmg_y0; long g_dmg_x1; long g_dmg_y1;

void sfc_bind(long *pix, long w, long h) {
    g_sfc = pix;
    g_sfc_w = w;
    g_sfc_h = h;
    g_dmg_x0 = 0; g_dmg_y0 = 0; g_dmg_x1 = -1; g_dmg_y1 = -1;
}

long sfc_dirty() { return g_dmg_x1 >= g_dmg_x0; }

void sfc_clear_damage() {
    g_dmg_x0 = 0; g_dmg_y0 = 0; g_dmg_x1 = -1; g_dmg_y1 = -1;
}

// ---------- what ui_window reads ----------
//
// nano-ui.h computes the pointer's position within the window as
// `g_mouse_x - g_win[win].x`, and asks whether it is inside using
// g_win[win].w/h. A process is told both of those by SYS_WINPOLL, already in
// CLIENT coordinates -- so the window's origin here is (0,0) and the mouse
// position is the client one. Giving the shim the same shape is what lets
// ui_window stay unmodified.
//
// Only the four fields nano-ui.h touches. A fuller struct Win would invite
// an app to reach for something the kernel has and a process does not.
struct Win {
    long x;
    long y;
    long w;
    long h;
};

struct Win g_win[1];
long g_mouse_x;
long g_mouse_y;

// Call once per frame with what win_poll returned.
void sfc_input(long mx, long my, long cw, long ch) {
    g_mouse_x = mx;
    g_mouse_y = my;
    g_win[0].x = 0;
    g_win[0].y = 0;
    g_win[0].w = cw;
    g_win[0].h = ch;
}

// ---------- colours ----------
//
// The same packing nano-fb.h uses. Defined here rather than included, because
// nano-fb.h is the kernel's framebuffer and reaches hardware.
long rgb(long r, long g, long b) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return (r << 16) | (g << 8) | b;
}

// ---------- the five drawing calls nano-ui.h makes ----------
//
// The handle argument is ignored: a process has exactly one surface bound at
// a time. Keeping the parameter means nano-ui.h does not have to change.

void wm_win_pixel(long hnd, long x, long y, long colour) {
    if (x < 0 || y < 0 || x >= g_sfc_w || y >= g_sfc_h) return;
    g_sfc[y * g_sfc_w + x] = colour;
}

void wm_win_fill(long hnd, long x, long y, long w, long h, long colour) {
    long yy;
    long x0; long y0; long x1; long y1;
    x0 = x; y0 = y; x1 = x + w; y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_sfc_w) x1 = g_sfc_w;
    if (y1 > g_sfc_h) y1 = g_sfc_h;
    yy = y0;
    while (yy < y1) {
        long xx;
        long row;
        row = yy * g_sfc_w;
        xx = x0;
        while (xx < x1) { g_sfc[row + xx] = colour; xx = xx + 1; }
        yy = yy + 1;
    }
}

void wm_win_frame(long hnd, long x, long y, long w, long h, long colour) {
    wm_win_fill(hnd, x, y, w, 1, colour);
    wm_win_fill(hnd, x, y + h - 1, w, 1, colour);
    wm_win_fill(hnd, x, y, 1, h, colour);
    wm_win_fill(hnd, x + w - 1, y, 1, h, colour);
}

void wm_win_glyph(long hnd, long px, long py, long ch, long fg) {
    long row;
    long base;
    if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
    base = (ch - FONT_FIRST) * FONT_H;
    row = 0;
    while (row < FONT_H) {
        long bits;
        long col;
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

// ---------- damage ----------
//
// In the kernel this tells the compositor which part of the screen to
// repaint. Here it accumulates a box the program uses to decide how much of
// its buffer to send. Same contract, one fewer layer.
void wm_invalidate(long hnd, long x, long y, long w, long h) {
    long x1;
    long y1;
    x1 = x + w - 1;
    y1 = y + h - 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_sfc_w - 1) x1 = g_sfc_w - 1;
    if (y1 > g_sfc_h - 1) y1 = g_sfc_h - 1;
    if (x1 < x || y1 < y) return;
    if (g_dmg_x1 < g_dmg_x0) {
        g_dmg_x0 = x; g_dmg_y0 = y; g_dmg_x1 = x1; g_dmg_y1 = y1;
        return;
    }
    if (x < g_dmg_x0) g_dmg_x0 = x;
    if (y < g_dmg_y0) g_dmg_y0 = y;
    if (x1 > g_dmg_x1) g_dmg_x1 = x1;
    if (y1 > g_dmg_y1) g_dmg_y1 = y1;
}

// The widgets ask whether the pointer is over THIS window before treating a
// click as theirs. A process is only told about its own window's pointer, so
// the answer is always yes -- the kernel has already done the test.
long wm_hit_win(long hnd, long x, long y) { return 1; }

#endif
