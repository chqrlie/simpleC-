// nano-mm.h — physical frames, 4 KiB paging, and a kernel heap.
//
// Compiled by nano_cc with --kernel. Include AFTER nano-kernel.h.
//
// Three layers, each built on the one below:
//
//   1. Which physical RAM exists, from the Multiboot memory map. Not a guess:
//      the loader is the only thing that knows, and assuming "128 MiB, probably"
//      is how a kernel comes to write into a memory hole.
//   2. A frame allocator: one bit per 4 KiB page of physical memory.
//   3. Paging at 4 KiB granularity, which means splitting the 2 MiB pages
//      boot32.s set up, and then a heap on top.

#ifndef NANO_MM_H
#define NANO_MM_H

// Preemption can arrive between any two instructions here, and every one of
// these structures has a window where it is half-updated: the frame bitmap
// between testing a bit and setting it, the page tables between allocating a
// table and installing it, the heap's free list between unlinking a block and
// relinking it. Masking interrupts around each is what makes them atomic --
// see the note in nano-thread.h about why that is the right lock on one core
// and what has to change on more.
extern long irq_save();
extern void irq_restore(long were_enabled);

extern long kernel_end_addr();
extern long multiboot_info_addr();
extern void tlb_invlpg(long virt);
extern void tlb_flush();
extern long read_cr3_();

#define PAGE_SIZE   4096
#define PAGE_SHIFT  12

// Page-table entry bits.
#define PTE_PRESENT 1
#define PTE_WRITE   2
#define PTE_USER    4
#define PTE_PWT     8
#define PTE_PCD     16
#define PTE_HUGE    128        // in a PD entry: this maps 2 MiB directly
#define PTE_ADDR    0x000FFFFFFFFFF000

// Bit 63: no execute. It is a RESERVED bit until EFER.NXE is set, and a
// reserved bit that is set faults on every access to the page -- so this is
// only ever ORed in through nx_bit(), which returns zero when the CPU or the
// kernel has not turned NXE on. A hardening flag that bricks the mapping when
// it is unavailable is worse than not having it.
#define PTE_NX      0x8000000000000000

// ---------- 1. the physical memory map ----------
long mm_ram_top;               // one past the highest usable physical byte
long mm_ram_total;             // usable bytes, which is not top minus zero

// A Multiboot 1 information structure:
//   +0  flags        bit 6 set means the memory map below is valid
//   +44 mmap_length
//   +48 mmap_addr
// and each map entry is
//   +0  size (NOT counting itself, so the next entry is at +size+4)
//   +4  base_addr (8)
//   +12 length (8)
//   +20 type      1 = available RAM
#define MB_FLAG_MMAP 64

long mm_scan_memory() {
    long mbi;
    long flags;
    long len;
    long addr;
    long p;
    long end;

    mm_ram_top = 0;
    mm_ram_total = 0;

    mbi = multiboot_info_addr();
    if (!mbi) return 0;
    flags = mem32(mbi + 0);
    if (!(flags & MB_FLAG_MMAP)) return 0;

    len  = mem32(mbi + 44);
    addr = mem32(mbi + 48);
    p = addr;
    end = addr + len;
    while (p + 24 <= end) {
        long esz;
        long base;
        long elen;
        long type;
        esz  = mem32(p);
        base = mem64(p + 4);
        elen = mem64(p + 12);
        type = mem32(p + 20);
        if (esz < 20) return 0;                 // malformed: stop, do not spin
        if (type == 1) {
            mm_ram_total = mm_ram_total + elen;
            if (base + elen > mm_ram_top) mm_ram_top = base + elen;
        }
        p = p + esz + 4;
    }
    // Only the first 4 GiB is identity-mapped, and the frame allocator has to
    // be able to touch a frame to hand it out, so cap there.
    if (mm_ram_top > 0x100000000) mm_ram_top = 0x100000000;
    return mm_ram_top > 0;
}

// ---------- 2. the frame allocator ----------
// A bitmap: one bit per 4 KiB frame, 1 meaning used. It is placed immediately
// after the kernel image, and it marks itself used -- an allocator that can
// hand out the memory it is stored in is a very short-lived allocator.
long mm_bitmap;                // physical address of the bitmap
long mm_bitmap_frames;         // how many frames it covers
long mm_bitmap_bytes;
long mm_free_frames;
long mm_used_frames;
long mm_next_hint;             // where the last search stopped

void frame_mark_used(long frame) {
    char *b;
    if (frame < 0 || frame >= mm_bitmap_frames) return;
    b = (char *)(mm_bitmap + (frame >> 3));
    if (!(b[0] & (1 << (frame & 7)))) {
        b[0] = b[0] | (1 << (frame & 7));
        mm_free_frames = mm_free_frames - 1;
        mm_used_frames = mm_used_frames + 1;
    }
}

void frame_mark_free(long frame) {
    char *b;
    if (frame < 0 || frame >= mm_bitmap_frames) return;
    b = (char *)(mm_bitmap + (frame >> 3));
    if (b[0] & (1 << (frame & 7))) {
        b[0] = b[0] & ~(1 << (frame & 7));
        mm_free_frames = mm_free_frames + 1;
        mm_used_frames = mm_used_frames - 1;
    }
}

long frame_is_used(long frame) {
    char *b;
    if (frame < 0 || frame >= mm_bitmap_frames) return 1;
    b = (char *)(mm_bitmap + (frame >> 3));
    return (b[0] >> (frame & 7)) & 1;
}

void frame_mark_range(long base, long len, long used) {
    long f;
    long last;
    f = base >> PAGE_SHIFT;
    last = (base + len + PAGE_SIZE - 1) >> PAGE_SHIFT;
    while (f < last) {
        if (used) frame_mark_used(f); else frame_mark_free(f);
        f = f + 1;
    }
}

// Returns a physical address, or 0. Zero is never a valid frame here because
// the first megabyte is reserved, which is convenient: 0 can mean failure
// without a separate flag.
long frame_alloc() {
    long i;
    long tries;
    long f;
    long got;
    f = irq_save();               // test-then-set is not atomic on its own
    i = mm_next_hint;
    tries = 0;
    got = 0;
    while (tries < mm_bitmap_frames) {
        if (i >= mm_bitmap_frames) i = 0;
        if (!frame_is_used(i)) {
            frame_mark_used(i);
            mm_next_hint = i + 1;
            got = i << PAGE_SHIFT;
            tries = mm_bitmap_frames;
        } else {
            i = i + 1;
            tries = tries + 1;
        }
    }
    irq_restore(f);
    return got;
}

void frame_free(long phys) {
    long f;
    f = irq_save();
    frame_mark_free(phys >> PAGE_SHIFT);
    irq_restore(f);
}

// Writing to a physical address directly is only legal because the first
// 4 GiB is identity-mapped; on a kernel with a higher-half map this would need
// a temporary mapping first.
void frame_zero(long phys) {
    long *p;
    long i;
    p = (long *)phys;
    i = 0;
    while (i < PAGE_SIZE / 8) { p[i] = 0; i = i + 1; }
}

long frame_alloc_zeroed() {
    long f;
    f = frame_alloc();
    if (f) frame_zero(f);
    return f;
}

long mm_init_frames() {
    long kend;
    long bmneed;

    if (!mm_scan_memory()) return 0;

    mm_bitmap_frames = mm_ram_top >> PAGE_SHIFT;
    mm_bitmap_bytes = (mm_bitmap_frames + 7) / 8;

    kend = kernel_end_addr();
    mm_bitmap = (kend + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    // Everything starts USED and is then freed from the map. The other way
    // round -- start free, mark the holes -- means anything the map does not
    // mention is handed out, and firmware tables live in exactly those gaps.
    {
        char *b;
        long i;
        b = (char *)mm_bitmap;
        i = 0;
        while (i < mm_bitmap_bytes) { b[i] = 0xFF; i = i + 1; }
    }
    mm_free_frames = 0;
    mm_used_frames = mm_bitmap_frames;

    // Free what the loader called available.
    {
        long mbi;
        long len;
        long addr;
        long p;
        long end;
        mbi = multiboot_info_addr();
        len  = mem32(mbi + 44);
        addr = mem32(mbi + 48);
        p = addr;
        end = addr + len;
        while (p + 24 <= end) {
            long esz;
            esz = mem32(p);
            if (esz < 20) break;
            if (mem32(p + 20) == 1) {
                long base;
                long elen;
                base = mem64(p + 4);
                elen = mem64(p + 12);
                if (base < mm_ram_top) {
                    if (base + elen > mm_ram_top) elen = mm_ram_top - base;
                    frame_mark_range(base, elen, 0);
                }
            }
            p = p + esz + 4;
        }
    }

    // Then take back everything that is already in use, whatever the map said.
    frame_mark_range(0, 0x100000, 1);                  // the first MiB: BIOS,
                                                       // page tables, stack,
                                                       // Multiboot info
    frame_mark_range(0x100000, kend - 0x100000, 1);    // the kernel image
    bmneed = (mm_bitmap_bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    frame_mark_range(mm_bitmap, bmneed, 1);            // the bitmap itself

    mm_next_hint = 0;
    return 1;
}

// ---------- 3. paging at 4 KiB ----------
// boot32.s built a 4 GiB identity map out of 2 MiB pages, which cannot express
// a single 4 KiB mapping. Mapping one means finding the 2 MiB page that covers
// it and SPLITTING it into a page table of 512 entries that say the same thing
// -- otherwise the split would silently unmap everything else in that 2 MiB.

long pt_index(long virt, long level) {
    // level 4 = PML4, 3 = PDPT, 2 = PD, 1 = PT
    return (virt >> (12 + 9 * (level - 1))) & 511;
}

// Read the table one level down, allocating it if absent.
long pt_next(long table, long index, long create) {
    long *e;
    long v;
    e = (long *)(table + index * 8);
    v = e[0];
    if (v & PTE_PRESENT) {
        if (v & PTE_HUGE) return 0;            // caller must split first
        return v & PTE_ADDR;
    }
    if (!create) return 0;
    {
        long f;
        f = frame_alloc_zeroed();
        if (!f) return 0;
        e[0] = f | PTE_PRESENT | PTE_WRITE;
        return f;
    }
}

// Turn a 2 MiB page directory entry into a page table describing the same
// 2 MiB, so that one 4 KiB entry inside it can then be changed.
long pd_split(long pd, long index) {
    long *e;
    long v;
    long base;
    long pt;
    long *t;
    long i;

    e = (long *)(pd + index * 8);
    v = e[0];
    if (!(v & PTE_PRESENT)) return 0;
    if (!(v & PTE_HUGE)) return v & PTE_ADDR;   // already a table

    base = v & 0x000FFFFFFFE00000;              // the 2 MiB frame it mapped
    pt = frame_alloc_zeroed();
    if (!pt) return 0;

    t = (long *)pt;
    i = 0;
    while (i < 512) {
        t[i] = (base + i * PAGE_SIZE) | PTE_PRESENT | PTE_WRITE;
        i = i + 1;
    }
    e[0] = pt | PTE_PRESENT | PTE_WRITE;
    tlb_flush();
    return pt;
}

// The three vmm_* calls below each take the page-table ROOT to work in, so a
// caller can build a mapping in an address space that is not the running one --
// which is exactly what loading a program into a fresh address space needs.
// The unsuffixed versions operate on the current CR3 and are what the rest of
// the kernel uses.
//
// The root is a physical address, and every level below it is reached by
// treating that physical address as a pointer. That only works because the
// first 4 GiB is identity-mapped, so physical and virtual agree there and all
// page tables are allocated from it. On a kernel with a higher-half map this
// would need a temporary window; here the identity map IS the window.
long vmm_map_in(long root, long virt, long phys, long flags) {
    long cr3;
    long pdpt;
    long pd;
    long pt;
    long *e;
    long irqf;

    // A table allocated but not yet installed is invisible to anyone else; a
    // 2 MiB page half-way through being split is worse than either state.
    irqf = irq_save();
    cr3 = root & PTE_ADDR;
    pdpt = pt_next(cr3, pt_index(virt, 4), 1);
    if (!pdpt) { irq_restore(irqf); return 0; }
    pd = pt_next(pdpt, pt_index(virt, 3), 1);
    if (!pd) { irq_restore(irqf); return 0; }

    // The PD entry may be a 2 MiB page; split it before touching one 4 KiB
    // slot inside it.
    {
        long *pde;
        pde = (long *)(pd + pt_index(virt, 2) * 8);
        if ((pde[0] & PTE_PRESENT) && (pde[0] & PTE_HUGE)) {
            pt = pd_split(pd, pt_index(virt, 2));
            if (!pt) { irq_restore(irqf); return 0; }
        } else {
            pt = pt_next(pd, pt_index(virt, 2), 1);
            if (!pt) { irq_restore(irqf); return 0; }
        }
    }

    e = (long *)(pt + pt_index(virt, 1) * 8);
    e[0] = (phys & PTE_ADDR) | flags | PTE_PRESENT;
    tlb_invlpg(virt);
    irq_restore(irqf);
    return 1;
}

long vmm_map(long virt, long phys, long flags) {
    return vmm_map_in(read_cr3_(), virt, phys, flags);
}

long vmm_unmap_in(long root, long virt) {
    long cr3;
    long pdpt;
    long pd;
    long pt;
    long *e;

    cr3 = root & PTE_ADDR;
    pdpt = pt_next(cr3, pt_index(virt, 4), 0);
    if (!pdpt) return 0;
    pd = pt_next(pdpt, pt_index(virt, 3), 0);
    if (!pd) return 0;
    {
        long *pde;
        pde = (long *)(pd + pt_index(virt, 2) * 8);
        if ((pde[0] & PTE_PRESENT) && (pde[0] & PTE_HUGE)) {
            pt = pd_split(pd, pt_index(virt, 2));
            if (!pt) return 0;
        } else {
            pt = pt_next(pd, pt_index(virt, 2), 0);
            if (!pt) return 0;
        }
    }
    e = (long *)(pt + pt_index(virt, 1) * 8);
    e[0] = 0;
    tlb_invlpg(virt);
    return 1;
}

long vmm_unmap(long virt) { return vmm_unmap_in(read_cr3_(), virt); }

// What a virtual address currently resolves to, or 0. Used by the tests, and
// the only honest way to check that a mapping did what it said.
long vmm_resolve_in(long root, long virt) {
    long cr3;
    long pdpt;
    long pd;
    long *pde;
    long pt;
    long *e;

    cr3 = root & PTE_ADDR;
    pdpt = pt_next(cr3, pt_index(virt, 4), 0);
    if (!pdpt) return 0;
    pd = pt_next(pdpt, pt_index(virt, 3), 0);
    if (!pd) return 0;
    pde = (long *)(pd + pt_index(virt, 2) * 8);
    if (!(pde[0] & PTE_PRESENT)) return 0;
    if (pde[0] & PTE_HUGE) return (pde[0] & 0x000FFFFFFFE00000) + (virt & 0x1FFFFF);
    pt = pde[0] & PTE_ADDR;
    e = (long *)(pt + pt_index(virt, 1) * 8);
    if (!(e[0] & PTE_PRESENT)) return 0;
    return (e[0] & PTE_ADDR) + (virt & 0xFFF);
}

long vmm_resolve(long virt) { return vmm_resolve_in(read_cr3_(), virt); }

// The page-table flags on a mapping, or 0 if it is not mapped. resolve() alone
// cannot tell "mapped read-only" from "mapped writable", so a test that only
// checks the address would pass on a loader that ignored segment permissions
// entirely.
long vmm_flags_in(long root, long virt) {
    long cr3;
    long pdpt;
    long pd;
    long *pde;
    long pt;
    long *e;

    cr3 = root & PTE_ADDR;
    pdpt = pt_next(cr3, pt_index(virt, 4), 0);
    if (!pdpt) return 0;
    pd = pt_next(pdpt, pt_index(virt, 3), 0);
    if (!pd) return 0;
    pde = (long *)(pd + pt_index(virt, 2) * 8);
    if (!(pde[0] & PTE_PRESENT)) return 0;
    if (pde[0] & PTE_HUGE) return pde[0] & 0xFFF;
    pt = pde[0] & PTE_ADDR;
    e = (long *)(pt + pt_index(virt, 1) * 8);
    if (!(e[0] & PTE_PRESENT)) return 0;
    return e[0] & 0xFFF;
}

// ---------- 4. the kernel heap ----------
// A first-fit free list with coalescing, on pages mapped just above the
// identity map so that using it exercises the mapping code rather than
// quietly living in memory that was already there.
//
// Unlike the compiler's allocator, this one really frees: a kernel runs
// forever, and an allocator that only moves forward runs out.
#define HEAP_BASE  0x100000000        // 4 GiB, one byte past the identity map
#define HEAP_MAGIC 0x4B4D414C4C4F4331 // "KMALLOC1"

struct Block {
    long magic;
    long size;                 // usable bytes, not counting this header
    long free;
    struct Block *next;
    struct Block *prev;
};

struct Block *heap_head;
long heap_end;                 // one past the last mapped heap byte
long heap_pages;
long kmalloc_calls;
long kfree_calls;

long heap_grow(long bytes) {
    long need;
    long got;
    need = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    got = 0;
    while (got < need) {
        long f;
        f = frame_alloc();
        if (!f) return 0;
        if (!vmm_map(heap_end + got, f, PTE_WRITE)) { frame_free(f); return 0; }
        heap_pages = heap_pages + 1;
        got = got + PAGE_SIZE;
    }
    heap_end = heap_end + got;
    return got;
}

long heap_init(long initial_pages) {
    struct Block *b;
    heap_end = HEAP_BASE;
    heap_pages = 0;
    heap_head = 0;
    kmalloc_calls = 0;
    kfree_calls = 0;
    if (!heap_grow(initial_pages * PAGE_SIZE)) return 0;
    b = (struct Block *)HEAP_BASE;
    b->magic = HEAP_MAGIC;
    b->size = (heap_end - HEAP_BASE) - sizeof(struct Block);
    b->free = 1;
    b->next = 0;
    b->prev = 0;
    heap_head = b;
    return 1;
}

// Split a block if the remainder is worth having. A split that leaves less
// than a header behind produces a block that can never be used and can never
// be merged, which is how a heap fragments itself to death.
void block_split(struct Block *b, long want) {
    struct Block *n;
    if (b->size < want + (long)sizeof(struct Block) + 32) return;
    n = (struct Block *)((char *)b + sizeof(struct Block) + want);
    n->magic = HEAP_MAGIC;
    n->size = b->size - want - sizeof(struct Block);
    n->free = 1;
    n->next = b->next;
    n->prev = b;
    if (b->next) b->next->prev = n;
    b->next = n;
    b->size = want;
}

void *kmalloc(long n) {
    struct Block *b;
    long f;
    if (!heap_head) return 0;                  // heap_init has not run
    // The whole search-split-mark sequence has to be one step. Preempted
    // between finding a free block and marking it used, two threads walk away
    // with the same block and the second one's writes land in the first one's
    // memory.
    f = irq_save();
    kmalloc_calls = kmalloc_calls + 1;
    if (n < 1) n = 1;
    n = (n + 15) & ~15;                        // keep every payload 16-aligned

    b = heap_head;
    while (b) {
        if (b->free && b->size >= n) {
            block_split(b, n);
            b->free = 0;
            irq_restore(f);
            return (void *)((char *)b + sizeof(struct Block));
        }
        if (!b->next) break;
        b = b->next;
    }

    // Nothing fitted: extend the heap and put the new pages in a block at the
    // end, merging with the last one if it happens to be free.
    {
        long added;
        struct Block *tail;
        struct Block *nb;
        tail = b;
        added = heap_grow(n + sizeof(struct Block));
        if (!added) { irq_restore(f); return 0; }
        nb = (struct Block *)((char *)tail + sizeof(struct Block) + tail->size);
        nb->magic = HEAP_MAGIC;
        nb->size = added - sizeof(struct Block);
        nb->free = 1;
        nb->next = 0;
        nb->prev = tail;
        tail->next = nb;
        if (tail->free) {
            // merge, so a run of grows does not leave a chain of small blocks
            tail->size = tail->size + sizeof(struct Block) + nb->size;
            tail->next = 0;
            nb = tail;
        }
        block_split(nb, n);
        nb->free = 0;
        irq_restore(f);
        return (void *)((char *)nb + sizeof(struct Block));
    }
}

void *kcalloc(long count, long size) {
    long n;
    char *p;
    long i;
    n = count * size;
    p = (char *)kmalloc(n);
    if (!p) return 0;
    i = 0;
    while (i < n) { p[i] = 0; i = i + 1; }
    return (void *)p;
}

void kfree(void *ptr) {
    struct Block *b;
    long f;
    if (!ptr) return;
    f = irq_save();                            // coalescing relinks neighbours
    kfree_calls = kfree_calls + 1;
    b = (struct Block *)((char *)ptr - sizeof(struct Block));
    // A wrong pointer here corrupts the whole heap silently. The magic turns
    // that into a message.
    if (b->magic != HEAP_MAGIC) {
        irq_restore(f);
        puts("kfree: not a heap block (bad magic)\n");
        return;
    }
    if (b->free) {
        irq_restore(f);
        puts("kfree: double free\n");
        return;
    }
    b->free = 1;
    // Coalesce forwards then backwards, or the heap ends up as a long list of
    // adjacent free blocks that no large request can ever use.
    if (b->next && b->next->free) {
        b->size = b->size + sizeof(struct Block) + b->next->size;
        b->next = b->next->next;
        if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->free) {
        b->prev->size = b->prev->size + sizeof(struct Block) + b->size;
        b->prev->next = b->next;
        if (b->next) b->next->prev = b->prev;
    }
    irq_restore(f);
}

long heap_blocks(long want_free) {
    struct Block *b;
    long n;
    n = 0;
    b = heap_head;
    while (b) {
        if (b->free == want_free) n = n + 1;
        b = b->next;
    }
    return n;
}

long heap_bytes_free() {
    struct Block *b;
    long n;
    n = 0;
    b = heap_head;
    while (b) {
        if (b->free) n = n + b->size;
        b = b->next;
    }
    return n;
}

// Unmap the first page, so that dereferencing a null pointer faults.
//
// It does not, otherwise: the identity map covers the whole first 4 GiB, so a
// write to address 0 lands in the interrupt vector table and SUCCEEDS. The
// most common bug in C -- following a null pointer -- silently corrupts low
// memory and shows up later as something unrelated.
//
// Call this AFTER anything that reads the BIOS data area, because the ACPI
// RSDP search reads the EBDA segment from 0x40E, which is in this page.
long mm_protect_null() {
    return vmm_unmap(0);
}

long mm_init() {
    if (!mm_init_frames()) return 0;
    if (!heap_init(16)) return 0;              // 64 KiB to start
    return 1;
}

#endif
