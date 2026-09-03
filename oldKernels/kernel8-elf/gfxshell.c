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
#include "nano-mm.h"
#include "nano-thread.h"
#include "nano-fs.h"
#include "nano-proc.h"
#include "nano-int.h"
#include "nano-acpi.h"
#include "nano-srv.h"

// The order above is not alphabetical and not arbitrary. nano-int.h holds the
// one interrupt dispatcher, and it routes a vector to the scheduler, and now
// also to the syscall table, by testing for their include guards -- there are
// no function pointers to register a handler with. Anything the dispatcher
// calls has to be defined before it. nano-fs.h moved up for the same reason:
// nano-proc.h loads programs off the filesystem, so it has to come after it.

// The user programs, embedded in this image by progs.s. Accessor functions
// rather than `extern char blob[]`, because nano_cc has no way to declare an
// array defined in another object and take its address.
extern long prog_hello_addr();
extern long prog_hello_size();
extern long prog_twin_addr();
extern long prog_twin_size();
extern long prog_wild_addr();
extern long prog_wild_size();

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

void print_sig(long table) {
    long i;
    i = 0;
    while (i < 4) { putc(mem8(table + i)); i = i + 1; }
}

void cmd_acpi() {
    long i;
    if (!acpi_root) { puts("no ACPI tables found\n"); return; }
    printf("RSDP 0x%x rev %d\n", acpi_rsdp, acpi_rev);
    printf("%s at 0x%x, %d tables\n",
           acpi_root_is_xsdt ? "XSDT" : "RSDT", acpi_root, acpi_ntables);
    puts("tables:");
    i = 0;
    while (i < acpi_ntables) {
        long t;
        t = acpi_table_at(i);
        if (t) { putc(' '); print_sig(t); }
        i = i + 1;
    }
    putc('\n');
    printf("CPUs %d, PM timer 0x%x\n", acpi_cpus, acpi_pm_tmr);
    printf("C2 latency %d us, C3 %d us\n", acpi_c2_lat, acpi_c3_lat);
    printf("idle state: %s\n", acpi_cstate_name());
}

// Idle over one second and report what fraction of it the core was stopped.
// The ACPI PM timer keeps running while the core is halted, which is exactly
// why it can measure this and a CPU-driven clock cannot.
void cmd_idle() {
    long t0;
    long n;
    long a;
    long b;
    long elapsed;
    puts("measuring one second of idle...\n");
    g_c1_entries = 0;
    g_c2_entries = 0;
    a = pm_timer_read();
    t0 = g_ticks;
    n = 0;
    while (g_ticks < t0 + g_hz) { acpi_idle(); n = n + 1; }
    b = pm_timer_read();
    elapsed = pm_timer_delta(a, b);
    printf("%d wake-ups in %d ticks\n", n, g_ticks - t0);
    printf("C1 %d  C2 %d\n", g_c1_entries, g_c2_entries);
    if (acpi_pm_tmr) printf("PM timer advanced %d\n", elapsed);
    puts("a spinning core would show millions of wake-ups\n");
}

void cmd_uptime() {
    long secs;
    secs = g_ticks / g_hz;
    printf("up %d ticks at %d Hz = %d seconds\n", g_ticks, g_hz, secs);
    printf("keys dropped %d, spurious IRQs %d\n", g_kbd_dropped, g_spurious);
}

// A deliberate fault, so the exception reporter can be seen working rather
// than only trusted. Before the IDT existed this rebooted the machine with no
// message at all.
void cmd_fault() {
    long *bad;
    puts("touching unmapped memory on purpose...\n");
    bad = (long *)0x00007FFFFFFFF000;
    *bad = 1;
}

void cmd_mem() {
    printf("RAM %d KiB usable, top 0x%x\n", mm_ram_total / 1024, mm_ram_top);
    printf("frames %d free / %d total (%d KiB free)\n",
           mm_free_frames, mm_bitmap_frames, mm_free_frames * 4);
    printf("kernel ends 0x%x, bitmap 0x%x\n", kernel_end_addr(), mm_bitmap);
    printf("heap %d pages at 0x%x\n", heap_pages, HEAP_BASE);
    printf("  %d bytes free in %d blocks, %d in use\n",
           heap_bytes_free(), heap_blocks(1), heap_blocks(0));
    printf("  %d kmalloc / %d kfree\n", kmalloc_calls, kfree_calls);
}

// Allocate, write, verify and free, so the heap is exercised rather than only
// described. The addresses are printed because a heap that hands the same
// block back after a free is the thing worth seeing.
void cmd_heaptest() {
    char *a;
    char *b;
    char *c;
    long i;
    a = (char *)kmalloc(1000);
    b = (char *)kmalloc(2000);
    c = (char *)kmalloc(4000);
    printf("allocated 0x%x 0x%x 0x%x\n", a, b, c);
    i = 0;
    while (i < 1000) { a[i] = 'A'; i = i + 1; }
    i = 0;
    while (i < 2000) { b[i] = 'B'; i = i + 1; }
    i = 0;
    while (i < 4000) { c[i] = 'C'; i = i + 1; }
    if (a[0] == 'A' && a[999] == 'A' && b[1999] == 'B' && c[3999] == 'C')
        puts("contents intact\n");
    else puts("BLOCKS OVERLAPPED\n");
    kfree(b);
    {
        char *b2;
        b2 = (char *)kmalloc(2000);
        printf("freed then re-asked: 0x%x %s\n", b2,
               b2 == b ? "(same block reused)" : "(different block)");
        kfree(b2);
    }
    kfree(a);
    kfree(c);
    printf("after freeing: %d bytes free in %d blocks\n",
           heap_bytes_free(), heap_blocks(1));
}

// Map a fresh physical frame over a virtual address, show that it moved, then
// put it back. The identity map is what it was mapped to before.
void cmd_maptest() {
    long virt;
    long phys;
    virt = 0xD0000000;
    printf("0x%x resolves to 0x%x\n", virt, vmm_resolve(virt));
    phys = frame_alloc_zeroed();
    if (!phys) { puts("out of frames\n"); return; }
    vmm_map(virt, phys, PTE_WRITE);
    printf("mapped to frame 0x%x, now resolves to 0x%x\n", phys, vmm_resolve(virt));
    {
        long *p;
        p = (long *)virt;
        p[0] = 0xFEEDFACE;
        printf("wrote through it; the frame holds 0x%x\n", mem64(phys));
    }
    vmm_unmap(virt);
    printf("unmapped, resolves to 0x%x\n", vmm_resolve(virt));
    frame_free(phys);
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
long g_anim_stop;
long g_anim_count;

// A box bouncing around the panel, redrawn from its own thread. The point is
// that the shell stays usable while this runs -- you can type, and commands
// answer, with no cooperation from the animation at all.
void animator(long seed) {
    long x;
    long y;
    long dx;
    long dy;
    long w;
    long col;
    long id;

    id = g_anim_count;
    g_anim_count = g_anim_count + 1;

    w = 26;
    x = g_panel_x + 20 + (seed * 37) % (g_panel_w - 80);
    y = g_panel_y + 40 + (seed * 53) % (g_panel_h - 120);
    dx = (seed % 2) ? 4 : -4;
    dy = (seed % 3) ? 3 : -3;
    col = rgb(60 + (seed * 71) % 190, 90 + (seed * 37) % 160, 120 + (seed * 53) % 130);

    while (!g_anim_stop) {
        fb_fill(x, y, w, w, 0x010409);            // erase
        x = x + dx;
        y = y + dy;
        if (x < g_panel_x + 4) { x = g_panel_x + 4; dx = 0 - dx; }
        if (x + w > g_panel_x + g_panel_w - 4) { x = g_panel_x + g_panel_w - 4 - w; dx = 0 - dx; }
        if (y < g_panel_y + 24) { y = g_panel_y + 24; dy = 0 - dy; }
        if (y + w > g_panel_y + g_panel_h - 24) { y = g_panel_y + g_panel_h - 24 - w; dy = 0 - dy; }
        fb_fill(x, y, w, w, col);
        fb_rect(x, y, w, w, 0xffffff);
        thread_sleep_ms(30);
    }
    fb_fill(x, y, w, w, 0x010409);
    thread_exit(0);
}

// ---------------------------------------------------------------------------
// Servers
// ---------------------------------------------------------------------------
long g_crash_req;
long g_blink_srv;
long g_crash_srv;

// A supervised service: a small indicator in the corner of the title bar that
// blinks while it is healthy. If it stops, you can see that it stopped.
void blinker_server(long unused) {
    long on;
    long me;
    me = srv_self();
    on = 0;
    for (;;) {
        srv_beat(me);
        fb_fill(fb_width - 24, 8, 10, 10, on ? 0x3fb950 : 0x0d3018);
        on = !on;
        thread_sleep_ms(250);
    }
}

// A service that faults on request, so the recovery can be watched rather than
// described. Writing through a null pointer is the ordinary way a driver dies,
// and page 0 is unmapped at boot precisely so that it faults instead of
// quietly scribbling on the interrupt vector table.
void crasher_server(long unused) {
    long me;
    me = srv_self();
    for (;;) {
        srv_beat(me);
        if (g_crash_req) {
            long *p;
            g_crash_req = 0;
            p = (long *)0;
            p[0] = 1;
        }
        thread_sleep_ms(40);
    }
}

// The kernel printf has no field width, so pad by hand rather than write
// "%-9s" and have every following column read the wrong argument.
void pad_to(char *s, long width) {
    long n;
    n = 0;
    while (s[n]) { putc(s[n]); n = n + 1; }
    while (n < width) { putc(' '); n = n + 1; }
}

void cmd_srv() {
    long i;
    puts("name      state    thr restarts faults hangs\n");
    i = 0;
    while (i < g_nsrv) {
        pad_to(g_srv[i].name, 10);
        pad_to(srv_state_name(g_srv[i].state), 9);
        printf("%d   %d", g_srv[i].thread, g_srv[i].restarts);
        printf("        %d      %d\n", g_srv[i].faults, g_srv[i].hangs);
        i = i + 1;
    }
    printf("supervisor: %d checks, %d restarts\n", g_sup_checks, g_sup_restarts);
}

void cmd_crash() {
    long before;
    before = g_srv[g_crash_srv].restarts;
    puts("asking the crasher service to dereference a null pointer...\n");
    g_crash_req = 1;
    thread_sleep_ms(600);
    printf("restarts %d -> %d. this shell never stopped.\n",
           before, g_srv[g_crash_srv].restarts);
    puts("a crash is contained. corruption would not be -- that needs\n");
    puts("a separate address space per server, which is the next step.\n");
}

// ---------------------------------------------------------------------------
// Shell tools
// ---------------------------------------------------------------------------
char g_cwd[256];
char g_path[512];
char g_path2[512];
char g_iobuf[4096];

// Resolve a possibly-relative path against the working directory.
char *resolve(char *p, char *out) {
    long i;
    long k;
    if (p[0] == '/') {
        k = 0;
        while (p[k] && k < 500) { out[k] = p[k]; k = k + 1; }
        out[k] = 0;
        return out;
    }
    k = 0;
    while (g_cwd[k] && k < 250) { out[k] = g_cwd[k]; k = k + 1; }
    if (k == 0 || out[k - 1] != '/') { out[k] = '/'; k = k + 1; }
    i = 0;
    while (p[i] && k < 500) { out[k] = p[i]; k = k + 1; i = i + 1; }
    out[k] = 0;
    return out;
}

// Split a command line into words in place. Returns the count; argv points
// into `line`, which the caller owns.
long split(char *line, char **argv, long maxv) {
    long n;
    long i;
    n = 0;
    i = 0;
    for (;;) {
        while (line[i] == ' ') { line[i] = 0; i = i + 1; }
        if (!line[i]) return n;
        if (n >= maxv) return n;
        argv[n] = line + i;
        n = n + 1;
        while (line[i] && line[i] != ' ') i = i + 1;
    }
}

void cmd_ls(char *path) {
    long dir;
    long i;
    char nm[64];
    dir = fs_lookup(resolve(path, g_path));
    if (!dir) { printf("ls: %s: not found\n", g_path); return; }
    if (fs_type(dir) != T_DIR) {
        printf("%s  %d bytes\n", g_path, fs_size(dir));
        return;
    }
    i = 0;
    for (;;) {
        long ino;
        ino = fs_readdir(dir, i, nm);
        if (!ino) break;
        if (strcmp(nm, ".") && strcmp(nm, "..")) {
            if (fs_type(ino) == T_DIR) { puts(nm); puts("/\n"); }
            else printf("%s  %d\n", nm, fs_size(ino));
        }
        i = i + 1;
    }
}

void cmd_cat(char *path) {
    long ino;
    long off;
    ino = fs_lookup(resolve(path, g_path));
    if (!ino) { printf("cat: %s: not found\n", g_path); return; }
    if (fs_type(ino) == T_DIR) { printf("cat: %s is a directory\n", g_path); return; }
    off = 0;
    for (;;) {
        long n;
        long i;
        n = fs_read(ino, off, g_iobuf, 4096);
        if (n <= 0) break;
        i = 0;
        while (i < n) { putc(g_iobuf[i]); i = i + 1; }
        off = off + n;
    }
}

void cmd_mkdir(char *path) {
    if (!fs_mkdir(resolve(path, g_path))) printf("mkdir: %s failed\n", g_path);
}

void cmd_rm(char *path) {
    if (!fs_unlink(resolve(path, g_path)))
        printf("rm: %s failed (missing, or a non-empty directory)\n", g_path);
}

void cmd_touch(char *path) {
    if (!fs_create(resolve(path, g_path))) printf("touch: %s failed\n", g_path);
}

// cp really copies the bytes. mv does not -- see cmd_mv.
void cmd_cp(char *from, char *to) {
    long src;
    long dst;
    long off;
    src = fs_lookup(resolve(from, g_path));
    if (!src) { printf("cp: %s: not found\n", g_path); return; }
    if (fs_type(src) == T_DIR) { puts("cp: cannot copy a directory\n"); return; }
    resolve(to, g_path2);
    dst = fs_lookup(g_path2);
    if (dst) fs_truncate(dst);
    else dst = fs_create(g_path2);
    if (!dst) { printf("cp: cannot create %s\n", g_path2); return; }
    off = 0;
    for (;;) {
        long n;
        n = fs_read(src, off, g_iobuf, 4096);
        if (n <= 0) break;
        if (fs_write(dst, off, g_iobuf, n) != n) { puts("cp: write failed\n"); return; }
        off = off + n;
    }
    printf("copied %d bytes\n", off);
}

// mv is a directory operation: the inode does not move, so a large file
// renames in the time it takes to rewrite two 32-byte entries. cp above has to
// read and write every byte; this does not.
void cmd_mv(char *from, char *to) {
    resolve(from, g_path);
    resolve(to, g_path2);
    if (!fs_rename(g_path, g_path2)) printf("mv: %s -> %s failed\n", g_path, g_path2);
}

void cmd_write(char *path, char *text, long append) {
    long ino;
    long off;
    long n;
    resolve(path, g_path);
    ino = fs_lookup(g_path);
    if (!ino) ino = fs_create(g_path);
    if (!ino) { printf("write: %s failed\n", g_path); return; }
    off = append ? fs_size(ino) : 0;
    if (!append) fs_truncate(ino);
    n = strlen(text);
    fs_write(ino, off, text, n);
    fs_write(ino, off + n, "\n", 1);
}

void cmd_cd(char *path) {
    long ino;
    resolve(path, g_path);
    ino = fs_lookup(g_path);
    if (!ino) { printf("cd: %s: not found\n", g_path); return; }
    if (fs_type(ino) != T_DIR) { printf("cd: %s is not a directory\n", g_path); return; }
    strcpy(g_cwd, g_path);
}

void cmd_df() {
    long free_blocks;
    free_blocks = blocks_free();
    printf("%d blocks of %d bytes\n", sb_nblocks, BLK_SIZE);
    printf("%d free (%d KiB), %d used\n",
           free_blocks, free_blocks / 2, sb_nblocks - sb_data_start - free_blocks);
    printf("inodes: %d, data starts at block %d\n", sb_ninodes, sb_data_start);
}

// Write a whole string to a file. The length comes from strlen rather than
// being counted by hand -- a hardcoded length that is one too long reads past
// the end of the literal and writes whatever follows it in memory into the
// file, which is exactly what happened the first time.
void put_file(char *path, char *text) {
    long ino;
    ino = fs_create(path);
    if (ino) fs_write(ino, 0, text, strlen(text));
}

// Something to look at on the first boot, so `ls` is not an empty room.
void fs_populate() {
    fs_mkdir("/src");
    fs_mkdir("/doc");
    put_file("/doc/readme",
        "nano-os\n"
        "\n"
        "everything here was compiled by nano_cc, which compiled itself.\n"
        "the filesystem is a ram disk with a unix-shaped layout:\n"
        "superblock, bitmaps, inodes, directory entries.\n"
        "\n"
        "try: ls, cat, cp, mv, mkdir, rm, write, df\n");
    put_file("/src/hello.c",
        "int main() {\n"
        "    puts(\"hello from a file\\n\");\n"
        "    return 0;\n"
        "}\n");

    // The programs. They are compiled by nano_cc, linked at 512 GiB and
    // embedded in this kernel image by progs.s, because there is no disk yet
    // to have put them on. Copying them onto the RAM disk at boot is what makes
    // `exec /bin/hello` read a real file rather than jump to a symbol.
    fs_mkdir("/bin");
    {
        long ino;
        ino = fs_create("/bin/hello");
        if (ino) fs_write(ino, 0, (char *)prog_hello_addr(), prog_hello_size());
        ino = fs_create("/bin/twin");
        if (ino) fs_write(ino, 0, (char *)prog_twin_addr(), prog_twin_size());
        ino = fs_create("/bin/wild");
        if (ino) fs_write(ino, 0, (char *)prog_wild_addr(), prog_wild_size());
    }
    strcpy(g_cwd, "/");
}

char *state_name(long st) {
    if (st == T_UNUSED) return "unused ";
    if (st == T_READY) return "ready  ";
    if (st == T_RUNNING) return "running";
    if (st == T_BLOCKED) return "blocked";
    return "done   ";
}

void cmd_ps() {
    long i;
    printf("id state   slices space   name\n");
    i = 0;
    while (i < MAX_THREADS) {
        if (g_threads[i].state != T_UNUSED) {
            // "kernel" or "own": which address space the thread runs in. A
            // thread list that does not say this cannot tell a kernel worker
            // apart from a loaded program, which after this milestone is the
            // most useful thing about it.
            printf("%d  %s %d %s %s\n", i, state_name(g_threads[i].state),
                   g_threads[i].slices,
                   g_threads[i].root == g_kernel_root ? "kernel " : "own    ",
                   g_threads[i].name);
        }
        i = i + 1;
    }
    printf("%d context switches, %d of them across address spaces\n",
           g_switches, g_space_switches);
}

char *proc_state_name(long st) {
    if (st == P_RUNNING) return "running";
    if (st == P_EXITED) return "exited ";
    if (st == P_KILLED) return "killed ";
    return "free   ";
}

void cmd_procs() {
    long i;
    proc_poll();
    printf("pid state   thread exit name\n");
    i = 0;
    while (i < MAX_PROCS) {
        if (g_procs[i].state != P_FREE) {
            printf("%d   %s %d      %d    %s\n",
                   g_procs[i].pid, proc_state_name(g_procs[i].state),
                   g_procs[i].tid, g_procs[i].exitcode, g_procs[i].name);
        }
        i = i + 1;
    }
    printf("%d running, %d killed by a fault, %d syscalls served\n",
           proc_alive(), g_proc_faults, g_syscalls);
}

// A number at the end of a command line. Returns 0 for anything that is not
// one, which is also a perfectly good argument, so nothing here needs to
// distinguish "no argument" from "the argument zero".
long shell_atol(char *s) {
    long v;
    long neg;
    v = 0;
    neg = 0;
    if (*s == '-') { neg = 1; s = s + 1; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s = s + 1; }
    return neg ? -v : v;
}

// Run a program and wait for it. The shell blocks, but the rest of the system
// does not: this yields while it waits, so the animators keep drawing and any
// background process keeps running.
void cmd_exec(char *path, long arg) {
    long pid;
    long code;
    pid = proc_spawn(path, arg, path);
    if (!pid) { puts("cannot run "); puts(path); puts(": "); puts(proc_reject); putc('\n'); return; }
    printf("[pid %d]\n", pid);
    code = proc_wait(pid);
    if (code == -1) printf("[pid %d killed by a fault]\n", pid);
    else printf("[pid %d exited with %d]\n", pid, code);
}

// Start a program and come straight back to the prompt.
void cmd_run(char *path, long arg) {
    long pid;
    pid = proc_spawn(path, arg, path);
    if (!pid) { puts("cannot run "); puts(path); puts(": "); puts(proc_reject); putc('\n'); return; }
    printf("[pid %d started in the background]\n", pid);
}

// Reap finished processes and give their address spaces back.
//
// It has to be somebody else's thread: a process cannot free the page tables
// it is standing on. A shell command could do the reaping, but then a
// background program that exits while the shell sits waiting for a keystroke
// would hold its frames until the next Enter.
long g_reaped;

void reaper(long unused) {
    for (;;) {
        g_reaped = g_reaped + proc_poll();
        thread_sleep_ms(200);
    }
}

void cmd_spawn() {
    long id;
    g_anim_stop = 0;
    id = thread_create((long)animator, g_anim_count + 1, "anim");
    if (id < 0) { puts("no free thread slot\n"); return; }
    printf("spawned thread %d; the shell stays usable\n", id);
}

void cmd_stopall() {
    g_anim_stop = 1;
    thread_sleep_ms(120);
    puts("animators asked to stop\n");
    panel_clear();
}

void cmd_help() {
    puts("commands:\n");
    puts("  help clear ver fbinfo pci acpi\n");
    puts("  idle uptime fault\n");
    puts("  mem heaptest maptest\n");
    puts("  ps procs spawn stopall\n");
    puts("  exec <prog> [n]   run a program and wait\n");
    puts("  run  <prog> [n]   run one in the background\n");
    puts("  srv crash\n");
    puts("  ls cat mkdir rm touch cp mv write append cd pwd df\n");
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

// The shell, as a thread. Everything from here on runs with the scheduler
// live, which is what lets a background animation and a keyboard prompt
// coexist without either knowing about the other.
void shell_thread(long unused) {
    char line[128];
    long n;

    // Services start once the scheduler is live, so the supervisor has
    // something to schedule.
    if (!fs_format(2048, 128)) puts("filesystem format failed\n");
    else fs_populate();

    srv_init(100);
    g_blink_srv = srv_register("blinker", (long)blinker_server, 0, 100);
    g_crash_srv = srv_register("crasher", (long)crasher_server, 0, 100);
    srv_start(g_blink_srv);
    srv_start(g_crash_srv);

    splash();
    puts("framebuffer console up. type help.\n");
    printf("timer %d Hz, %d CPU, idle %s\n", g_hz, acpi_cpus, acpi_cstate_name());
    printf("%d KiB RAM, %d frames free\n\n", mm_ram_total / 1024, mm_free_frames);

    for (;;) {
        long c;
        puts(g_cwd);
        puts("> ");
        n = 0;
        for (;;) {
            c = keyboard_getchar_irq();
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
        else if (!strcmp(line, "ver")) puts("nano-os 0.5: threads, a filesystem, and processes in their own address spaces\n");
        else if (!strcmp(line, "fbinfo")) cmd_fbinfo();
        else if (!strcmp(line, "pci")) cmd_pci();
        else if (!strcmp(line, "acpi")) cmd_acpi();
        else if (!strcmp(line, "idle")) cmd_idle();
        else if (!strcmp(line, "uptime")) cmd_uptime();
        else if (!strcmp(line, "fault")) cmd_fault();
        else if (!strcmp(line, "mem")) cmd_mem();
        else if (!strcmp(line, "heaptest")) cmd_heaptest();
        else if (!strcmp(line, "maptest")) cmd_maptest();
        else if (!strcmp(line, "ps")) cmd_ps();
        else if (!strcmp(line, "procs")) cmd_procs();
        else if (!strcmp(line, "spawn")) cmd_spawn();
        else if (!strcmp(line, "stopall")) cmd_stopall();
        else if (!strcmp(line, "srv")) cmd_srv();
        else if (!strcmp(line, "crash")) cmd_crash();
        else if (!strcmp(line, "demo")) cmd_demo();
        else if (!strcmp(line, "bars")) cmd_bars();
        else if (!strcmp(line, "grad")) cmd_grad();
        else if (!strcmp(line, "lines")) cmd_lines();
        else if (!strcmp(line, "circles")) cmd_circles();
        else if (!strcmp(line, "font")) cmd_font();
        else if (starts_with(line, "echo ")) { puts(line + 5); putc('\n'); }
        else if (!strcmp(line, "pwd")) { puts(g_cwd); putc('\n'); }
        else if (!strcmp(line, "df")) cmd_df();
        else if (!strcmp(line, "ls")) cmd_ls(g_cwd);
        else {
            // Anything with arguments. `write` and `append` keep the rest of
            // the line verbatim, so the text can contain spaces.
            char work[128];
            char *av[8];
            long ac;
            strcpy(work, line);
            ac = split(work, av, 8);
            if (ac == 0) { }
            else if (!strcmp(av[0], "ls") && ac >= 2) cmd_ls(av[1]);
            else if (!strcmp(av[0], "cat") && ac >= 2) cmd_cat(av[1]);
            else if (!strcmp(av[0], "mkdir") && ac >= 2) cmd_mkdir(av[1]);
            else if (!strcmp(av[0], "rm") && ac >= 2) cmd_rm(av[1]);
            else if (!strcmp(av[0], "touch") && ac >= 2) cmd_touch(av[1]);
            else if (!strcmp(av[0], "cd") && ac >= 2) cmd_cd(av[1]);
            else if (!strcmp(av[0], "cp") && ac >= 3) cmd_cp(av[1], av[2]);
            else if (!strcmp(av[0], "mv") && ac >= 3) cmd_mv(av[1], av[2]);
            else if (!strcmp(av[0], "exec") && ac >= 2)
                cmd_exec(av[1], ac >= 3 ? shell_atol(av[2]) : 0);
            else if (!strcmp(av[0], "run") && ac >= 2)
                cmd_run(av[1], ac >= 3 ? shell_atol(av[2]) : 0);
            else if (starts_with(line, "write ") && ac >= 3)
                cmd_write(av[1], line + 6 + strlen(av[1]) + 1, 0);
            else if (starts_with(line, "append ") && ac >= 3)
                cmd_write(av[1], line + 7 + strlen(av[1]) + 1, 1);
            else { puts("unknown: "); puts(line); putc('\n'); }
        }
    }
}

int main() {
    serial_init();
    kbd_init();
    interrupts_init(100);          // IDT, PIC, 100 Hz timer, IRQ keyboard
    mm_init();                     // frames, 4 KiB paging, kernel heap
    g_anim_stop = 0;
    g_anim_count = 0;
    acpi_init();                   // reads the EBDA pointer at 0x40E...
    acpi_enable();
    acpi_pick_cstate();
    mm_protect_null();             // ...so unmap page 0 only after that.
                                   // Without this a null dereference lands in
                                   // the interrupt vector table and succeeds.
    g_crash_req = 0;

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

    thread_init();
    proc_init();                   // process table, and CR0.WP so a read-only
                                   // page is read-only in ring 0 as well
    g_reaped = 0;
    thread_create((long)shell_thread, 0, "shell");
    thread_create((long)reaper, 0, "reaper");
    sched_start();                 // does not return

    puts("UNREACHABLE\n");
    return 0;
}
