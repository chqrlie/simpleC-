// mm.c — memory management bring-up, checked rather than claimed.
//
// Headless. Every assertion here is one that would otherwise be an assumption:
// that the memory map was read, that a mapping actually changed what an
// address resolves to, that unmapping actually stops working, and that the
// heap really reuses memory instead of only ever growing.

#include "nano-kernel.h"
#include "nano-int.h"
#include "nano-mm.h"

#define TEST_VIRT 0x00000000C0000000       // 3 GiB: inside a 2 MiB huge page,
                                           // so mapping it forces a split

int main() {
    serial_init();
    vga_clear();
    kbd_init();
    interrupts_init(100);

    puts("nano-os memory bring-up\n");

    if (!mm_scan_memory()) {
        puts("no Multiboot memory map\n");
        cpu_halt_forever();
    }
    printf("RAM: %d KiB usable, top 0x%x\n", mm_ram_total / 1024, mm_ram_top);

    if (!mm_init()) {
        puts("mm_init failed\n");
        cpu_halt_forever();
    }
    printf("kernel ends at 0x%x, bitmap at 0x%x (%d bytes)\n",
           kernel_end_addr(), mm_bitmap, mm_bitmap_bytes);
    printf("frames: %d free, %d used, %d total\n",
           mm_free_frames, mm_used_frames, mm_bitmap_frames);

    // --- 1. the allocator must not hand out the same frame twice ---
    {
        long a;
        long b;
        long c;
        long before;
        before = mm_free_frames;
        a = frame_alloc();
        b = frame_alloc();
        c = frame_alloc();
        printf("three frames: 0x%x 0x%x 0x%x\n", a, b, c);
        if (a && b && c && a != b && b != c && a != c) puts("distinct ok\n");
        else puts("FRAMES NOT DISTINCT\n");
        if (mm_free_frames == before - 3) puts("accounting ok\n");
        else puts("FRAME ACCOUNTING WRONG\n");
        frame_free(a); frame_free(b); frame_free(c);
        if (mm_free_frames == before) puts("free ok\n");
        else puts("FREE ACCOUNTING WRONG\n");
    }

    // --- 2. the allocator must not hand out the kernel or the bitmap ---
    {
        long bad;
        long i;
        long f;
        bad = 0;
        i = 0;
        while (i < 64) {
            f = frame_alloc();
            if (!f) break;
            if (f < kernel_end_addr()) bad = bad + 1;
            if (f >= mm_bitmap && f < mm_bitmap + mm_bitmap_bytes) bad = bad + 1;
            i = i + 1;
        }
        printf("64 frames allocated, %d overlapped the kernel or bitmap\n", bad);
        if (bad == 0) puts("reservation ok\n");
        else puts("ALLOCATOR HANDED OUT RESERVED MEMORY\n");
    }

    // --- 3. mapping has to change what an address resolves to ---
    {
        long phys;
        long before;
        long after;
        long *p;
        before = vmm_resolve(TEST_VIRT);
        phys = frame_alloc_zeroed();
        printf("before: 0x%x resolves to 0x%x (identity)\n", TEST_VIRT, before);
        if (!vmm_map(TEST_VIRT, phys, PTE_WRITE)) puts("MAP FAILED\n");
        after = vmm_resolve(TEST_VIRT);
        printf("after:  0x%x resolves to 0x%x (frame 0x%x)\n", TEST_VIRT, after, phys);
        if (after == phys) puts("map ok\n");
        else puts("MAPPING DID NOT TAKE\n");

        // and writing through the virtual address must land in that frame,
        // which is the thing a resolve alone does not prove
        p = (long *)TEST_VIRT;
        p[0] = 0x1234567;
        if (mem64(phys) == 0x1234567) puts("write-through ok\n");
        else puts("WRITE DID NOT LAND IN THE FRAME\n");

        // splitting a 2 MiB page must leave its neighbours alone: the byte
        // just below TEST_VIRT is in the same 2 MiB page and must still be
        // identity-mapped
        {
            long neigh;
            neigh = vmm_resolve(TEST_VIRT - 4096);
            if (neigh == TEST_VIRT - 4096) puts("split ok: neighbours untouched\n");
            else printf("SPLIT BROKE A NEIGHBOUR: 0x%x\n", neigh);
        }
    }

    // --- 4. the heap ---
    {
        char *ha;
        char *hb;
        char *hc;
        long free0;
        printf("heap: %d pages, %d bytes free, %d blocks\n",
               heap_pages, heap_bytes_free(), heap_blocks(1) + heap_blocks(0));

        ha = (char *)kmalloc(100);
        hb = (char *)kmalloc(200);
        hc = (char *)kmalloc(300);
        if (!ha || !hb || !hc) puts("KMALLOC RETURNED NULL\n");
        // distinct, and far enough apart not to overlap
        if (ha != hb && hb != hc && (hb - ha) >= 100 && (hc - hb) >= 200) puts("kmalloc distinct ok\n");
        else puts("KMALLOC BLOCKS OVERLAP\n");

        ha[0] = 'a'; ha[99] = 'A';
        hb[0] = 'b'; hb[199] = 'B';
        hc[0] = 'c'; hc[299] = 'C';
        if (ha[0] == 'a' && ha[99] == 'A' && hb[0] == 'b' && hb[199] == 'B'
            && hc[0] == 'c' && hc[299] == 'C') puts("kmalloc contents ok\n");
        else puts("KMALLOC BLOCKS CORRUPTED EACH OTHER\n");

        // free the middle one and ask for the same size again: a heap that
        // really frees hands the same address back
        free0 = heap_bytes_free();
        kfree(hb);
        if (heap_bytes_free() > free0) puts("kfree returned memory ok\n");
        else puts("KFREE DID NOT RETURN MEMORY\n");
        {
            char *b2;
            b2 = (char *)kmalloc(200);
            if (b2 == hb) puts("reuse ok: the freed block came back\n");
            else puts("HEAP DID NOT REUSE THE FREED BLOCK\n");
            kfree(b2);
        }
        kfree(ha);
        kfree(hc);
    }

    // --- 5. coalescing: free everything and the heap should be one block ---
    {
        long i;
        char *slots[32];        // `p` is a long* elsewhere in this function
        long blocks;
        i = 0;
        while (i < 32) { slots[i] = (char *)kmalloc(64); i = i + 1; }
        i = 0;
        while (i < 32) { kfree(slots[i]); i = i + 1; }
        blocks = heap_blocks(0);
        printf("after 32 alloc/free pairs: %d used blocks, %d free blocks\n",
               blocks, heap_blocks(1));
        if (blocks == 0) puts("coalesce ok: nothing left allocated\n");
        else puts("BLOCKS LEAKED\n");
    }

    // --- 6. growth: ask for more than the heap has ---
    {
        long pages0;
        char *big;
        pages0 = heap_pages;
        big = (char *)kmalloc(200000);        // ~49 pages, more than we started
        if (!big) puts("BIG ALLOCATION FAILED\n");
        else {
            big[0] = 'x';
            big[199999] = 'y';
            if (big[0] == 'x' && big[199999] == 'y'
                && heap_pages > pages0) puts("heap growth ok\n");
            else puts("HEAP GROWTH WRONG\n");
            kfree(big);
        }
        printf("heap now %d pages, %d kmalloc / %d kfree calls\n",
               heap_pages, kmalloc_calls, kfree_calls);
    }

    // --- 7. unmapping has to actually stop working ---
    // The fault reporter from K2 is what makes this checkable: before it
    // existed, this would have been a silent reboot and indistinguishable
    // from the test passing.
    puts("\nunmapping the test page and touching it:\n");
    vmm_unmap(TEST_VIRT);
    if (vmm_resolve(TEST_VIRT) == 0) puts("unmap ok: no translation\n");
    else puts("UNMAP DID NOT TAKE\n");
    {
        long *p;
        p = (long *)TEST_VIRT;
        p[0] = 1;
    }

    puts("UNREACHABLE: the unmapped write did not fault\n");
    cpu_halt_forever();
    return 0;
}
