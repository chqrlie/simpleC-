// fs.c — filesystem bring-up, checked rather than claimed.
//
// Headless. The tests that matter are the ones where a plausible-looking
// implementation would still be wrong:
//
//   * a file bigger than the direct blocks reach, so the indirect block is
//     genuinely used rather than merely present
//   * deleting a file has to give the blocks BACK, not just remove the name
//   * renaming must not copy: the inode number has to be the same afterwards
//   * removing a non-empty directory must be refused, or its contents are
//     orphaned and their blocks are lost forever
//   * two threads writing different files at once must not corrupt each other

#include "nano-kernel.h"
#include "nano-mm.h"
#include "nano-thread.h"
#include "nano-int.h"
#include "nano-fs.h"

char g_buf[8192];
char g_buf2[8192];
long g_worker_bad;
long g_worker_done;

void fill(char *b, long n, long seed) {
    long i;
    i = 0;
    while (i < n) { b[i] = (char)((i * 7 + seed) & 255); i = i + 1; }
}

long check(char *b, long n, long seed) {
    long i;
    i = 0;
    while (i < n) {
        if (b[i] != (char)((i * 7 + seed) & 255)) return 0;
        i = i + 1;
    }
    return 1;
}

// Each worker writes its own file, reads it back and verifies. Before the
// filesystem took a lock, two of these interleaving inside balloc would hand
// the same block to both.
void worker(long seed) {
    char name[32];
    long ino;
    long i;

    name[0] = 'w'; name[1] = '0' + seed; name[2] = 0;
    ino = fs_create(name);
    if (!ino) { g_worker_bad = g_worker_bad + 1; g_worker_done = g_worker_done + 1; thread_exit(0); }

    i = 0;
    while (i < 12) {
        char blk[512];
        fill(blk, 512, seed * 31 + i);
        if (fs_write(ino, i * 512, blk, 512) != 512) g_worker_bad = g_worker_bad + 1;
        thread_yield();
        i = i + 1;
    }
    i = 0;
    while (i < 12) {
        char blk[512];
        fs_read(ino, i * 512, blk, 512);
        if (!check(blk, 512, seed * 31 + i)) g_worker_bad = g_worker_bad + 1;
        thread_yield();
        i = i + 1;
    }
    g_worker_done = g_worker_done + 1;
    thread_exit(0);
}

void main_thread(long unused) {
    puts("scheduler running\n");

    if (!fs_format(2048, 128)) { puts("format failed\n"); cpu_halt_forever(); }
    printf("formatted: %d blocks, %d inodes, data starts at %d\n",
           sb_nblocks, sb_ninodes, sb_data_start);
    printf("%d blocks free (%d KiB)\n", blocks_free(), blocks_free() / 2);

    // --- 1. create, write, read back ---
    {
        long ino;
        long n;
        ino = fs_create("/hello.txt");
        if (!ino) puts("CREATE FAILED\n");
        n = fs_write(ino, 0, "hello from nano-os\n", 19);
        printf("wrote %d bytes, size is %d\n", n, fs_size(ino));
        n = fs_read(ino, 0, g_buf, 64);
        g_buf[n] = 0;
        printf("read back: %s", g_buf);
        if (n == 19 && !strcmp(g_buf, "hello from nano-os\n")) puts("read/write ok\n");
        else puts("READ BACK WRONG\n");
    }

    // --- 2. a file past the direct blocks ---
    // 8 direct blocks reach 4096 bytes. 6000 forces the indirect block, which
    // is the part a smaller test would leave completely unexercised.
    {
        long ino;
        long n;
        long free0;
        free0 = blocks_free();
        ino = fs_create("/big.bin");
        fill(g_buf, 6000, 3);
        n = fs_write(ino, 0, g_buf, 6000);
        printf("big file: wrote %d, size %d, used %d blocks\n",
               n, fs_size(ino), free0 - blocks_free());
        memset(g_buf2, 0, 8192);
        fs_read(ino, 0, g_buf2, 6000);
        if (n == 6000 && check(g_buf2, 6000, 3)) puts("indirect block ok\n");
        else puts("LARGE FILE CORRUPTED\n");

        // and reading past the end must stop at the end, not run on
        n = fs_read(ino, 5990, g_buf2, 100);
        if (n == 10) puts("short read at EOF ok\n");
        else printf("EOF READ WRONG: got %d, wanted 10\n", n);
    }

    // --- 3. directories ---
    {
        long i;
        char nm[64];
        long ino;
        if (!fs_mkdir("/src")) puts("MKDIR FAILED\n");
        if (!fs_mkdir("/src/kernel")) puts("NESTED MKDIR FAILED\n");
        fs_create("/src/main.c");
        fs_create("/src/util.c");
        fs_create("/src/kernel/boot.s");

        puts("/src contains:");
        i = 0;
        for (;;) {
            ino = fs_readdir(fs_lookup("/src"), i, nm);
            if (!ino) break;
            putc(' '); puts(nm);
            i = i + 1;
        }
        putc('\n');
        if (fs_lookup("/src/kernel/boot.s")) puts("nested path lookup ok\n");
        else puts("NESTED LOOKUP FAILED\n");
        if (!fs_lookup("/src/nope.c")) puts("missing file correctly not found\n");
        else puts("FOUND A FILE THAT DOES NOT EXIST\n");
    }

    // --- 4. deleting has to return the blocks ---
    {
        long free0;
        long free1;
        long free2;
        free0 = blocks_free();
        fs_unlink("/big.bin");
        free1 = blocks_free();
        printf("deleting big.bin returned %d blocks\n", free1 - free0);
        if (free1 - free0 >= 12) puts("unlink freed the blocks ok\n");
        else puts("UNLINK DID NOT FREE THE BLOCKS\n");
        if (!fs_lookup("/big.bin")) puts("gone from the directory ok\n");
        else puts("STILL IN THE DIRECTORY\n");

        // and the space must be reusable, not merely counted
        {
            long ino;
            ino = fs_create("/again.bin");
            fill(g_buf, 6000, 9);
            fs_write(ino, 0, g_buf, 6000);
            free2 = blocks_free();
            memset(g_buf2, 0, 8192);
            fs_read(ino, 0, g_buf2, 6000);
            if (check(g_buf2, 6000, 9)) puts("reused space ok\n");
            else puts("REUSED BLOCKS ARE CORRUPT\n");
            fs_unlink("/again.bin");
        }
    }

    // --- 5. rename must not copy ---
    {
        long before;
        long after;
        before = fs_lookup("/hello.txt");
        if (!fs_rename("/hello.txt", "/src/greeting.txt")) puts("RENAME FAILED\n");
        after = fs_lookup("/src/greeting.txt");
        printf("inode before %d, after %d\n", before, after);
        if (before == after && before != 0) puts("rename moved the name, not the data\n");
        else puts("RENAME COPIED OR LOST THE FILE\n");
        if (!fs_lookup("/hello.txt")) puts("old name gone ok\n");
        else puts("OLD NAME STILL THERE\n");
        {
            long n;
            n = fs_read(after, 0, g_buf, 64);
            g_buf[n] = 0;
            if (!strcmp(g_buf, "hello from nano-os\n")) puts("contents survived ok\n");
            else puts("CONTENTS LOST IN THE RENAME\n");
        }
    }

    // --- 6. removing a non-empty directory must be refused ---
    {
        if (fs_unlink("/src")) puts("REMOVED A NON-EMPTY DIRECTORY\n");
        else puts("refused to remove a non-empty directory ok\n");
        if (fs_lookup("/src/greeting.txt")) puts("its contents are still there ok\n");
        else puts("CONTENTS WERE ORPHANED\n");
    }

    // --- 7. two threads writing at once ---
    {
        long a;
        long b;
        long c;
        g_worker_bad = 0;
        g_worker_done = 0;
        a = thread_create((long)worker, 1, "fs-w1");
        b = thread_create((long)worker, 2, "fs-w2");
        c = thread_create((long)worker, 3, "fs-w3");
        thread_join(a);
        thread_join(b);
        thread_join(c);
        printf("three concurrent writers: %d errors\n", g_worker_bad);
        if (g_worker_bad == 0) puts("concurrent fs ok\n");
        else puts("CONCURRENT ACCESS CORRUPTED THE FILESYSTEM\n");
        // and each file must still hold its own data, not someone else's
        {
            char blk[512];
            long ino;
            long bad;
            bad = 0;
            ino = fs_lookup("w1");
            fs_read(ino, 0, blk, 512);
            if (!check(blk, 512, 31)) bad = bad + 1;
            ino = fs_lookup("w2");
            fs_read(ino, 0, blk, 512);
            if (!check(blk, 512, 62)) bad = bad + 1;
            ino = fs_lookup("w3");
            fs_read(ino, 0, blk, 512);
            if (!check(blk, 512, 93)) bad = bad + 1;
            if (bad == 0) puts("each file kept its own data ok\n");
            else puts("FILES OVERWROTE EACH OTHER\n");
        }
    }

    printf("\n%d of %d blocks free\n", blocks_free(), sb_nblocks - sb_data_start);
    puts("filesystem bring-up complete\n");
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

    puts("nano-os filesystem bring-up\n");
    thread_create((long)main_thread, 0, "main");
    sched_start();
    return 0;
}
