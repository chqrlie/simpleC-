// gfx.c — bring up a real graphics mode and draw into it.
//
// Compiled by nano_cc itself. Everything on screen is written pixel by pixel
// into a linear framebuffer whose address was read out of PCI configuration
// space at boot; nothing here goes through a BIOS or a firmware service,
// because in long mode there is no BIOS left to call.
//
// The serial port carries a running commentary, so `make gfxtest` can check
// the result headlessly and `make gfxshot` can photograph it.

#include "nano-kernel.h"
#include "nano-fb.h"

void draw_gradient(long x, long y, long w, long h) {
    long j;
    j = 0;
    while (j < h) {
        long i;
        i = 0;
        while (i < w) {
            fb_pixel(x + i, y + j, rgb(i * 255 / w, j * 255 / h, 160));
            i = i + 1;
        }
        j = j + 1;
    }
}

void draw_bars(long x, long y, long w, long h) {
    long i;
    long n;
    n = 8;
    i = 0;
    while (i < n) {
        long c;
        c = 0;
        if (i == 0) c = rgb(255, 255, 255);
        if (i == 1) c = rgb(255, 255, 0);
        if (i == 2) c = rgb(0, 255, 255);
        if (i == 3) c = rgb(0, 255, 0);
        if (i == 4) c = rgb(255, 0, 255);
        if (i == 5) c = rgb(255, 0, 0);
        if (i == 6) c = rgb(0, 0, 255);
        if (i == 7) c = rgb(40, 40, 40);
        fb_fill(x + i * (w / n), y, w / n, h, c);
        i = i + 1;
    }
}

int main() {
    long ok;

    kernel_init();
    puts("nano-os graphics bring-up\n");

    ok = fb_init(1024, 768);
    if (!ok) {
        puts("FB: no Bochs VBE adapter found, staying in text mode\n");
        for (;;) { }
    }

    printf("FB: %dx%d at %d bpp, pitch %d\n", fb_width, fb_height, fb_bpp, fb_pitch);
    printf("FB: base 0x%x\n", fb_base);

    fb_clear(rgb(16, 18, 28));

    // a border, so it is obvious whether the pitch is right: a wrong pitch
    // shears the image and the right-hand edge walks diagonally
    fb_rect(0, 0, fb_width, fb_height, rgb(80, 90, 120));
    fb_rect(2, 2, fb_width - 4, fb_height - 4, rgb(50, 56, 80));

    draw_bars(40, 40, 944, 120);
    draw_gradient(40, 190, 944, 110);

    // lines from a common origin: any rounding error in Bresenham shows up as
    // a kink, and a wrong stride shows up as a fan that is not a fan
    long i;
    i = 0;
    while (i <= 16) {
        fb_line(512, 470, 40 + i * 59, 720, rgb(90 + i * 10, 200 - i * 8, 255 - i * 12));
        i = i + 1;
    }

    // concentric circles
    i = 1;
    while (i <= 9) {
        fb_circle(512, 580, i * 12, rgb(255, 200 - i * 15, 60 + i * 20));
        i = i + 1;
    }

    // text, at two scales, straight onto the pixels
    fb_scale = 2;
    fb_text(40, 330, "nano-os  1024x768x32  linear framebuffer", rgb(230, 235, 255), -1);
    fb_scale = 1;
    fb_text(40, 356, "mode set through the Bochs VBE registers at 0x1CE/0x1CF;", rgb(150, 160, 190), -1);
    fb_text(40, 368, "base address read from PCI config space via 0xCF8/0xCFC.", rgb(150, 160, 190), -1);
    fb_text(40, 384, "ABCDEFGHIJKLMNOPQRSTUVWXYZ  abcdefghijklmnopqrstuvwxyz", rgb(200, 200, 120), -1);
    fb_text(40, 396, "0123456789  !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~", rgb(200, 200, 120), -1);
    fb_text(40, 412, "every pixel here was written by code this compiler built.", rgb(120, 200, 160), -1);

    // a read-back check: write a known pixel, read it, and report. If the
    // framebuffer address were wrong we would be writing into RAM somewhere
    // and this would still pass -- so it is a check on the MMIO path, not
    // proof that anything is visible.
    fb_pixel(5, 5, rgb(1, 2, 3));
    printf("FB: readback 0x%x (want 0x10203)\n", fb_get(5, 5));

    puts("FB: drawing complete\n");
    for (;;) { }
    return 0;
}
