// nano-fs.h — a small Unix-shaped filesystem on a RAM disk.
//
// Compiled by nano_cc with --kernel. Include AFTER nano-mm.h (it allocates the
// disk) and nano-thread.h (it locks).
//
// WHY NOT FAT16. FAT is the interoperable choice and it is a lot of code for
// the same demonstration: BPB parsing, cluster chains, 8.3 names, long-name
// entries, two copies of the table to keep in step. This layout is the classic
// Unix one -- superblock, bitmaps, inodes, directory entries -- which is a
// third of the code, supports real directories and hard links naturally, and
// can be read top to bottom.
//
// ON-DISK LAYOUT, in 512-byte blocks:
//
//   0            superblock
//   1            inode bitmap   (one bit per inode)
//   2            block bitmap   (one bit per block; 4096 blocks per block)
//   3 .. 3+N     inode table    (8 inodes per block)
//   ...          data blocks
//
// An inode is 128 bytes: type, size, link count, eight direct block pointers
// and one single-indirect block, with room left over. Every field is 8 bytes
// wide because nano_cc has no 16- or 32-bit integer type and an on-disk struct
// with mixed field widths cannot be declared correctly here -- wasteful, and
// honest about why.
//
// Eight direct blocks reach 4 KiB; the indirect block holds 64 more pointers,
// so the largest file is 36 KiB. Small on purpose: enough for source files and
// the compiler's output, and growing it is one more indirection level rather
// than a redesign.

#ifndef NANO_FS_H
#define NANO_FS_H

#define BLK_SIZE     512
#define INODE_SZ     128
#define INODES_PER_BLK (BLK_SIZE / INODE_SZ)      // 4
#define NDIRECT      8
#define NINDIRECT    (BLK_SIZE / 8)          // 64 pointers in an indirect block
#define MAXFILE      ((NDIRECT + NINDIRECT) * BLK_SIZE)

#define FS_MAGIC     0x4E414E4F46531000      // "NANOFS" and a version

#define T_FREE  0
#define T_FILE  1
#define T_DIR   2

#define NAME_MAX  27                          // 28 bytes with the terminator
#define DIRENT_SZ 32                          // inode number + name
#define DIRENTS_PER_BLK (BLK_SIZE / DIRENT_SZ)

#define FS_MAX_OPEN 32

// ---------- the block device ----------
// A RAM disk: blocks are just memory. A real driver would put a request on a
// queue and sleep; this returns immediately, which is the one way it is not
// representative and worth saying so.
long disk_base;
long disk_blocks;

long fs_dev_init(long nblocks) {
    long bytes;
    bytes = nblocks * BLK_SIZE;
    disk_base = (long)kmalloc(bytes);
    if (!disk_base) return 0;
    memset((void *)disk_base, 0, bytes);
    disk_blocks = nblocks;
    return 1;
}

long blk_addr(long b) {
    if (b < 0 || b >= disk_blocks) return 0;
    return disk_base + b * BLK_SIZE;
}

long blk_read(long b, char *into) {
    long a;
    a = blk_addr(b);
    if (!a) return 0;
    memcpy(into, (void *)a, BLK_SIZE);
    return 1;
}

long blk_write(long b, char *from) {
    long a;
    a = blk_addr(b);
    if (!a) return 0;
    memcpy((void *)a, from, BLK_SIZE);
    return 1;
}

// ---------- superblock ----------
// Kept in memory as plain longs rather than as a struct read off the disk.
long sb_magic;
long sb_nblocks;
long sb_ninodes;
long sb_inode_start;      // first block of the inode table
long sb_data_start;       // first data block
long fs_mounted;

struct Mutex fs_lock;

void sb_write() {
    long a;
    long *p;
    a = blk_addr(0);
    p = (long *)a;
    p[0] = FS_MAGIC;
    p[1] = sb_nblocks;
    p[2] = sb_ninodes;
    p[3] = sb_inode_start;
    p[4] = sb_data_start;
}

long sb_read() {
    long a;
    long *p;
    a = blk_addr(0);
    if (!a) return 0;
    p = (long *)a;
    if (p[0] != FS_MAGIC) return 0;
    sb_magic = p[0];
    sb_nblocks = p[1];
    sb_ninodes = p[2];
    sb_inode_start = p[3];
    sb_data_start = p[4];
    return 1;
}

// ---------- bitmaps ----------
long bitmap_get(long blk, long i) {
    char *b;
    b = (char *)(blk_addr(blk) + (i >> 3));
    return (b[0] >> (i & 7)) & 1;
}

void bitmap_set(long blk, long i, long v) {
    char *b;
    b = (char *)(blk_addr(blk) + (i >> 3));
    if (v) b[0] = b[0] | (1 << (i & 7));
    else   b[0] = b[0] & ~(1 << (i & 7));
}

#define IMAP_BLK 1
#define BMAP_BLK 2

long balloc() {
    long i;
    i = sb_data_start;
    while (i < sb_nblocks) {
        if (!bitmap_get(BMAP_BLK, i)) {
            bitmap_set(BMAP_BLK, i, 1);
            memset((void *)blk_addr(i), 0, BLK_SIZE);
            return i;
        }
        i = i + 1;
    }
    return 0;                                 // 0 is the superblock, so it can
                                              // mean "none" without a flag
}

void bfree(long b) {
    if (b >= sb_data_start && b < sb_nblocks) bitmap_set(BMAP_BLK, b, 0);
}

long blocks_free() {
    long i;
    long n;
    n = 0;
    i = sb_data_start;
    while (i < sb_nblocks) { if (!bitmap_get(BMAP_BLK, i)) n = n + 1; i = i + 1; }
    return n;
}

// ---------- inodes ----------
// 64 bytes each: type, size, nlink, then 8 direct pointers and 1 indirect.
#define INO_TYPE    0
#define INO_SIZE    1
#define INO_NLINK   2
#define INO_DIRECT  3                         // words 3..10
#define INO_INDIRECT 11
#define INO_WORDS   (INODE_SZ / 8)            // 16

long ino_addr(long ino) {
    long blk;
    long off;
    if (ino < 1 || ino > sb_ninodes) return 0;
    blk = sb_inode_start + (ino - 1) / INODES_PER_BLK;
    off = ((ino - 1) % INODES_PER_BLK) * INODE_SZ;
    return blk_addr(blk) + off;
}

long ino_get(long ino, long field) {
    long a;
    long *p;
    a = ino_addr(ino);
    if (!a) return 0;
    p = (long *)a;
    return p[field];
}

void ino_set(long ino, long field, long v) {
    long a;
    long *p;
    a = ino_addr(ino);
    if (!a) return;
    p = (long *)a;
    p[field] = v;
}

long ialloc(long type) {
    long i;
    i = 1;
    while (i <= sb_ninodes) {
        if (!bitmap_get(IMAP_BLK, i)) {
            long a;
            long *p;
            long k;
            bitmap_set(IMAP_BLK, i, 1);
            a = ino_addr(i);
            p = (long *)a;
            k = 0;
            while (k < INO_WORDS) { p[k] = 0; k = k + 1; }
            ino_set(i, INO_TYPE, type);
            ino_set(i, INO_NLINK, 1);
            return i;
        }
        i = i + 1;
    }
    return 0;
}

// Which disk block holds byte offset `off` of this inode? `alloc` decides
// whether to grow the file or report a hole.
long ino_block(long ino, long off, long alloc) {
    long n;
    n = off / BLK_SIZE;
    if (n < NDIRECT) {
        long b;
        b = ino_get(ino, INO_DIRECT + n);
        if (!b && alloc) { b = balloc(); if (b) ino_set(ino, INO_DIRECT + n, b); }
        return b;
    }
    n = n - NDIRECT;
    if (n >= NINDIRECT) return 0;
    {
        long ib;
        long *tab;
        long b;
        ib = ino_get(ino, INO_INDIRECT);
        if (!ib) {
            if (!alloc) return 0;
            ib = balloc();
            if (!ib) return 0;
            ino_set(ino, INO_INDIRECT, ib);
        }
        tab = (long *)blk_addr(ib);
        b = tab[n];
        if (!b && alloc) { b = balloc(); if (b) tab[n] = b; }
        return b;
    }
}

void ino_truncate(long ino) {
    long n;
    n = 0;
    while (n < NDIRECT) {
        long b;
        b = ino_get(ino, INO_DIRECT + n);
        if (b) { bfree(b); ino_set(ino, INO_DIRECT + n, 0); }
        n = n + 1;
    }
    {
        long ib;
        ib = ino_get(ino, INO_INDIRECT);
        if (ib) {
            long *tab;
            long i;
            tab = (long *)blk_addr(ib);
            i = 0;
            while (i < NINDIRECT) { if (tab[i]) bfree(tab[i]); i = i + 1; }
            bfree(ib);
            ino_set(ino, INO_INDIRECT, 0);
        }
    }
    ino_set(ino, INO_SIZE, 0);
}

void ifree(long ino) {
    ino_truncate(ino);
    ino_set(ino, INO_TYPE, T_FREE);
    bitmap_set(IMAP_BLK, ino, 0);
}

// ---------- reading and writing an inode's bytes ----------
long ino_read(long ino, long off, char *dst, long n) {
    long size;
    long got;
    size = ino_get(ino, INO_SIZE);
    if (off >= size) return 0;
    if (off + n > size) n = size - off;
    got = 0;
    while (got < n) {
        long b;
        long inblk;
        long chunk;
        b = ino_block(ino, off + got, 0);
        inblk = (off + got) % BLK_SIZE;
        chunk = BLK_SIZE - inblk;
        if (chunk > n - got) chunk = n - got;
        if (b) memcpy(dst + got, (void *)(blk_addr(b) + inblk), chunk);
        else   memset(dst + got, 0, chunk);   // a hole reads as zeros
        got = got + chunk;
    }
    return got;
}

long ino_write(long ino, long off, char *src, long n) {
    long done;
    if (off + n > MAXFILE) n = MAXFILE - off;
    if (n <= 0) return 0;
    done = 0;
    while (done < n) {
        long b;
        long inblk;
        long chunk;
        b = ino_block(ino, off + done, 1);
        if (!b) break;                        // disk full
        inblk = (off + done) % BLK_SIZE;
        chunk = BLK_SIZE - inblk;
        if (chunk > n - done) chunk = n - done;
        memcpy((void *)(blk_addr(b) + inblk), src + done, chunk);
        done = done + chunk;
    }
    if (off + done > ino_get(ino, INO_SIZE)) ino_set(ino, INO_SIZE, off + done);
    return done;
}

// ---------- directories ----------
// A directory's data is an array of 32-byte entries: an 8-byte inode number
// then a name. An entry with inode 0 is free.
long dir_lookup(long dir, char *name) {
    long off;
    long size;
    char ent[DIRENT_SZ];
    size = ino_get(dir, INO_SIZE);
    off = 0;
    while (off < size) {
        long *ino;
        ino_read(dir, off, ent, DIRENT_SZ);
        ino = (long *)ent;
        if (ino[0] && !strcmp(ent + 8, name)) return ino[0];
        off = off + DIRENT_SZ;
    }
    return 0;
}

long dir_add(long dir, char *name, long ino) {
    long off;
    long size;
    char ent[DIRENT_SZ];
    long *p;
    long i;

    if (strlen(name) > NAME_MAX) return 0;
    if (dir_lookup(dir, name)) return 0;       // already there

    // reuse a free slot before growing the directory
    size = ino_get(dir, INO_SIZE);
    off = 0;
    while (off < size) {
        ino_read(dir, off, ent, DIRENT_SZ);
        p = (long *)ent;
        if (!p[0]) break;
        off = off + DIRENT_SZ;
    }

    i = 0;
    while (i < DIRENT_SZ) { ent[i] = 0; i = i + 1; }
    p = (long *)ent;
    p[0] = ino;
    i = 0;
    while (name[i] && i < NAME_MAX) { ent[8 + i] = name[i]; i = i + 1; }
    ent[8 + i] = 0;
    return ino_write(dir, off, ent, DIRENT_SZ) == DIRENT_SZ;
}

long dir_remove(long dir, char *name) {
    long off;
    long size;
    char ent[DIRENT_SZ];
    size = ino_get(dir, INO_SIZE);
    off = 0;
    while (off < size) {
        long *p;
        ino_read(dir, off, ent, DIRENT_SZ);
        p = (long *)ent;
        if (p[0] && !strcmp(ent + 8, name)) {
            long i;
            i = 0;
            while (i < DIRENT_SZ) { ent[i] = 0; i = i + 1; }
            ino_write(dir, off, ent, DIRENT_SZ);
            return 1;
        }
        off = off + DIRENT_SZ;
    }
    return 0;
}

// Is this directory empty apart from . and ..? Removing a non-empty directory
// would orphan everything inside it, and nothing would ever free those blocks.
long dir_empty(long dir) {
    long off;
    long size;
    char ent[DIRENT_SZ];
    size = ino_get(dir, INO_SIZE);
    off = 0;
    while (off < size) {
        long *p;
        ino_read(dir, off, ent, DIRENT_SZ);
        p = (long *)ent;
        if (p[0] && strcmp(ent + 8, ".") && strcmp(ent + 8, "..")) return 0;
        off = off + DIRENT_SZ;
    }
    return 1;
}

// ---------- paths ----------
#define ROOT_INO 1

// Resolve a path to an inode. If `parent_out` is given, stop one level short
// and also hand back the final component, which is what create and remove need.
long path_walk(char *path, long *parent_out, char *last_out) {
    long cur;
    long i;
    char comp[64];

    cur = ROOT_INO;
    i = 0;
    if (path[0] == '/') i = 1;

    for (;;) {
        long k;
        long next;
        // skip separators
        while (path[i] == '/') i = i + 1;
        if (!path[i]) {
            if (parent_out) { *parent_out = 0; if (last_out) last_out[0] = 0; }
            return cur;
        }
        k = 0;
        while (path[i] && path[i] != '/' && k < 63) { comp[k] = path[i]; k = k + 1; i = i + 1; }
        comp[k] = 0;
        // if this is the last component and a parent was asked for, stop here
        {
            long j;
            j = i;
            while (path[j] == '/') j = j + 1;
            if (!path[j] && parent_out) {
                *parent_out = cur;
                if (last_out) { long m; m = 0; while (comp[m]) { last_out[m] = comp[m]; m = m + 1; } last_out[m] = 0; }
                return dir_lookup(cur, comp);       // 0 if it does not exist
            }
        }
        if (ino_get(cur, INO_TYPE) != T_DIR) return 0;
        next = dir_lookup(cur, comp);
        if (!next) return 0;
        cur = next;
    }
}

// ---------- the operations ----------
long fs_mkdir(char *path) {
    long parent;
    char name[64];
    long ino;
    long ok;

    mutex_lock(&fs_lock);
    ino = path_walk(path, &parent, name);
    if (ino || !parent) { mutex_unlock(&fs_lock); return 0; }   // exists, or bad path
    ino = ialloc(T_DIR);
    if (!ino) { mutex_unlock(&fs_lock); return 0; }
    dir_add(ino, ".", ino);
    dir_add(ino, "..", parent);
    ok = dir_add(parent, name, ino);
    if (!ok) ifree(ino);
    mutex_unlock(&fs_lock);
    return ok;
}

long fs_create(char *path) {
    long parent;
    char name[64];
    long ino;

    mutex_lock(&fs_lock);
    ino = path_walk(path, &parent, name);
    if (ino) { mutex_unlock(&fs_lock); return ino; }            // already exists
    if (!parent) { mutex_unlock(&fs_lock); return 0; }
    ino = ialloc(T_FILE);
    if (!ino) { mutex_unlock(&fs_lock); return 0; }
    if (!dir_add(parent, name, ino)) { ifree(ino); ino = 0; }
    mutex_unlock(&fs_lock);
    return ino;
}

long fs_lookup(char *path) {
    long r;
    mutex_lock(&fs_lock);
    r = path_walk(path, 0, 0);
    mutex_unlock(&fs_lock);
    return r;
}

long fs_size(long ino) { return ino_get(ino, INO_SIZE); }
long fs_type(long ino) { return ino_get(ino, INO_TYPE); }

long fs_read(long ino, long off, char *dst, long n) {
    long r;
    mutex_lock(&fs_lock);
    r = ino_read(ino, off, dst, n);
    mutex_unlock(&fs_lock);
    return r;
}

long fs_write(long ino, long off, char *src, long n) {
    long r;
    mutex_lock(&fs_lock);
    r = ino_write(ino, off, src, n);
    mutex_unlock(&fs_lock);
    return r;
}

long fs_truncate(long ino) {
    mutex_lock(&fs_lock);
    ino_truncate(ino);
    mutex_unlock(&fs_lock);
    return 1;
}

long fs_unlink(char *path) {
    long parent;
    char name[64];
    long ino;
    long ok;

    mutex_lock(&fs_lock);
    ino = path_walk(path, &parent, name);
    if (!ino || !parent) { mutex_unlock(&fs_lock); return 0; }
    if (ino_get(ino, INO_TYPE) == T_DIR && !dir_empty(ino)) {
        mutex_unlock(&fs_lock);
        return 0;                                  // refuse: would orphan the
                                                   // contents and leak blocks
    }
    ok = dir_remove(parent, name);
    if (ok) {
        long nl;
        nl = ino_get(ino, INO_NLINK) - 1;
        ino_set(ino, INO_NLINK, nl);
        if (nl <= 0) ifree(ino);
    }
    mutex_unlock(&fs_lock);
    return ok;
}

// Rename is a directory operation, not a copy: the inode does not move, so a
// 60 KiB file renames in the time it takes to rewrite two 32-byte entries.
long fs_rename(char *from, char *to) {
    long fp;
    long tp;
    char fname[64];
    char tname[64];
    long ino;
    long existing;

    mutex_lock(&fs_lock);
    ino = path_walk(from, &fp, fname);
    if (!ino || !fp) { mutex_unlock(&fs_lock); return 0; }
    existing = path_walk(to, &tp, tname);
    if (!tp) { mutex_unlock(&fs_lock); return 0; }
    if (existing) {
        // Overwriting is allowed for files, not for a non-empty directory.
        if (ino_get(existing, INO_TYPE) == T_DIR && !dir_empty(existing)) {
            mutex_unlock(&fs_lock);
            return 0;
        }
        dir_remove(tp, tname);
        ifree(existing);
    }
    if (!dir_add(tp, tname, ino)) { mutex_unlock(&fs_lock); return 0; }
    dir_remove(fp, fname);
    mutex_unlock(&fs_lock);
    return 1;
}

// Read the nth entry of a directory. Returns the inode, or 0 when done.
long fs_readdir(long dir, long index, char *name_out) {
    long off;
    long size;
    long seen;
    char ent[DIRENT_SZ];

    mutex_lock(&fs_lock);
    size = ino_get(dir, INO_SIZE);
    off = 0;
    seen = 0;
    while (off < size) {
        long *p;
        ino_read(dir, off, ent, DIRENT_SZ);
        p = (long *)ent;
        if (p[0]) {
            if (seen == index) {
                long i;
                i = 0;
                while (ent[8 + i]) { name_out[i] = ent[8 + i]; i = i + 1; }
                name_out[i] = 0;
                mutex_unlock(&fs_lock);
                return p[0];
            }
            seen = seen + 1;
        }
        off = off + DIRENT_SZ;
    }
    mutex_unlock(&fs_lock);
    return 0;
}

// ---------- bring-up ----------
long fs_format(long nblocks, long ninodes) {
    long itable_blocks;

    if (!fs_dev_init(nblocks)) return 0;

    // Getting this wrong overlaps the inode table with the data area, and the
    // first file written then corrupts an inode.
    itable_blocks = (ninodes * INODE_SZ + BLK_SIZE - 1) / BLK_SIZE;

    sb_nblocks = nblocks;
    sb_ninodes = ninodes;
    sb_inode_start = 3;
    sb_data_start = 3 + itable_blocks;
    sb_write();

    // reserve the metadata blocks so balloc can never hand them out
    {
        long i;
        i = 0;
        while (i < sb_data_start) { bitmap_set(BMAP_BLK, i, 1); i = i + 1; }
    }
    bitmap_set(IMAP_BLK, 0, 1);                // inode 0 means "none"

    mutex_init(&fs_lock);

    // the root directory
    {
        long root;
        root = ialloc(T_DIR);
        if (root != ROOT_INO) return 0;
        dir_add(root, ".", root);
        dir_add(root, "..", root);
    }

    fs_mounted = 1;
    return 1;
}

#endif
