// oswin.c — the machine compiles a graphical program and gives it a window.
//
// This is the piece K15 promised and K16 and K17 kept postponing. The two
// halves have existed separately for a while:
//
//   K8 put the C compiler inside the OS.
//   K9 put the assembler inside it, so source became a process without
//      anything outside the machine being involved.
//   K11..K17 built a compositor, a window manager, widgets and a renderer.
//
// What was missing was that those two had never been the SAME IMAGE, and a
// user process had no way to ask for a window even if they had been. So this
// image is the union -- filesystem, ELF loader, address spaces, compiler,
// assembler, window manager, compositor -- and there are five new syscalls.
//
//     cc --minimal --nasm --bss --kernel wingl.c wingl.asm
//     as wingl.asm /bin/wingl -b 0x8000000000
//     exec /bin/wingl
//
// ...and a rotating cube appears in a window, drawn by a program this machine
// compiled, in the program's own address space, with the program's own
// arithmetic.
//
// WHAT CROSSES THE BOUNDARY IS A HANDLE, NOT A POINTER. The process draws into
// memory it got from sbrk and calls SYS_WINBLIT; the kernel copies, clipped to
// the window's client area. The window's backing buffer never leaves the
// kernel, so the address-space isolation from K7 holds -- there is no shared
// mapping to get wrong, because there is no shared mapping.
//
// The second program, winbad, is the one that matters for the boundary. It
// misbehaves on purpose -- blits out of bounds, blits at a window it does not
// own, polls a window it has closed -- and reports what the kernel said. A
// kernel test that calls its own clipping code and finds that it clips proves
// the function works. Only a process can prove that a process cannot get past
// it.
//
// nano-kernel.h first, so console output does NOT get mirrored onto the
// framebuffer: the window manager owns the screen in this image.
#include "nano-kernel.h"
#include "nano-fb.h"
#include "nano-mouse.h"
#include "nano-mm.h"
#include "nano-thread.h"
#include "nano-fs.h"
#include "nano-wm.h"
#include "nano-wmin.h"
#include "nano-proc.h"
#include "nano-int.h"
#include "nano-term.h"

extern long prog_cc_addr();
extern long prog_cc_size();
extern long prog_as_addr();
extern long prog_as_size();
extern long prog_wingl_addr();
extern long prog_wingl_size();
extern long prog_ugears_addr();
extern long prog_ugears_size();
extern long prog_hgl_addr();
extern long prog_hgl_size();
extern long prog_hglapi_addr();
extern long prog_hglapi_size();
extern long prog_winbad_addr();
extern long prog_winbad_size();

long g_fail;

void fail(char *msg) {
    printf("FAIL: %s\n", msg);
    g_fail = g_fail + 1;
}

void expect(char *what, long got, long want) {
    if (got == want) printf("  ok  %s = %d\n", what, got);
    else {
        printf("  got %d, wanted %d\n", got, want);
        fail(what);
    }
}

void expect_true(char *what, long got) {
    if (got) printf("  ok  %s\n", what);
    else fail(what);
}

// ---------- installing the toolchain on the RAM disk ----------

long install(char *path, long addr, long size) {
    long ino;
    ino = fs_create(path);
    if (!ino) { printf("could not create %s\n", path); return 0; }
    if (fs_write(ino, 0, (char *)addr, size) != size) {
        printf("short write installing %s\n", path);
        return 0;
    }
    printf("installed %s (%d bytes)\n", path, size);
    return ino;
}

long run_cc(char *cwd, char *in, char *out) {
    char *av[8];
    long pid;
    av[0] = "cc";
    av[1] = "--minimal";
    av[2] = "--nasm";
    av[3] = "--bss";
    av[4] = "--kernel";
    av[5] = in;
    av[6] = out;
    pid = proc_spawn("/bin/cc", 7, av, "cc", cwd);
    if (!pid) { printf("SPAWN FAILED cc: %s\n", proc_reject); return -2; }
    return proc_wait(pid);
}

long run_as(char *cwd, char *in, char *out) {
    char *av[8];
    long pid;
    av[0] = "as";
    av[1] = in;
    av[2] = out;
    av[3] = "-b";
    av[4] = "0x8000000000";
    pid = proc_spawn("/bin/as", 5, av, "as", cwd);
    if (!pid) { printf("SPAWN FAILED as: %s\n", proc_reject); return -2; }
    return proc_wait(pid);
}

// Compile and assemble one source file into /bin. Returns 1 on success.
long build(char *src, char *asmout, char *binout) {
    long code;
    long ino;

    printf("\n-- cc %s -> %s --\n", src, asmout);
    code = run_cc("/src", src, asmout);
    printf("cc exited with %d\n", code);
    if (code != 0) { fail("the compile"); return 0; }
    ino = fs_lookup(asmout[0] == '/' ? asmout : "/src/x");
    printf("\n-- as %s -> %s --\n", asmout, binout);
    code = run_as("/src", asmout, binout);
    printf("as exited with %d\n", code);
    if (code != 0) { fail("the assemble"); return 0; }
    ino = fs_lookup(binout);
    if (!ino) { fail("no binary was produced"); return 0; }
    printf("%s is %d bytes\n", binout, fs_size(ino));
    {
        char hdr[8];
        if (fs_read(ino, 0, hdr, 8) != 8) { fail("cannot read the header"); return 0; }
        if ((hdr[0] & 255) != 0x7F || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F') {
            fail("what came out is not an ELF file");
            return 0;
        }
    }
    puts("it is an ELF file\n");
    return 1;
}

// ---------- the desktop this all happens on ----------

long g_console;
long g_kwin;                    // a kernel-owned window: handle 0

void build_desktop() {
    wm_init(rgb(20, 24, 34));
    wmin_init();
    term_init();
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);

    // The FIRST window, so it gets handle 0 -- which is what winbad blits at
    // to prove a process cannot draw into a window the kernel owns.
    g_kwin = wm_create(40, 40, 300, 120, "kernel");
    wm_decorate(g_kwin);
    wm_win_fill(g_kwin, WM_BORDER, WM_TITLE_H,
                wm_client_w(g_kwin), wm_client_h(g_kwin), rgb(58, 44, 44));
    wm_win_text(g_kwin, WM_BORDER + 8, WM_TITLE_H + 10,
                "this window belongs to the kernel", rgb(230, 210, 200));
    wm_win_text(g_kwin, WM_BORDER + 8, WM_TITLE_H + 28,
                "a process may not blit into it", rgb(230, 210, 200));

    g_console = term_create(40, 190, 60, 14, "build");
    if (g_console >= 0) {
        term_puts(g_console, "nano-os K18\n");
        term_puts(g_console, "cc + as + ELF loader + window manager,\n");
        term_puts(g_console, "all in one image. five new syscalls:\n");
        term_puts(g_console, "winopen winblit winpresent winpoll\n");
        term_puts(g_console, "winclose -- a HANDLE crosses, not a\n");
        term_puts(g_console, "pointer.\n");
        term_flush(g_console);
    }

    wm_cursor_show(1);
    mouse_warp(fb_width / 2, fb_height / 2);
    wm_cursor_move(g_mouse_x, g_mouse_y);
    wm_present();
}

// How many windows a given pid currently owns.
long windows_owned_by(long pid) {
    long i;
    long n;
    n = 0;
    i = 0;
    while (i < WM_MAXWIN) {
        if (g_win[i].used && g_win_owner[i] == pid) n = n + 1;
        i = i + 1;
    }
    return n;
}

// A hash of a window's client area, and the number of DISTINCT COLOURS in it.
//
// The first version of this counted pixels that differed from the window's
// background, and it was useless: the program blits its whole client area
// every frame, so from the second frame onwards every pixel differs and the
// count is the area, forever. It reported two distinct frames for sixty
// frames of animation and the failure looked like the program not drawing.
//
// Two properties, and neither of them decides in advance what the picture
// should look like:
//
//   the hash CHANGES between frames        -> it animated
//   there are several distinct colours     -> it drew structure, not a fill
//
// A solid-colour blit would satisfy "every pixel changed" and fails both.
long client_hash(long hnd) {
    long h;
    long j;
    long cw;
    long ch;
    if (hnd < 0 || hnd >= WM_MAXWIN || !g_win[hnd].used) return 0;
    cw = wm_client_w(hnd);
    ch = wm_client_h(hnd);
    h = 5381;
    j = 0;
    while (j < ch) {
        long i;
        i = 0;
        while (i < cw) {
            h = ((h * 33) + g_win[hnd].pix[(wm_client_y() + j) * g_win[hnd].w
                                           + wm_client_x() + i]) & 0xFFFFFFFF;
            i = i + 1;
        }
        j = j + 1;
    }
    return h;
}

// Raised from 16 for the gears. A saturating counter answers "at least this
// many", and 16 was low enough that a flat-shaded scene and a wireframe gave
// the same answer -- the tell was that the number came back EXACTLY at the
// cap. wingl's own check wants 3, so nothing below is affected.
#define MAX_SEEN 64

long g_seen[MAX_SEEN];

long client_colours(long hnd) {
    long n;
    long j;
    long cw;
    long ch;
    if (hnd < 0 || hnd >= WM_MAXWIN || !g_win[hnd].used) return 0;
    cw = wm_client_w(hnd);
    ch = wm_client_h(hnd);
    n = 0;
    j = 0;
    while (j < ch) {
        long i;
        i = 0;
        while (i < cw) {
            long c;
            long k;
            long found;
            c = g_win[hnd].pix[(wm_client_y() + j) * g_win[hnd].w
                               + wm_client_x() + i];
            found = 0;
            k = 0;
            while (k < n) { if (g_seen[k] == c) found = 1; k = k + 1; }
            if (!found) {
                if (n >= MAX_SEEN) return n;      // enough to answer the question
                g_seen[n] = c;
                n = n + 1;
            }
            i = i + 1;
        }
        j = j + 1;
    }
    return n;
}

// ---------- watching a process while it runs ----------

long g_saw_window;              // the handle the process opened, or -1
long g_max_colours;             // the most distinct colours it ever had up
long g_frames_seen;             // how many distinct pictures we caught it at
long g_kwin_hash_before;
long g_kwin_hash_after;

long kwin_hash() {
    long h;
    long j;
    h = 5381;
    j = 0;
    while (j < g_win[g_kwin].h) {
        long i;
        i = 0;
        while (i < g_win[g_kwin].w) {
            h = ((h * 33) + g_win[g_kwin].pix[j * g_win[g_kwin].w + i]) & 0xFFFFFFFF;
            i = i + 1;
        }
        j = j + 1;
    }
    return h;
}

// Spawn a program and watch it, rather than just waiting for it. proc_wait
// would tell us the exit code and nothing about what happened on screen while
// it ran -- and what happens on screen is the entire point of this milestone.
long run_and_watch(char *path, long pid_out) {
    char *av[2];
    long pid;
    long last_ink;

    av[0] = path;
    pid = proc_spawn(path, 1, av, path, "/");
    if (!pid) { printf("SPAWN FAILED %s: %s\n", path, proc_reject); return -2; }

    g_saw_window = -1;
    g_max_colours = 0;
    g_frames_seen = 0;
    last_ink = 0;

    for (;;) {
        long i;
        long done;

        proc_poll();
        done = 1;
        i = 0;
        while (i < MAX_PROCS) {
            if (g_procs[i].pid == pid) {
                if (g_procs[i].state == P_RUNNING) done = 0;
            }
            i = i + 1;
        }

        // Look for a window this pid owns, and see what is in it.
        i = 0;
        while (i < WM_MAXWIN) {
            if (g_win[i].used && g_win_owner[i] == pid) {
                long h;
                long cols;
                g_saw_window = i;
                h = client_hash(i);
                if (h != last_ink) { g_frames_seen = g_frames_seen + 1; last_ink = h; }
                cols = client_colours(i);
                if (cols > g_max_colours) g_max_colours = cols;
            }
            i = i + 1;
        }

        if (done) break;
        thread_yield();
    }

    {
        long i;
        i = 0;
        while (i < MAX_PROCS) {
            if (g_procs[i].pid == pid) {
                if (g_procs[i].state == P_EXITED) return g_procs[i].exitcode;
                if (g_procs[i].state == P_KILLED) return -1;
            }
            i = i + 1;
        }
    }
    return -2;
}

// ---------- the test ----------

void main_thread(long unused) {
    long ok;
    long code;
    long owner_pid;

    puts("scheduler running\n");

    if (!fs_format(4096, 256)) { puts("format failed\n"); cpu_halt_forever(); }
    fs_mkdir("/bin");
    fs_mkdir("/src");
    install("/bin/cc", prog_cc_addr(), prog_cc_size());
    install("/bin/as", prog_as_addr(), prog_as_size());
    install("/src/wingl.c", prog_wingl_addr(), prog_wingl_size());
    install("/src/winbad.c", prog_winbad_addr(), prog_winbad_size());
    // The renderer itself, so the compiler inside the machine can include it.
    install("/src/gears.c", prog_ugears_addr(), prog_ugears_size());
    install("/src/nano-gl.h", prog_hgl_addr(), prog_hgl_size());
    install("/src/nano-glapi.h", prog_hglapi_addr(), prog_hglapi_size());

    build_desktop();
    g_kwin_hash_before = kwin_hash();

    // ============================================================
    // 1. the machine builds a graphical program
    // ============================================================
    puts("\n== 1. the machine compiles and assembles a graphical program ==\n");
    ok = build("wingl.c", "wingl.asm", "/bin/wingl");
    expect_true("wingl was built inside the machine", ok);
    if (!ok) { puts("\nOSWINTEST DONE\n"); cpu_halt_forever(); }

    // ============================================================
    // 2. ...and runs it, and it gets a window
    // ============================================================
    puts("\n== 2. exec /bin/wingl ==\n");
    {
        long before;
        before = 0;
        {
            long i;
            i = 0;
            while (i < WM_MAXWIN) { if (g_win[i].used) before = before + 1; i = i + 1; }
        }
        printf("windows on the desktop before: %d\n", before);

        code = run_and_watch("/bin/wingl", 0);
        printf("wingl exited with %d\n", code);
        printf("it owned window %d, put up to %d distinct colours in it, "
               "and we caught it at %d distinct frames\n",
               g_saw_window, g_max_colours, g_frames_seen);

        // 7 is computed by the program, not a constant in the file, so a
        // loader that ran the wrong bytes could not produce it.
        expect("the program exited with the code it computed", code, 7);
        expect_true("it opened a window", g_saw_window >= 0);
        // A background, a horizon and a cube is at least three colours. A
        // program that blitted a solid fill would give one, and would still
        // have "changed every pixel".
        expect_true("...and drew structure into it, not a flat fill",
                    g_max_colours >= 3);
        // Several distinct pictures means it ANIMATED. One blit that happened
        // to land would give one.
        expect_true("...and it animated", g_frames_seen > 3);

        // And the window is gone now, because the process that owned it is.
        {
            long after;
            after = 0;
            {
                long i;
                i = 0;
                while (i < WM_MAXWIN) { if (g_win[i].used) after = after + 1; i = i + 1; }
            }
            printf("windows on the desktop after: %d\n", after);
            expect("the process's window went with the process", after, before);
        }
    }

    // ============================================================
    // 3. the boundary, tested from the wrong side of it
    // ============================================================
    puts("\n== 3. cc winbad.c, and let it try to get past the boundary ==\n");
    ok = build("winbad.c", "winbad.asm", "/bin/winbad");
    expect_true("winbad was built inside the machine", ok);
    if (ok) {
        code = run_and_watch("/bin/winbad", 0);
        printf("winbad exited with %d\n", code);
        // Every bit of the exit code is one check that FAILED, so 0 is the
        // only passing value and the number says which ones if it is not.
        expect("every one of winbad's twenty-two checks held", code, 0);
    }

    // The kernel's own window must be untouched, pixel for pixel, after a
    // process spent a whole program trying to draw into it.
    g_kwin_hash_after = kwin_hash();
    expect_true("the kernel's window is bit-for-bit unchanged",
                g_kwin_hash_after == g_kwin_hash_before);

    // ============================================================
    // 3b. the renderer, compiled INTO a program
    // ============================================================
    //
    // This is the one the milestone is about. src/gears.c includes nano-gl.h
    // and nano-glapi.h -- the same two files these kernel images compile
    // against -- and the compiler inside the machine compiles all 2,500 lines
    // of it into a process. The process gets its pixels and its depth buffer
    // from sbrk, renders into them with a real GL, and blits the result
    // through the window syscalls.
    //
    // Nothing about the boundary changes: what crosses it is still a handle
    // and a copy. The renderer is simply on the other side of it now.
    puts("\n== 3b. cc gears.c -- the whole renderer, into a process ==\n");
    {
        long t0;
        long t1;
        long ino;

        t0 = g_ticks;
        ok = build("gears.c", "gears.asm", "/bin/gears");
        t1 = g_ticks;
        expect_true("gears was built inside the machine, renderer and all", ok);
        printf("that took %d ticks\n", t1 - t0);

        if (ok) {
            ino = fs_lookup("/bin/gears");
            printf("/bin/gears is %d bytes, the loader's limit is %d\n",
                   fs_size(ino), ELF_MAX);
            expect_true("...and it fits under the loader's limit",
                        fs_size(ino) < ELF_MAX);

            // Freeing the intermediate: gears.asm is about 600 KB and the RAM
            // disk is 2 MB. Leaving it there is what makes the NEXT thing to
            // be compiled fail with a disk-full error that looks like a
            // compiler bug.
            fs_unlink("/src/gears.asm");

            puts("\n== 3c. run it ==\n");
            code = run_and_watch("/bin/gears", 0);
            printf("gears exited with %d\n", code);
            expect("every one of the program's own checks held", code, 0);
            expect_true("it got a window", g_saw_window >= 0);
            // "at least", not "at most": client_colours stops counting at
            // MAX_SEEN and returns, so a number equal to the cap means the
            // real one is somewhere above it.
            printf("%d distinct frames, at least %d colours\n",
                   g_frames_seen, g_max_colours);
            expect_true("...and drew more than one frame in it", g_frames_seen > 4);
            // Flat shading on three materials: the background, three gear
            // colours and a shade per face orientation. A wireframe or a
            // failed render would be far below this.
            expect_true("...lit, with a shade per face orientation",
                        g_max_colours > 20);
        }
    }

    // ============================================================
    // 4. more programs than the table has room for
    // ============================================================
    //
    // The process table is MAX_PROCS entries and a finished process still
    // occupies one. Without recycling, the machine can run MAX_PROCS programs
    // in its entire lifetime and then quietly stops -- which is exactly what
    // happened the first time this image was left respawning a program for a
    // screenshot: it ran ten times and the desktop went empty.
    //
    // "Quietly" is the part that matters. proc_spawn returned 0, nothing
    // faulted, nothing printed. So: run more programs than there are slots and
    // require every one of them to start AND to open its window.
    puts("\n== 4. running more programs than the process table holds ==\n");
    {
        long runs;
        long started;
        long allzero;
        long windows_before;
        runs = MAX_PROCS + 6;
        started = 0;
        allzero = 1;
        windows_before = 0;
        {
            long i;
            i = 0;
            while (i < WM_MAXWIN) { if (g_win[i].used) windows_before = windows_before + 1; i = i + 1; }
        }
        {
            long k;
            k = 0;
            while (k < runs) {
                long c;
                c = run_and_watch("/bin/winbad", 0);
                if (c != -2) started = started + 1;
                if (c != 0) allzero = 0;
                k = k + 1;
            }
        }
        printf("%d of %d runs started; %d table entries were recycled\n",
               started, runs, g_procs_recycled);
        expect("every run started, table full or not", started, runs);
        expect_true("...and every one of them passed its own checks", allzero);
        expect_true("...and the table did have to recycle", g_procs_recycled > 0);
        {
            long after;
            after = 0;
            {
                long i;
                i = 0;
                while (i < WM_MAXWIN) { if (g_win[i].used) after = after + 1; i = i + 1; }
            }
            // Twenty-two windows opened and closed. If any had leaked, the
            // window table would be full and the later runs would have failed
            // to open one -- which the exit codes above would have caught, but
            // this says it directly.
            expect("no window leaked across twenty-two runs", after, windows_before);
        }
    }

    // ============================================================
    // 5. what it cost
    // ============================================================
    puts("\n== 5. what it cost ==\n");
    printf("syscalls made: %d\n", g_syscalls);
    printf("processes that faulted: %d\n", g_proc_faults);
    expect("no process faulted", g_proc_faults, 0);
    printf("heap: %d pages mapped, %d bytes free\n", heap_pages, heap_bytes_free());
    printf("frames free: %d\n", mm_free_frames);

    if (g_fail) printf("\n%d CHECKS FAILED\n", g_fail);
    else puts("\nPASS: the machine compiled a graphical program and gave it a window\n");

    puts("\nOSWINTEST DONE\n");

    // Leave a picture on the screen. The program runs sixty frames and then
    // closes its own window, so a screenshot taken at an arbitrary moment
    // would catch an empty desktop about as often as not -- respawn it.
    //
    // Which is also the first thing on this machine that behaves like a
    // desktop rather than a test: a process ends, its window goes with it, and
    // something starts another one.
    puts("\nrunning wingl on a loop, for the screenshot\n");
    {
        char *av[2];
        long pid;
        av[0] = "/bin/wingl";
        pid = 0;
        for (;;) {
            proc_poll();
            if (!proc_alive()) pid = proc_spawn("/bin/wingl", 1, av, "/bin/wingl", "/");
            wm_present();
            thread_yield();
        }
    }
    cpu_halt_forever();
}

int main() {
    serial_init();
    g_fail = 0;

    puts("\nnano-os: a program this machine compiled, in a window\n");

    if (!fb_init(1024, 768)) { puts("fb_init failed\n"); for (;;) { } }
    if (!mm_init())          { puts("mm_init failed\n"); for (;;) { } }
    mm_protect_null();

    kbd_init();
    interrupts_init(100);
    thread_init();
    proc_init();

    thread_create((long)main_thread, 0, "main");
    sched_start();
    cpu_halt_forever();
    return 0;
}
