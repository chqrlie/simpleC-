// elf.c — loading and running programs, checked rather than claimed.
//
// The interesting tests are the ones a loader that "works" would still fail:
//
//   * the exit code has to be the one the program returned, not zero. Zero is
//     also what a process that never ran leaves behind.
//   * a file the process created has to be visible to the kernel afterwards,
//     which is the only proof the syscall boundary carried a pointer and a
//     length rather than doing something local.
//   * two copies of the SAME binary, at the SAME virtual address, must not see
//     each other's memory. This is the whole point of address spaces, and it
//     is the one thing that would have looked fine before them.
//   * a program that faults must be the only casualty, three different ways.
//   * the frames have to come back. A loader that leaks an address space per
//     process looks perfect until the twentieth one.
//   * a corrupt binary has to be refused, with a reason.

#include "nano-kernel.h"
#include "nano-mm.h"
#include "nano-thread.h"
#include "nano-fs.h"
#include "nano-proc.h"
#include "nano-int.h"

extern long prog_hello_addr();
extern long prog_hello_size();
extern long prog_twin_addr();
extern long prog_twin_size();
extern long prog_wild_addr();
extern long prog_wild_size();

char g_readback[512];

// Copy one of the embedded program images onto the RAM disk.
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

void populate() {
    long ino;

    fs_mkdir("/bin");
    fs_mkdir("/doc");

    ino = fs_create("/doc/readme");
    // strlen rather than a hand-counted literal: counting it by hand is how a
    // write runs one byte past the end of the string and pulls in whatever the
    // linker put next.
    {
        char *txt;
        txt = "nano-os: a kernel built by a compiler that builds itself.\n";
        fs_write(ino, 0, txt, strlen(txt));
    }

    install("/bin/hello", prog_hello_addr(), prog_hello_size());
    install("/bin/twin",  prog_twin_addr(),  prog_twin_size());
    install("/bin/wild",  prog_wild_addr(),  prog_wild_size());
}

void main_thread(long unused) {
    long frames_at_start;

    puts("scheduler running\n");

    if (!fs_format(4096, 128)) { puts("format failed\n"); cpu_halt_forever(); }
    populate();
    printf("%d frames free before any process\n", mm_free_frames);
    if (g_nx_on) puts("NX is on: a page marked no-execute really is\n");
    else puts("NX IS NOT AVAILABLE -- non-executable pages are decoration\n");
    frames_at_start = mm_free_frames;

    // --- 1. load and run one program ---
    {
        long pid;
        long code;
        {
            char *av_hello[1];
            av_hello[0] = "/bin/hello";
            pid = proc_spawn("/bin/hello", 1, av_hello, "hello", "/");
        }
        if (!pid) { printf("SPAWN FAILED: %s\n", proc_reject); }
        else {
            printf("spawned pid %d\n", pid);
            code = proc_wait(pid);
            printf("hello exited with %d\n", code);
            if (code == 7) puts("exit code crossed the boundary ok\n");
            else puts("WRONG EXIT CODE\n");
        }
    }

    // --- 2. did the process really write to our filesystem? ---
    {
        long ino;
        long n;
        ino = fs_lookup("/hello.out");
        if (!ino) puts("THE PROCESS DID NOT CREATE ITS FILE\n");
        else {
            n = fs_read(ino, 0, g_readback, 200);
            g_readback[n] = 0;
            printf("kernel reads /hello.out: %s", g_readback);
            if (!strcmp(g_readback, "written by a user process\n"))
                puts("process wrote a file the kernel can read ok\n");
            else puts("FILE CONTENTS WRONG\n");
        }
    }

    // --- 3. two processes, one binary, one virtual address ---
    // Both twins fill a buffer at 0x8000002000-ish with a different byte and
    // then keep checking it while the scheduler moves between them. Sharing an
    // address space would give thousands of wrong bytes; the number printed is
    // the evidence, not the word "ok".
    {
        long a;
        long b;
        long ra;
        long rb;
        {
            char *av1[2];
            char *av2[2];
            av1[0] = "/bin/twin"; av1[1] = "1";
            av2[0] = "/bin/twin"; av2[1] = "2";
            a = proc_spawn("/bin/twin", 2, av1, "twin1", "/");
            b = proc_spawn("/bin/twin", 2, av2, "twin2", "/");
        }
        if (!a || !b) printf("TWIN SPAWN FAILED: %s\n", proc_reject);
        else {
            printf("two processes running the same binary: pids %d and %d\n", a, b);
            ra = proc_wait(a);
            rb = proc_wait(b);
            printf("twin results: %d and %d bytes wrong\n", ra, rb);
            if (ra == 0 && rb == 0) puts("separate address spaces ok\n");
            else puts("THE TWO PROCESSES SHARED MEMORY\n");
            printf("%d address-space switches so far\n", g_space_switches);
        }
    }

    // --- 4. three ways to die, and the system carries on ---
    {
        long mode;
        long faults_before;
        faults_before = g_proc_faults;
        mode = 0;
        while (mode < 4) {
            long pid;
            long code;
            {
                char *av_wild[2];
                char digit[2];
                digit[0] = (char)(48 + mode);
                digit[1] = 0;
                av_wild[0] = "/bin/wild"; av_wild[1] = digit;
                pid = proc_spawn("/bin/wild", 2, av_wild, "wild", "/");
            }
            if (!pid) { printf("WILD SPAWN FAILED: %s\n", proc_reject); break; }
            code = proc_wait(pid);
            printf("wild(%d) finished with %d\n", mode, code);
            if (code != -1) printf("MODE %d WAS NOT STOPPED\n", mode);
            mode = mode + 1;
        }
        printf("%d processes killed by a fault\n", g_proc_faults - faults_before);
        if (g_proc_faults - faults_before == 4) puts("all four faults contained ok\n");
        else puts("A FAULTING PROCESS WAS NOT CONTAINED\n");
    }

    // --- 5. the kernel is still here and still working ---
    {
        long ino;
        long n;
        ino = fs_lookup("/doc/readme");
        n = fs_read(ino, 0, g_readback, 200);
        g_readback[n] = 0;
        printf("after four faults, the filesystem still reads: %s", g_readback);
        if (n > 10) puts("kernel survived every fault ok\n");
        else puts("KERNEL STATE DAMAGED\n");
    }

    // --- 6. the frames came back ---
    // Six processes have been created and destroyed. If an address space leaks,
    // this is where it shows: the count would be short by the size of a program
    // plus its stack, every time.
    {
        long now;
        proc_poll();
        now = mm_free_frames;
        printf("frames free: %d at the start, %d now (difference %d)\n",
               frames_at_start, now, frames_at_start - now);
        if (frames_at_start - now == 0) puts("every address space was reclaimed ok\n");
        else puts("FRAMES LEAKED\n");
    }

    // --- 7. a file that is not a program ---
    {
        long pid;
        pid = proc_spawn("/doc/readme", 0, 0, "notelf", "/");
        if (pid) puts("RAN SOMETHING THAT IS NOT AN ELF FILE\n");
        else printf("refused /doc/readme: %s\n", proc_reject);

        pid = proc_spawn("/bin/nothing", 0, 0, "missing", "/");
        if (pid) puts("RAN A FILE THAT DOES NOT EXIST\n");
        else printf("refused /bin/nothing: %s\n", proc_reject);

        // A real ELF header with a segment that points outside user space. The
        // magic and the class are right, so anything checking only those would
        // load it -- and map a frame over the kernel.
        {
            long ino;
            long i;
            char hdr[128];
            i = 0;
            while (i < 128) { hdr[i] = 0; i = i + 1; }
            hdr[0] = 0x7F; hdr[1] = 'E'; hdr[2] = 'L'; hdr[3] = 'F';
            hdr[4] = 2; hdr[5] = 1; hdr[6] = 1;
            hdr[16] = 2;                       // e_type = ET_EXEC
            hdr[18] = 62;                      // e_machine = x86-64
            hdr[32] = 64;                      // e_phoff
            hdr[54] = 56;                      // e_phentsize
            hdr[56] = 1;                       // e_phnum
            hdr[64] = 1;                       // p_type = PT_LOAD
            hdr[68] = 7;                       // p_flags = RWX
            hdr[80] = 0; hdr[81] = 0x10;       // p_vaddr = 0x1000 -- kernel space
            hdr[96] = 16;                      // p_filesz
            hdr[104] = 16;                     // p_memsz
            ino = fs_create("/bad.elf");
            fs_write(ino, 0, hdr, 128);
            pid = proc_spawn("/bad.elf", 0, 0, "bad", "/");
            if (pid) puts("LOADED A SEGMENT OUTSIDE USER SPACE\n");
            else printf("refused /bad.elf: %s\n", proc_reject);
        }

        // The header rules, one at a time. Each of these is a VALID ELF file
        // in every other respect -- right magic, right class, right machine,
        // a segment in the right place -- so nothing but the rule under test
        // can be what refuses it.
        //
        // Built by hand rather than by the toolchain on purpose: the toolchain
        // cannot produce them, which is exactly why a test that only used the
        // toolchain's output would never exercise these paths at all.
        {
            long ino;
            long i;
            char hdr[128];
            long k;

            k = 0;
            while (k < 3) {
                i = 0;
                while (i < 128) { hdr[i] = 0; i = i + 1; }
                hdr[0] = 0x7F; hdr[1] = 'E'; hdr[2] = 'L'; hdr[3] = 'F';
                hdr[4] = 2; hdr[5] = 1; hdr[6] = 1;
                hdr[16] = 2;                       // e_type = ET_EXEC
                hdr[18] = 62;                      // e_machine = x86-64
                hdr[32] = 64;                      // e_phoff
                hdr[54] = 56;                      // e_phentsize
                hdr[56] = 1;                       // e_phnum
                hdr[64] = 1;                       // p_type = PT_LOAD
                // p_vaddr = USER_BASE = 0x8000000000, little-endian: the
                // 0x80 is the FIFTH byte of the field, not the second. p_vaddr
                // is at phdr offset 16 and the phdr starts at 64, so that is
                // hdr[84]; e_entry is at 24, so hdr[28].
                hdr[84] = 0x80;
                hdr[28] = 0x80;
                hdr[96] = 16;                      // p_filesz
                hdr[104] = 16;                     // p_memsz
                hdr[68] = 5;                       // p_flags = R | X

                if (k == 0) {
                    hdr[68] = 7;                   // ... and writable too
                    ino = fs_create("/wx.elf");
                    fs_write(ino, 0, hdr, 128);
                    pid = proc_spawn("/wx.elf", 0, 0, "wx", "/");
                    if (pid) puts("LOADED A WRITABLE EXECUTABLE SEGMENT\n");
                    else printf("refused /wx.elf: %s\n", proc_reject);
                } else if (k == 1) {
                    hdr[104] = 64;                 // memsz > filesz, and PF_X
                    ino = fs_create("/zx.elf");
                    fs_write(ino, 0, hdr, 128);
                    pid = proc_spawn("/zx.elf", 0, 0, "zx", "/");
                    if (pid) puts("LOADED ZERO-FILLED EXECUTABLE PAGES\n");
                    else printf("refused /zx.elf: %s\n", proc_reject);
                } else {
                    hdr[68] = 6;                   // p_flags = R | W, no X
                    ino = fs_create("/ne.elf");
                    fs_write(ino, 0, hdr, 128);
                    pid = proc_spawn("/ne.elf", 0, 0, "ne", "/");
                    if (pid) puts("ENTERED A NON-EXECUTABLE SEGMENT\n");
                    else printf("refused /ne.elf: %s\n", proc_reject);
                }
                k = k + 1;
            }
        }
    }

    printf("\n%d syscalls, %d context switches, %d of them across address spaces\n",
           g_syscalls, g_switches, g_space_switches);
    puts("elf loader bring-up complete\n");
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

    puts("nano-os ELF loader bring-up\n");
    thread_create((long)main_thread, 0, "main");
    sched_start();
    return 0;
}
