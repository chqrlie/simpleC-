// edit.c — a text editor, saving to a disk that outlives the machine.
//
// Menus, tabs, open and save, on the widgets this milestone added to
// nano-ui.h: a multi-line editing area with a real caret, a menu bar, a list
// box and a tab strip. Below them, the keyboard learned the keys an editor
// needs -- arrows, Home, End, Delete, PgUp, PgDn -- which it could not
// produce at all before: the 0xE0 prefix those keys arrive behind was being
// ignored and the scancode behind it looked up in the ASCII table, which has
// no entry for any of them. Every arrow press was silently dropped.
//
// The acceptance test is the one the filesystem milestone made possible:
// type a file, save it, reboot the machine, open it again. Everything else in
// here is in service of that being true rather than nearly true.
//
// The tests run first and the editor is left running afterwards, the same
// shape as every other image in this tree.

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

// ---------- tabs ----------
//
// Four open files, each with its own buffer, caret and scroll position. A tab
// is not a view onto a shared document -- switching tabs must not move the
// other file's caret, which is the bug you get from one Edit and a swapped
// pointer.
#define MAXTABS 4

struct Edit g_ed[MAXTABS];
char g_edbuf[MAXTABS * ED_CAP];
// Tab titles, as the flat NUL-separated block ui_tabs and ui_list both want.
char g_tabnames[MAXTABS * 64];
// The path each tab was opened from or last saved to, "" if never saved.
char g_paths[MAXTABS * 64];
long g_ntabs;
long g_tab;

// ---------- the directory listing, for open and save ----------
#define MAXENTS 64
char g_names[MAXENTS * 64];
long g_nents;
long g_sel;

long g_winh;
struct Ui g_ui;

// Which modal is up. No modal is 0; a modal is not a separate window here,
// it replaces the panel contents, which is the cheapest thing that is still
// honestly modal -- the editor cannot be typed into while it is up.
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

// Read the root directory into the flat name block the list box takes.
void refresh_listing() {
    long root;
    long i;
    long off;
    root = fs_lookup("/");
    g_nents = 0;
    off = 0;
    if (root <= 0) return;
    i = 0;
    while (i < MAXENTS) {
        char nm[64];
        long j;
        if (!fs_readdir(root, i, nm)) { i = i + 1; continue; }
        // "." and ".." are real entries and belong in a directory listing,
        // but not in a file picker -- opening "." is not a thing this editor
        // can do and showing it invites the click.
        if (!strcmp(nm, ".") || !strcmp(nm, "..")) { i = i + 1; continue; }
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
            // Show the basename, not the path: a tab strip of "/docs/a.txt"
            // is mostly slashes.
            long last;
            last = 0;
            j = 0;
            while (p[j]) { if (p[j] == '/') last = j + 1; j = j + 1; }
            j = last;
            while (p[j]) { g_tabnames[off] = p[j]; off = off + 1; j = j + 1; }
        }
        // A star for unsaved changes, because a tab that looks identical
        // whether or not it has been saved is how work gets closed away.
        if (g_ed[i].dirty) { g_tabnames[off] = '*'; off = off + 1; }
        g_tabnames[off] = 0; off = off + 1;
        i = i + 1;
    }
}

long new_tab() {
    long inherit;
    if (g_ntabs >= MAXTABS) { set_status("no free tab"); return 0 - 1; }
    // Inherit the gutter setting from the tab in front, or turning it on once
    // is undone by every File > New.
    inherit = (g_ntabs > 0) ? g_ed[g_tab].nums : 0;
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
    // Shift the whole tab down, contents and all. Copying struct Edit by
    // assignment is fine -- nano_cc's struct copy was fixed in K15 and there
    // is a test for it -- but the BUFFER pointer inside would then point at
    // the wrong slot, so the buffers are re-pointed afterwards.
    k = i;
    while (k < g_ntabs - 1) {
        g_ed[k] = g_ed[k + 1];
        str_copy(g_paths + k * 64, g_paths + (k + 1) * 64, 64);
        k = k + 1;
    }
    g_ntabs = g_ntabs - 1;
    // Re-point every buffer at its own slot and move the bytes with it.
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
    long ino;
    long n;
    ino = fs_lookup(path);
    if (ino <= 0) { set_status("no such file"); return 0; }
    n = fs_size(ino);
    if (n > ED_CAP - 1) n = ED_CAP - 1;
    {
        long keep;
        keep = g_ed[t].nums;
        ed_init(&g_ed[t], g_edbuf + t * ED_CAP);
        g_ed[t].nums = keep;
    }
    if (n > 0) fs_read(ino, 0, g_ed[t].buf, n);
    g_ed[t].buf[n] = 0;
    g_ed[t].len = n;
    g_ed[t].dirty = 0;
    ed_layout(&g_ed[t]);
    str_copy(g_paths + t * 64, path, 64);
    rebuild_tabnames();
    return 1;
}

long save_tab(long t, char *path) {
    long ino;
    long n;
    ino = fs_lookup(path);
    if (ino <= 0) ino = fs_create(path);
    if (ino <= 0) { set_status("could not create"); return 0; }
    // TRUNCATE FIRST. Without it, saving a file shorter than it was leaves
    // the tail of the old contents on the end -- the file reads back correct
    // for its new length and wrong for its size, which is the sort of thing
    // that survives every test that checks a prefix.
    fs_truncate(ino);
    n = fs_write(ino, 0, g_ed[t].buf, g_ed[t].len);
    if (n != g_ed[t].len) { set_status("short write"); return 0; }
    // And the sync, or none of this outlives the machine, which is the whole
    // point of having done the disk milestone first.
    if (fs_sync() < 0) { set_status("saved to memory only -- no disk"); return 0; }
    g_ed[t].dirty = 0;
    str_copy(g_paths + t * 64, path, 64);
    rebuild_tabnames();
    set_status("saved");
    return 1;
}

// ---------- the menus ----------
//
// No function pointers, so this is a table and a switch, the same shape as
// the display-list opcodes.
char *g_menu_tops[3];
char *g_menu_items[10];
long  g_menu_owner[10];

#define M_NEW   0
#define M_OPEN  1
#define M_SAVE  2
#define M_SAVEAS 3
#define M_CLOSE 4
#define M_QUIT  5
#define M_HOME  6
#define M_END   7
#define M_ABOUT 8
#define M_NUMS  9

void menus_init() {
    g_menu_tops[0] = "File";
    g_menu_tops[1] = "Edit";
    g_menu_tops[2] = "Help";
    g_menu_items[M_NEW]    = "New";      g_menu_owner[M_NEW]    = 0;
    g_menu_items[M_OPEN]   = "Open...";  g_menu_owner[M_OPEN]   = 0;
    g_menu_items[M_SAVE]   = "Save";     g_menu_owner[M_SAVE]   = 0;
    g_menu_items[M_SAVEAS] = "Save As..."; g_menu_owner[M_SAVEAS] = 0;
    g_menu_items[M_CLOSE]  = "Close Tab"; g_menu_owner[M_CLOSE]  = 0;
    g_menu_items[M_QUIT]   = "Halt";     g_menu_owner[M_QUIT]   = 0;
    g_menu_items[M_HOME]   = "Top";      g_menu_owner[M_HOME]   = 1;
    g_menu_items[M_END]    = "Bottom";   g_menu_owner[M_END]    = 1;
    g_menu_items[M_ABOUT]  = "About";    g_menu_owner[M_ABOUT]  = 2;
    g_menu_items[M_NUMS]   = "Line Numbers"; g_menu_owner[M_NUMS] = 1;
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
    else if (item == M_QUIT) { fs_sync(); cpu_halt_forever(); }
    else if (item == M_HOME) { g_ed[g_tab].caret = 0; ed_scroll_to_caret(&g_ed[g_tab]); }
    else if (item == M_END) {
        g_ed[g_tab].caret = g_ed[g_tab].len;
        ed_scroll_to_caret(&g_ed[g_tab]);
    }
    else if (item == M_ABOUT) set_status("nano-os editor");
    else if (item == M_NUMS) {
        // Per TAB, not global: one file open at line 4000 wants numbers and
        // the scratch buffer beside it does not.
        g_ed[g_tab].nums = !g_ed[g_tab].nums;
        set_status(g_ed[g_tab].nums ? "line numbers on" : "line numbers off");
    }
}

// ---------- one frame of interface ----------

#define PANW 560
#define PANH 380

void frame_ui() {
    long chosen;
    long t;

    ui_begin(&g_ui, g_winh, WM_BORDER + 6, WM_TITLE_H + 6, PANW);

    chosen = ui_menubar(&g_ui, g_menu_tops, 3, g_menu_items, g_menu_owner, 10);
    do_menu(chosen);

    if (g_modal == MODAL_NONE) {
        rebuild_tabnames();
        t = ui_tabs(&g_ui, g_tabnames, g_ntabs, g_tab);
        if (t >= 0) g_tab = t;
        else if (t <= UI_TAB_CLOSE(0)) close_tab(0 - 2 - t);

        ui_row(&g_ui, 1);
        ui_edit(&g_ui, &g_ed[g_tab], 260);

        ui_row(&g_ui, 2);
        ui_label(&g_ui, g_paths[g_tab * 64] ? g_paths + g_tab * 64 : "untitled");
        ui_label(&g_ui, g_status);
    } else if (g_modal == MODAL_OPEN) {
        ui_row(&g_ui, 1);
        ui_label(&g_ui, "Open which file?");
        ui_row(&g_ui, 1);
        {
            long act;
            act = ui_list(&g_ui, g_names, g_nents, &g_sel, 220);
            if (act >= 0) {
                char path[80];
                long off;
                path[0] = '/';
                off = ui_list_name_at(g_names, act);
                str_copy(path + 1, g_names + off, 78);
                if (g_ed[g_tab].dirty || g_paths[g_tab * 64]) {
                    if (new_tab() < 0) { /* stay on this tab */ }
                }
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
        ui_list(&g_ui, g_names, g_nents, &g_sel, 160);
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

// ============================================================
// the tests
// ============================================================

// 1. the buffer model, with no interface anywhere near it
char g_tbuf[ED_CAP];
struct Edit g_t;

void type_str(struct Edit *e, char *s) {
    long i;
    i = 0;
    while (s[i]) { ed_key(e, s[i]); i = i + 1; }
}

void test_buffer() {
    puts("\n-- 1. the buffer, before any of it is drawn --\n");

    ed_init(&g_t, g_tbuf);
    type_str(&g_t, "hello\nworld\n");
    expect("three lines, the last one empty", g_t.nlines, 3);
    expect("the caret is at the end", g_t.caret, 12);
    expect_true("and it is dirty", g_t.dirty == 1);

    // Arrows. These are the keys that did not exist an hour ago.
    ed_key(&g_t, KEY_UP);
    expect("up from the empty last line lands on line 1", ed_line_of(&g_t, g_t.caret), 1);
    ed_key(&g_t, KEY_HOME);
    expect("home goes to column 0", ed_col_of(&g_t, g_t.caret), 0);
    ed_key(&g_t, KEY_END);
    expect("end goes to the end of THAT line", ed_col_of(&g_t, g_t.caret), 5);

    // The column-memory rule: moving up a ragged file clamps rather than
    // wrapping, and does not move to column 0.
    ed_init(&g_t, g_tbuf);
    type_str(&g_t, "long line here\nab\nlong again");
    g_t.caret = g_t.len;                       // end of "long again"
    ed_key(&g_t, KEY_UP);
    expect("up onto a SHORT line clamps to its length", ed_col_of(&g_t, g_t.caret), 2);
    ed_key(&g_t, KEY_UP);
    expect("...and up again stays clamped, not reset to 0",
           ed_col_of(&g_t, g_t.caret), 2);

    // Backspace and Delete are the same operation at different offsets.
    ed_init(&g_t, g_tbuf);
    type_str(&g_t, "abcd");
    ed_key(&g_t, KEY_LEFT);
    ed_key(&g_t, '\b');
    expect_true("backspace removes the byte BEFORE the caret",
                !strcmp(g_t.buf, "abd"));
    expect("...and the caret follows it", g_t.caret, 2);
    ed_key(&g_t, KEY_DEL);
    expect_true("delete removes the byte AT the caret", !strcmp(g_t.buf, "ab"));
    expect("...and the caret does NOT move", g_t.caret, 2);

    // Inserting a newline in the middle splits the line, which is the edit
    // most likely to get the line table wrong.
    ed_init(&g_t, g_tbuf);
    type_str(&g_t, "abcdef");
    g_t.caret = 3;
    ed_key(&g_t, '\n');
    expect("splitting gives two lines", g_t.nlines, 2);
    expect_true("...with the right bytes", !strcmp(g_t.buf, "abc\ndef"));
    expect("...and the caret at the start of the second", ed_col_of(&g_t, g_t.caret), 0);

    // Scrolling. rows is normally set by the widget; set it here so the
    // model can be tested without drawing anything.
    ed_init(&g_t, g_tbuf);
    {
        long i;
        i = 0;
        while (i < 40) { type_str(&g_t, "x\n"); i = i + 1; }
    }
    g_t.rows = 10;
    g_t.caret = 0; ed_scroll_to_caret(&g_t);
    expect("caret at the top scrolls to the top", g_t.top, 0);
    g_t.caret = g_t.len; ed_scroll_to_caret(&g_t);
    expect_true("caret at the end scrolls the last line into view",
                g_t.top + g_t.rows > ed_line_of(&g_t, g_t.caret));
}

// 2. save, and read it back through the filesystem
void test_save_load() {
    long ino;
    long n;

    puts("\n-- 2. it reaches the disk --\n");

    g_ntabs = 0;
    new_tab();
    type_str(&g_ed[0], "saved from the editor\nsecond line\n");
    expect_true("the tab is dirty before saving", g_ed[0].dirty == 1);
    expect_true("saving works", save_tab(0, "/edit1.txt") == 1);
    expect_true("...and clears dirty", g_ed[0].dirty == 0);

    ino = fs_lookup("/edit1.txt");
    expect_true("the file exists in the filesystem", ino > 0);
    expect("...at the length the buffer had", fs_size(ino), g_ed[0].len);

    // Load it into a second tab and compare, rather than trusting the first.
    new_tab();
    expect_true("loading it back works", load_into_tab(1, "/edit1.txt") == 1);
    expect("...to the same length", g_ed[1].len, g_ed[0].len);
    expect_true("...byte for byte", !strcmp(g_ed[1].buf, g_ed[0].buf));
    expect_true("...and a freshly loaded file is not dirty", g_ed[1].dirty == 0);

    // The truncate case. Save something SHORTER over it; without the
    // truncate in save_tab the tail of the old file survives and the file is
    // longer than what is on screen.
    ed_init(&g_ed[1], g_edbuf + ED_CAP);
    type_str(&g_ed[1], "short");
    expect_true("saving a shorter file over a longer one",
                save_tab(1, "/edit1.txt") == 1);
    ino = fs_lookup("/edit1.txt");
    expect("...leaves the file SHORT, with no tail of the old one",
           fs_size(ino), 5);
    n = fs_read(ino, 0, g_tbuf, 64);
    g_tbuf[n] = 0;
    expect_true("...and the right bytes", !strcmp(g_tbuf, "short"));
}

// 3. tabs keep their own place
void test_tabs() {
    puts("\n-- 3. tabs are separate documents --\n");

    g_ntabs = 0;
    new_tab();
    type_str(&g_ed[0], "first file");
    new_tab();
    type_str(&g_ed[1], "second file, longer\nwith two lines");
    g_ed[1].caret = 5;

    expect("two tabs", g_ntabs, 2);
    expect_true("tab 0 kept its own text", !strcmp(g_ed[0].buf, "first file"));
    expect("tab 0 kept its own caret", g_ed[0].caret, 10);
    expect("tab 1 has a different caret", g_ed[1].caret, 5);
    expect("...and its own line count", g_ed[1].nlines, 2);

    // Closing the FIRST tab has to move the second one down, contents and
    // all. Getting this wrong leaves tab 0's buffer pointer aimed at the
    // slot that was just freed, which reads as the text vanishing.
    close_tab(0);
    expect("one tab left", g_ntabs, 1);
    expect_true("...and it is the one that survived, with its text",
                !strcmp(g_ed[0].buf, "second file, longer\nwith two lines"));
    expect("...and its line table came with it", g_ed[0].nlines, 2);
}

// 4. the directory listing the open dialog shows
void test_listing() {
    long i;
    long found;

    puts("\n-- 4. the open dialog can see the disk --\n");

    fs_create("/alpha.txt");
    fs_create("/beta.txt");
    refresh_listing();
    expect_true("the listing found some files", g_nents >= 3);

    found = 0;
    i = 0;
    while (i < g_nents) {
        long off;
        off = ui_list_name_at(g_names, i);
        if (!strcmp(g_names + off, "alpha.txt")) found = found + 1;
        if (!strcmp(g_names + off, ".") || !strcmp(g_names + off, "..")) found = found + 100;
        i = i + 1;
    }
    expect("alpha.txt is in it", found, 1);
    expect_true("...and . and .. are NOT, because they are not openable",
                found < 100);
}


// 5. THE ACCEPTANCE TEST: a file typed here outlives the machine
//
// Same shape as fsdisk.c and for the same reason -- one image, run twice
// against one drive, and the second run is the only one that proves anything.
// This goes through save_tab and load_into_tab, the paths the menu items use,
// rather than through fs_write directly: the point is that THE EDITOR'S save
// is durable, not that the filesystem underneath it is.
//
// Driving the File menu by pointer coordinates would have tested the same
// path plus the pixel positions of a dropdown, and failed whenever a label
// changed width. The widgets are shown working by screenshot; this checks the
// thing that has to be true.
#define KEEP_PATH "/keep.txt"
#define KEEP_TEXT "typed in the editor, before the machine was switched off\nsecond line\n"

long g_was_second_boot;

void test_persist() {
    long ino;

    puts("\n-- 5. does a saved file outlive the machine --\n");

    ino = fs_lookup(KEEP_PATH);
    if (ino <= 0) {
        long t;
        puts("  FIRST BOOT: no keep.txt, typing and saving one\n");
        g_ntabs = 0;
        t = new_tab();
        type_str(&g_ed[t], KEEP_TEXT);
        expect_true("typed it", g_ed[t].len == (long)strlen(KEEP_TEXT));
        expect_true("saved it through the editor's own save",
                    save_tab(t, KEEP_PATH) == 1);
        puts("  now reboot against the same drive; it must load it back\n");
    } else {
        long t;
        g_was_second_boot = 1;
        puts("  A LATER BOOT: keep.txt is on the disk\n");
        g_ntabs = 0;
        t = new_tab();
        expect_true("the editor loads it back", load_into_tab(t, KEEP_PATH) == 1);
        expect("...at the length it was typed at", g_ed[t].len, (long)strlen(KEEP_TEXT));
        expect_true("...byte for byte", !strcmp(g_ed[t].buf, KEEP_TEXT));
        expect("...and the line table came back with it", g_ed[t].nlines, 3);
        expect_true("...and it is not marked dirty", g_ed[t].dirty == 0);
        // The caret must be somewhere valid in a file it has never seen typed.
        expect_true("...and the caret is inside the buffer",
                    g_ed[t].caret >= 0 && g_ed[t].caret <= g_ed[t].len);
    }
}

void run_tests() {
    test_buffer();
    test_save_load();
    test_tabs();
    test_listing();
    test_persist();

    // A first boot has not demonstrated persistence and must not claim it --
    // the same correction fsdisk.c needed after its sabotage produced a green
    // run for exactly the failure it exists to catch.
    if (g_fail) printf("\n%d CHECKS FAILED\n", g_fail);
    else if (!g_was_second_boot)
        puts("\nSAVED: nothing proved yet -- boot again on the same drive\n");
    else puts("\nPASS: a file typed in the editor outlived the machine\n");
    puts("\nEDITTEST DONE\n");
}

void build_window() {
    wm_init(rgb(24, 28, 38));
    wmin_init();
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);
    g_winh = wm_create(40, 30, PANW + WM_BORDER * 2 + 12,
                      PANH + WM_TITLE_H + WM_BORDER + 12, "editor");
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
        // kbd_getkey_nb, not kbd_getchar_nb: the char version deliberately
        // drops anything above 255, which is every key this editor added.
        // Reading the editor's arrows through the character API would have
        // them silently vanish -- the same shape as the bug in the IRQ
        // handler, one layer up.
        for (;;) {
            long k;
            k = kbd_getkey_nb();
            if (k == 0) break;
            // wm_input_key wants a character for its own shortcuts; a
            // non-character key is never one of those and goes straight on.
            if (k > 255) key = k;
            else if (!wm_input_key(k)) key = k;
            touched = 1;
        }

        if (!g_win[g_winh].used) { wm_present(); cpu_idle(); continue; }

        // Only when something happened. An editor with nothing being typed
        // into it is the idle case this toolkit is built around, and a caret
        // that does not blink is a fair price for a machine that is quiet.
        if (touched || key) {
            ui_input(&g_ui, down, pressed, released, key);
            frame_ui();
            wm_present();
        }
        cpu_idle();
    }
}

void main_thread(long unused) {
    puts("\nnano-os: a text editor\n");

    if (!ata_init()) {
        puts("FAIL: no ATA disk -- run qemu with -drive\n");
        g_fail = g_fail + 1;
        puts("\nEDITTEST DONE\n");
        cpu_halt_forever();
    }
    printf("  ata: %d sectors\n", ata_sectors);

    if (!fs_dev_init_ata(FS_BLOCKS)) {
        puts("FAIL: could not read the disk\n");
        g_fail = g_fail + 1;
        puts("\nEDITTEST DONE\n");
        cpu_halt_forever();
    }
    if (!fs_mount()) {
        puts("  no filesystem on this disk, formatting\n");
        if (!fs_format_on_dev(FS_BLOCKS, FS_INODES)) fail("format failed");
    } else {
        puts("  mounted an existing filesystem\n");
    }

    menus_init();
    set_status("ready");
    g_ntabs = 0;
    new_tab();

    run_tests();

    // Leave the tests' files behind and start clean, so what is on screen is
    // an empty editor rather than the last assertion's leftovers.
    g_ntabs = 0;
    new_tab();
    // On by default, and set HERE rather than before run_tests -- the tests
    // reset the tab table, so anything set earlier is wiped before the window
    // ever opens. Found by screenshotting and seeing no gutter.
    g_ed[0].nums = 1;
    set_status("ready");
    fs_sync();

    build_window();
    frame_ui();
    wm_present();
    puts("editor up; the machine is now interactive\n");
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
