// fsdisk.c — the filesystem, on a disk that outlives the machine.
//
// The whole point of this image is that it is run TWICE against the same
// drive, and the two runs do different things because the disk is different.
// There is no flag, no argument and no state outside the disk itself:
//
//   first boot   the image reads back all zeroes, sb_read rejects the magic,
//                so this formats, writes files, and syncs.
//   second boot  the superblock is there, so this mounts and reads the files
//                back, and every byte has to match what the first boot wrote.
//
// The second run is the only thing that proves anything. A filesystem that
// writes and reads back within one boot is a data structure -- that is what
// this already was. Surviving the machine being switched off is the feature.
//
// Run it a third time and it still passes, which is worth having: a first
// boot that formats and a second that mounts could both be right while the
// SYNC of a mounted-not-formatted filesystem is broken, and only a third run
// notices.

#include "nano-kernel.h"
#include "nano-int.h"
#include "nano-mm.h"
#include "nano-thread.h"
#include "nano-ata.h"
#include "nano-fs.h"

long g_fail;
// Whether this run formatted a blank disk rather than mounting one.
long g_first_boot;

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

// Small enough to sync quickly, large enough to need indirect blocks.
#define NBLOCKS 2048
#define NINODES 128

char g_buf[4096];

// A file whose contents are derived from its index, so that reading it back
// on a later boot checks the BYTES and not merely that a file of the right
// length exists. A zero-filled file of the right size would pass a length
// check and prove nothing at all.
void fill_pattern(char *p, long n, long seed) {
    long i;
    i = 0;
    while (i < n) {
        p[i] = (seed * 7 + i * 13 + (i >> 5)) & 0x7F;
        i = i + 1;
    }
}

long check_pattern(char *p, long n, long seed) {
    long i;
    i = 0;
    while (i < n) {
        if (p[i] != ((seed * 7 + i * 13 + (i >> 5)) & 0x7F)) return 0;
        i = i + 1;
    }
    return 1;
}

// Three files: one short, one spanning several blocks, one big enough to need
// an indirect block. The indirect one matters -- a sync that writes only the
// direct blocks would pass on the first two.
#define BIG_SIZE 4096

void write_the_files() {
    long ino;
    long n;

    ino = fs_create("/hello.txt");
    expect_true("created /hello.txt", ino > 0);
    n = fs_write(ino, 0, "hello from a disk\n", 18);
    expect("wrote the greeting", n, 18);

    if (!fs_mkdir("/docs")) fail("mkdir /docs");
    ino = fs_create("/docs/notes.txt");
    expect_true("created /docs/notes.txt", ino > 0);
    fill_pattern(g_buf, 1500, 3);
    n = fs_write(ino, 0, g_buf, 1500);
    expect("wrote 1500 bytes across blocks", n, 1500);

    ino = fs_create("/big.bin");
    expect_true("created /big.bin", ino > 0);
    fill_pattern(g_buf, BIG_SIZE, 9);
    n = fs_write(ino, 0, g_buf, BIG_SIZE);
    expect("wrote a file big enough to need an indirect block", n, BIG_SIZE);
}

void read_the_files() {
    long ino;
    long n;

    ino = fs_lookup("/hello.txt");
    expect_true("/hello.txt is still there", ino > 0);
    if (ino > 0) {
        n = fs_read(ino, 0, g_buf, 64);
        g_buf[n] = 0;
        expect("...and is the right length", n, 18);
        expect_true("...and says what it said", !strcmp(g_buf, "hello from a disk\n"));
    }

    ino = fs_lookup("/docs/notes.txt");
    expect_true("/docs/notes.txt survived, directory and all", ino > 0);
    if (ino > 0) {
        n = fs_read(ino, 0, g_buf, 1500);
        expect("...at its full length", n, 1500);
        expect_true("...byte for byte", check_pattern(g_buf, 1500, 3));
    }

    ino = fs_lookup("/big.bin");
    expect_true("/big.bin survived", ino > 0);
    if (ino > 0) {
        n = fs_read(ino, 0, g_buf, BIG_SIZE);
        expect("...at its full length", n, BIG_SIZE);
        expect_true("...including the blocks reached through the indirect",
                    check_pattern(g_buf, BIG_SIZE, 9));
    }
}

void main_thread(long unused) {
    long synced;
    long mounted;

    puts("\n-- the disk --\n");
    if (!ata_init()) {
        // Loudly, and stop. Falling back to memory here would let every check
        // below pass while proving nothing, which is the failure this whole
        // milestone is about.
        puts("FAIL: no ATA disk on the primary channel -- run qemu with -drive\n");
        g_fail = g_fail + 1;
        printf("\n%d CHECKS FAILED\n", g_fail);
        puts("\nFSDISK DONE\n");
        cpu_halt_forever();
    }
    printf("  ata: present, %d sectors (%d KiB)\n", ata_sectors, ata_sectors / 2);
    expect_true("the disk is big enough for the filesystem", ata_sectors >= NBLOCKS);

    if (!fs_dev_init_ata(NBLOCKS)) {
        puts("FAIL: could not read the disk image in\n");
        g_fail = g_fail + 1;
        printf("\n%d CHECKS FAILED\n", g_fail);
        puts("\nFSDISK DONE\n");
        cpu_halt_forever();
    }
    printf("  read %d blocks in, %d reads, %d errors\n",
           NBLOCKS, ata_reads, ata_errors);

    mounted = fs_mount();

    if (!mounted) {
        puts("\n-- FIRST BOOT: no filesystem on this disk, formatting --\n");
        if (!fs_format_on_dev(NBLOCKS, NINODES)) {
            fail("format failed");
        } else {
            printf("  %d blocks, %d inodes, data starts at %d\n",
                   sb_nblocks, sb_ninodes, sb_data_start);
            expect_true("formatting did not knock it off the disk", disk_is_ata == 1);
            write_the_files();
            // Read them back BEFORE syncing, so that a failure here is a
            // filesystem bug and a failure on the next boot is a durability
            // bug. Two different faults that would otherwise look identical.
            puts("  reading back in the same boot:\n");
            read_the_files();
            synced = fs_sync();
            expect("synced every block to the disk", synced, NBLOCKS);
            printf("  %d sector writes, %d errors\n", ata_writes, ata_errors);
            puts("\n  Now reboot this image against the SAME disk. It should\n");
            puts("  mount instead of formatting, and every file should be there.\n");
            g_first_boot = 1;
        }
    } else {
        puts("\n-- A LATER BOOT: found a filesystem, mounting --\n");
        printf("  %d blocks, %d inodes, data starts at %d\n",
               sb_nblocks, sb_ninodes, sb_data_start);
        expect("the superblock survived with its geometry", sb_nblocks, NBLOCKS);
        expect("...and its inode count", sb_ninodes, NINODES);
        read_the_files();
        // Sync again, so that a third run is testing the sync of a MOUNTED
        // filesystem rather than only of a freshly formatted one.
        synced = fs_sync();
        expect("a mounted filesystem syncs too", synced, NBLOCKS);
    }

    printf("\nheap: %d pages mapped\n", heap_pages);
    // A FIRST boot has proved nothing about durability and must not say it
    // has. Found by sabotage: with fs_sync stubbed out, the second boot saw a
    // blank disk, took the format branch, passed every check it ran and
    // printed "files outlived the machine" -- a green result for the exact
    // failure the image exists to detect. The run can only claim the feature
    // when it MOUNTED what a previous run left.
    if (g_fail) printf("\n%d CHECKS FAILED\n", g_fail);
    else if (g_first_boot)
        puts("\nFORMATTED: nothing proved yet -- boot again on the same disk\n");
    else puts("\nPASS: files outlived the machine\n");
    puts("\nFSDISK DONE\n");
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

    puts("nano-os: a filesystem on a real disk\n");
    g_fail = 0;
    g_first_boot = 0;
    thread_create((long)main_thread, 0, "main");
    sched_start();
    return 0;
}
