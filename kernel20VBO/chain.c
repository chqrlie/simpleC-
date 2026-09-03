// chain.c — source to running program, entirely inside the machine.
//
// The OS has had a compiler for a while. This is the test for the other half:
// /bin/as, the assembler, so that a C file on the RAM disk can become an ELF on
// the RAM disk and then a process, with nothing outside the machine involved.
//
//     cc --minimal --nasm --bss --kernel prog.c prog.asm
//     as prog.asm /bin/prog -b 0x8000000000
//     exec /bin/prog
//
// "It ran" is not the test. The binary the OS assembles is dumped over the
// serial line as hex and compared, byte for byte, against the binary the same
// assembler produces on Linux from the same input. Identical output from two
// machines is a much narrower claim than a program that happens to work.
//
// The exit code is the second half of it: 33 is computed by the program at run
// time (1..10 summed, minus 22) rather than being a constant sitting in the
// file, so a loader that transferred control to the wrong place cannot produce
// it by accident.
//
// And the failure cases, because an assembler that accepts everything is worse
// than one that runs nothing: an unknown mnemonic, a missing input file, and an
// output the program itself could not have written.

#include "nano-kernel.h"
#include "nano-mm.h"
#include "nano-thread.h"
#include "nano-fs.h"
#include "nano-proc.h"
#include "nano-int.h"

extern long prog_cc_addr();
extern long prog_cc_size();
extern long prog_as_addr();
extern long prog_as_size();
extern long prog_prog_addr();
extern long prog_prog_size();

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

// Spawn a program with up to five arguments and wait. -1 means it faulted.
long run5(char *path, char *cwd, char *a1, char *a2, char *a3) {
    char *av[8];
    long n;
    long pid;

    av[0] = path;
    n = 1;
    if (a1) { av[n] = a1; n = n + 1; }
    if (a2) { av[n] = a2; n = n + 1; }
    if (a3) { av[n] = a3; n = n + 1; }

    pid = proc_spawn(path, n, av, path, cwd);
    if (!pid) { printf("SPAWN FAILED %s: %s\n", path, proc_reject); return -2; }
    return proc_wait(pid);
}

// The compiler's flags do not fit in run5's five slots, so this one is spelled
// out rather than made general. A variadic spawn helper would be a nicer thing
// to have and a worse thing to debug.
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

// Print a file as hex, 32 bytes to a line. The Makefile compares this against
// the same bytes produced on the host; anything that reformats it here has to
// be reproducible there, so it is deliberately the dullest possible encoding.
void dump_hex(char *path) {
    long ino;
    long size;
    long off;
    char *digits;

    digits = "0123456789abcdef";
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
        while (i < n) {
            long b;
            b = g_buf[i] & 255;
            putc(digits[(b >> 4) & 15]);
            putc(digits[b & 15]);
            if (((off + i) % 32) == 31) putc('\n');
            i = i + 1;
        }
        off = off + n;
    }
    if ((size % 32) != 0) putc('\n');
}

void main_thread(long unused) {
    long frames_at_start;
    long code;
    long ino;

    puts("scheduler running\n");

    if (!fs_format(4096, 192)) { puts("format failed\n"); cpu_halt_forever(); }
    fs_mkdir("/bin");
    fs_mkdir("/src");
    install("/bin/cc", prog_cc_addr(), prog_cc_size());
    install("/bin/as", prog_as_addr(), prog_as_size());
    install("/src/prog.c", prog_prog_addr(), prog_prog_size());

    frames_at_start = mm_free_frames;

    // --- 1. compile ---
    puts("\n-- cc prog.c prog.asm --\n");
    code = run_cc("/src", "prog.c", "prog.asm");
    printf("cc exited with %d\n", code);
    if (code != 0) puts("COMPILE FAILED\n");
    ino = fs_lookup("/src/prog.asm");
    if (!ino) puts("NO ASSEMBLY OUTPUT\n");
    else printf("/src/prog.asm is %d bytes\n", fs_size(ino));

    // --- 2. assemble ---
    puts("\n-- as prog.asm /bin/prog --\n");
    code = run_as("/src", "prog.asm", "/bin/prog");
    printf("as exited with %d\n", code);
    if (code != 0) puts("ASSEMBLE FAILED\n");

    ino = fs_lookup("/bin/prog");
    if (!ino) puts("NO BINARY\n");
    else {
        char hdr[64];
        printf("/bin/prog is %d bytes\n", fs_size(ino));
        // Look at it as a file before trying to run it. A loader that refuses
        // it would say so, but "the assembler produced something that is not an
        // ELF" and "the loader is broken" are different problems and it is
        // worth being able to tell them apart from the output alone.
        if (fs_read(ino, 0, hdr, 64) != 64) puts("CANNOT READ THE HEADER\n");
        else if ((hdr[0] & 255) != 0x7F || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F')
            puts("NOT AN ELF FILE\n");
        else puts("it is an ELF file\n");
    }

    // --- 3. the comparison ---
    puts("\n---8<--- prog begins\n");
    dump_hex("/bin/prog");
    puts("---8<--- prog ends\n");

    // --- 4. run it ---
    puts("\n-- exec /bin/prog --\n");
    code = run5("/bin/prog", "/", 0, 0, 0);
    printf("prog exited with %d\n", code);
    if (code == 33) puts("the exit code was computed at run time, and it is right\n");
    else puts("WRONG EXIT CODE\n");

    // --- 5. the failure cases ---
    puts("\n-- the failure cases --\n");

    {
        long bad;
        bad = fs_create("/src/bad.asm");
        fs_write(bad, 0, "_start:\n    sqrtps xmm0, xmm1\n    ret\n", 37);
        code = run_as("/src", "bad.asm", "/src/bad.out");
        printf("unknown mnemonic: exit %d\n", code);
        if (code == 0) puts("ASSEMBLED AN INSTRUCTION IT DOES NOT KNOW\n");
    }

    code = run_as("/src", "nosuchfile.asm", "/src/x.out");
    printf("missing input: exit %d\n", code);
    if (code == 0) puts("ASSEMBLED A FILE THAT DOES NOT EXIST\n");

    {
        // A reservation with no count. The label would otherwise be defined at
        // an address nothing reserved space for.
        long bad;
        bad = fs_create("/src/noc.asm");
        fs_write(bad, 0, "_start:\n    ret\nsection .bss\nbuf resb\n", 38);
        code = run_as("/src", "noc.asm", "/src/noc.out");
        printf("resb with no count: exit %d\n", code);
        if (code == 0) puts("ACCEPTED A RESERVATION WITH NO SIZE\n");
    }

    // --- 6. the frames ---
    {
        long i;
        i = 0;
        while (i < 30) { proc_poll(); thread_yield(); i = i + 1; }
    }
    printf("\n%d frames free before, %d after\n", frames_at_start, mm_free_frames);
    printf("%d syscalls, %d context switches, %d across address spaces\n",
           g_syscalls, g_switches, g_space_switches);

    puts("\nCHAINTEST DONE\n");
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

    puts("\nnano-os: source to running program, inside the machine\n");
    thread_create((long)main_thread, 0, "main");
    sched_start();
    cpu_halt_forever();
    return 0;
}
