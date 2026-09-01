// nano-fb.h — a linear framebuffer, found and programmed by hand.
//
// Compiled by nano_cc with --kernel. Two jobs:
//
//   1. Ask the display adapter for a graphics mode. QEMU's `-kernel` loader is
//      Multiboot 1 and does not hand a kernel a framebuffer, and there is no
//      BIOS left to call once we are in long mode, so the mode has to be set
//      by writing the adapter's registers directly. QEMU's default `-vga std`
//      is the Bochs adapter, whose registers live behind an index/data pair at
//      ports 0x1CE/0x1CF.
//
//   2. Find out WHERE that framebuffer is. The adapter reports its own base
//      address in its PCI configuration space, so we walk the PCI bus through
//      the configuration ports at 0xCF8/0xCFC, find the display-class device,
//      and read BAR0. Hard-coding 0xFD000000 would work on one QEMU build and
//      quietly draw into nothing on the next.
//
// Everything here stays inside the subset nano_cc compiles.
//
// Include this BEFORE nano-kernel.h if you want console output to land on the
// framebuffer rather than in VGA text mode -- nano-kernel.h switches on this
// file's include guard.

#ifndef NANO_FB_H
#define NANO_FB_H

extern int  inb(int port);
extern void outb(int port, int val);
extern int  inw(int port);
extern void outw(int port, int val);
extern long inl(int port);
extern void outl(int port, long val);
extern void mmio_write32(long addr, long val);
extern long mmio_read32(long addr);

// ---------- Bochs VBE (the "BGA") ----------
#define VBE_INDEX   0x01CE
#define VBE_DATA    0x01CF

#define VBE_ID          0
#define VBE_XRES        1
#define VBE_YRES        2
#define VBE_BPP         3
#define VBE_ENABLE      4
#define VBE_BANK        5
#define VBE_VIRT_WIDTH  6
#define VBE_VIRT_HEIGHT 7
#define VBE_X_OFFSET    8
#define VBE_Y_OFFSET    9

#define VBE_DISABLED    0x00
#define VBE_ENABLED     0x01
#define VBE_LFB         0x40        // use the linear framebuffer, not banks

long fb_base;       // physical address of the framebuffer
long fb_width;
long fb_height;
long fb_pitch;      // bytes per scanline
long fb_bpp;
long fb_ok;         // 1 once a mode is set and a base address is known

void vbe_write(int index, int value) {
    outw(VBE_INDEX, index);
    outw(VBE_DATA, value);
}

int vbe_read(int index) {
    outw(VBE_INDEX, index);
    return inw(VBE_DATA);
}

// ---------- PCI configuration space ----------
// The 0xCF8 address register is
//   bit 31 enable | bus 23:16 | device 15:11 | function 10:8 | register 7:2
// and the dword at that address then appears at 0xCFC.
long pci_read32(int bus, int slot, int func, int off) {
    long addr;
    addr = 0x80000000;
    addr = addr + bus * 65536;
    addr = addr + slot * 2048;
    addr = addr + func * 256;
    addr = addr + (off & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}

void pci_write32(int bus, int slot, int func, int off, long val) {
    long addr;
    addr = 0x80000000;
    addr = addr + bus * 65536;
    addr = addr + slot * 2048;
    addr = addr + func * 256;
    addr = addr + (off & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

// Find the display adapter and return its BAR0 base address, or 0.
//
// Only bus 0 is scanned. A PC can have 256 buses behind bridges, but QEMU's
// default machine puts the VGA device on bus 0, and walking the whole tree
// needs bridge enumeration that buys nothing here. Say so rather than let a
// future reader assume this is a complete PCI scan.
long pci_find_framebuffer() {
    int slot;
    slot = 0;
    while (slot < 32) {
        long id;
        id = pci_read32(0, slot, 0, 0x00);
        if (id != -1 && (id & 0xFFFF) != 0xFFFF) {
            long cls;
            cls = pci_read32(0, slot, 0, 0x08);
            // class 0x03 = display controller, in the top byte of register 0x08
            if (((cls >> 24) & 0xFF) == 0x03) {
                long bar;
                bar = pci_read32(0, slot, 0, 0x10);
                if ((bar & 1) == 0) {          // memory BAR, not I/O
                    // make sure the device is allowed to answer to memory
                    // cycles at all: bit 1 of the command register
                    long cmd;
                    cmd = pci_read32(0, slot, 0, 0x04);
                    pci_write32(0, slot, 0, 0x04, cmd | 0x2 | 0x4);
                    return bar & 0xFFFFFFF0;
                }
            }
        }
        slot = slot + 1;
    }
    return 0;
}

// ---------- mode setting ----------
// Returns 1 on success. Leaves fb_ok at 0 and changes nothing if the adapter
// is not a Bochs VBE one, so a caller can fall back to VGA text.
long fb_init(long w, long h) {
    long id;
    fb_ok = 0;

    // The version register reads back 0xB0C0..0xB0C5 on a Bochs adapter.
    id = vbe_read(VBE_ID);
    if (id < 0xB0C0 || id > 0xB0CF) return 0;

    // The mode registers are only writable while the adapter is disabled.
    vbe_write(VBE_ENABLE, VBE_DISABLED);
    vbe_write(VBE_XRES, w);
    vbe_write(VBE_YRES, h);
    vbe_write(VBE_BPP, 32);
    vbe_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB);

    // Read the geometry BACK rather than assuming we got what we asked for.
    // The adapter clamps to what its video memory can hold, and drawing to the
    // size we requested rather than the size we were given would run off the
    // end of every scanline.
    fb_width  = vbe_read(VBE_XRES);
    fb_height = vbe_read(VBE_YRES);
    fb_bpp    = vbe_read(VBE_BPP);
    if (fb_width < 1 || fb_height < 1 || fb_bpp != 32) return 0;

    // The virtual width is the real stride, and it is not always the visible
    // width -- the adapter rounds it up. Using the visible width as the pitch
    // shears the whole image.
    fb_pitch = vbe_read(VBE_VIRT_WIDTH) * 4;
    if (fb_pitch < fb_width * 4) fb_pitch = fb_width * 4;

    fb_base = pci_find_framebuffer();
    if (fb_base == 0) return 0;

    fb_ok = 1;
    return 1;
}

// ---------- drawing ----------
// 32 bits per pixel, 0x00RRGGBB.
long rgb(long r, long g, long b) {
    return ((r & 255) * 65536) + ((g & 255) * 256) + (b & 255);
}

void fb_pixel(long x, long y, long colour) {
    if (!fb_ok) return;
    if (x < 0 || y < 0 || x >= fb_width || y >= fb_height) return;
    mmio_write32(fb_base + y * fb_pitch + x * 4, colour);
}

long fb_get(long x, long y) {
    if (!fb_ok) return 0;
    if (x < 0 || y < 0 || x >= fb_width || y >= fb_height) return 0;
    return mmio_read32(fb_base + y * fb_pitch + x * 4);
}

void fb_fill(long x, long y, long w, long h, long colour) {
    long yy;
    if (!fb_ok) return;
    if (x < 0) { w = w + x; x = 0; }
    if (y < 0) { h = h + y; y = 0; }
    if (x + w > fb_width)  w = fb_width - x;
    if (y + h > fb_height) h = fb_height - y;
    yy = 0;
    while (yy < h) {
        long row;
        long xx;
        row = fb_base + (y + yy) * fb_pitch + x * 4;
        xx = 0;
        while (xx < w) {
            mmio_write32(row + xx * 4, colour);
            xx = xx + 1;
        }
        yy = yy + 1;
    }
}

void fb_clear(long colour) { fb_fill(0, 0, fb_width, fb_height, colour); }

void fb_rect(long x, long y, long w, long h, long colour) {
    fb_fill(x, y, w, 1, colour);
    fb_fill(x, y + h - 1, w, 1, colour);
    fb_fill(x, y, 1, h, colour);
    fb_fill(x + w - 1, y, 1, h, colour);
}

// Bresenham, integer only -- there is no floating point in this compiler and
// there would be no FPU state set up for it if there were.
void fb_line(long x0, long y0, long x1, long y1, long colour) {
    long dx, dy, sx, sy, err;
    dx = x1 - x0; if (dx < 0) dx = 0 - dx;
    dy = y1 - y0; if (dy < 0) dy = 0 - dy;
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx - dy;
    for (;;) {
        fb_pixel(x0, y0, colour);
        if (x0 == x1 && y0 == y1) return;
        long e2;
        e2 = err * 2;
        if (e2 > 0 - dy) { err = err - dy; x0 = x0 + sx; }
        if (e2 < dx)     { err = err + dx; y0 = y0 + sy; }
    }
}

// Midpoint circle, also integer only.
void fb_circle(long cx, long cy, long r, long colour) {
    long x, y, d;
    x = r; y = 0; d = 1 - r;
    while (x >= y) {
        fb_pixel(cx + x, cy + y, colour); fb_pixel(cx + y, cy + x, colour);
        fb_pixel(cx - x, cy + y, colour); fb_pixel(cx - y, cy + x, colour);
        fb_pixel(cx - x, cy - y, colour); fb_pixel(cx - y, cy - x, colour);
        fb_pixel(cx + x, cy - y, colour); fb_pixel(cx + y, cy - x, colour);
        y = y + 1;
        if (d < 0) { d = d + 2 * y + 1; }
        else { x = x - 1; d = d + 2 * (y - x) + 1; }
    }
}

// ---------- text ----------
#include "nano-font.h"

long fb_scale;      // 1 = 8x8 cells, 2 = 16x16, and so on
long fb_lead;       // extra blank pixels between text rows
long fb_cx;         // cursor, in characters
long fb_cy;
long fb_fg;
long fb_bg;

// The console occupies a rectangle, not the whole screen, so a scroll does not
// disturb anything drawn beside it.
long fb_win_x;
long fb_win_y;
long fb_win_w;
long fb_win_h;

long fb_cell_w() { return FONT_W * fb_scale; }
// Glyph row 7 is the descender line, so without a little leading the tails of
// g j p q y land on the top of the next row of text.
long fb_cell_h() { return FONT_H * fb_scale + fb_lead; }

long fb_cols() { return fb_win_w / fb_cell_w(); }
long fb_rows() { return fb_win_h / fb_cell_h(); }

// Draw one glyph with its top-left corner at pixel (px, py). `bg` of -1 means
// draw only the lit pixels, so text can sit on top of whatever is already
// there instead of stamping a rectangle over it.
void fb_glyph(long px, long py, long ch, long fg, long bg) {
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
            long lit;
            lit = (bits >> col) & 1;
            if (lit) {
                fb_fill(px + col * fb_scale, py + row * fb_scale,
                        fb_scale, fb_scale, fg);
            } else if (bg >= 0) {
                fb_fill(px + col * fb_scale, py + row * fb_scale,
                        fb_scale, fb_scale, bg);
            }
            col = col + 1;
        }
        row = row + 1;
    }
}

// Move the console window up by one text row and blank the last. A framebuffer
// has no hardware scroll, so this is a real copy -- and it copies only the
// console rectangle, which is why anything drawn next to it survives.
void fb_scroll() {
    long cell;
    long y;
    long limit;
    cell = fb_cell_h();
    limit = fb_rows() * cell;
    y = 0;
    while (y < limit - cell) {
        long src;
        long dst;
        long x;
        dst = fb_base + (fb_win_y + y) * fb_pitch + fb_win_x * 4;
        src = fb_base + (fb_win_y + y + cell) * fb_pitch + fb_win_x * 4;
        x = 0;
        while (x < fb_win_w) {
            mmio_write32(dst + x * 4, mmio_read32(src + x * 4));
            x = x + 1;
        }
        y = y + 1;
    }
    fb_fill(fb_win_x, fb_win_y + limit - cell, fb_win_w, cell, fb_bg);
}

void fb_putc(long c) {
    if (!fb_ok) return;
    if (c == '\n') {
        fb_cx = 0;
        fb_cy = fb_cy + 1;
    } else if (c == '\r') {
        fb_cx = 0;
    } else if (c == '\b') {
        if (fb_cx > 0) {
            fb_cx = fb_cx - 1;
            fb_glyph(fb_win_x + fb_cx * fb_cell_w(),
                     fb_win_y + fb_cy * fb_cell_h(), ' ', fb_fg, fb_bg);
        }
    } else {
        fb_glyph(fb_win_x + fb_cx * fb_cell_w(),
                 fb_win_y + fb_cy * fb_cell_h(), c, fb_fg, fb_bg);
        fb_cx = fb_cx + 1;
        if (fb_cx >= fb_cols()) { fb_cx = 0; fb_cy = fb_cy + 1; }
    }
    while (fb_cy >= fb_rows()) { fb_scroll(); fb_cy = fb_cy - 1; }
}

void fb_puts(char *s) {
    while (*s) { fb_putc(*s); s = s + 1; }
}

// Text at an arbitrary pixel position, not on the character grid, and without
// disturbing the console cursor. `bg` of -1 leaves the background alone.
void fb_text(long px, long py, char *s, long fg, long bg) {
    long x;
    x = px;
    while (*s) {
        fb_glyph(x, py, *s, fg, bg);
        x = x + FONT_W * fb_scale;
        s = s + 1;
    }
}

// Place the console in a rectangle of the screen. Call fb_init first.
void fb_console_at(long x, long y, long w, long h) {
    fb_win_x = x; fb_win_y = y; fb_win_w = w; fb_win_h = h;
    fb_cx = 0; fb_cy = 0;
}

void fb_console_init(long scale, long fg, long bg) {
    fb_scale = scale;
    fb_lead = 2 * scale;
    fb_fg = fg;
    fb_bg = bg;
    fb_console_at(0, 0, fb_width, fb_height);
    fb_clear(bg);
}

#endif
