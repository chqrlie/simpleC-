// edit.c — the text editor, as a process.
//
// The kernel-side kernel/edit.c stays where it is: it carries the test suite
// that proves the buffer, the tabs and the save path, and it runs those tests
// with the filesystem directly in front of it. This is the same editor as a
// PROGRAM -- so that when it crashes it takes itself down and nothing else.
//
// What is shared and what is not:
//
//   SHARED, byte for byte: nano-ui.h. struct Edit, ed_key, ed_layout, the
//   caret rules, ui_edit, ui_menubar, ui_list, ui_tabs, the scrollbars and
//   the line-number gutter are the same file the kernel images compile. It
//   is not copied here; the Makefile refreshes it from the kernel's own copy
//   at build time.
//
//   NOT shared: everything that touches the machine. fs_lookup/fs_create/
//   fs_read/fs_write/fs_truncate/fs_sync become open/read/write/ftruncate_/
//   fsync_ syscalls, and wm_* becomes win_*. That is the whole difference
//   between the two, and it is confined to this file.

#include "nano-uiapp.h"
#include "nano-ui.h"

#define W 560
#define H 400

long g_pix[W * H];
struct Ui g_ui;
long g_hnd;

// ---------- tabs ----------
#define MAXTABS 4

struct Edit g_ed[MAXTABS];
char g_edbuf[MAXTABS * ED_CAP];
char g_tabnames[MAXTABS * 64];
char g_paths[MAXTABS * 64];
long g_ntabs;
long g_tab;

// ---------- the directory listing ----------
#define MAXENTS 48
char g_names[MAXENTS * 64];
long g_nents;
long g_sel;

#define MODAL_NONE 0
#define MODAL_OPEN 1
#define MODAL_SAVE 2
long g_modal;
char g_namefield[64];
char g_status[96];

void set_status(char *s) {
    long i;
    i = 0;
    while (s[i] && i < 95) { g_status[i] = s[i]; i = i + 1; }
    g_status[i] = 0;
}

void str_copy(char *dst, char *src, long cap) {
    long i;
    i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i = i + 1; }
    dst[i] = 0;
}

// Read the root directory through the syscall, into the flat NUL-separated
// block ui_list wants. readdir_ returns the entry INODE, not a boolean --
// testing it for == 1 matches only the root and shows an empty list.
void refresh_listing() {
    long i;
    long off;
    g_nents = 0;
    off = 0;
    i = 0;
    while (i < MAXENTS && g_nents < MAXENTS) {
        char nm[64];
        long j;
        if (readdir_("/", i, nm) <= 0) { i = i + 1; continue; }
        if (!ustrcmp(nm, ".") || !ustrcmp(nm, "..")) { i = i + 1; continue; }
        j = 0;
        while (nm[j] && off < MAXENTS * 64 - 2) { g_names[off] = nm[j]; off = off + 1; j = j + 1; }
        g_names[off] = 0; off = off + 1;
        g_nents = g_nents + 1;
        i = i + 1;
    }
}

void rebuild_tabnames() {
    long i;
    long off;
    off = 0;
    i = 0;
    while (i < g_ntabs) {
        char *p;
        long j;
        p = g_paths + i * 64;
        j = 0;
        if (!p[0]) {
            char *u;
            u = "untitled";
            while (u[j]) { g_tabnames[off] = u[j]; off = off + 1; j = j + 1; }
        } else {
            long last;
            last = 0;
            j = 0;
            while (p[j]) { if (p[j] == '/') last = j + 1; j = j + 1; }
            j = last;
            while (p[j]) { g_tabnames[off] = p[j]; off = off + 1; j = j + 1; }
        }
        if (g_ed[i].dirty) { g_tabnames[off] = '*'; off = off + 1; }
        g_tabnames[off] = 0; off = off + 1;
        i = i + 1;
    }
}

long new_tab() {
    long inherit;
    if (g_ntabs >= MAXTABS) { set_status("no free tab"); return 0 - 1; }
    inherit = (g_ntabs > 0) ? g_ed[g_tab].nums : 1;
    ed_init(&g_ed[g_ntabs], g_edbuf + g_ntabs * ED_CAP);
    g_ed[g_ntabs].nums = inherit;
    g_paths[g_ntabs * 64] = 0;
    g_ntabs = g_ntabs + 1;
    g_tab = g_ntabs - 1;
    rebuild_tabnames();
    return g_tab;
}

void close_tab(long i) {
    long k;
    if (i < 0 || i >= g_ntabs) return;
    k = i;
    while (k < g_ntabs - 1) {
        g_ed[k] = g_ed[k + 1];
        str_copy(g_paths + k * 64, g_paths + (k + 1) * 64, 64);
        k = k + 1;
    }
    g_ntabs = g_ntabs - 1;
    k = i;
    while (k < g_ntabs) {
        long b;
        b = 0;
        while (b <= g_ed[k].len) { g_edbuf[k * ED_CAP + b] = g_edbuf[(k + 1) * ED_CAP + b]; b = b + 1; }
        g_ed[k].buf = g_edbuf + k * ED_CAP;
        ed_layout(&g_ed[k]);
        k = k + 1;
    }
    if (g_ntabs == 0) new_tab();
    if (g_tab >= g_ntabs) g_tab = g_ntabs - 1;
    rebuild_tabnames();
}

long load_into_tab(long t, char *path) {
    long fd;
    long n;
    long keep;
    fd = open(path, O_RDONLY);
    if (fd < 0) { set_status("no such file"); return 0; }
    n = fsize(fd);
    if (n > ED_CAP - 1) n = ED_CAP - 1;
    keep = g_ed[t].nums;
    ed_init(&g_ed[t], g_edbuf + t * ED_CAP);
    g_ed[t].nums = keep;
    if (n > 0) read(fd, g_ed[t].buf, n);
    close(fd);
    g_ed[t].buf[n] = 0;
    g_ed[t].len = n;
    g_ed[t].dirty = 0;
    ed_layout(&g_ed[t]);
    str_copy(g_paths + t * 64, path, 64);
    rebuild_tabnames();
    return 1;
}

long save_tab(long t, char *path) {
    long fd;
    long n;

    // TRUNCATE FIRST, through the syscall this milestone added. Without it,
    // saving a file shorter than it was leaves the tail of the old contents
    // behind -- the file reads back correct for its new length and wrong for
    // its size, which passes every check that only looks at a prefix.
    ftruncate_(path);

    fd = open(path, O_WRONLY | O_CREAT);
    if (fd < 0) { set_status("could not create"); return 0; }
    n = write(fd, g_ed[t].buf, g_ed[t].len);
    close(fd);
    if (n != g_ed[t].len) { set_status("short write"); return 0; }

    // And the sync, or none of this outlives the machine.
    if (fsync_() < 0) { set_status("saved to memory only -- no disk"); return 0; }

    g_ed[t].dirty = 0;
    str_copy(g_paths + t * 64, path, 64);
    rebuild_tabnames();
    set_status("saved");
    return 1;
}

// ---------- menus ----------
char *g_menu_tops[3];
char *g_menu_items[10];
long  g_menu_owner[10];

#define M_NEW    0
#define M_OPEN   1
#define M_SAVE   2
#define M_SAVEAS 3
#define M_CLOSE  4
#define M_QUIT   5
#define M_HOME   6
#define M_END    7
#define M_NUMS   8
#define M_ABOUT  9

long g_quit;

void menus_init() {
    g_menu_tops[0] = "File";
    g_menu_tops[1] = "Edit";
    g_menu_tops[2] = "Help";
    g_menu_items[M_NEW]    = "New";          g_menu_owner[M_NEW]    = 0;
    g_menu_items[M_OPEN]   = "Open...";      g_menu_owner[M_OPEN]   = 0;
    g_menu_items[M_SAVE]   = "Save";         g_menu_owner[M_SAVE]   = 0;
    g_menu_items[M_SAVEAS] = "Save As...";   g_menu_owner[M_SAVEAS] = 0;
    g_menu_items[M_CLOSE]  = "Close Tab";    g_menu_owner[M_CLOSE]  = 0;
    g_menu_items[M_QUIT]   = "Quit";         g_menu_owner[M_QUIT]   = 0;
    g_menu_items[M_HOME]   = "Top";          g_menu_owner[M_HOME]   = 1;
    g_menu_items[M_END]    = "Bottom";       g_menu_owner[M_END]    = 1;
    g_menu_items[M_NUMS]   = "Line Numbers"; g_menu_owner[M_NUMS]   = 1;
    g_menu_items[M_ABOUT]  = "About";        g_menu_owner[M_ABOUT]  = 2;
}

void do_menu(long item) {
    if (item < 0) return;
    if (item == M_NEW) { new_tab(); set_status("new"); }
    else if (item == M_OPEN) { refresh_listing(); g_sel = 0; g_modal = MODAL_OPEN; }
    else if (item == M_SAVE) {
        if (g_paths[g_tab * 64]) save_tab(g_tab, g_paths + g_tab * 64);
        else { refresh_listing(); g_namefield[0] = 0; g_modal = MODAL_SAVE; }
    }
    else if (item == M_SAVEAS) {
        refresh_listing();
        str_copy(g_namefield, g_paths + g_tab * 64, 64);
        g_modal = MODAL_SAVE;
    }
    else if (item == M_CLOSE) close_tab(g_tab);
    // Quit EXITS THE PROCESS, which is the whole point of being one. The
    // kernel-side version had to halt the machine.
    else if (item == M_QUIT) { fsync_(); g_quit = 1; }
    else if (item == M_HOME) { g_ed[g_tab].caret = 0; ed_scroll_to_caret(&g_ed[g_tab]); }
    else if (item == M_END) {
        g_ed[g_tab].caret = g_ed[g_tab].len;
        ed_scroll_to_caret(&g_ed[g_tab]);
    }
    else if (item == M_NUMS) {
        g_ed[g_tab].nums = !g_ed[g_tab].nums;
        set_status(g_ed[g_tab].nums ? "line numbers on" : "line numbers off");
    }
    else if (item == M_ABOUT) set_status("nano-os editor, as a process");
}

void frame() {
    long chosen;
    long t;

    ui_begin(&g_ui, 0, 6, 6, W - 12);

    chosen = ui_menubar(&g_ui, g_menu_tops, 3, g_menu_items, g_menu_owner, 10);
    do_menu(chosen);

    if (g_modal == MODAL_NONE) {
        rebuild_tabnames();
        t = ui_tabs(&g_ui, g_tabnames, g_ntabs, g_tab);
        if (t >= 0) g_tab = t;
        else if (t <= UI_TAB_CLOSE(0)) close_tab(0 - 2 - t);

        ui_row(&g_ui, 1);
        ui_edit(&g_ui, &g_ed[g_tab], 280);

        ui_row(&g_ui, 2);
        ui_label(&g_ui, g_paths[g_tab * 64] ? g_paths + g_tab * 64 : "untitled");
        ui_label(&g_ui, g_status);
    } else if (g_modal == MODAL_OPEN) {
        ui_row(&g_ui, 1);
        ui_label(&g_ui, "Open which file?");
        ui_row(&g_ui, 1);
        {
            long act;
            act = ui_list(&g_ui, g_names, g_nents, &g_sel, 230);
            if (act >= 0) {
                char path[80];
                long off;
                path[0] = '/';
                off = ui_list_name_at(g_names, act);
                str_copy(path + 1, g_names + off, 78);
                if (g_ed[g_tab].dirty || g_paths[g_tab * 64]) new_tab();
                if (load_into_tab(g_tab, path)) set_status("opened");
                g_modal = MODAL_NONE;
            }
        }
        ui_row(&g_ui, 2);
        if (ui_button(&g_ui, "Cancel")) g_modal = MODAL_NONE;
        ui_label(&g_ui, g_status);
    } else {
        ui_row(&g_ui, 1);
        ui_label(&g_ui, "Save as (name, then Save):");
        ui_row(&g_ui, 1);
        ui_text(&g_ui, g_namefield, 64);
        ui_row(&g_ui, 1);
        ui_list(&g_ui, g_names, g_nents, &g_sel, 170);
        ui_row(&g_ui, 3);
        if (ui_button(&g_ui, "Save")) {
            char path[80];
            if (g_namefield[0] == '/') str_copy(path, g_namefield, 80);
            else { path[0] = '/'; str_copy(path + 1, g_namefield, 78); }
            if (g_namefield[0]) { save_tab(g_tab, path); g_modal = MODAL_NONE; }
            else set_status("name it first");
        }
        if (ui_button(&g_ui, "Cancel")) g_modal = MODAL_NONE;
        ui_label(&g_ui, g_status);
    }

    ui_end(&g_ui);
}

int main(int argc, char **argv) {
    long poll[6];
    long prev_down;
    long i;

    i = 0;
    while (i < W * H) { g_pix[i] = rgb(236, 238, 242); i = i + 1; }

    g_hnd = win_open(40, 30, W + 8, H + 28, "editor");
    if (g_hnd < 0) { write(1, "edit: no window\n", 16); return 1; }

    sfc_bind(g_pix, W, H);
    ui_init(&g_ui);
    ui_forget_all();
    menus_init();
    g_namefield[0] = 0;
    g_ntabs = 0;
    new_tab();
    set_status("ready");
    prev_down = 0;

    // If a file was named on the command line, open it. This is how the file
    // manager will hand a file to the editor.
    if (argc > 1) {
        if (load_into_tab(0, argv[1])) set_status("opened");
    }

    sfc_input(0, 0, W, H);
    ui_input(&g_ui, 0, 0, 0, 0);
    frame();
    win_blit(g_hnd, g_pix, W, H, 0);
    win_present(g_hnd);
    write(1, "edit: up\n", 9);

    while (!g_quit) {
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
        nap(20);
    }

    win_close(g_hnd);
    write(1, "edit: exited cleanly\n", 21);
    return 0;
}
