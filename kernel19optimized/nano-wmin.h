// nano-wmin.h — what a click means.
//
// The compositor in nano-wm.h knows how to paint windows. It has no opinion
// about which window a point at (412, 233) is in, and it should not: painting
// walks the z-order back to front, hit testing walks it front to back, and
// conflating the two is how you end up with a click landing on the window
// underneath the one you can see.
//
// This layer is the whole of the interaction model:
//
//   press on a title bar   -> raise, focus, and start dragging
//   press on a close box   -> destroy the window
//   press on a client area -> raise and focus, nothing else
//   press on the desktop   -> focus nothing
//   motion while dragging  -> move the window, keeping the grab point fixed
//   release                -> stop dragging
//
// Requires nano-wm.h and nano-mouse.h.

#ifndef NANO_WMIN_H
#define NANO_WMIN_H

#define WM_PART_NONE   0
#define WM_PART_TITLE  1
#define WM_PART_CLOSE  2
#define WM_PART_CLIENT 3

// Drag state. g_drag_dx/dy are the offset from the window's origin to the point
// that was grabbed. Storing the grab OFFSET rather than the last mouse position
// is what makes a drag stable: if a packet is missed, or the cursor is clamped
// at the edge of the screen, the window still ends up under the pointer instead
// of drifting away from it by the accumulated error.
long g_drag_win;
long g_drag_dx;
long g_drag_dy;

long g_prev_btn;        // button state at the previous event, for edges
long g_wmin_clicks;     // presses acted on
long g_wmin_drags;      // drags started
long g_wmin_closes;     // windows closed by their button
long g_wmin_closed;     // handle of the last one, or -1. A latch: the layer
                        // above wants to know that the user closed something,
                        // and this says which, without wmin knowing what a
                        // process is.

// ---------- hit testing ----------

// Front to back, so the window you can see is the one you hit. Returns a
// handle, or -1 for the desktop.
long wm_hit_win(long x, long y) {
    long i;
    i = g_nwin - 1;
    while (i >= 0) {
        long hnd;
        hnd = g_order[i];
        if (g_win[hnd].used && g_win[hnd].visible) {
            if (x >= g_win[hnd].x && x < g_win[hnd].x + g_win[hnd].w &&
                y >= g_win[hnd].y && y < g_win[hnd].y + g_win[hnd].h)
                return hnd;
        }
        i = i - 1;
    }
    return -1;
}

// Which part of that window, in screen coordinates.
long wm_hit_part(long hnd, long x, long y) {
    long lx;
    long ly;
    if (hnd < 0 || !g_win[hnd].used) return WM_PART_NONE;
    lx = x - g_win[hnd].x;
    ly = y - g_win[hnd].y;
    if (lx < 0 || ly < 0 || lx >= g_win[hnd].w || ly >= g_win[hnd].h)
        return WM_PART_NONE;
    if (ly < WM_TITLE_H) {
        // The close box is tested BEFORE the title bar, because it is inside
        // it. The other order gives a close button that starts a drag.
        if (!g_win[hnd].fixed) {
            long cx;
            long cy;
            cx = wm_close_x(hnd);
            cy = wm_close_y();
            if (lx >= cx && lx < cx + WM_CLOSE_SZ &&
                ly >= cy && ly < cy + WM_CLOSE_SZ)
                return WM_PART_CLOSE;
        }
        return WM_PART_TITLE;
    }
    return WM_PART_CLIENT;
}

// ---------- the state machine ----------

// One mouse event. Position is already in screen coordinates and already
// clamped; `btn` is the current button mask, not a change.
void wm_input_mouse(long x, long y, long btn) {
    long pressed;
    long released;

    pressed  = (btn & 1) && !(g_prev_btn & 1);
    released = !(btn & 1) && (g_prev_btn & 1);
    g_prev_btn = btn;

    wm_cursor_move(x, y);

    if (g_drag_win >= 0) {
        if (released) {
            g_drag_win = -1;
        } else {
            // The grab point stays under the pointer. Note this is driven by
            // the absolute position, not by a delta accumulated across events,
            // so a dropped event costs nothing at all.
            wm_move(g_drag_win, x - g_drag_dx, y - g_drag_dy);
        }
        return;
    }

    if (!pressed) return;

    {
        long hnd;
        long part;
        hnd = wm_hit_win(x, y);
        if (hnd < 0) { wm_set_focus(-1); return; }

        part = wm_hit_part(hnd, x, y);
        g_wmin_clicks = g_wmin_clicks + 1;

        if (part == WM_PART_CLOSE) {
            g_wmin_closes = g_wmin_closes + 1;
            g_wmin_closed = hnd;
            wm_destroy(hnd);
            return;
        }

        // Raise before focusing. wm_set_focus repaints two title bars and
        // wm_raise repaints one window; doing them in this order means the
        // title bar redraw lands on a window that is already at the front, and
        // so is not immediately painted over by the raise.
        wm_raise(hnd);
        wm_set_focus(hnd);

        if (part == WM_PART_TITLE) {
            g_drag_win = hnd;
            g_drag_dx = x - g_win[hnd].x;
            g_drag_dy = y - g_win[hnd].y;
            g_wmin_drags = g_wmin_drags + 1;
        }
    }
}

// Drain the mouse queue into the window manager. Returns the number of events
// consumed, which is also the answer to "does anything need presenting".
long wm_pump_mouse() {
    struct MEvent e;
    long n;
    n = 0;
    while (mouse_pop(&e)) {
        wm_input_mouse(e.x, e.y, e.btn);
        n = n + 1;
    }
    return n;
}

void wmin_init() {
    g_drag_win = -1;
    g_prev_btn = 0;
    g_drag_dx = 0;
    g_drag_dy = 0;
    g_wmin_clicks = 0;
    g_wmin_drags = 0;
    g_wmin_closes = 0;
    g_wmin_closed = -1;
}

#endif
