// uidemo.c — the widget toolkit, running as a PROCESS.
//
// The smallest thing that proves the port: a window opened by syscall, the
// unmodified nano-ui.h drawing into a buffer this program owns, and the
// buffer handed to the kernel with SYS_WINBLIT. No kernel function is called
// anywhere below; everything crosses the boundary through a number and a
// register convention.
//
// It exists before the editor and the file manager because if the toolkit
// does not work in user space then neither of those can, and finding that out
// from three hundred lines is better than from three thousand.
//
// It also carries the crash test, which is the whole reason for doing this:
// press "Crash on purpose" and the program dereferences a null pointer. The
// machine must survive that, and keep this program's window closed rather
// than taking the rest of the system with it.

#include "nano-uiapp.h"
#include "nano-ui.h"

#define W 420
#define H 300

long g_pix[W * H];
struct Ui g_ui;
long g_hnd;

long g_check1;
long g_check2;
long g_slider;
long g_presses;
long g_crash;
char g_field[48];

// A frame. Identical in shape to the kernel-side demos, which is the point.
void frame() {
    ui_begin(&g_ui, 0, 8, 8, W - 16);

    ui_row(&g_ui, 1);
    ui_label(&g_ui, "nano-ui, in a process");

    ui_row(&g_ui, 2);
    if (ui_button(&g_ui, "press me")) g_presses = g_presses + 1;
    if (ui_button(&g_ui, "reset")) g_presses = 0;

    ui_row(&g_ui, 2);
    ui_checkbox(&g_ui, "one", &g_check1);
    ui_checkbox(&g_ui, "two", &g_check2);

    ui_row(&g_ui, 1);
    ui_slider(&g_ui, &g_slider, 0, 100);

    ui_row(&g_ui, 1);
    ui_text(&g_ui, g_field, 48);

    ui_row(&g_ui, 1);
    ui_progress(&g_ui, g_presses, 0, 10);

    ui_row(&g_ui, 1);
    // The button this whole milestone is about.
    if (ui_button(&g_ui, "Crash on purpose")) g_crash = 1;

    ui_end(&g_ui);
}

int main(int argc, char **argv) {
    long poll[6];
    long prev_down;
    long i;

    i = 0;
    while (i < W * H) { g_pix[i] = rgb(46, 50, 60); i = i + 1; }

    g_hnd = win_open(120, 90, W + 8, H + 28, "uidemo");
    if (g_hnd < 0) {
        write(1, "uidemo: no window\n", 18);
        return 1;
    }

    sfc_bind(g_pix, W, H);
    ui_init(&g_ui);
    ui_forget_all();
    g_field[0] = 0;
    prev_down = 0;

    // First frame unconditionally, so there is something on screen before
    // anything is touched.
    sfc_input(0, 0, W, H);
    ui_input(&g_ui, 0, 0, 0, 0);
    frame();
    win_blit(g_hnd, g_pix, W, H, 0);
    win_present(g_hnd);
    write(1, "uidemo: up\n", 11);

    for (;;) {
        long down;
        long pressed;
        long released;
        long key;

        if (win_poll(g_hnd, poll) < 0) break;

        down = poll[2] & 1;
        pressed = (down && !prev_down);
        released = (!down && prev_down);
        prev_down = down;
        key = poll[3];

        // A KEY, not only the button. The test that drives this from the
        // kernel side would otherwise have to compute the button's pixel
        // position from the toolkit's layout constants -- which it does not
        // include, and which are free to change the moment a row is added
        // above it. A keystroke is position-independent and tests the same
        // fault.
        if (key == 'x' || key == 'X') {
            write(1, "uidemo: about to dereference a null pointer\n", 43);
            g_crash = 1;
        }

        // Only when something happened, the same rule the kernel-side demos
        // use. An idle program costs the machine nothing.
        if (pressed || released || key || down) {
            sfc_input(poll[0], poll[1], poll[4], poll[5]);
            ui_input(&g_ui, down, pressed, released, key);
            sfc_clear_damage();
            frame();
            if (sfc_dirty()) {
                win_blit(g_hnd, g_pix, W, H, 0);
                win_present(g_hnd);
            }
        }

        if (g_crash) {
            long *boom;
            // Deliberately NO write() immediately before the fault: the first
            // attempt printed here and the kernel faulted inside the syscall
            // rather than in this program, which made the fault look like a
            // kernel bug instead of the intended one. Announced one loop
            // earlier instead, so the crash is the only thing happening.
            // The kernel unmaps page zero (mm_protect_null), so this is a
            // page fault in this process's own address space. The fault
            // handler should kill the thread and keep going.
            boom = (long *)0;
            boom[0] = 1;
            write(1, "uidemo: STILL HERE -- the write to zero did nothing\n", 51);
            g_crash = 0;
        }

        nap(20);
    }

    win_close(g_hnd);
    return 0;
}
