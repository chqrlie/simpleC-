// gfxshell.c — the mini-OS shell, running on a graphics-mode framebuffer.
//
// Same shell as shell.c, but the console is drawn pixel by pixel into a
// linear framebuffer instead of poked into VGA text memory, so it can also
// draw. Compiled by nano_cc itself.
//
// nano-fb.h comes first on purpose: nano-kernel.h checks for its include guard
// and sends putc() to the framebuffer when it is there.

#include "nano-fb.h"
#include "nano-kernel.h"

#define COL_BG      0x0d1117
#define COL_FG      0xc9d1d9
#define COL_ACCENT  0x58a6ff
#define COL_DIM     0x6e7681
#define COL_OK      0x3fb950
#define COL_WARN    0xd29922

long g_panel_x;
long g_panel_y;
long g_panel_w;
long g_panel_h;

// The console occupies the left column; the right-hand panel is the drawing
// surface. Keeping them apart means a scroll never has to repaint the art.
void chrome() {
    fb_clear(COL_BG);

    fb_fill(0, 0, fb_width, 26, 0x161b22);
    fb_fill(0, 26, fb_width, 1, 0x30363d);
    fb_scale = 2;
    fb_text(12, 5, "nano-os", COL_ACCENT, -1);
    fb_scale = 1;
    fb_text(130, 10, "compiled by nano_cc, which compiled itself", COL_DIM, -1);
    fb_scale = 2;                       // back to the console's size

    g_panel_x = 528;
    g_panel_y = 44;
    g_panel_w = fb_width - g_panel_x - 16;
    g_panel_h = fb_height - g_panel_y - 16;
    fb_fill(g_panel_x, g_panel_y, g_panel_w, g_panel_h, 0x010409);
    fb_rect(g_panel_x, g_panel_y, g_panel_w, g_panel_h, 0x30363d);
    fb_scale = 1;
    fb_text(g_panel_x + 8, g_panel_y + 8, "drawing surface", COL_DIM, -1);
    fb_scale = 2;
}

void panel_clear() {
    fb_fill(g_panel_x + 1, g_panel_y + 1, g_panel_w - 2, g_panel_h - 2, 0x010409);
    fb_scale = 1;
    fb_text(g_panel_x + 8, g_panel_y + 8, "drawing surface", COL_DIM, -1);
    fb_scale = 2;
}

void cmd_bars() {
    long i;
    long w;
    panel_clear();
    w = (g_panel_w - 40) / 8;
    i = 0;
    while (i < 8) {
        long c;
        c = 0;
        if (i == 0) c = 0xffffff;
        if (i == 1) c = 0xffff00;
        if (i == 2) c = 0x00ffff;
        if (i == 3) c = 0x00ff00;
        if (i == 4) c = 0xff00ff;
        if (i == 5) c = 0xff0000;
        if (i == 6) c = 0x0000ff;
        if (i == 7) c = 0x282828;
        fb_fill(g_panel_x + 20 + i * w, g_panel_y + 40, w, 120, c);
        i = i + 1;
    }
    puts("drew colour bars\n");
}

void cmd_grad() {
    long j;
    long w;
    long h;
    panel_clear();
    w = g_panel_w - 40;
    h = 160;
    j = 0;
    while (j < h) {
        long i;
        i = 0;
        while (i < w) {
            fb_pixel(g_panel_x + 20 + i, g_panel_y + 40 + j,
                     rgb(i * 255 / w, j * 255 / h, 200 - j * 255 / h));
            i = i + 1;
        }
        j = j + 1;
    }
    puts("drew a gradient\n");
}

void cmd_lines() {
    long i;
    long cx;
    long cy;
    panel_clear();
    cx = g_panel_x + g_panel_w / 2;
    cy = g_panel_y + 60;
    i = 0;
    while (i <= 20) {
        fb_line(cx, cy,
                g_panel_x + 20 + i * ((g_panel_w - 40) / 20),
                g_panel_y + g_panel_h - 40,
                rgb(80 + i * 8, 220 - i * 8, 255 - i * 6));
        i = i + 1;
    }
    puts("drew a line fan\n");
}

void cmd_circles() {
    long i;
    long cx;
    long cy;
    panel_clear();
    cx = g_panel_x + g_panel_w / 2;
    cy = g_panel_y + g_panel_h / 2;
    i = 1;
    while (i <= 14) {
        fb_circle(cx, cy, i * 14, rgb(255, 210 - i * 12, 40 + i * 14));
        i = i + 1;
    }
    puts("drew concentric circles\n");
}

void cmd_font() {
    long c;
    long x;
    long y;
    panel_clear();
    c = 32;
    x = 0;
    y = 0;
    while (c <= 126) {
        fb_glyph(g_panel_x + 24 + x * 18, g_panel_y + 40 + y * 20, c, COL_FG, -1);
        x = x + 1;
        if (x >= 22) { x = 0; y = y + 1; }
        c = c + 1;
    }
    puts("drew the whole font, 95 glyphs\n");
}

// Everything at once, laid out in the panel: the screenshot people actually
// want to see.
void cmd_demo() {
    long i;
    long w;
    long cx;
    panel_clear();
    w = (g_panel_w - 40) / 8;
    i = 0;
    while (i < 8) {
        long c;
        c = 0;
        if (i == 0) c = 0xffffff;
        if (i == 1) c = 0xffff00;
        if (i == 2) c = 0x00ffff;
        if (i == 3) c = 0x00ff00;
        if (i == 4) c = 0xff00ff;
        if (i == 5) c = 0xff0000;
        if (i == 6) c = 0x0000ff;
        if (i == 7) c = 0x303030;
        fb_fill(g_panel_x + 20 + i * w, g_panel_y + 34, w, 70, c);
        i = i + 1;
    }

    long j;
    j = 0;
    while (j < 70) {
        i = 0;
        while (i < w * 8) {
            fb_pixel(g_panel_x + 20 + i, g_panel_y + 116 + j,
                     rgb(i * 255 / (w * 8), j * 255 / 70, 190 - j * 2));
            i = i + 1;
        }
        j = j + 1;
    }

    cx = g_panel_x + g_panel_w / 2;
    i = 0;
    while (i <= 24) {
        fb_line(cx, g_panel_y + 210,
                g_panel_x + 20 + (i * (g_panel_w - 40)) / 24,
                g_panel_y + 400,
                rgb(70 + i * 7, 220 - i * 6, 255 - i * 5));
        i = i + 1;
    }

    i = 1;
    while (i <= 12) {
        fb_circle(cx, g_panel_y + 510, i * 12, rgb(255 - i * 10, 200 - i * 12, 60 + i * 16));
        i = i + 1;
    }

    fb_scale = 1;
    fb_text(g_panel_x + 8, g_panel_y + g_panel_h - 18,
            "every pixel written by code this compiler built", COL_DIM, -1);
    fb_scale = 2;
    puts("drew bars, gradient, lines and circles\n");
}

void cmd_fbinfo() {
    printf("resolution %dx%d at %d bpp\n", fb_width, fb_height, fb_bpp);
    printf("pitch      %d bytes per scanline\n", fb_pitch);
    printf("base       0x%x (from PCI BAR0)\n", fb_base);
    printf("console    %dx%d characters\n", fb_cols(), fb_rows());
}

void cmd_pci() {
    long slot;
    long found;
    found = 0;
    slot = 0;
    while (slot < 32) {
        long id;
        id = pci_read32(0, slot, 0, 0x00);
        if (id != -1 && (id & 0xFFFF) != 0xFFFF) {
            long cls;
            cls = pci_read32(0, slot, 0, 0x08);
            printf("00:%d vendor %x device %x class %x\n",
                   slot, id & 0xFFFF, (id >> 16) & 0xFFFF, (cls >> 24) & 0xFF);
            found = found + 1;
        }
        slot = slot + 1;
    }
    printf("%d devices on bus 0\n", found);
}

void cmd_help() {
    puts("commands:\n");
    puts("  help clear ver fbinfo pci\n");
    puts("  demo bars grad lines circles font\n");
    puts("  echo <text>\n");
}

// Something on the panel at boot, so the first screen shows the framebuffer
// is genuinely live rather than merely cleared.
void splash() {
    long i;
    long cx;
    long cy;
    cx = g_panel_x + g_panel_w / 2;
    cy = g_panel_y + g_panel_h / 2;
    i = 0;
    while (i < 60) {
        fb_line(cx, cy,
                g_panel_x + 20 + (i * (g_panel_w - 40)) / 60,
                g_panel_y + g_panel_h - 30,
                rgb(30 + i * 3, 90 + i * 2, 200 - i * 2));
        i = i + 3;
    }
    i = 1;
    while (i <= 10) {
        fb_circle(cx, cy, i * 13, rgb(255 - i * 12, 180 - i * 10, 90 + i * 15));
        i = i + 1;
    }
    fb_scale = 1;
    fb_text(g_panel_x + 8, g_panel_y + g_panel_h - 18,
            "every pixel written by code this compiler built", COL_DIM, -1);
    fb_scale = 2;
}

int starts_with(char *s, char *p) {
    while (*p) {
        if (*s != *p) return 0;
        s = s + 1; p = p + 1;
    }
    return 1;
}

int main() {
    char line[128];
    long n;

    serial_init();
    kbd_init();

    if (!fb_init(1024, 768)) {
        vga_clear();
        puts("no Bochs VBE adapter; cannot start the graphics shell\n");
        for (;;) { }
    }

    fb_console_init(2, COL_FG, COL_BG);       // 16x16 cells, readable at 1024x768
    chrome();
    // the console lives under the title bar, in the left column, and stops
    // short of the drawing panel so a scroll never touches it
    fb_console_at(12, 44, 500, fb_height - 60);
    g_have_fb = 1;

    splash();
    puts("framebuffer console up. type help.\n\n");

    for (;;) {
        long c;
        puts("> ");
        n = 0;
        for (;;) {
            c = keyboard_getchar();
            if (c == '\n') { putc('\n'); break; }
            if (c == '\b') {
                if (n > 0) { n = n - 1; putc('\b'); }
            } else if (n < 120) {
                line[n] = c;
                n = n + 1;
                putc(c);
            }
        }
        line[n] = 0;

        if (n == 0) { }
        else if (!strcmp(line, "help")) cmd_help();
        else if (!strcmp(line, "clear")) { chrome(); fb_cx = 0; fb_cy = 0; }
        else if (!strcmp(line, "ver")) puts("nano-os 0.2, framebuffer edition\n");
        else if (!strcmp(line, "fbinfo")) cmd_fbinfo();
        else if (!strcmp(line, "pci")) cmd_pci();
        else if (!strcmp(line, "demo")) cmd_demo();
        else if (!strcmp(line, "bars")) cmd_bars();
        else if (!strcmp(line, "grad")) cmd_grad();
        else if (!strcmp(line, "lines")) cmd_lines();
        else if (!strcmp(line, "circles")) cmd_circles();
        else if (!strcmp(line, "font")) cmd_font();
        else if (starts_with(line, "echo ")) { puts(line + 5); putc('\n'); }
        else { puts("unknown: "); puts(line); putc('\n'); }
    }
    return 0;
}
