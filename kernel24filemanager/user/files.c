// files.c — the file manager, as a process.
//
// Same relationship to kernel/files.c as user/edit.c has to kernel/edit.c:
// the kernel-side one keeps the test suite and runs it against the
// filesystem directly; this is the program.
//
// nano-ui.h is shared unmodified, so the icon grid, the scrollbars and the
// list box are the same code the kernel images compile. Everything that
// touches the machine goes through a syscall: fs_lookup/fs_readdir/fs_mkdir/
// fs_rename/fs_unlink/fs_sync become open/readdir_/mkdir_/rename_/unlink/
// fsync_, three of which did not exist before this milestone.
//
// It can also LAUNCH the editor, which is the thing a file manager is for and
// the thing that only works because both are processes.

#include "nano-uiapp.h"
#include "nano-ui.h"

#define W 520
#define H 380

long g_pix[W * H];
struct Ui g_ui;
long g_hnd;

#define MAXENTS 96
char g_names[MAXENTS * 64];
long g_kinds[MAXENTS];
long g_nents;
long g_sel;
long g_sel_prev;

char g_cwd[256];
char g_status[96];
char g_namefield[64];
long g_quit;

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

long str_len(char *s) { long i; i = 0; while (s[i]) i = i + 1; return i; }

// "/" + "a" is "/a"; "/docs" + "a" is "/docs/a". Written once because three
// of the four places that need it would otherwise get the double slash wrong.
void path_join(char *out, char *dir, char *name, long cap) {
    long n;
    str_copy(out, dir, cap);
    n = str_len(out);
    if (n > 0 && out[n - 1] != '/') { out[n] = '/'; n = n + 1; out[n] = 0; }
    str_copy(out + n, name, cap - n);
}

void path_up(char *p) {
    long n;
    n = str_len(p);
    while (n > 1 && p[n - 1] != '/') n = n - 1;
    if (n > 1) n = n - 1;
    if (n < 1) n = 1;
    p[n] = 0;
}

// Is this path a directory? Asked directly rather than inferred: reading
// index 0 of a FILE would parse the file's own bytes as directory entries and
// answer from whatever it contained.
long is_dir(char *path) { return isdir_(path) == 1; }

void refresh() {
    long i;
    long off;
    g_nents = 0;
    off = 0;
    i = 0;
    while (i < MAXENTS && g_nents < MAXENTS) {
        char nm[64];
        char full[320];
        long j;
        if (readdir_(g_cwd, i, nm) <= 0) { i = i + 1; continue; }
        if (!ustrcmp(nm, ".")) { i = i + 1; continue; }
        if (!ustrcmp(nm, "..") && !ustrcmp(g_cwd, "/")) { i = i + 1; continue; }

        path_join(full, g_cwd, nm, 320);
        g_kinds[g_nents] = is_dir(full);
        if (!ustrcmp(nm, "..")) g_kinds[g_nents] = 1;

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
    if (!ustrcmp(nm, "..")) {
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
        set_status("file selected -- Edit opens it");
    }
}

long do_new_folder(char *name) {
    char full[320];
    if (!name[0]) { set_status("name it first"); return 0; }
    path_join(full, g_cwd, name, 320);
    if (mkdir_(full) < 0) { set_status("mkdir failed (exists?)"); return 0; }
    if (fsync_() < 0) { set_status("made it, but not on a disk"); return 0; }
    refresh();
    set_status("folder created");
    return 1;
}

long do_delete() {
    char *nm;
    char full[320];
    nm = sel_name();
    if (!nm[0]) return 0;
    // Unlinking ".." would take the parent's entry with it.
    if (!ustrcmp(nm, "..")) { set_status("cannot delete .."); return 0; }
    path_join(full, g_cwd, nm, 320);
    if (unlink(full) <= 0) { set_status("delete failed (folder not empty?)"); return 0; }
    if (fsync_() < 0) { set_status("deleted, but not on a disk"); return 0; }
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
    if (!ustrcmp(nm, "..")) { set_status("cannot rename .."); return 0; }
    path_join(from, g_cwd, nm, 320);
    path_join(dest, g_cwd, to, 320);
    if (rename_(from, dest) < 0) { set_status("rename failed"); return 0; }
    if (fsync_() < 0) { set_status("renamed, but not on a disk"); return 0; }
    refresh();
    set_status("renamed");
    return 1;
}

char *g_fm_tops[2];
char *g_fm_items[6];
long  g_fm_owner[6];

#define F_NEWDIR 0
#define F_RENAME 1
#define F_DELETE 2
#define F_UP     3
#define F_QUIT   4
#define F_ABOUT  5

void menus_init() {
    g_fm_tops[0] = "File";
    g_fm_tops[1] = "Help";
    g_fm_items[F_NEWDIR] = "New Folder"; g_fm_owner[F_NEWDIR] = 0;
    g_fm_items[F_RENAME] = "Rename";     g_fm_owner[F_RENAME] = 0;
    g_fm_items[F_DELETE] = "Delete";     g_fm_owner[F_DELETE] = 0;
    g_fm_items[F_UP]     = "Up";         g_fm_owner[F_UP]     = 0;
    g_fm_items[F_QUIT]   = "Quit";       g_fm_owner[F_QUIT]   = 0;
    g_fm_items[F_ABOUT]  = "About";      g_fm_owner[F_ABOUT]  = 1;
}

void do_menu(long item) {
    if (item < 0) return;
    if (item == F_NEWDIR) do_new_folder(g_namefield);
    else if (item == F_RENAME) do_rename(g_namefield);
    else if (item == F_DELETE) do_delete();
    else if (item == F_UP) {
        if (ustrcmp(g_cwd, "/")) {
            path_up(g_cwd);
            g_sel = 0;
            ui_forget_all();
            refresh();
            set_status("up");
        }
    }
    else if (item == F_QUIT) { fsync_(); g_quit = 1; }
    else if (item == F_ABOUT) set_status("nano-os files, as a process");
}

void frame() {
    long chosen;
    long hit;

    ui_begin(&g_ui, 0, 6, 6, W - 12);

    chosen = ui_menubar(&g_ui, g_fm_tops, 2, g_fm_items, g_fm_owner, 6);
    do_menu(chosen);

    ui_row(&g_ui, 1);
    ui_label(&g_ui, g_cwd);

    ui_row(&g_ui, 1);
    hit = ui_icongrid(&g_ui, g_names, g_kinds, g_nents, &g_sel, 215);
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

int main(int argc, char **argv) {
    long poll[6];
    long prev_down;
    long i;

    i = 0;
    while (i < W * H) { g_pix[i] = rgb(236, 238, 242); i = i + 1; }

    g_hnd = win_open(70, 60, W + 8, H + 28, "files");
    if (g_hnd < 0) { write(1, "files: no window\n", 17); return 1; }

    sfc_bind(g_pix, W, H);
    ui_init(&g_ui);
    ui_forget_all();
    menus_init();
    str_copy(g_cwd, "/", 256);
    g_namefield[0] = 0;
    g_sel = 0;
    g_sel_prev = 0 - 1;
    set_status("ready");
    refresh();

    sfc_input(0, 0, W, H);
    ui_input(&g_ui, 0, 0, 0, 0);
    frame();
    win_blit(g_hnd, g_pix, W, H, 0);
    win_present(g_hnd);
    write(1, "files: up\n", 10);

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
    write(1, "files: exited cleanly\n", 22);
    return 0;
}
