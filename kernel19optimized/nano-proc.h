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
extern long enable_nx();

// Whether the NX bit means anything on this machine. Set once, in proc_init,
// from the CPU's own answer -- not assumed, because a page marked no-execute
// on a CPU without NXE is not a hardened page, it is a page that faults on
// every access including reads.
long g_nx_on;

// The no-execute bit, or nothing at all. Every caller goes through this rather
// than naming PTE_NX directly, so there is one place that knows the difference
// between "the CPU supports this" and "we would like it to".
long nx_bit() { if (g_nx_on) return PTE_NX; return 0; }

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
    long fresh;
    long want;

    have = vmm_flags_in(root, virt);
    phys = vmm_resolve_in(root, virt) & ~(PAGE_SIZE - 1);
    fresh = 0;
    if (!phys) {
        phys = frame_alloc_zeroed();
        if (!phys) return 0;
        have = 0;
        fresh = 1;
    }
    // The union of what the page already had and what this segment needs. A
    // page shared by a read-only and a writable segment has to be writable;
    // pretending otherwise gives a fault on the first store to a global.
    want = flags | (have & (PTE_WRITE | PTE_USER));

    // NX goes the OTHER WAY, and getting that backwards would be silent: a
    // page is executable if ANY segment sharing it is executable, so the
    // no-execute bit only survives when both sides asked for it. Taking the
    // union here instead would mark a shared code/data page non-executable and
    // the program would fault on its own instructions.
    if (!fresh && !(have & PTE_NX)) want = want & ~PTE_NX;

    if (!vmm_map_in(root, virt, phys, want)) return 0;
    as_mark_user_path(root, virt);
    return phys;
}

// Is this page present AND reachable by the process itself?
//
// PTE_USER is the part that matters. A page that is present but has the user
// bit clear is a KERNEL page that happens to be mapped in this address space
// -- which is every page of the identity map, in every process. Checking only
// for presence would accept all of them and validate nothing.
long as_user_page(long root, long virt) {
    long f;
    f = vmm_flags_in(root, virt);
    if (!(f & PTE_PRESENT)) return 0;
    if (!(f & PTE_USER)) return 0;
    return 1;
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

// Copy bytes into another address space, one page-chunk at a time.
//
// as_poke writes one aligned word and gets away with never crossing a page.
// A string does cross, and a copy that resolved the page once and then ran off
// the end of it would write into whatever frame happened to be next -- which
// in a fresh address space is usually the program's own text.
long as_copy_in(long root, long virt, char *src, long n) {
    long done;
    done = 0;
    while (done < n) {
        long dst;
        long pageoff;
        long chunk;
        dst = vmm_resolve_in(root, virt + done);
        if (!dst) return 0;
        pageoff = (virt + done) & (PAGE_SIZE - 1);
        chunk = PAGE_SIZE - pageoff;
        if (chunk > n - done) chunk = n - done;
        memcpy((char *)dst, src + done, chunk);
        done = done + chunk;
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

    // ---- what the segment is allowed to BE, as opposed to where it goes ----
    //
    // These are properties of the file, checkable before a byte of it is
    // mapped, and they cannot be worked around by choosing different contents.
    // That is the whole reason to prefer them to inspecting the bytes: a
    // scanner looks for a shape somebody chose, and a shape can be changed.

    // Writable and executable at once. Every technique that ends in "and then
    // jump to the bytes we just wrote" needs one page that is both, and no
    // honest segment here is.
    if ((eflags & PF_W) && (eflags & PF_X)) { elf_reject = "segment is both writable and executable"; return 0; }

    // Zero-filled executable pages. memsz beyond filesz is memory the loader
    // supplies rather than the file, so an executable segment asking for it is
    // asking for a run of zero bytes it can execute -- which no compiler emits
    // and which is a comfortable place to land.
    if ((eflags & PF_X) && memsz > filesz) { elf_reject = "executable segment wants zero-filled pages"; return 0; }

    // Executable and not readable is not a thing x86-64 paging can express, so
    // a file asking for it is describing something the loader would silently
    // widen. Say no rather than quietly granting more than was asked.
    if ((eflags & PF_X) && !(eflags & PF_R)) { elf_reject = "executable segment is not readable"; return 0; }

    flags = PTE_USER;
    if (eflags & PF_W) flags = flags | PTE_WRITE;
    // Whatever the file did not ask to be executable, is not.
    if (!(eflags & PF_X)) flags = flags | nx_bit();

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
    long entry;
    long entry_ok;
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
    entry_ok = 0;
    entry = rd_le(img, 24, 8);
    i = 0;
    while (i < phnum) {
        char *ph;
        ph = img + phoff + i * phent;
        if (rd_le(ph, 0, 4) == PT_LOAD) {
            long vaddr;
            long memsz;
            long eflags;
            vaddr = rd_le(ph, 16, 8);
            memsz = rd_le(ph, 40, 8);
            eflags = rd_le(ph, 4, 4);
            if (!elf_map_segment(root, img, len, ph)) return 0;
            if (vaddr + memsz > top) top = vaddr + memsz;
            // The entry point has to land in something the file itself marked
            // executable. Without this the header could point anywhere the
            // program has memory -- its own data, its own stack -- and the
            // loader would jump there and let the fault explain it afterwards.
            if ((eflags & PF_X) && entry >= vaddr && entry < vaddr + memsz) entry_ok = 1;
            loaded = loaded + 1;
        }
        i = i + 1;
    }
    if (!loaded) { elf_reject = "no loadable segments"; return 0; }
    if (!entry_ok) { elf_reject = "entry point is not in an executable segment"; return 0; }

    top_out[0] = (top + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    return entry;
}

// ---------- processes ----------

#define MAX_PROCS 16

// 16, not 8. The compiler holds its source file, its output file and every
// header it is nested inside open at the same time, and 8 was enough right up
// until a program included a header that included another one.
#define MAX_FDS   16

// argv. Eight arguments of 128 bytes is not a POSIX limit, it is a limit that
// fits on the initial stack page without needing a second one.
#define MAX_ARGS  8
#define ARG_MAX   128

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
    char cwd[64];              // what a relative path is relative to
    // Where a resolved relative path is assembled. Per-process rather than one
    // shared buffer: the timer can preempt a syscall anywhere, and two
    // processes opening relative paths would otherwise take turns overwriting
    // each other's filename between building it and using it.
    char pathbuf[128];
};

struct Proc g_procs[MAX_PROCS];

// How many finished processes have had their table entry taken back because
// nothing was left. Worth counting rather than doing silently: it is exactly
// the number of exit codes that were thrown away.
long g_procs_recycled;

long g_next_pid;
long g_proc_faults;

void proc_init() {
    long i;
    i = 0;
    while (i < MAX_PROCS) { g_procs[i].state = P_FREE; g_procs[i].pid = 0; i = i + 1; }
    g_next_pid = 1;
    g_proc_faults = 0;
    g_procs_recycled = 0;
    enable_write_protect();
    g_nx_on = enable_nx();
}

long proc_slot() {
    long i;
    long oldest;
    long oldest_pid;

    i = 0;
    while (i < MAX_PROCS) { if (g_procs[i].state == P_FREE) return i; i = i + 1; }

    // Nothing free. Take back the OLDEST finished entry.
    //
    // Without this the table fills up with the corpses of processes that ran
    // and exited, and the machine can only ever run MAX_PROCS programs in its
    // whole lifetime -- which is what happened the first time this image left
    // a program respawning on a loop for a screenshot: it ran ten times and
    // then quietly stopped, and the desktop was empty.
    //
    // Oldest by pid, because pids only increase. Recycling loses that entry's
    // exit code, so the count above says how often it has happened; if it is
    // climbing, something is spawning and never collecting.
    oldest = -1;
    oldest_pid = 0;
    i = 0;
    while (i < MAX_PROCS) {
        if (g_procs[i].state == P_EXITED || g_procs[i].state == P_KILLED) {
            if (oldest < 0 || g_procs[i].pid < oldest_pid) {
                oldest = i;
                oldest_pid = g_procs[i].pid;
            }
        }
        i = i + 1;
    }
    if (oldest >= 0) {
        g_procs[oldest].state = P_FREE;
        g_procs[oldest].pid = 0;
        g_procs_recycled = g_procs_recycled + 1;
        return oldest;
    }
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
long proc_build_stack(long root, long stack_top, long entry, long argc, long argv) {
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
    // The C calling convention, arranged by iretq: _start does `call main`
    // without touching either register, so whatever is here is what main's
    // (argc, argv) are.
    s = s - 8; as_poke(root, s, argv);         // rsi = argv
    s = s - 8; as_poke(root, s, argc);         // rdi = argc
    s = s - 8; as_poke(root, s, 0);            // rbp
    i = 0;
    while (i < 8) { s = s - 8; as_poke(root, s, 0); i = i + 1; }   // r8..r15

    return s;
}

char *proc_reject;

// Lay argc/argv out at the top of the new process's stack and return the
// address the argv array ended up at, with *sp_out moved below everything
// written. Zero means the arguments did not fit.
//
// The strings have to live in the process's own memory: the kernel's copies
// are in the kernel heap, which the program can reach today only because
// nothing runs in ring 3 yet, and which would be a dangling pointer the moment
// anything freed them.
long proc_push_args(long root, long argc, char **argv, long *sp_out) {
    long s;
    long addr[MAX_ARGS];
    long i;

    s = sp_out[0];
    if (argc < 0 || argc > MAX_ARGS) return 0;

    i = 0;
    while (i < argc) {
        long len;
        len = 0;
        while (argv[i][len] && len < ARG_MAX - 1) len = len + 1;
        s = s - (len + 1);
        s = s & ~7;
        if (!as_copy_in(root, s, argv[i], len)) return 0;
        if (!as_copy_in(root, s + len, "", 1)) return 0;   // the terminator
        addr[i] = s;
        i = i + 1;
    }

    // The array itself, with the NULL that tells a program where argv ends
    // even if it ignores argc.
    s = s - (argc + 1) * 8;
    s = s & ~15;
    i = 0;
    while (i < argc) { if (!as_poke(root, s + i * 8, addr[i])) return 0; i = i + 1; }
    if (!as_poke(root, s + argc * 8, 0)) return 0;

    sp_out[0] = s;
    return s;
}

// Load `path` off the filesystem into a new address space and start it.
// Returns a pid, or 0 with proc_reject set.
long proc_spawn(char *path, long argc, char **argv, char *name, char *cwd) {
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
    // A stack is never code, and neither is a heap. Saying so costs nothing
    // and removes the two places a program is most likely to be persuaded to
    // execute something it was handed rather than something it was built from.
    va = USER_STACK_TOP - USER_STACK_SIZE;
    while (va < USER_STACK_TOP) {
        if (!as_touch(root, va, PTE_USER | PTE_WRITE | nx_bit())) {
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
        i = 0;
        if (cwd) while (i < 63 && cwd[i]) { g_procs[slot].cwd[i] = cwd[i]; i = i + 1; }
        if (!i) { g_procs[slot].cwd[0] = '/'; i = 1; }
        g_procs[slot].cwd[i] = 0;
    }

    {
        long sp;
        long uargv;
        sp = USER_STACK_TOP - 16;
        uargv = proc_push_args(root, argc, argv, &sp);
        if (!uargv && argc > 0) { as_destroy(root); proc_reject = "arguments too long"; return 0; }
        rsp = proc_build_stack(root, sp, entry, argc, uargv);
    }
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
#ifdef NANO_WM_H
                // A window with no process behind it can never be repainted,
                // and its handle can be reissued to somebody else.
                win_close_owned(g_procs[i].pid);
#endif
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
// A heartbeat while waiting, because the longest thing this OS does is compile
// something, and a compile that prints nothing until it finishes is
// indistinguishable from a machine that has stopped. When the in-OS build of a
// 2,500-line file stalled, the serial log's last line was the one saying the
// compile had started -- and that told me nothing about whether anything was
// still running.
//
// Every 200 ticks, which is far apart enough not to be noise and close enough
// that a stall is obvious within seconds of watching.
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
#define SYS_BRK    12
// Sleep for n milliseconds. SYS_YIELD gives up the rest of a slice and is
// immediately runnable again, which is the wrong thing for a program that has
// nothing to do until the next frame: round-robin keeps handing it the CPU and
// it keeps handing it back. This takes the process out of the ready set until
// the timer puts it back, so a GUI program idling at 20 frames a second costs
// the machine nothing between frames.
//
// Answered in nano-int.h rather than here, for the same reason SYS_TICKS is:
// it needs the clock, and the clock is defined in the file that includes this
// one.
#define SYS_NAP    18

// The window calls. Numbers exist whether or not a window manager was
// compiled in; the IMPLEMENTATIONS are behind #ifdef NANO_WM_H and every one
// of them answers -1 when it is absent. A syscall number that means one thing
// in one image and nothing in another would be far worse than a number that
// always means the same thing and sometimes fails.
#define SYS_WINOPEN    13
#define SYS_WINBLIT    14
#define SYS_WINPRESENT 15
#define SYS_WINPOLL    16
#define SYS_WINCLOSE   17

// ---------- validating a pointer that came from a process ----------
//
// Every syscall below that takes a pointer used to dereference it as given.
// That is a hole the size of the machine: SYS_WRITE on fd 1 does putc(p[i]),
// so a process could hand it a kernel address and have the kernel print its
// own memory to the console, and SYS_WINPOLL writes six longs to wherever it
// is told. The kernel runs on the process's page tables -- the identity map
// is in PML4 entry 0 and is present in every address space -- so a kernel
// address supplied by a process resolves and the access succeeds.
//
// K18 tested the window blit's CLIPPING from inside a process and was right
// to; what it did not test was the pointer. Clipping decides how much gets
// copied, and says nothing about where it is copied FROM.
//
// The range must lie inside user space AND every page of it must be present
// and USER-accessible in that process's own tables. Checking the bounds alone
// would not be enough: a user-space address that the process never mapped
// would fault in the kernel, which is a crash rather than an exploit but is
// still the kernel dying for a caller's mistake.
long user_range_ok(long slot, long ptr, long bytes) {
    long va;
    long end;

    if (slot < 0) return 0;
    if (bytes <= 0) return 0;
    if (ptr < USER_BASE) return 0;
    if (ptr >= USER_TOP) return 0;

    end = ptr + bytes;
    if (end < ptr) return 0;                 // wrapped
    if (end > USER_TOP) return 0;

    va = ptr & ~(PAGE_SIZE - 1);
    while (va < end) {
        if (!as_user_page(g_procs[slot].root, va)) return 0;
        va = va + PAGE_SIZE;
    }

    return 1;
}

// A NUL-terminated string from a process. The length is not known in advance,
// so the pages are checked as the string is walked -- and it must terminate
// inside user space rather than running off the end of the last mapped page.
long user_string_ok(long slot, long ptr, long max) {
    long i;

    if (slot < 0 || ptr < USER_BASE || ptr >= USER_TOP) return 0;

    i = 0;
    while (i < max) {
        char *c;
        if (!user_range_ok(slot, ptr + i, 1)) return 0;
        c = (char *)(ptr + i);
        if (c[0] == 0) return 1;
        i = i + 1;
    }

    return 0;
}

// Set by a syscall that must not simply return to its caller. The dispatcher
// in nano-int.h checks it and reschedules instead.
long g_syscall_resched;
long g_syscalls;

// Which process is running, by the thread that is running it. Going through
// the thread id rather than a "current process" global means there is one
// source of truth: if the scheduler switched, this answer changed with it.
long proc_current() { return proc_by_tid(g_current); }

// The flag values are Linux's, because the C library that sits on top of this
// (nano-libc.h, shared with the hosted build) already speaks them and giving
// them different numbers here would mean a translation layer whose only job is
// to be wrong once.
//
// Only two bits do anything. There is no permission model, so a mode argument
// would be a number the kernel records and never checks -- worse than not
// having one, because it looks like a control.
#define O_CREAT 64
#define O_TRUNC 512

// Turn whatever the program passed into an absolute path.
//
// This exists because of one line in the compiler: `#include "util.h"` opens
// the name verbatim, so without a working directory a program can only ever
// include a header by its full path -- and then the same source file cannot be
// compiled here and on Linux, which kills the only comparison worth making.
//
// Deliberately does NOT understand "." or "..". The shell normalises those when
// it sets its own cwd, and a second, subtly different implementation of path
// cleanup in the kernel is how two parts of a system come to disagree about
// which file a name means.
char *proc_path(long slot, char *path) {
    long n;
    long i;
    if (slot < 0 || path[0] == '/') return path;

    n = 0;
    while (g_procs[slot].cwd[n] && n < 63) { g_procs[slot].pathbuf[n] = g_procs[slot].cwd[n]; n = n + 1; }
    if (n && g_procs[slot].pathbuf[n - 1] != '/') { g_procs[slot].pathbuf[n] = '/'; n = n + 1; }
    i = 0;
    while (path[i] && n < 127) { g_procs[slot].pathbuf[n] = path[i]; n = n + 1; i = i + 1; }
    g_procs[slot].pathbuf[n] = 0;
    // A name that did not fit would resolve to a DIFFERENT, shorter path that
    // might well exist. Refusing is the only safe answer.
    if (path[i]) return "";
    return g_procs[slot].pathbuf;
}

long sys_open(long slot, char *path, long flags) {
    long ino;
    long fd;
    if (slot < 0) return -1;
    path = proc_path(slot, path);
    ino = fs_lookup(path);
    if (!ino && (flags & O_CREAT)) ino = fs_create(path);
    if (!ino) return -1;
    // Truncate an EXISTING file that is being opened for writing. Without this
    // a second, shorter write leaves the tail of the first one in place and
    // the file is a valid-looking splice of two different outputs -- which is
    // exactly what a compiler run over an earlier, longer output would produce.
    if ((flags & O_TRUNC) && fs_type(ino) == T_FILE) fs_truncate(ino);
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
            if (!as_touch(g_procs[slot].root, va, PTE_USER | PTE_WRITE | nx_bit())) return 0;
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
#ifdef NANO_WM_H
// ---------- windows, owned by processes ----------
//
// A window handle crosses the syscall boundary, and that is the whole point of
// this layer: the process asks for a window, draws into ITS OWN memory, and
// blits. The window's backing buffer never leaves the kernel, so the address
// space isolation from K7 holds -- a process cannot be handed a pointer into
// another process's window, because it is never handed a pointer at all.
//
// Ownership is recorded here rather than in struct Win, so that nano-wm.h
// stays a window manager and knows nothing about processes. A window opened by
// the kernel itself has owner 0, which no process has.
long g_win_owner[WM_MAXWIN];

long win_owned_by(long hnd, long pid) {
    if (hnd < 0 || hnd >= WM_MAXWIN) return 0;
    if (!g_win[hnd].used) return 0;
    return g_win_owner[hnd] == pid && pid != 0;
}

// Destroy every window a process left behind. Called from proc_poll, which is
// the first point at which the process is provably not running.
//
// Without this a window outlives the only thing that could repaint it: it sits
// on the desktop showing the last frame forever, and worse, the handle can be
// reissued to a different process which then inherits somebody else's pixels.
long win_close_owned(long pid) {
    long i;
    long n;
    n = 0;
    if (pid == 0) return 0;
    i = 0;
    while (i < WM_MAXWIN) {
        if (g_win[i].used && g_win_owner[i] == pid) {
            g_win_owner[i] = 0;
            wm_destroy(i);
            n = n + 1;
        }
        i = i + 1;
    }
    return n;
}
#endif

// Those pointers are NOT validated. At CPL 0 there is nothing to validate
// against: the process could dereference the same address itself without
// asking. Once programs run in ring 3 every pointer here needs a range check,
// and that check is part of the ring-3 milestone rather than something to
// pretend is already here.
// Six arguments, which is exactly nano_cc's ceiling and exactly what
// SYS_WINBLIT needs: a handle, a pointer, a width, a height and a destination
// x and y. The first three arrive in rdi/rsi/rdx as before; the fourth and
// fifth are r10 and r8, which is the register Linux picked for the same reason
// -- `syscall` destroys rcx, so the fourth argument cannot live there.
//
// Everything that already existed still passes 0 for d and e, so no existing
// program or call site changes meaning.
long syscall_dispatch(long nr, long a, long b, long c, long d, long e) {
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
        if (c < 0) return -1;
        if (!user_range_ok(slot, b, c)) return -1;
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
        if (c < 0) return -1;
        if (!user_range_ok(slot, b, c)) return -1;
        {
            long n;
            n = fs_read(g_procs[slot].fd_ino[a], g_procs[slot].fd_pos[a], (char *)b, c);
            if (n > 0) g_procs[slot].fd_pos[a] = g_procs[slot].fd_pos[a] + n;
            return n;
        }
    }

    if (nr == SYS_OPEN) {
        if (!user_string_ok(slot, a, 256)) return -1;
        return sys_open(slot, (char *)a, b);
    }

    if (nr == SYS_CLOSE) {
        if (slot < 0 || a < 3 || a >= MAX_FDS) return -1;
        g_procs[slot].fd_ino[a] = 0;
        return 0;
    }

    // lseek(fd, off, whence). The whence argument is c; 0 is SEEK_SET, which
    // is what every caller before this passed implicitly by leaving c zero, so
    // adding it changed nothing that already worked.
    if (nr == SYS_SEEK) {
        long base;
        if (slot < 0 || a < 3 || a >= MAX_FDS || !g_procs[slot].fd_ino[a]) return -1;
        if (c == 1)      base = g_procs[slot].fd_pos[a];
        else if (c == 2) base = fs_size(g_procs[slot].fd_ino[a]);
        else if (c == 0) base = 0;
        else             return -1;
        if (base + b < 0) return -1;
        g_procs[slot].fd_pos[a] = base + b;
        return base + b;
    }

    if (nr == SYS_SIZE) {
        if (slot < 0 || a < 3 || a >= MAX_FDS || !g_procs[slot].fd_ino[a]) return -1;
        return fs_size(g_procs[slot].fd_ino[a]);
    }

    if (nr == SYS_SBRK)   return sys_sbrk(slot, a);

    // brk(addr): move the break to an absolute address and return where it
    // ended up; brk(0) just reports it. That is the Linux shape, and it is
    // here because nano-libc.h's allocator was written against it -- the same
    // library file serves the hosted build and this one, so the cheapest place
    // to absorb the difference is the kernel that has a choice.
    //
    // On failure it returns the OLD break unchanged, which is also Linux's
    // behaviour and is what the allocator's `got < want` check is looking for.
    // Returning -1 here would be read as an enormous successful break.
    if (nr == SYS_BRK) {
        long cur;
        if (slot < 0) return 0;
        cur = g_procs[slot].brk;
        if (a <= cur) return cur;
        if (!sys_sbrk(slot, a - cur)) return cur;
        return g_procs[slot].brk;
    }

    if (nr == SYS_GETPID) { if (slot < 0) return 0; return g_procs[slot].pid; }
    if (nr == SYS_YIELD)  { g_syscall_resched = 1; return 0; }
    // SYS_TICKS is answered by the dispatcher in nano-int.h, which is the file
    // that owns g_ticks and is included after this one.
    if (nr == SYS_UNLINK) return fs_unlink(proc_path(slot, (char *)a));

#ifdef NANO_WM_H
    // ---------- the window calls ----------

    // (x, y, w, h, title) -> a handle, or -1. The title is copied by
    // wm_create, so the process's string does not have to outlive the call.
    if (nr == SYS_WINOPEN) {
        long hnd;
        if (slot < 0) return -1;
        if (c < 16 || d < 16 || c > 1024 || d > 768) return -1;
        hnd = wm_create(a, b, c, d, (char *)e);
        if (hnd < 0) return -1;
        wm_decorate(hnd);
        g_win_owner[hnd] = g_procs[slot].pid;
        wm_raise(hnd);
        return hnd;
    }

    // (handle, pixels, w, h, offset) -> pixels copied, or -1.
    //
    // ONE destination offset, not an x and a y, and that is the argument limit
    // showing up again: syscall_dispatch takes six parameters and the syscall
    // number is one of them, so five are left. A handle, a pointer, a width, a
    // height and a position is six things. So the position is a LINEAR offset
    // into the client area -- row-major, the same index you would use into the
    // backing buffer itself -- which is one number and reads as one idea,
    // rather than an x and a y bit-packed into a long, which is one number and
    // reads as a trick.
    //
    // The copy goes one row at a time and is clipped to the window's CLIENT
    // area, so a process cannot paint over its own title bar or past the edge
    // of its window, let alone into the next one. Clipping here rather than
    // trusting the caller is the whole difference between a syscall and a
    // shared buffer.
    if (nr == SYS_WINBLIT) {
        long *src;
        long j;
        long copied;
        long cw;
        long ch;
        long ox;
        long oy;
        if (slot < 0 || !win_owned_by(a, g_procs[slot].pid)) return -1;
        if (b == 0 || c <= 0 || d <= 0) return -1;
        // c*d longs are about to be READ from the caller's buffer. The
        // clipping below decides how much lands in the window; it says
        // nothing about where it comes from.
        if (c > 1 << 20 || d > 1 << 20) return -1;
        if (!user_range_ok(slot, b, c * d * 8)) return -1;
        cw = wm_client_w(a);
        ch = wm_client_h(a);
        if (cw <= 0 || ch <= 0) return -1;
        if (e < 0) return -1;
        ox = e % cw;
        oy = e / cw;
        src = (long *)b;
        copied = 0;
        j = 0;
        while (j < d) {
            long dy;
            dy = oy + j;
            if (dy >= 0 && dy < ch) {
                long i;
                i = 0;
                while (i < c) {
                    long dx;
                    dx = ox + i;
                    if (dx >= 0 && dx < cw) {
                        wm_win_pixel(a, wm_client_x() + dx, wm_client_y() + dy,
                                     src[j * c + i]);
                        copied = copied + 1;
                    }
                    i = i + 1;
                }
            }
            j = j + 1;
        }
        // Damage only the rectangle that was actually written, clipped the
        // same way. Invalidating the whole client area would work and would
        // throw away everything the compositor milestone was for.
        {
            long rx; long ry; long rw; long rh;
            rx = ox; ry = oy; rw = c; rh = d;
            if (rx < 0) { rw = rw + rx; rx = 0; }
            if (ry < 0) { rh = rh + ry; ry = 0; }
            if (rx + rw > cw) rw = cw - rx;
            if (ry + rh > ch) rh = ch - ry;
            if (rw > 0 && rh > 0)
                wm_invalidate(a, wm_client_x() + rx, wm_client_y() + ry, rw, rh);
        }
        return copied;
    }

    // (handle) -> 0. Push whatever this window has damaged to the screen.
    // Separate from the blit on purpose: a process that blits three times and
    // presents once pays for one composite, not three.
    if (nr == SYS_WINPRESENT) {
        if (slot < 0 || !win_owned_by(a, g_procs[slot].pid)) return -1;
        wm_present();
        return 0;
    }

    // (handle, ptr to six longs) -> 1 while the window exists, 0 once it does
    // not. The six are the pointer x and y in CLIENT coordinates, the button
    // state, one keystroke (0 if none, and only when this window has focus),
    // and the CLIENT SIZE.
    //
    // The size is in there because a program has no other way to learn it. It
    // asked for a window of a given outside size; how much of that is border
    // and title bar is the window manager's business and can change. A program
    // that hardcodes "minus four and minus eighteen" is a program that draws
    // over its own title bar the day the theme changes.
    //
    // Returning 0 for a window that is gone is how a process finds out that
    // somebody clicked its close box. Without it, a program whose window has
    // been destroyed spins forever blitting into nothing.
    if (nr == SYS_WINPOLL) {
        long *out;
        if (slot < 0) return 0;
        if (!win_owned_by(a, g_procs[slot].pid)) return 0;
        if (b && !user_range_ok(slot, b, 6 * 8)) return 0;
        out = (long *)b;
        if (out) {
            out[0] = g_mouse_x - g_win[a].x - wm_client_x();
            out[1] = g_mouse_y - g_win[a].y - wm_client_y();
            out[2] = g_mouse_btn;

            // One keystroke, and ONLY to the focused window. The slot was
            // here from the start and always returned zero; wiring it to the
            // global ring without the focus test is what would let a
            // background program eat the keys meant for whatever the user is
            // actually looking at. Same reason SYS_READ on fd 0 returns
            // end-of-file rather than the shell's keyboard.
            out[3] = 0;
            if (g_focus == a && kbd_available()) out[3] = kbd_getchar_nb();
            out[4] = wm_client_w(a);
            out[5] = wm_client_h(a);
        }
        return 1;
    }

    if (nr == SYS_WINCLOSE) {
        if (slot < 0 || !win_owned_by(a, g_procs[slot].pid)) return -1;
        g_win_owner[a] = 0;
        wm_destroy(a);
        return 0;
    }
#endif

    return -1;
}

#endif
