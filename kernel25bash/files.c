// files.c — a file manager, with icons.
//
// The $20 side job, and it is small because the editor milestone paid for
// most of it: the list box, the scrollbars and the menu bar already existed.
// What is new here is the icon grid and the operations -- open, new folder,
// rename, delete -- against the real filesystem on the real disk.
//
// Icons are DRAWN rather than loaded. There is no image loader in this OS
// yet, and a 32x32 bitmap per type is 4KB of table each; a folder is a
// rectangle with a tab and a document is a rectangle with a folded corner.
// Crude, costs nothing to store, and scales to whatever size a caller asks
// for -- which a bitmap would not without a resampler.
//
// Navigation is real: double-clicking a folder goes into it, and ".." comes
// back. That means paths, which means the one piece of string handling worth
// being careful about in here.

#include "nano-kernel.h"
#include "nano-fb.h"
#include "nano-mouse.h"
#include "nano-int.h"
#include "nano-mm.h"
#include "nano-thread.h"
#include "nano-ata.h"
#include "nano-fs.h"
#include "nano-wm.h"
#include "nano-wmin.h"
#include "nano-term.h"
#include "nano-ui.h"

long g_fail;

void fail(char *msg) {
    printf("FAIL: %s\n", msg);
    g_fail = g_fail + 1;
}

void expect_true(char *what, long cond) {
    if (cond) printf("  ok  %s\n", what);
    else fail(what);
}

void expect(char *what, long got, long want) {
    if (got == want) printf("  ok  %s = %d\n", what, got);
    else {
        printf("  got %d, wanted %d\n", got, want);
        fail(what);
    }
}

#define FS_BLOCKS 2048
#define FS_INODES 128

#define MAXENTS 128
char g_names[MAXENTS * 64];
long g_kinds[MAXENTS];          // 1 = directory, 0 = file
long g_nents;
long g_sel;
// The item selected by the PREVIOUS click, so that clicking an
// already-selected item opens it -- a double-click without a clock.
long g_sel_prev;

char g_cwd[256];                // always starts with '/', never ends with one
                                // unless it IS "/"
char g_status[96];
char g_namefield[64];

long g_winh;
struct Ui g_ui;

#define PANW 520
#define PANH 360

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

long str_len(char *s) {
    long i;
    i = 0;
    while (s[i]) i = i + 1;
    return i;
}

// Join the current directory and a name into a path.
//
// The whole reason this is a function: "/" + "a" is "/a" and "/docs" + "a" is
// "/docs/a", and writing that inline in four places is how three of them end
// up with a double slash. fs_lookup does not forgive one.
void path_join(char *out, char *dir, char *name, long cap) {
    long n;
    str_copy(out, dir, cap);
    n = str_len(out);
    if (n > 0 && out[n - 1] != '/') { out[n] = '/'; n = n + 1; out[n] = 0; }
    str_copy(out + n, name, cap - n);
}

// Drop the last component. "/a/b" becomes "/a"; "/a" becomes "/"; "/" stays.
void path_up(char *p) {
    long n;
    n = str_len(p);
    while (n > 1 && p[n - 1] != '/') n = n - 1;
    if (n > 1) n = n - 1;                  // drop the slash too, unless root
    if (n < 1) n = 1;
    p[n] = 0;
}

void refresh() {
    long dir;
    long i;
    long off;

    dir = fs_lookup(g_cwd);
    g_nents = 0;
    off = 0;
    if (dir <= 0) { set_status("cannot read that directory"); return; }

    i = 0;
    while (i < MAXENTS && g_nents < MAXENTS) {
        char nm[64];
        char full[320];
        long j;
        long ino;
        if (!fs_readdir(dir, i, nm)) { i = i + 1; continue; }
        // "." is never useful. ".." is, and it is the only way back, so it
        // stays -- but not at the root, where it points at itself and would
        // be a button that does nothing.
        if (!strcmp(nm, ".")) { i = i + 1; continue; }
        if (!strcmp(nm, "..") && !strcmp(g_cwd, "/")) { i = i + 1; continue; }

        path_join(full, g_cwd, nm, 320);
        ino = fs_lookup(full);
        g_kinds[g_nents] = (ino > 0 && fs_type(ino) == T_DIR) ? 1 : 0;
        if (!strcmp(nm, "..")) g_kinds[g_nents] = 1;

        j = 0;
        while (nm[j] && off < MAXENTS * 64 - 2) { g_names[off] = nm[j]; off = off + 1; j = j + 1; }
        g_names[off] = 0; off = off + 1;
        g_nents = g_nents + 1;
        i = i + 1;
    }
    if (g_sel >= g_nents) g_sel = g_nents - 1;
    if (g_sel < 0) g_sel = 0;
}

char *sel_name() {
    if (g_sel < 0 || g_sel >= g_nents) return "";
    return g_names + ui_list_name_at(g_names, g_sel);
}

void enter_selected() {
    char *nm;
    char full[320];
    nm = sel_name();
    if (!nm[0]) return;
    if (!strcmp(nm, "..")) {
        path_up(g_cwd);
        g_sel = 0;
        ui_forget_all();
        refresh();
        set_status("up");
        return;
    }
    path_join(full, g_cwd, nm, 320);
    if (g_kinds[g_sel]) {
        str_copy(g_cwd, full, 256);
        g_sel = 0;
        ui_forget_all();
        refresh();
        set_status("opened folder");
    } else {
        long ino;
        ino = fs_lookup(full);
        if (ino > 0) {
            // No viewer here -- that is the editor's job. Report what it is,
            // which is the honest thing a file manager can say on its own.
            printf("");
            set_status("file selected");
        }
    }
}

long do_new_folder(char *name) {
    char full[320];
    if (!name[0]) { set_status("name it first"); return 0; }
    path_join(full, g_cwd, name, 320);
    if (fs_lookup(full) > 0) { set_status("already exists"); return 0; }
    if (!fs_mkdir(full)) { set_status("mkdir failed"); return 0; }
    if (fs_sync() < 0) { set_status("made it, but not on a disk"); return 0; }
    refresh();
    set_status("folder created");
    return 1;
}

long do_delete() {
    char *nm;
    char full[320];
    nm = sel_name();
    if (!nm[0]) return 0;
    // Refusing to delete ".." matters: it is in the listing as a way back,
    // and unlinking it would take the parent's entry with it.
    if (!strcmp(nm, "..")) { set_status("cannot delete .."); return 0; }
    path_join(full, g_cwd, nm, 320);
    if (!fs_unlink(full)) { set_status("delete failed (folder not empty?)"); return 0; }
    if (fs_sync() < 0) { set_status("deleted, but not on a disk"); return 0; }
    refresh();
    set_status("deleted");
    return 1;
}

long do_rename(char *to) {
    char *nm;
    char from[320];
    char dest[320];
    nm = sel_name();
    if (!nm[0]) return 0;
    if (!to[0]) { set_status("type the new name first"); return 0; }
    if (!strcmp(nm, "..")) { set_status("cannot rename .."); return 0; }
    path_join(from, g_cwd, nm, 320);
    path_join(dest, g_cwd, to, 320);
    if (fs_lookup(dest) > 0) { set_status("that name is taken"); return 0; }
    if (!fs_rename(from, dest)) { set_status("rename failed"); return 0; }
    if (fs_sync() < 0) { set_status("renamed, but not on a disk"); return 0; }
    refresh();
    set_status("renamed");
    return 1;
}

// ---------- one frame ----------

char *g_fm_tops[2];
char *g_fm_items[5];
long  g_fm_owner[5];

#define F_NEWDIR 0
#define F_RENAME 1
#define F_DELETE 2
#define F_UP     3
#define F_ABOUT  4

void menus_init() {
    g_fm_tops[0] = "File";
    g_fm_tops[1] = "Help";
    g_fm_items[F_NEWDIR] = "New Folder"; g_fm_owner[F_NEWDIR] = 0;
    g_fm_items[F_RENAME] = "Rename";     g_fm_owner[F_RENAME] = 0;
    g_fm_items[F_DELETE] = "Delete";     g_fm_owner[F_DELETE] = 0;
    g_fm_items[F_UP]     = "Up";         g_fm_owner[F_UP]     = 0;
    g_fm_items[F_ABOUT]  = "About";      g_fm_owner[F_ABOUT]  = 1;
}

void do_menu(long item) {
    if (item < 0) return;
    if (item == F_NEWDIR) do_new_folder(g_namefield);
    else if (item == F_RENAME) do_rename(g_namefield);
    else if (item == F_DELETE) do_delete();
    else if (item == F_UP) {
        if (strcmp(g_cwd, "/")) {
            path_up(g_cwd);
            g_sel = 0;
            ui_forget_all();
            refresh();
            set_status("up");
        }
    }
    else if (item == F_ABOUT) set_status("nano-os files");
}

void frame_ui() {
    long chosen;
    long hit;

    ui_begin(&g_ui, g_winh, WM_BORDER + 6, WM_TITLE_H + 6, PANW);

    chosen = ui_menubar(&g_ui, g_fm_tops, 2, g_fm_items, g_fm_owner, 5);
    do_menu(chosen);

    ui_row(&g_ui, 1);
    ui_label(&g_ui, g_cwd);

    ui_row(&g_ui, 1);
    hit = ui_icongrid(&g_ui, g_names, g_kinds, g_nents, &g_sel, 210);
    // A click SELECTS; a click on the already-selected item opens it. There
    // is no double-click timer in this machine, and putting a clock inside a
    // widget to invent one is worse than this rule.
    if (hit >= 0 && hit == g_sel_prev) enter_selected();
    if (hit >= 0) g_sel_prev = hit;

    ui_row(&g_ui, 2);
    ui_label(&g_ui, "name:");
    ui_text(&g_ui, g_namefield, 64);

    ui_row(&g_ui, 4);
    if (ui_button(&g_ui, "Open")) enter_selected();
    if (ui_button(&g_ui, "New Folder")) do_new_folder(g_namefield);
    if (ui_button(&g_ui, "Rename")) do_rename(g_namefield);
    if (ui_button(&g_ui, "Delete")) do_delete();

    ui_row(&g_ui, 1);
    ui_label(&g_ui, g_status);

    ui_end(&g_ui);
}

// ============================================================
// the tests
// ============================================================

void test_paths() {
    char p[256];

    puts("\n-- 1. paths, because a double slash is not forgiven --\n");

    path_join(p, "/", "a.txt", 256);
    expect_true("root + name has ONE slash", !strcmp(p, "/a.txt"));
    path_join(p, "/docs", "a.txt", 256);
    expect_true("a directory + name", !strcmp(p, "/docs/a.txt"));
    path_join(p, "/docs/", "a.txt", 256);
    expect_true("...and a trailing slash does not double up",
                !strcmp(p, "/docs/a.txt"));

    str_copy(p, "/a/b", 256); path_up(p);
    expect_true("up from /a/b is /a", !strcmp(p, "/a"));
    str_copy(p, "/a", 256); path_up(p);
    expect_true("up from /a is /", !strcmp(p, "/"));
    str_copy(p, "/", 256); path_up(p);
    expect_true("up from / stays /", !strcmp(p, "/"));
}

void test_listing() {
    long i;
    long dirs;
    long dotdot;

    puts("\n-- 2. the listing knows folders from files --\n");

    fs_mkdir("/pics");
    fs_create("/readme.txt");
    fs_sync();

    str_copy(g_cwd, "/", 256);
    refresh();
    expect_true("root has entries", g_nents >= 2);

    dirs = 0; dotdot = 0;
    i = 0;
    while (i < g_nents) {
        char *nm;
        nm = g_names + ui_list_name_at(g_names, i);
        if (!strcmp(nm, "pics")) {
            expect_true("pics is marked a FOLDER", g_kinds[i] == 1);
            dirs = dirs + 1;
        }
        if (!strcmp(nm, "readme.txt")) {
            expect_true("readme.txt is marked a FILE", g_kinds[i] == 0);
        }
        if (!strcmp(nm, ".")) fail(". should not be listed");
        if (!strcmp(nm, "..")) dotdot = dotdot + 1;
        i = i + 1;
    }
    expect("found the folder", dirs, 1);
    expect("...and .. is NOT offered at the root, where it goes nowhere",
           dotdot, 0);
}

void test_navigate() {
    long i;
    long found;

    puts("\n-- 3. going in and coming back --\n");

    str_copy(g_cwd, "/", 256);
    refresh();
    // Select "pics" and enter it.
    i = 0; found = 0 - 1;
    while (i < g_nents) {
        if (!strcmp(g_names + ui_list_name_at(g_names, i), "pics")) found = i;
        i = i + 1;
    }
    expect_true("pics is in the listing", found >= 0);
    g_sel = found;
    enter_selected();
    expect_true("we are in /pics", !strcmp(g_cwd, "/pics"));

    refresh();
    found = 0 - 1;
    i = 0;
    while (i < g_nents) {
        if (!strcmp(g_names + ui_list_name_at(g_names, i), "..")) found = i;
        i = i + 1;
    }
    expect_true("...and .. IS offered here, because it goes somewhere",
                found >= 0);
    g_sel = found;
    enter_selected();
    expect_true("...and it takes us back to /", !strcmp(g_cwd, "/"));
}

void test_operations() {
    long ino;

    puts("\n-- 4. new folder, rename, delete --\n");

    str_copy(g_cwd, "/", 256);
    refresh();

    expect_true("made a folder", do_new_folder("newdir") == 1);
    expect_true("...and it is on the filesystem", fs_lookup("/newdir") > 0);
    expect_true("...typed as a directory", fs_type(fs_lookup("/newdir")) == T_DIR);
    expect_true("making it twice is refused", do_new_folder("newdir") == 0);

    // Rename it, then check BOTH that the new name exists and the old is gone
    // -- a rename that copies would pass the first check alone.
    {
        long i;
        long at;
        refresh();
        at = 0 - 1;
        i = 0;
        while (i < g_nents) {
            if (!strcmp(g_names + ui_list_name_at(g_names, i), "newdir")) at = i;
            i = i + 1;
        }
        g_sel = at;
        expect_true("renamed it", do_rename("renamed") == 1);
        expect_true("...the new name is there", fs_lookup("/renamed") > 0);
        expect("...and the OLD name is gone", fs_lookup("/newdir"), 0);
    }

    // Delete it.
    {
        long i;
        long at;
        refresh();
        at = 0 - 1;
        i = 0;
        while (i < g_nents) {
            if (!strcmp(g_names + ui_list_name_at(g_names, i), "renamed")) at = i;
            i = i + 1;
        }
        g_sel = at;
        expect_true("deleted it", do_delete() == 1);
        expect("...and it is gone from the filesystem", fs_lookup("/renamed"), 0);
    }

    // The guards. ".." must not be deletable or renameable -- unlinking it
    // would take the parent's entry with it.
    str_copy(g_cwd, "/pics", 256);
    refresh();
    {
        long i;
        long at;
        at = 0 - 1;
        i = 0;
        while (i < g_nents) {
            if (!strcmp(g_names + ui_list_name_at(g_names, i), "..")) at = i;
            i = i + 1;
        }
        g_sel = at;
        expect_true("deleting .. is refused", do_delete() == 0);
        expect_true("renaming .. is refused", do_rename("nope") == 0);
        ino = fs_lookup("/pics");
        expect_true("...and /pics is still there afterwards", ino > 0);
    }
    str_copy(g_cwd, "/", 256);
    refresh();
}

long g_was_second_boot;

void test_persist() {
    puts("\n-- 5. does a folder made here outlive the machine --\n");

    if (fs_lookup("/made-by-files") <= 0) {
        puts("  FIRST BOOT: making it\n");
        str_copy(g_cwd, "/", 256);
        expect_true("made it", do_new_folder("made-by-files") == 1);
        puts("  now reboot against the same drive\n");
    } else {
        g_was_second_boot = 1;
        puts("  A LATER BOOT: it is on the disk\n");
        expect_true("the folder survived", fs_lookup("/made-by-files") > 0);
        expect_true("...still typed as a directory",
                    fs_type(fs_lookup("/made-by-files")) == T_DIR);
        str_copy(g_cwd, "/made-by-files", 256);
        refresh();
        expect_true("...and can be entered", g_nents >= 1);
        str_copy(g_cwd, "/", 256);
        refresh();
    }
}

void run_tests() {
    test_paths();
    test_listing();
    test_navigate();
    test_operations();
    test_persist();

    if (g_fail) printf("\n%d CHECKS FAILED\n", g_fail);
    else if (!g_was_second_boot)
        puts("\nMADE: nothing proved about durability yet -- boot again\n");
    else puts("\nPASS: a file manager, on files that outlive the machine\n");
    puts("\nFILESTEST DONE\n");
}

void build_window() {
    wm_init(rgb(24, 28, 38));
    wmin_init();
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);
    g_winh = wm_create(50, 40, PANW + WM_BORDER * 2 + 12,
                       PANH + WM_TITLE_H + WM_BORDER + 12, "files");
    ui_init(&g_ui);
    ui_forget_all();
}

long g_prev_down;

void event_loop() {
    for (;;) {
        struct MEvent e;
        long down;
        long pressed;
        long released;
        long key;
        long touched;

        pressed = 0; released = 0; key = 0;
        touched = 0;
        down = g_prev_down;
        while (mouse_pop(&e)) {
            wm_input_mouse(e.x, e.y, e.btn);
            down = e.btn & 1;
            if (down && !g_prev_down) pressed = 1;
            if (!down && g_prev_down) released = 1;
            g_prev_down = down;
            touched = 1;
        }
        for (;;) {
            long k;
            k = kbd_getkey_nb();
            if (k == 0) break;
            if (k > 255) key = k;
            else if (!wm_input_key(k)) key = k;
            touched = 1;
        }

        if (!g_win[g_winh].used) { wm_present(); cpu_idle(); continue; }

        if (touched || key) {
            ui_input(&g_ui, down, pressed, released, key);
            frame_ui();
            wm_present();
        }
        cpu_idle();
    }
}

void main_thread(long unused) {
    puts("\nnano-os: a file manager\n");

    if (!ata_init()) {
        puts("FAIL: no ATA disk -- run qemu with -drive\n");
        g_fail = g_fail + 1;
        puts("\nFILESTEST DONE\n");
        cpu_halt_forever();
    }
    if (!fs_dev_init_ata(FS_BLOCKS)) {
        puts("FAIL: could not read the disk\n");
        g_fail = g_fail + 1;
        puts("\nFILESTEST DONE\n");
        cpu_halt_forever();
    }
    if (!fs_mount()) {
        puts("  no filesystem here, formatting\n");
        if (!fs_format_on_dev(FS_BLOCKS, FS_INODES)) fail("format failed");
    } else {
        puts("  mounted an existing filesystem\n");
    }

    menus_init();
    str_copy(g_cwd, "/", 256);
    g_namefield[0] = 0;
    set_status("ready");
    g_sel = 0;
    g_sel_prev = 0 - 1;

    run_tests();

    // The tests leave their last message in the status line, and the last one
    // they run is a deliberate failure ("cannot rename .."). Starting the
    // interface showing an error from a test that PASSED is a small lie.
    set_status("ready");
    g_sel = 0;
    g_sel_prev = 0 - 1;
    str_copy(g_cwd, "/", 256);
    refresh();
    fs_sync();

    build_window();
    frame_ui();
    wm_present();
    puts("files up; the machine is now interactive\n");
    event_loop();
}

int main() {
    serial_init();
    g_fail = 0;
    if (!fb_init(1024, 768)) { puts("fb_init failed\n"); for (;;) { } }
    if (!mm_init())          { puts("mm_init failed\n"); for (;;) { } }
    mm_protect_null();
    kbd_init();
    interrupts_init(100);
    thread_init();
    thread_create((long)main_thread, 0, "main");
    sched_start();
    return 0;
}
