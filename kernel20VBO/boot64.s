/* boot64.s — the 64-bit half of the boot path.
 *
 * The 32-bit stub (boot32.s) enables long mode and far-jumps to
 * long_mode_start, which lives at the very start of the 64-bit kernel image
 * (linked at 0x101000).  We set the data segments and stack, then call main()
 * in the nano_cc-compiled kernel.  Also provides inb/outb (the port-I/O
 * instructions the freestanding runtime needs).
 *
 * Assembled with `as --64`; linked FIRST in kernel64.elf so long_mode_start
 * is at 0x101000.
 */
.code64

/* placed first by the linker script so its address is exactly 0x101000 */
.section .text.entry, "ax"
.globl long_mode_start
long_mode_start:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    mov %ax, %fs
    mov %ax, %gs
    mov $0x90000, %rsp          /* stack in free low RAM */

    call main
.halt:
    hlt
    jmp .halt

.section .text
/* int  inb(int port)          SysV: port in edi, return in eax */
.globl inb
inb:
    mov %edi, %edx
    xor %eax, %eax
    in  %dx, %al
    ret

/* void outb(int port, int val) SysV: port in edi, value in esi */
.globl outb
outb:
    mov %edi, %edx
    mov %esi, %eax
    out %al, %dx
    ret

/* 16- and 32-bit port I/O. The Bochs VBE index/data registers at 0x1CE/0x1CF
 * are WORD ports and the PCI configuration ports at 0xCF8/0xCFC are DWORD
 * ports; doing either a byte at a time does not work, because the device sees
 * one access, not two halves of one. */

/* int inw(int port) */
.globl inw
inw:
    mov %edi, %edx
    xor %eax, %eax
    in  %dx, %ax
    ret

/* void outw(int port, int val) */
.globl outw
outw:
    mov %edi, %edx
    mov %esi, %eax
    out %ax, %dx
    ret

/* long inl(int port) — returned zero-extended, so a BAR with bit 31 set does
 * not come back as a negative long in a compiler with no unsigned type. */
.globl inl
inl:
    mov %edi, %edx
    xor %eax, %eax
    in  %dx, %eax
    ret

/* void outl(int port, long val) */
.globl outl
outl:
    mov %edi, %edx
    mov %esi, %eax
    out %eax, %dx
    ret

/* void mmio_write32(long addr, long val) — a 32-bit store to a physical
 * address. nano_cc has no 32-bit integer type, so it cannot express one:
 * an 8-byte store would write the neighbouring pixel too, and a 1-byte store
 * would need four of them. */
.globl mmio_write32
mmio_write32:
    mov %esi, %eax
    mov %eax, (%rdi)
    ret

/* long mmio_read32(long addr) */
.globl mmio_read32
mmio_read32:
    xor %eax, %eax
    mov (%rdi), %eax
    ret

/* ---- row primitives ------------------------------------------------------
 *
 * mmio_write32 above is correct and it is what the compositor used for every
 * pixel it ever wrote.  Measured, that came to about fifty-seven instructions
 * per pixel: nano_cc recomputes `fb_base + fy*fb_pitch + fx*4` and `src[i]`
 * from scratch on every iteration, two imuls and a dozen stack moves each, and
 * then makes a call for the store itself.  A window drag dirties one window's
 * area per mouse report, a hundred reports a second, and under an emulator
 * that is the whole machine.
 *
 * These three do a ROW at a time.  The address arithmetic happens once, in the
 * caller, and the inner loop is five instructions or a rep-string.  Nothing
 * about the pixel format changes, so every existing checksum still matches.
 *
 * void fb_blit32(long dst, long *src, long n)
 *   n pixels from a long[] (one pixel per long, low 32 bits used) to n
 *   consecutive 32-bit slots at dst.  The stride mismatch — 8 in, 4 out — is
 *   why this cannot be a rep movsd. */
.globl fb_blit32
fb_blit32:
    test %rdx, %rdx
    jle .Lblit32_out
.Lblit32_loop:
    mov (%rsi), %eax
    mov %eax, (%rdi)
    add $8, %rsi
    add $4, %rdi
    dec %rdx
    jnz .Lblit32_loop
.Lblit32_out:
    ret

/* void fb_fill32(long dst, long val, long n) — n copies of the low 32 bits of
 * val, written consecutively at dst. */
.globl fb_fill32
fb_fill32:
    test %rdx, %rdx
    jle .Lfill32_out
    mov %rdx, %rcx
    mov %esi, %eax
    cld
    rep stosl
.Lfill32_out:
    ret

/* void fb_move32(long dst, long src, long n) — n 32-bit words from one
 * framebuffer address to another.  Used by the console scroll, where source
 * and destination are both video memory and never overlap within a row. */
.globl fb_move32
fb_move32:
    test %rdx, %rdx
    jle .Lmove32_out
    mov %rdx, %rcx
    cld
    rep movsl
.Lmove32_out:
    ret

/* void fb_copy64(long *dst, long *src, long n) — n longs, one pixel each, from
 * one backing buffer to another. This is the blit a process asks for with
 * SYS_WINBLIT: its own pixels into its window. Both sides are ordinary memory,
 * both are long[], so a whole row is one rep movsq. */
.globl fb_copy64
fb_copy64:
    test %rdx, %rdx
    jle .Lcopy64_out
    mov %rdx, %rcx
    cld
    rep movsq
.Lcopy64_out:
    ret

/* long kernel_end_addr(void) — the end of the loaded image, from the linker
 * script. Physical memory below this is the kernel itself and must never be
 * handed out as a free frame. */
.globl kernel_end_addr
kernel_end_addr:
    lea _kernel_end(%rip), %rax
    ret

/* long multiboot_info_addr(void) — the pointer boot32.s parked at 0x7000
 * before its own page-table setup could clobber EBX. */
.globl multiboot_info_addr
multiboot_info_addr:
    mov $0x7000, %rax
    mov (%rax), %eax            /* zero-extends: it is a 32-bit address */
    ret

/* void tlb_invlpg(long virt) — drop one page's cached translation.
 *
 * Named tlb_invlpg rather than invlpg: the instruction owns that word, and a
 * global symbol with the same spelling is asking for trouble. The call itself
 * matters more than it looks -- the CPU caches page-table walks, so a table
 * edited without this keeps using the OLD mapping until something happens to
 * evict it, which makes the change appear to work intermittently. */
.globl tlb_invlpg
tlb_invlpg:
    invlpg (%rdi)
    ret

/* void tlb_flush(void) — reload CR3, dropping every non-global entry. */
.globl tlb_flush
tlb_flush:
    mov %cr3, %rax
    mov %rax, %cr3
    ret

/* long read_cr3_(void) — boot64 needs its own, since isr.s is not linked into
 * every image here. */
.globl read_cr3_
read_cr3_:
    mov %cr3, %rax
    ret

/* void write_cr3_(long root) — install a page-table root.
 *
 * This is the entire address-space switch. Writing CR3 also flushes every
 * non-global TLB entry, which is why no invlpg is needed after it; and why
 * doing it on every context switch, rather than only when the address space
 * actually changes, would be a real cost rather than a tidy simplification.
 *
 * Everything the kernel touches -- its own code, the stacks, the heap -- lives
 * in the bottom 512 GiB, and every address space shares that one PML4 entry.
 * If that were not true, this instruction would unmap the stack it is running
 * on halfway through returning. */
.globl write_cr3_
write_cr3_:
    mov %rdi, %cr3
    ret

/* void enable_write_protect(void) — set CR0.WP (bit 16).
 *
 * Without this bit, ring 0 may write to a page whose entry says read-only. The
 * CPU only enforces the write permission against ring 3. So a kernel that maps
 * a program's .text without PTE_WRITE and never sets WP has not protected
 * anything -- the store lands, and a test asserting "the text segment is
 * read-only" passes for the wrong reason, or rather fails to fail.
 *
 * Nothing in this kernel writes through a read-only mapping: the identity map
 * is writable throughout, and the heap is mapped writable. */
.globl enable_write_protect
enable_write_protect:
    mov %cr0, %rax
    or $0x10000, %rax
    mov %rax, %cr0
    ret

/* long enable_nx(void) — turn on EFER.NXE, so bit 63 of a page-table entry
 * means "no execute" instead of "reserved, fault the moment you use it".
 * Returns 1 if the CPU has it, 0 if not.
 *
 * Until this runs, setting that bit does not harden anything -- it makes every
 * access to the page a reserved-bit page fault. So nothing may set it before
 * the return value has been checked.
 *
 * The CPUID check is not ceremony. Writing a reserved EFER bit on a CPU
 * without NX is a #GP, and a triple fault this early looks like a bad page
 * table rather than a bad MSR write, which is a long way to go for a wrong
 * answer. The extended leaf has to be checked for existence first, for the
 * same reason: CPUID with an unsupported leaf returns the highest one instead
 * of failing, and its EDX would be read as if it meant something. */
.globl enable_nx
enable_nx:
    push %rbx                   /* cpuid clobbers it, and it is callee-saved */
    mov $0x80000000, %eax
    cpuid
    cmp $0x80000001, %eax
    jb .Lno_nx                  /* the leaf that reports NX does not exist */
    mov $0x80000001, %eax
    cpuid
    test $(1 << 20), %edx       /* EDX.NX */
    jz .Lno_nx
    mov $0xC0000080, %ecx       /* IA32_EFER */
    rdmsr
    or $(1 << 11), %eax         /* NXE */
    wrmsr
    mov $1, %rax
    pop %rbx
    ret
.Lno_nx:
    xor %eax, %eax
    pop %rbx
    ret
