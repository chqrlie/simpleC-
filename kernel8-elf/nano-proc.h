// nano-proc.h — address spaces, an ELF64 loader, processes and a syscall table.
//
// Compiled by nano_cc with --kernel. Include AFTER nano-kernel.h, nano-mm.h,
// nano-thread.h and nano-fs.h, and BEFORE nano-int.h -- the interrupt
// dispatcher switches on this file's include guard to route vector 0x80, and
// it cannot call something the compiler has not seen yet.
//
// ---------------------------------------------------------------------------
// What "its own address space" buys, and what it does not
// ---------------------------------------------------------------------------
// Until now a program was a thread: one set of page tables for everything, so a
// stray pointer in one task could land in the middle of another and the symptom
// would surface somewhere unrelated, much later. The supervisor in nano-srv.h
// could restart a service that CRASHED, but nothing could contain a service
// that scribbled.
//
// After this, each process gets its own page-table root. The kernel is shared
// -- it has to be, the interrupt handlers run out of it -- but user memory is
// not. Two processes can both live at 0x8000000000 and never see each other.
//
// What this does NOT do yet, and it matters: programs still run in ring 0.
// The address space is private, so a process cannot accidentally reach another
// one. It can still deliberately reach anything it likes, because at CPL 0 it
// may write CR3, execute cli, and address the whole kernel through the identity
// map. Isolation from accident: yes. Isolation from malice: no. Ring 3 needs a
// TSS, a kernel stack per process and a user GDT selector, and it is its own
// milestone rather than a footnote to this one.
//
// One thing IS enforced against the process itself: CR0.WP is on, so a
// read-only segment is genuinely read-only even at CPL 0. Without it, mapping
// .text without PTE_WRITE would be decoration -- the write would succeed and
// the test proving otherwise would be a test of nothing.

#ifndef NANO_PROC_H
#define NANO_PROC_H

extern void write_cr3_(long root);
extern void enable_write_protect();

// ---------- where user memory lives ----------
// PML4 entry 1: virtual addresses 512 GiB .. 1 TiB. Entry 0 (everything below
// 512 GiB) holds the identity map and the kernel heap and is shared by every
// address space; entry 1 is per-process and is the only thing as_create makes
// a private copy of.
//
// Putting user space in its own PML4 slot rather than at the traditional
// 0x400000 is what makes the private/shared split one word wide. At 0x400000
// it would sit inside the identity map, and every process would have to carve
// a hole in the mapping the kernel is using.
#define USER_PML4       1
#define USER_BASE       0x8000000000
#define USER_TOP        0x8080000000        // 512 GiB + 2 GiB
#define USER_STACK_TOP  0x8010000000        // 256 MiB into user space
#define USER_STACK_SIZE 65536               // 16 pages
#define USER_HEAP_GAP   0x1000              // guard page after the last segment

#define ELF_MAX 262144                      // biggest program we will load

// ---------- address spaces ----------

// A fresh page-table root that shares everything except user space.
//
// The copy is by top-level entry, so any kernel mapping that exists NOW is
// shared automatically. A kernel mapping created LATER in a previously unused
// PML4 slot would not propagate to address spaces that already exist. Nothing
// here does that -- the heap grows inside entry 0, which every space already
// points at -- but it is the assumption that would break first.
long as_create() {
    long root;
    long *dst;
    long *src;
    long i;

    root = frame_alloc_zeroed();
    if (!root) return 0;
    dst = (long *)root;
    src = (long *)(g_kernel_root & PTE_ADDR);
    i = 0;
    while (i < 512) {
        if (i != USER_PML4) dst[i] = src[i];
        i = i + 1;
    }
    return root;
}

// Free a page-table subtree and every frame it maps. level 3 = PDPT, 2 = PD,
// 1 = PT. Only ever called on user space, which is built entirely out of 4 KiB
// mappings -- vmm_map_in never creates a 2 MiB entry in a fresh space, and
// pd_split only fires on one that was already there.
void as_free_level(long table, long level) {
    long *e;
    long i;
    if (!table) return;
    e = (long *)table;
    i = 0;
    while (i < 512) {
        long v;
        v = e[i];
        if (v & PTE_PRESENT) {
            if (level == 1) frame_free(v & PTE_ADDR);
            else if (!(v & PTE_HUGE)) as_free_level(v & PTE_ADDR, level - 1);
        }
        e[i] = 0;
        i = i + 1;
    }
    frame_free(table);
}

// Returns the number of frames it gave back, so a caller can prove the space
// was actually reclaimed rather than merely forgotten.
long as_destroy(long root) {
    long *e;
    long before;
    long freed;

    if (!root) return 0;
    // Destroying the address space we are standing in would unmap the page
    // tables the CPU is walking. Refusing is the only safe answer.
    if ((root & PTE_ADDR) == (read_cr3_() & PTE_ADDR)) return -1;

    before = mm_free_frames;
    e = (long *)(root & PTE_ADDR);
    if (e[USER_PML4] & PTE_PRESENT) as_free_level(e[USER_PML4] & PTE_ADDR, 3);
    e[USER_PML4] = 0;
    frame_free(root & PTE_ADDR);
    freed = mm_free_frames - before;
    return freed;
}

// The intermediate tables need the USER bit too, or a ring-3 access is refused
// at the level above the one that granted it. Nothing checks this today --
// everything runs at CPL 0 -- but a mapping that is only half-user is the kind
// of thing that works until the day the privilege level changes and then fails
// with a fault that points at the wrong table.
void as_mark_user_path(long root, long virt) {
    long *e;
    long next;

    e = (long *)((root & PTE_ADDR) + pt_index(virt, 4) * 8);
    if (!(e[0] & PTE_PRESENT)) return;
    e[0] = e[0] | PTE_USER;
    next = e[0] & PTE_ADDR;

    e = (long *)(next + pt_index(virt, 3) * 8);
    if (!(e[0] & PTE_PRESENT)) return;
    e[0] = e[0] | PTE_USER;
    next = e[0] & PTE_ADDR;

    e = (long *)(next + pt_index(virt, 2) * 8);
    if (!(e[0] & PTE_PRESENT)) return;
    if (e[0] & PTE_HUGE) return;
    e[0] = e[0] | PTE_USER;
}

// Give a user page a frame, or reuse the one already there.
//
// Reuse is not an optimisation, it is a correctness requirement: ld packs .text
// and .data into the same 4 KiB page when they are small, so the second
// segment's mapping pass sees a page the first one already filled. Allocating a
// fresh frame for it would throw away the tail of .text and the program would
// jump into zeroes.
long as_touch(long root, long virt, long flags) {
    long phys;
    long have;

    have = vmm_flags_in(root, virt);
    phys = vmm_resolve_in(root, virt) & ~(PAGE_SIZE - 1);
    if (!phys) {
        phys = frame_alloc_zeroed();
        if (!phys) return 0;
        have = 0;
    }
    // The union of what the page already had and what this segment needs. A
    // page shared by a read-only and a writable segment has to be writable;
    // pretending otherwise gives a fault on the first store to a global.
    if (!vmm_map_in(root, virt, phys, flags | (have & (PTE_WRITE | PTE_USER)))) return 0;
    as_mark_user_path(root, virt);
    return phys;
}

// Write one aligned word into another address space, through the identity map.
//
// Legal only because every frame is below 4 GiB and the bottom 4 GiB is mapped
// one-to-one, so a physical address is also a usable virtual one here. An
// 8-byte value never straddles a page when the address is 8-byte aligned, which
// every stack slot is.
long as_poke(long root, long virt, long val) {
    long phys;
    phys = vmm_resolve_in(root, virt);
    if (!phys) return 0;
    {
        long *p;
        p = (long *)phys;
        p[0] = val;
    }
    return 1;
}

// ---------- reading an ELF64 file ----------
// Every multi-byte field is read a byte at a time. Not for portability: nano_cc
// has no 16- or 32-bit integer type, so there is no `short` to load a 2-byte
// field into and no way to declare a struct whose members have the widths the
// format specifies. Byte at a time is the only way to say it, and it is at
// least explicit about endianness.
long rd_le(char *p, long off, long n) {
    long v;
    long i;
    v = 0;
    i = n - 1;
    while (i >= 0) {
        v = (v << 8) | (p[off + i] & 255);   // & 255: char is signed here
        i = i - 1;
    }
    return v;
}

#define PT_LOAD 1
#define PF_X    1
#define PF_W    2
#define PF_R    4

char *elf_reject;     // why the last load failed, for the caller to print

long elf_check(char *img, long len) {
    elf_reject = "";
    if (len < 64)                     { elf_reject = "shorter than an ELF header"; return 0; }
    if ((img[0] & 255) != 0x7F)       { elf_reject = "not ELF (bad magic)"; return 0; }
    if (img[1] != 'E' || img[2] != 'L' || img[3] != 'F') { elf_reject = "not ELF (bad magic)"; return 0; }
    if ((img[4] & 255) != 2)          { elf_reject = "not 64-bit"; return 0; }
    if ((img[5] & 255) != 1)          { elf_reject = "not little-endian"; return 0; }
    if (rd_le(img, 16, 2) != 2)       { elf_reject = "not an executable (ET_EXEC)"; return 0; }
    if (rd_le(img, 18, 2) != 62)      { elf_reject = "not x86-64"; return 0; }
    if (rd_le(img, 54, 2) != 56)      { elf_reject = "unexpected program header size"; return 0; }
    return 1;
}

// Takes the program header itself rather than its six fields: nano_cc passes
// arguments in registers and stops at six, and a function whose signature is
// one field away from the limit is a trap for whoever adds p_align next.
long elf_map_segment(long root, char *img, long imglen, char *ph) {
    long off;
    long vaddr;
    long filesz;
    long memsz;
    long eflags;
    long flags;
    long start;
    long end;
    long va;
    long k;

    eflags = rd_le(ph, 4, 4);
    off    = rd_le(ph, 8, 8);
    vaddr  = rd_le(ph, 16, 8);
    filesz = rd_le(ph, 32, 8);
    memsz  = rd_le(ph, 40, 8);

    // A program header is a request from a file we did not write. Both of these
    // checks are the difference between "refuses to load a corrupt binary" and
    // "maps a frame over the kernel because a field said so".
    if (vaddr < USER_BASE || vaddr + memsz > USER_TOP) { elf_reject = "segment outside user space"; return 0; }
    if (filesz > memsz)                                { elf_reject = "filesz exceeds memsz"; return 0; }
    if (off + filesz > imglen)                         { elf_reject = "segment runs past the end of the file"; return 0; }

    flags = PTE_USER;
    if (eflags & PF_W) flags = flags | PTE_WRITE;

    start = vaddr & ~(PAGE_SIZE - 1);
    end = (vaddr + memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    va = start;
    while (va < end) {
        if (!as_touch(root, va, flags)) { elf_reject = "out of memory mapping a segment"; return 0; }
        va = va + PAGE_SIZE;
    }

    // Copy the file bytes in, a page-chunk at a time, through the identity map.
    // The frames arrived zeroed, so the memsz-beyond-filesz part -- .bss -- is
    // already correct and needs no second pass.
    k = 0;
    while (k < filesz) {
        long dst;
        long n;
        long pageoff;
        pageoff = (vaddr + k) & (PAGE_SIZE - 1);
        n = PAGE_SIZE - pageoff;
        if (n > filesz - k) n = filesz - k;
        dst = vmm_resolve_in(root, vaddr + k);
        if (!dst) { elf_reject = "a page vanished between mapping and filling it"; return 0; }
        memcpy((char *)dst, img + off + k, n);
        k = k + n;
    }
    return 1;
}

// Load every PT_LOAD segment into `root`. Returns the entry point, or 0 with
// elf_reject set. *top_out gets the highest virtual address used, which is
// where the process heap starts.
long elf_load(long root, char *img, long len, long *top_out) {
    long phoff;
    long phnum;
    long phent;
    long i;
    long top;
    long loaded;

    if (!elf_check(img, len)) return 0;

    phoff = rd_le(img, 32, 8);
    phent = rd_le(img, 54, 2);
    phnum = rd_le(img, 56, 2);
    if (phoff + phnum * phent > len) { elf_reject = "program headers past the end of the file"; return 0; }

    top = USER_BASE;
    loaded = 0;
    i = 0;
    while (i < phnum) {
        char *ph;
        ph = img + phoff + i * phent;
        if (rd_le(ph, 0, 4) == PT_LOAD) {
            long vaddr;
            long memsz;
            vaddr = rd_le(ph, 16, 8);
            memsz = rd_le(ph, 40, 8);
            if (!elf_map_segment(root, img, len, ph)) return 0;
            if (vaddr + memsz > top) top = vaddr + memsz;
            loaded = loaded + 1;
        }
        i = i + 1;
    }
    if (!loaded) { elf_reject = "no loadable segments"; return 0; }

    top_out[0] = (top + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    return rd_le(img, 24, 8);
}

// ---------- processes ----------

#define MAX_PROCS 16
#define MAX_FDS   8

#define P_FREE    0
#define P_RUNNING 1
#define P_EXITED  2
#define P_KILLED  3

struct Proc {
    long state;
    long pid;
    long root;                 // its page-table root
    long tid;                  // the kernel thread running it
    long entry;
    long brk_base;             // first byte of its heap
    long brk;                  // current break
    long exitcode;
    long fd_ino[MAX_FDS];      // 0 = closed; entries 0..2 are the console
    long fd_pos[MAX_FDS];
    char name[32];
};

struct Proc g_procs[MAX_PROCS];
long g_next_pid;
long g_proc_faults;

void proc_init() {
    long i;
    i = 0;
    while (i < MAX_PROCS) { g_procs[i].state = P_FREE; g_procs[i].pid = 0; i = i + 1; }
    g_next_pid = 1;
    g_proc_faults = 0;
    enable_write_protect();
}

long proc_slot() {
    long i;
    i = 0;
    while (i < MAX_PROCS) { if (g_procs[i].state == P_FREE) return i; i = i + 1; }
    return -1;
}

long proc_by_tid(long tid) {
    long i;
    i = 0;
    while (i < MAX_PROCS) {
        if (g_procs[i].state == P_RUNNING && g_procs[i].tid == tid) return i;
        i = i + 1;
    }
    return -1;
}

// Build the initial stack frame in the process's own address space, one poked
// word at a time. It has to look exactly like a thread caught mid-interrupt,
// for the same reason thread_build_stack does: that is the only shape
// isr_common knows how to resume.
long proc_build_stack(long root, long stack_top, long entry, long arg) {
    long s;
    long i;

    s = stack_top;

    s = s - 8; as_poke(root, s, 0x10);         // ss
    s = s - 8; as_poke(root, s, stack_top);    // rsp after iretq
    s = s - 8; as_poke(root, s, 0x202);        // rflags, interrupts enabled
    s = s - 8; as_poke(root, s, 0x08);         // cs
    s = s - 8; as_poke(root, s, entry);        // rip

    s = s - 8; as_poke(root, s, 0);            // error code
    s = s - 8; as_poke(root, s, 32);           // vector

    s = s - 8; as_poke(root, s, 0);            // rax
    s = s - 8; as_poke(root, s, 0);            // rbx
    s = s - 8; as_poke(root, s, 0);            // rcx
    s = s - 8; as_poke(root, s, 0);            // rdx
    s = s - 8; as_poke(root, s, 0);            // rsi
    s = s - 8; as_poke(root, s, arg);          // rdi = the argument
    s = s - 8; as_poke(root, s, 0);            // rbp
    i = 0;
    while (i < 8) { s = s - 8; as_poke(root, s, 0); i = i + 1; }   // r8..r15

    return s;
}

char *proc_reject;

// Load `path` off the filesystem into a new address space and start it.
// Returns a pid, or 0 with proc_reject set.
long proc_spawn(char *path, long arg, char *name) {
    long ino;
    long size;
    char *img;
    long slot;
    long root;
    long entry;
    long top;
    long rsp;
    long tid;
    long va;

    proc_reject = "";
    ino = fs_lookup(path);
    if (!ino)                    { proc_reject = "no such file"; return 0; }
    if (fs_type(ino) == T_DIR)   { proc_reject = "that is a directory"; return 0; }
    size = fs_size(ino);
    if (size <= 0)               { proc_reject = "empty file"; return 0; }
    if (size > ELF_MAX)          { proc_reject = "larger than the loader accepts"; return 0; }

    slot = proc_slot();
    if (slot < 0)                { proc_reject = "no free process slot"; return 0; }

    img = (char *)kmalloc(size);
    if (!img)                    { proc_reject = "out of kernel memory"; return 0; }
    if (fs_read(ino, 0, img, size) != size) { kfree(img); proc_reject = "short read"; return 0; }

    root = as_create();
    if (!root)                   { kfree(img); proc_reject = "out of frames"; return 0; }

    top = 0;
    entry = elf_load(root, img, size, &top);
    kfree(img);
    if (!entry) { as_destroy(root); proc_reject = elf_reject; return 0; }

    // The stack: mapped writable, and NOT next to the program's own data --
    // there is a wide unmapped gap between them, so a runaway stack faults
    // rather than eating the globals.
    va = USER_STACK_TOP - USER_STACK_SIZE;
    while (va < USER_STACK_TOP) {
        if (!as_touch(root, va, PTE_USER | PTE_WRITE)) {
            as_destroy(root);
            proc_reject = "out of frames for the stack";
            return 0;
        }
        va = va + PAGE_SIZE;
    }

    g_procs[slot].root = root;
    g_procs[slot].entry = entry;
    g_procs[slot].brk_base = top + USER_HEAP_GAP;
    g_procs[slot].brk = top + USER_HEAP_GAP;
    g_procs[slot].exitcode = 0;
    g_procs[slot].pid = g_next_pid;
    {
        long i;
        i = 0;
        while (i < MAX_FDS) { g_procs[slot].fd_ino[i] = 0; g_procs[slot].fd_pos[i] = 0; i = i + 1; }
    }
    {
        long i;
        i = 0;
        while (i < 31 && name[i]) { g_procs[slot].name[i] = name[i]; i = i + 1; }
        g_procs[slot].name[i] = 0;
    }

    rsp = proc_build_stack(root, USER_STACK_TOP - 16, entry, arg);
    tid = thread_adopt(rsp, root, g_procs[slot].pid, name);
    if (tid < 0) { as_destroy(root); proc_reject = "no free thread slot"; return 0; }

    g_procs[slot].tid = tid;
    g_procs[slot].state = P_RUNNING;
    g_next_pid = g_next_pid + 1;
    return g_procs[slot].pid;
}

// Reap processes whose thread has finished, and hand back their address spaces.
// Returns how many were reaped.
//
// The freeing cannot happen where the process ends -- thread_exit runs on the
// dying process's own stack, inside the address space it would be destroying.
// This runs on somebody else's thread, which is the first point at which the
// space is provably not in use.
long proc_poll() {
    long i;
    long n;
    n = 0;
    i = 0;
    while (i < MAX_PROCS) {
        if (g_procs[i].state == P_RUNNING) {
            long t;
            t = g_procs[i].tid;
            if (g_threads[t].state == T_DONE || g_threads[t].state == T_UNUSED) {
                if (g_threads[t].faulted) {
                    g_procs[i].state = P_KILLED;
                    g_proc_faults = g_proc_faults + 1;
                } else {
                    g_procs[i].state = P_EXITED;
                    g_procs[i].exitcode = g_threads[t].retval;
                }
                as_destroy(g_procs[i].root);
                g_procs[i].root = 0;
                g_threads[t].state = T_UNUSED;
                n = n + 1;
            }
        }
        i = i + 1;
    }
    return n;
}

long proc_alive() {
    long i;
    long n;
    n = 0;
    i = 0;
    while (i < MAX_PROCS) { if (g_procs[i].state == P_RUNNING) n = n + 1; i = i + 1; }
    return n;
}

// Wait for a pid, yielding meanwhile. Returns its exit code, or -1.
long proc_wait(long pid) {
    long i;
    for (;;) {
        proc_poll();
        i = 0;
        while (i < MAX_PROCS) {
            if (g_procs[i].pid == pid) {
                if (g_procs[i].state == P_EXITED) return g_procs[i].exitcode;
                if (g_procs[i].state == P_KILLED) return -1;
            }
            i = i + 1;
        }
        thread_yield();
    }
}

// ---------- the syscall boundary ----------
// Vector 0x80. The numbers are ours, not Linux's, and are listed in
// user/nano-user.h so both sides read from one place.
#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_READ   2
#define SYS_OPEN   3
#define SYS_CLOSE  4
#define SYS_SEEK   5
#define SYS_SIZE   6
#define SYS_SBRK   7
#define SYS_GETPID 8
#define SYS_YIELD  9
#define SYS_TICKS  10
#define SYS_UNLINK 11

// Set by a syscall that must not simply return to its caller. The dispatcher
// in nano-int.h checks it and reschedules instead.
long g_syscall_resched;
long g_syscalls;

// Which process is running, by the thread that is running it. Going through
// the thread id rather than a "current process" global means there is one
// source of truth: if the scheduler switched, this answer changed with it.
long proc_current() { return proc_by_tid(g_current); }

long sys_open(long slot, char *path, long mode) {
    long ino;
    long fd;
    if (slot < 0) return -1;
    ino = fs_lookup(path);
    if (!ino && mode == 1) ino = fs_create(path);
    if (!ino) return -1;
    fd = 3;
    while (fd < MAX_FDS) {
        if (!g_procs[slot].fd_ino[fd]) {
            g_procs[slot].fd_ino[fd] = ino;
            g_procs[slot].fd_pos[fd] = 0;
            return fd;
        }
        fd = fd + 1;
    }
    return -1;
}

long sys_sbrk(long slot, long delta) {
    long old;
    long want;
    long va;

    if (slot < 0) return 0;
    old = g_procs[slot].brk;
    if (delta <= 0) return old;
    want = old + delta;
    if (want > USER_STACK_TOP - USER_STACK_SIZE) return 0;   // would hit the stack

    va = old & ~(PAGE_SIZE - 1);
    while (va < want) {
        if (!vmm_resolve_in(g_procs[slot].root, va)) {
            if (!as_touch(g_procs[slot].root, va, PTE_USER | PTE_WRITE)) return 0;
        }
        va = va + PAGE_SIZE;
    }
    g_procs[slot].brk = want;
    return old;
}

// The whole boundary. Arguments arrive in registers and pointers are user
// virtual addresses in the address space that is currently installed -- which
// is the calling process's, because the interrupt did not change it.
//
// Those pointers are NOT validated. At CPL 0 there is nothing to validate
// against: the process could dereference the same address itself without
// asking. Once programs run in ring 3 every pointer here needs a range check,
// and that check is part of the ring-3 milestone rather than something to
// pretend is already here.
long syscall_dispatch(long nr, long a, long b, long c) {
    long slot;

    g_syscalls = g_syscalls + 1;
    slot = proc_current();

    if (nr == SYS_EXIT) {
        g_threads[g_current].retval = a;
        g_threads[g_current].state = T_DONE;
        if (g_threads[g_current].joiner >= 0) {
            long j;
            j = g_threads[g_current].joiner;
            if (g_threads[j].state == T_BLOCKED) g_threads[j].state = T_READY;
        }
        // Do not return into the process: it is finished, and its stack is
        // about to belong to nobody.
        g_syscall_resched = 1;
        return 0;
    }

    if (nr == SYS_WRITE) {
        char *p;
        long i;
        if (a == 1 || a == 2) {
            p = (char *)b;
            i = 0;
            while (i < c) { putc(p[i]); i = i + 1; }
            return c;
        }
        if (slot < 0 || a < 3 || a >= MAX_FDS || !g_procs[slot].fd_ino[a]) return -1;
        {
            long n;
            n = fs_write(g_procs[slot].fd_ino[a], g_procs[slot].fd_pos[a], (char *)b, c);
            if (n > 0) g_procs[slot].fd_pos[a] = g_procs[slot].fd_pos[a] + n;
            return n;
        }
    }

    if (nr == SYS_READ) {
        // fd 0 is the console, and there is nothing behind it yet: a process
        // has no terminal of its own, and handing it the shell's keyboard
        // would let a background task eat the shell's keystrokes. Returning 0
        // means end-of-file, which is at least a truthful answer.
        if (a == 0) return 0;
        if (slot < 0 || a < 3 || a >= MAX_FDS || !g_procs[slot].fd_ino[a]) return -1;
        {
            long n;
            n = fs_read(g_procs[slot].fd_ino[a], g_procs[slot].fd_pos[a], (char *)b, c);
            if (n > 0) g_procs[slot].fd_pos[a] = g_procs[slot].fd_pos[a] + n;
            return n;
        }
    }

    if (nr == SYS_OPEN)  return sys_open(slot, (char *)a, b);

    if (nr == SYS_CLOSE) {
        if (slot < 0 || a < 3 || a >= MAX_FDS) return -1;
        g_procs[slot].fd_ino[a] = 0;
        return 0;
    }

    if (nr == SYS_SEEK) {
        if (slot < 0 || a < 3 || a >= MAX_FDS || !g_procs[slot].fd_ino[a]) return -1;
        g_procs[slot].fd_pos[a] = b;
        return b;
    }

    if (nr == SYS_SIZE) {
        if (slot < 0 || a < 3 || a >= MAX_FDS || !g_procs[slot].fd_ino[a]) return -1;
        return fs_size(g_procs[slot].fd_ino[a]);
    }

    if (nr == SYS_SBRK)   return sys_sbrk(slot, a);
    if (nr == SYS_GETPID) { if (slot < 0) return 0; return g_procs[slot].pid; }
    if (nr == SYS_YIELD)  { g_syscall_resched = 1; return 0; }
    // SYS_TICKS is answered by the dispatcher in nano-int.h, which is the file
    // that owns g_ticks and is included after this one.
    if (nr == SYS_UNLINK) return fs_unlink((char *)a);

    return -1;
}

#endif
