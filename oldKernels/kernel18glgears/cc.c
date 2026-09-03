// cc.c — the compiler, running as a program on this OS, checked properly.
//
// "It printed something and exited 0" is not a test of a compiler. A compiler
// that emitted an empty file would pass that, and so would one that emitted
// plausible-looking assembly with a wrong offset in it.
//
// The test here is a comparison against an answer produced somewhere else: the
// SAME compiler source, compiled by the same nano_cc, running on Linux, given
// the SAME input bytes. Its output is dumped over the serial line between two
// markers, and the Makefile diffs it byte for byte against the host's. If the
// two agree on 468 lines of x86-64, the compiler inside the OS is not
// approximately working, it is producing identical code.
//
// The input is a real file in the source tree (src/demo.c), embedded in this
// image by progs.s and written onto the RAM disk at boot, so both sides are
// definitely compiling the same thing. It includes a header by a RELATIVE name,
// which only resolves because a process has a working directory -- that is the
// second file the compiler has to open through the syscall boundary, and it is
// the one that would fail silently if path resolution were wrong.
//
// The failure cases matter as much: a missing input, and a C file with an
// error in it. A compiler that exits 0 on a broken program is worse than one
// that does not run.

#include "nano-kernel.h"
#include "nano-mm.h"
#include "nano-thread.h"
#include "nano-fs.h"
#include "nano-proc.h"
#include "nano-int.h"

extern long prog_cc_addr();
extern long prog_cc_size();
extern long prog_demo_addr();
extern long prog_demo_size();
extern long prog_util_addr();
extern long prog_util_size();

char g_buf[1024];

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

// Run /bin/cc with a working directory and up to three arguments, and return
// its exit code. -1 means it faulted.
long run_cc(char *cwd, char *a1, char *a2, char *a3) {
    char *av[4];
    long n;
    long pid;

    av[0] = "cc";
    n = 1;
    if (a1) { av[n] = a1; n = n + 1; }
    if (a2) { av[n] = a2; n = n + 1; }
    if (a3) { av[n] = a3; n = n + 1; }

    pid = proc_spawn("/bin/cc", n, av, "cc", cwd);
    if (!pid) { printf("SPAWN FAILED: %s\n", proc_reject); return -2; }
    return proc_wait(pid);
}

// Print a file to the serial line exactly as it is on disk. No formatting, no
// interpretation -- whatever comes out here is what the Makefile compares.
void dump(char *path) {
    long ino;
    long size;
    long off;

    ino = fs_lookup(path);
    if (!ino) { printf("dump: %s does not exist\n", path); return; }
    size = fs_size(ino);
    off = 0;
    while (off < size) {
        long n;
        long i;
        n = size - off;
        if (n > 1024) n = 1024;
        n = fs_read(ino, off, g_buf, n);
        if (n <= 0) break;
        i = 0;
        while (i < n) { putc(g_buf[i]); i = i + 1; }
        off = off + n;
    }
}

void main_thread(long unused) {
    long frames_at_start;
    long frames_after_first;
    long heap_at_start;
    long code;
    long ino;

    puts("scheduler running\n");

    if (!fs_format(4096, 192)) { puts("format failed\n"); cpu_halt_forever(); }
    fs_mkdir("/bin");
    fs_mkdir("/src");
    fs_mkdir("/out");
    install("/bin/cc",    prog_cc_addr(),   prog_cc_size());
    install("/src/demo.c", prog_demo_addr(), prog_demo_size());
    install("/src/util.h", prog_util_addr(), prog_util_size());

    // A file over the OLD 36 KiB ceiling is now on disk. Read one byte back
    // from near its end: an inode whose double-indirect path is wrong returns
    // a zero here rather than failing, so the file would look installed and be
    // unloadable.
    {
        long size;
        char probe[1];
        ino = fs_lookup("/bin/cc");
        size = fs_size(ino);
        printf("/bin/cc is %d bytes, past the old %d byte limit\n", size, (8 + 64) * 512);
        if (fs_read(ino, size - 1, probe, 1) != 1) puts("CANNOT READ THE LAST BYTE\n");
        else if ((probe[0] & 255) != (((char *)prog_cc_addr())[size - 1] & 255))
            puts("LAST BYTE CAME BACK WRONG\n");
        else puts("the last byte of it reads back correctly\n");
    }

    frames_at_start = mm_free_frames;
    heap_at_start = heap_pages;
    printf("%d frames free, %d heap pages, before the compiler runs\n",
           frames_at_start, heap_pages);

    // --- 1. compile a real program, from its own directory ---
    puts("\n-- cc demo.c demo.s, with /src as the working directory --\n");
    code = run_cc("/src", "demo.c", "/out/demo.s", 0);
    printf("cc exited with %d\n", code);
    if (code != 0) puts("COMPILE FAILED\n");

    ino = fs_lookup("/out/demo.s");
    if (!ino) puts("NO OUTPUT FILE\n");
    else printf("/out/demo.s is %d bytes\n", fs_size(ino));

    // --- 2. the comparison ---
    // Everything between the markers is compared with the host's output. The
    // markers are on their own lines so the Makefile can cut on them without
    // needing to know anything about assembly.
    puts("\n---8<--- demo.s begins\n");
    dump("/out/demo.s");
    puts("---8<--- demo.s ends\n");

    // The baseline for the leak check: after one run, so the kernel heap has
    // already grown to hold an ELF image and will not grow again.
    {
        long i;
        i = 0;
        while (i < 10) { proc_poll(); thread_yield(); i = i + 1; }
    }
    frames_after_first = mm_free_frames;

    // --- 3. compiling again over an existing output ---
    // The second output is the same length, so a truncate bug would not show.
    // Compile something SHORTER over it first, then the real thing again, and
    // require the final size to be the short one -- without O_TRUNC the tail
    // of the long output survives and the file is a splice of two runs.
    {
        long shortino;
        long longsize;
        long shortsize;

        longsize = fs_size(fs_lookup("/out/demo.s"));
        shortino = fs_create("/src/tiny.c");
        fs_write(shortino, 0, "int f() { return 1; }\n", 22);

        code = run_cc("/src", "tiny.c", "/out/demo.s", 0);
        shortsize = fs_size(fs_lookup("/out/demo.s"));
        printf("\nrecompiled a shorter file over a %d byte output: now %d bytes\n",
               longsize, shortsize);
        if (code != 0) puts("SECOND COMPILE FAILED\n");
        else if (shortsize >= longsize) puts("THE OUTPUT FILE WAS NOT TRUNCATED\n");
        else puts("the old output was truncated, not overwritten in place\n");
    }

    // --- 4. things that must fail ---
    puts("\n-- the failure cases --\n");

    code = run_cc("/src", "nosuchfile.c", "/out/x.s", 0);
    printf("missing input: exit %d\n", code);
    if (code == 0) puts("COMPILED A FILE THAT DOES NOT EXIST\n");

    {
        long bad;
        bad = fs_create("/src/broken.c");
        fs_write(bad, 0, "int main() { return zzz ( ; }\n", 30);
        code = run_cc("/src", "broken.c", "/out/broken.s", 0);
        printf("broken input: exit %d\n", code);
        if (code == 0) puts("ACCEPTED A BROKEN PROGRAM\n");
    }

    // A relative include with NO working directory: the header cannot be found
    // and the compiler must say so rather than carry on without it.
    code = run_cc("/", "/src/demo.c", "/out/nocwd.s", 0);
    printf("relative include with the wrong cwd: exit %d\n", code);
    if (code == 0) puts("FOUND A HEADER IT SHOULD NOT HAVE\n");

    // --- 5. the frames ---
    //
    // Every one of the compiler's 19 MB of pages has to come back. A loader
    // that leaks an address space looks perfect for the first few runs and
    // then stops being able to start anything.
    //
    // The baseline is taken AFTER the first run, not before it, and the
    // difference is worth being precise about rather than tolerating with a
    // fudge factor. The first spawn kmallocs a 131 KB buffer to read the ELF
    // file into; the kernel heap has no page that big, so it grows -- and a
    // heap never gives its pages back, by design. That is 33 frames that leave
    // and do not return, once. It is not an address-space leak, and the way to
    // tell the two apart is that this one does not happen again: the second
    // spawn finds the block already in the free list.
    //
    // So the test is "no frames move across runs 2 to 6", which still fails
    // loudly if a single address space is not reclaimed, and the heap growth is
    // printed rather than hidden.
    {
        long i;
        i = 0;
        while (i < 30) { proc_poll(); thread_yield(); i = i + 1; }
    }
    printf("\n%d frames free before the first run, %d after all six\n",
           frames_at_start, mm_free_frames);
    printf("the kernel heap grew from %d pages to %d to hold a %d byte ELF image\n",
           heap_at_start, heap_pages, prog_cc_size());
    if (frames_at_start - mm_free_frames != heap_pages - heap_at_start)
        printf("UNEXPLAINED FRAMES: %d moved, %d of them heap growth\n",
               frames_at_start - mm_free_frames, heap_pages - heap_at_start);
    if (mm_free_frames == frames_after_first) puts("every frame came back\n");
    else printf("FRAMES LEAKED: %d over five runs\n", frames_after_first - mm_free_frames);

    printf("%d syscalls, %d context switches, %d of them across address spaces\n",
           g_syscalls, g_switches, g_space_switches);

    puts("\nCCTEST DONE\n");
    cpu_halt_forever();
}

int main() {
    serial_init();
    vga_clear();
    kbd_init();
    interrupts_init(100);
    if (!mm_init()) { puts("mm_init failed\n"); cpu_halt_forever(); }
    mm_protect_null();
    thread_init();
    proc_init();

    puts("\nnano-os: the compiler as a program\n");
    thread_create((long)main_thread, 0, "main");
    sched_start();
    cpu_halt_forever();
    return 0;
}
