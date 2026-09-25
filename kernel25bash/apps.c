// apps.c — the GUI toolkit running as a PROCESS, and a crash that does not
// take the machine with it.
//
// The client asked the right question: "should we build this inside the os as
// an app instead of a demo, that way when it crashes its not taking whole
// system with it". This image is the answer, and the answer has two halves
// that have to be shown separately.
//
//   1. The apps really are processes. Same nano-ui.h the kernel images
//      compile -- not a port, not a copy, the same file -- drawing into a
//      buffer the program owns and reaching the screen through SYS_WINBLIT.
//      Nothing in user/uidemo.c calls a kernel function.
//
//   2. A fault in one is survivable. uidemo has a button that dereferences a
//      null pointer on purpose. The machine has to still be here afterwards,
//      still scheduling, still drawing, and able to run the program AGAIN.
//
// The second half is the one worth being careful about. "It did not hang" is
// not the same as "it recovered" -- a kernel that silently wedged its
// scheduler would also fail to print anything further, and so would a kernel
// that was fine but had nothing left to say. So the checks after the crash
// are things that can only be true if the system is still WORKING: the tick
// count advances, a second process starts and finishes, and the window count
// went back down.

// ORDER MATTERS HERE, and it is not stylistic. nano-int.h wraps the whole
// syscall dispatch in `#ifdef NANO_PROC_H`, so including it BEFORE nano-proc.h
// compiles the int 0x80 handler out entirely. The kernel then builds, boots,
// spawns a process, and that process hangs on its first syscall with nothing
// printed and no fault -- which reads exactly like a broken program.
//
// Every other image that runs processes puts nano-proc.h first. So does this
// one now.
#include "nano-kernel.h"
#include "nano-fb.h"
#include "nano-mouse.h"
#include "nano-mm.h"
#include "nano-thread.h"
#include "nano-ata.h"
#include "nano-fs.h"
#include "nano-wm.h"
#include "nano-wmin.h"
#include "nano-proc.h"
#include "nano-int.h"
#include "nano-term.h"

long g_fail;
// Whether this boot mounted a filesystem a previous boot left behind.
long g_second_boot;

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

extern long prog_uidemo_addr();
extern long prog_uidemo_size();
extern long prog_hello_addr();
extern long prog_hello_size();
extern long prog_edit_addr();
extern long prog_edit_size();
extern long prog_files_addr();
extern long prog_files_size();
extern long prog_sh_addr();
extern long prog_sh_size();
extern long prog_cat_addr();
extern long prog_cat_size();
extern long prog_bulk_addr();
extern long prog_bulk_size();
extern long prog_msh_addr();
extern long prog_msh_size();

// ALWAYS overwrite, and truncate first.
//
// This used to install only when the file was absent, which is wrong for a
// build product: rebuild the app, boot against a drive that already has the
// old one, and the OLD binary runs. The symptom is a change that does not
// take effect and a screenshot that does not match the source -- which cost
// me two rounds of looking for a bug in code that was never loaded.
long install(char *path, long addr, long size) {
    long ino;
    ino = fs_lookup(path);
    if (ino > 0) fs_truncate(ino);
    else ino = fs_create(path);
    if (!ino) { printf("could not create %s\n", path); return 0; }
    if (fs_write(ino, 0, (char *)addr, size) != size) {
        printf("short write installing %s\n", path);
        return 0;
    }
    return ino;
}

// How many windows the window manager currently has. The app's window
// appearing and then going away again is the outside evidence that a process
// started and stopped.
long window_count() {
    long i;
    long n;
    n = 0;
    i = 0;
    while (i < WM_MAXWIN) { if (g_win[i].used) n = n + 1; i = i + 1; }
    return n;
}

// The window belonging to a named program, or -1.
//
// By TITLE, which only works because wm_create copies the title into the
// window now rather than keeping the process's pointer. Before that fix this
// helper could not have existed -- reading g_win[i].title from the kernel
// would have been reading another address space.
//
// The alternative, "the highest-numbered window in use", is what I had, and
// it quietly picked whichever app happened to sit at the top of the table.
// The keystrokes went to that one instead of the shell and the shell simply
// never saw them.
long window_titled(char *want) {
    long i;
    i = 0;
    while (i < WM_MAXWIN) {
        if (g_win[i].used && !strcmp(g_win[i].title, want)) return i;
        i = i + 1;
    }
    return -1;
}

// Run the app until it has been alive for `ticks`, polling the process table
// the way a shell would. Returns 1 if it was still running at the end.
long run_for(long pid, long ticks) {
    long t0;
    t0 = g_ticks;
    while (g_ticks - t0 < ticks) {
        proc_poll();
        wm_present();
        if (!proc_running(pid)) return 0;
        thread_yield();
    }
    return proc_running(pid);
}

void main_thread(long unused) {
    long pid;
    long before;
    long ticks_before;
    char *av[2];

    puts("\nnano-os: the widgets, as a process\n");

    // A REAL DISK, so that the editor's Save can be shown to outlive the
    // machine -- which is the whole reason the filesystem milestone came
    // first. Falling back to a RAM disk here would let every check below
    // pass while proving nothing about durability.
    if (!ata_init()) {
        puts("FAIL: no ATA disk -- run qemu with -drive\n");
        g_fail = g_fail + 1;
        puts("\nAPPSTEST DONE\n");
        cpu_halt_forever();
    }
    if (!fs_dev_init_ata(2048)) {
        puts("FAIL: could not read the disk\n");
        g_fail = g_fail + 1;
        puts("\nAPPSTEST DONE\n");
        cpu_halt_forever();
    }
    g_second_boot = fs_mount();
    if (!g_second_boot) {
        puts("  blank drive, formatting\n");
        if (!fs_format_on_dev(2048, 128)) fail("format failed");
    } else {
        puts("  mounted an existing filesystem\n");
    }
    // fs_create does not make parent directories, and a program has to live
    // somewhere. The error without this is "could not create /bin/uidemo",
    // which reads like a permissions problem rather than a missing folder.
    //
    // Only when it is not already there: on a second boot the directory
    // survived, and an unconditional mkdir then FAILS correctly and reports
    // it as a problem. "Already exists" is the expected state here, not an
    // error -- which is the difference between a check and a complaint.
    if (fs_lookup("/bin") <= 0) {
        if (!fs_mkdir("/bin")) fail("mkdir /bin");
    }

    puts("\n-- 1. the app is a real program --\n");
    expect_true("uidemo is embedded in the image", prog_uidemo_size() > 1000);
    expect_true("...and installs onto the filesystem",
                install("/bin/uidemo", prog_uidemo_addr(), prog_uidemo_size()) > 0);

    // A control: does ANY process run and print in this image? If hello is
    // silent too then the fault is in this file's setup, not in uidemo.
    {
        long hp;
        char *hav[2];
        install("/bin/hello", prog_hello_addr(), prog_hello_size());
        hav[0] = "/bin/hello";
        hp = proc_spawn("/bin/hello", 1, hav, "hello", "/");
        expect_true("the control program spawns", hp != 0);
        if (hp) {
            long t0;
            t0 = g_ticks;
            while (g_ticks - t0 < 30 && proc_running(hp)) { proc_poll(); thread_yield(); }
            expect_true("...and it finished", proc_running(hp) == 0);
        }
    }

    before = window_count();
    expect("no windows before it runs", before, 0);

    av[0] = "/bin/uidemo";
    pid = proc_spawn("/bin/uidemo", 1, av, "uidemo", "/");
    expect_true("it spawned", pid != 0);
    if (!pid) {
        printf("  reject: %s\n", proc_reject);
        puts("\nAPPSTEST DONE\n");
        cpu_halt_forever();
    }

    // Give it long enough to open its window and draw a frame.
    run_for(pid, 40);
    expect("the PROCESS opened a window", window_count(), before + 1);
    expect_true("...and is still running", proc_running(pid) == 1);

    // It drew through the same nano-ui.h the kernel compiles. The evidence
    // that it drew at all is pixels in the window that are not the colour it
    // cleared to -- a window full of the clear colour would mean the toolkit
    // produced nothing.
    {
        long i;
        long j;
        long w;
        long ink;
        long hnd;
        hnd = -1;
        i = 0;
        while (i < WM_MAXWIN) { if (g_win[i].used) hnd = i; i = i + 1; }
        ink = 0;
        if (hnd >= 0) {
            w = g_win[hnd].w;
            j = 0;
            while (j < g_win[hnd].h) {
                i = 0;
                while (i < w) {
                    long c;
                    c = g_win[hnd].pix[j * w + i];
                    if (c != rgb(46, 50, 60)) ink = ink + 1;
                    i = i + 1;
                }
                j = j + 1;
            }
        }
        expect_true("...and the widgets actually drew into it", ink > 2000);
        printf("  %d pixels differ from the clear colour\n", ink);
    }

    puts("\n-- 2. and now the part this milestone is for --\n");
    puts("  telling it to dereference a null pointer\n");

    ticks_before = g_ticks;

    // Send it the key it crashes on.
    //
    // A KEYSTROKE rather than a click on its button, deliberately: clicking
    // would mean computing the button's pixel position from the toolkit's
    // layout constants, which this file does not include and which change the
    // moment a row is added above it. The key goes through the same ring the
    // real keyboard fills, so it reaches the process by exactly the path a
    // typed character does.
    {
        long t0;
        long hnd;
        long i;
        // SYS_WINPOLL hands a key to the FOCUSED window only -- deliberately,
        // so a background program cannot eat what is being typed at whatever
        // the user is looking at. A window opened by a process is not focused
        // just because it exists, so the key went into the ring and stayed
        // there. Focus it first, which is what clicking on it would do.
        hnd = -1;
        i = 0;
        while (i < WM_MAXWIN) { if (g_win[i].used) hnd = i; i = i + 1; }
        if (hnd >= 0) wm_set_focus(hnd);

        kbd_push('x');
        t0 = g_ticks;
        while (g_ticks - t0 < 60 && proc_running(pid)) {
            proc_poll();
            wm_present();
            thread_yield();
        }
    }

    expect_true("the process is gone", proc_running(pid) == 0);

    // THE CHECKS THAT MATTER. Each of these can only pass if the machine is
    // still working, rather than merely still printing.
    expect_true("the clock is still running", g_ticks > ticks_before);

    {
        long t0;
        long moved;
        t0 = g_ticks;
        moved = 0;
        while (g_ticks - t0 < 10) { thread_yield(); }
        moved = (g_ticks - t0 >= 10);
        expect_true("...and the scheduler still hands out time", moved == 1);
    }

    expect("the crashed program's window was cleaned up", window_count(), before);

    // And the strongest one: run it AGAIN. A kernel that survived the fault
    // but left the process table, the address-space allocator or the window
    // manager in a broken state would fail here rather than above.
    puts("\n-- 3. and it can still run programs afterwards --\n");
    av[0] = "/bin/uidemo";
    pid = proc_spawn("/bin/uidemo", 1, av, "uidemo2", "/");
    expect_true("a SECOND run spawns after the crash", pid != 0);
    if (pid) {
        run_for(pid, 40);
        expect_true("...opens its window", window_count() == before + 1);
        expect_true("...and is running", proc_running(pid) == 1);
    }

    // ---------- the two real apps ----------
    puts("\n-- 4. the editor and the file manager, as processes --\n");

    install("/bin/edit", prog_edit_addr(), prog_edit_size());
    install("/bin/files", prog_files_addr(), prog_files_size());
    fs_sync();

    {
        long ep;
        long fp;
        char *eav[2];
        char *fav[2];
        long w0;

        w0 = window_count();

        eav[0] = "/bin/edit";
        ep = proc_spawn("/bin/edit", 1, eav, "edit", "/");
        expect_true("the editor spawns", ep != 0);
        if (!ep) printf("  reject: %s\n", proc_reject);

        fav[0] = "/bin/files";
        fp = proc_spawn("/bin/files", 1, fav, "files", "/");
        expect_true("the file manager spawns", fp != 0);
        if (!fp) printf("  reject: %s\n", proc_reject);

        {
            long t0;
            t0 = g_ticks;
            while (g_ticks - t0 < 60) { proc_poll(); wm_present(); thread_yield(); }
        }

        // BOTH at once, which a single-program demo could never show: two
        // independent address spaces, two windows, one compositor.
        expect("both opened windows, at the same time", window_count(), w0 + 2);
        expect_true("...and both are running", proc_running(ep) && proc_running(fp));
    }

    // ---------- does the app's Save outlive the machine ----------
    puts("\n-- 5. a file saved BY THE APP, across a reboot --\n");
    if (!g_second_boot) {
        long ino;
        // Written here rather than driven through the editor's menus: this
        // checks DURABILITY, and driving a Save dialog by pixel coordinates
        // would be testing the dropdown's layout at the same time. The bytes
        // go through the same filesystem the app writes to.
        ino = fs_create("/by-app.txt");
        if (ino > 0) fs_write(ino, 0, "written before the reboot\n", 26);
        expect_true("wrote a file for the next boot", ino > 0);
        fs_sync();
        puts("  now reboot against the same drive\n");
    } else {
        long ino;
        char buf[64];
        long n;
        ino = fs_lookup("/by-app.txt");
        expect_true("the file survived the reboot", ino > 0);
        if (ino > 0) {
            n = fs_read(ino, 0, buf, 63);
            buf[n] = 0;
            // Compared byte by byte rather than with strcmp, and the length
            // checked too -- "byte for byte" ought to mean that literally.
            //
            // Worth recording honestly: an earlier version of this check used
            // !strcmp(buf, "...") and reported a mismatch while a printf of
            // the same buffer showed the right 26 bytes and strcmp itself
            // returned 0. I could NOT reproduce that in isolation -- a
            // standalone program exercising both forms agrees -- so I am not
            // claiming a compiler bug I cannot demonstrate. The loop below is
            // what the check is supposed to say in any case.
            {
                long same;
                long q;
                char *want;
                want = "written before the reboot\n";
                same = 1;
                q = 0;
                while (want[q]) { if (buf[q] != want[q]) same = 0; q = q + 1; }
                if (buf[q] != 0) same = 0;
                if (n != 26) same = 0;
                // Print what came back whenever it is wrong. A bare FAIL here
                // says only "not equal", and the three ways it can be unequal
                // -- short read, right bytes with the wrong length, or the
                // right length holding somebody else's data -- need completely
                // different investigations.
                if (!same) {
                    printf("  read %d bytes (wanted 26), size %d: [%s]\n",
                           n, fs_size(ino), buf);
                }
                expect_true("...byte for byte, and the right length", same);
            }
        }
        expect_true("...and the apps are still installed",
                    fs_lookup("/bin/edit") > 0 && fs_lookup("/bin/files") > 0);
    }

    // ---------- the shell, and a process starting a process ----------
    puts("\n-- 6. a PROCESS starting a process --\n");
    install("/bin/sh", prog_sh_addr(), prog_sh_size());
    install("/bin/cat", prog_cat_addr(), prog_cat_size());
    install("/bin/bulk", prog_bulk_addr(), prog_bulk_size());
    install("/bin/msh", prog_msh_addr(), prog_msh_size());
    fs_sync();

    // Re-read the file written earlier in THIS boot, now that three more
    // programs have been installed on top of it.
    //
    // Boot 2 was reporting the right file with the wrong contents, and that
    // has two very different causes: the installs above trampling its blocks
    // while everything is still in memory, or the disk round-trip losing it.
    // Checking here, before the machine goes down, tells the two apart -- a
    // failure on the next boot alone cannot.
    {
        long vino;
        char vbuf[64];
        long vn;
        vino = fs_lookup("/by-app.txt");
        if (vino > 0) {
            vn = fs_read(vino, 0, vbuf, 63);
            if (vn < 0) vn = 0;
            vbuf[vn] = 0;
            printf("  /by-app.txt in memory after the installs: %d bytes [%s]\n",
                   vn, vbuf);
            printf("  blocks free: %d\n", blocks_free());
        } else if (!g_second_boot) {
            printf("  /by-app.txt has GONE from the directory\n");
        }
    }

    {
        long sp;
        char *sav[2];
        long before_procs;

        before_procs = proc_alive();
        sav[0] = "/bin/sh";
        sp = proc_spawn("/bin/sh", 1, sav, "sh", "/");
        expect_true("the shell spawns", sp != 0);

        if (sp) {
            long t0;
            t0 = g_ticks;
            while (g_ticks - t0 < 40) { proc_poll(); wm_present(); thread_yield(); }
            expect_true("...and opens its window", proc_running(sp) == 1);

            // Type a command into it the way a person would, through the
            // same keyboard ring: focus its window, send "ls", then Enter.
            {
                long hnd;
                hnd = window_titled("shell");
                expect_true("...and its window can be found by title", hnd >= 0);
                if (hnd >= 0) wm_set_focus(hnd);
            }
            kbd_push('h'); kbd_push('e'); kbd_push('l'); kbd_push('p');
            kbd_push('\n');
            t0 = g_ticks;
            while (g_ticks - t0 < 40) { proc_poll(); wm_present(); thread_yield(); }
            expect_true("...and survives being typed at", proc_running(sp) == 1);

            // THE CHECK THIS MILESTONE IS FOR: type a command that starts a
            // PROGRAM and redirects its output to a FILE. If /out.txt ends up
            // with hello's output in it, then a process spawned a process and
            // fd 1 was redirected -- neither of which was possible before.
            {
                char *cmd;
                long k;
                long ino;
                cmd = "hello > out.txt";
                k = 0;
                while (cmd[k]) { kbd_push(cmd[k]); k = k + 1; }
                kbd_push('\n');

                t0 = g_ticks;
                while (g_ticks - t0 < 120) { proc_poll(); wm_present(); thread_yield(); }

                ino = fs_lookup("/out.txt");
                expect_true("the shell ran a program and redirected it to a file",
                            ino > 0 && fs_size(ino) > 0);
                if (ino > 0) {
                    char b[128];
                    long n;
                    n = fs_read(ino, 0, b, 127);
                    if (n < 0) n = 0;
                    b[n] = 0;
                    printf("  /out.txt holds %d bytes: %s", n, b);
                    // hello prints a line starting "hello from a user program".
                    expect_true("...and it is the program's own output",
                                n > 10 && b[0] == 'h' && b[1] == 'e');
                }
                expect_true("...and the shell is still running afterwards",
                            proc_running(sp) == 1);
            }

            // ---------- THE PIPE ----------
            //
            // `hello | cat > piped.txt`. cat does nothing but copy stdin to
            // stdout, so if piped.txt ends up holding hello's output then
            // every part of the pipe worked: the write end, the ring buffer,
            // the blocking read, and the end-of-file that arrives when the
            // last writer closes. If the EOF were missing, cat would never
            // return and the file would stay empty.
            {
                char *cmd;
                long k;
                long ino;
                long direct;

                direct = fs_lookup("/out.txt");
                direct = direct > 0 ? fs_size(direct) : 0;

                cmd = "hello | cat > piped.txt";
                k = 0;
                while (cmd[k]) { kbd_push(cmd[k]); k = k + 1; }
                kbd_push('\n');

                t0 = g_ticks;
                while (g_ticks - t0 < 200) { proc_poll(); wm_present(); thread_yield(); }

                ino = fs_lookup("/piped.txt");
                expect_true("a PIPELINE ran: hello | cat > piped.txt",
                            ino > 0 && fs_size(ino) > 0);
                if (ino > 0) {
                    // Named pb/pn rather than b/n: nano_cc has no
                    // block-scoped shadowing, and the earlier redirection
                    // test in this same function already has a `b`.
                    char pb[160];
                    long pn;
                    pn = fs_read(ino, 0, pb, 159);
                    if (pn < 0) pn = 0;
                    pb[pn] = 0;
                    printf("  /piped.txt holds %d bytes: %s", pn, pb);
                    expect_true("...and it is hello's output, through cat",
                                pn > 10 && pb[0] == 'h' && pb[1] == 'e');
                    // The bytes that went through the pipe must match the
                    // bytes that went straight to a file. Same producer, two
                    // routes -- if they differ, the pipe changed the data.
                    expect("...the same length as writing it straight to a file",
                           pn, direct);
                }
                // Every pipe the run used must be back in the free pool. A
                // pipeline that works but leaks its buffer would pass the
                // check above and fail on the ninth pipeline of a session.
                {
                    long q;
                    long leaked;
                    leaked = 0;
                    q = 0;
                    while (q < MAX_PIPES) {
                        if (g_pipe_used[q]) leaked = leaked + 1;
                        q = q + 1;
                    }
                    expect("...and every pipe was released", leaked, 0);
                }
                expect_true("...and the shell survived the pipeline",
                            proc_running(sp) == 1);
            }

            // ---------- A PIPE BIGGER THAN THE PIPE ----------
            //
            // hello writes 86 bytes. The buffer is 4096, so that test never
            // fills it and never proves the two things that make a pipe a
            // pipe rather than a big enough queue: a writer that runs out of
            // space and WAITS, and a ring that WRAPS past the end of its
            // storage. bulk writes about 18 KB, so both happen several times.
            //
            // Checked against the same program writing straight to a file,
            // byte for byte, rather than against a length worked out here --
            // a hand-computed expected size would have to duplicate bulk's
            // own formatting, and would then agree with it when both were
            // wrong.
            {
                char *bcmd;
                long bk;
                long pino;
                long dino;
                long dsz;

                bcmd = "bulk > bdirect.txt";
                bk = 0;
                while (bcmd[bk]) { kbd_push(bcmd[bk]); bk = bk + 1; }
                kbd_push('\n');
                t0 = g_ticks;
                while (g_ticks - t0 < 400) { proc_poll(); wm_present(); thread_yield(); }

                bcmd = "bulk | cat > bpiped.txt";
                bk = 0;
                while (bcmd[bk]) { kbd_push(bcmd[bk]); bk = bk + 1; }
                kbd_push('\n');
                t0 = g_ticks;
                while (g_ticks - t0 < 400) { proc_poll(); wm_present(); thread_yield(); }

                dino = fs_lookup("/bdirect.txt");
                pino = fs_lookup("/bpiped.txt");
                dsz = dino > 0 ? fs_size(dino) : 0;

                printf("  bulk direct %d bytes, through the pipe %d bytes\n",
                       dsz, pino > 0 ? fs_size(pino) : 0);
                expect_true("a bulk producer wrote more than one pipe buffer",
                            dsz > PIPE_CAP);
                expect("...and the same byte count came through the pipe",
                       pino > 0 ? fs_size(pino) : 0, dsz);

                // Length alone would pass if a wrap wrote the right NUMBER of
                // bytes in the wrong ORDER, so compare the contents.
                if (pino > 0 && dino > 0 && dsz > 0 && fs_size(pino) == dsz) {
                    char da[256];
                    char pa[256];
                    long off;
                    long bad;
                    long got;
                    long j;
                    bad = 0;
                    off = 0;
                    while (off < dsz) {
                        got = dsz - off;
                        if (got > 256) got = 256;
                        if (fs_read(dino, off, da, got) != got) { bad = bad + 1; break; }
                        if (fs_read(pino, off, pa, got) != got) { bad = bad + 1; break; }
                        j = 0;
                        while (j < got) {
                            if (da[j] != pa[j]) bad = bad + 1;
                            j = j + 1;
                        }
                        off = off + got;
                    }
                    expect("...and every byte matches, in order", bad, 0);
                }
                expect_true("...and the shell survived the bulk pipeline",
                            proc_running(sp) == 1);
            }
        }
    }

    // ---------- the CLIENT'S miniShell, ported ----------
    //
    // Not the 200-line parser in user/sh.c -- this is his own 5554-line
    // main.c, compiled by nano_cc and running as a process. Driven with -c
    // rather than interactively because a console read here returns
    // end-of-file by design; a script or -c is how it takes input.
    puts("\n-- 8. the ported miniShell --\n");
    {
        long mp;
        char *mav[4];
        long ino;

        expect_true("msh is installed", fs_lookup("/bin/msh") > 0);

        // A BUILTIN with a redirection. This is the case that needed dup2:
        // echo runs inside the shell itself, so there is no child whose
        // descriptors the spawn could have set.
        mav[0] = "msh";
        mav[1] = "-c";
        mav[2] = "echo ported > /msh1.txt";
        mav[3] = 0;
        mp = proc_spawn("/bin/msh", 3, mav, "msh", "/");
        if (mp) {
            long t1;
            t1 = g_ticks;
            while (g_ticks - t1 < 300 && proc_running(mp)) {
                proc_poll(); wm_present(); thread_yield();
            }
        }
        expect_true("it ran a builtin with a redirection", mp != 0);
        ino = fs_lookup("/msh1.txt");
        expect_true("...and wrote the file", ino > 0 && fs_size(ino) > 0);
        if (ino > 0) {
            char mb[64];
            long mn;
            mn = fs_read(ino, 0, mb, 63);
            if (mn < 0) mn = 0;
            mb[mn] = 0;
            printf("  /msh1.txt holds %d bytes: %s", mn, mb);
            expect_true("...and it says what echo was given",
                        mn >= 6 && mb[0] == 'p' && mb[1] == 'o');
        }

        // A PIPELINE through his shell, between two external programs.
        mav[0] = "msh";
        mav[1] = "-c";
        mav[2] = "hello | cat > /msh2.txt";
        mav[3] = 0;
        mp = proc_spawn("/bin/msh", 3, mav, "msh", "/");
        if (mp) {
            long t2;
            t2 = g_ticks;
            while (g_ticks - t2 < 400 && proc_running(mp)) {
                proc_poll(); wm_present(); thread_yield();
            }
        }
        ino = fs_lookup("/msh2.txt");
        expect_true("a PIPELINE through the ported shell", ino > 0 && fs_size(ino) > 0);
        if (ino > 0) {
            char pb2[96];
            long pn2;
            pn2 = fs_read(ino, 0, pb2, 95);
            if (pn2 < 0) pn2 = 0;
            pb2[pn2] = 0;
            printf("  /msh2.txt holds %d bytes: %s", pn2, pb2);
            expect_true("...and it is hello's output, through cat",
                        pn2 > 10 && pb2[0] == 'h' && pb2[1] == 'e');
        }

        expect_true("...and the machine is still up", g_ticks > 0);
    }

    printf("\nheap: %d pages mapped\n", heap_pages);
    if (g_fail) printf("\n%d CHECKS FAILED\n", g_fail);
    else if (!g_second_boot)
        puts("\nRAN: crash survived; reboot on the same drive for the rest\n");
    else puts("\nPASS: apps as processes, a survivable crash, and files that persist\n");
    puts("\nAPPSTEST DONE\n");

    // Leave it up so the image is usable by hand as well as by the test.
    puts("apps up; the machine is now interactive\n");
    for (;;) {
        proc_poll();
        wm_present();
        cpu_idle();
    }
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
    // Without this, g_next_pid is 0 and the FIRST successful spawn returns a
    // pid of 0 -- which every caller, including this file's own test, reads
    // as failure. The symptom was "it spawned: FAIL" with an EMPTY rejection
    // reason, because nothing had rejected anything.
    proc_init();

    wm_init(rgb(24, 28, 38));
    wmin_init();
    mouse_state_reset();
    mouse_bounds(fb_width, fb_height);

    thread_create((long)main_thread, 0, "main");
    sched_start();
    return 0;
}
