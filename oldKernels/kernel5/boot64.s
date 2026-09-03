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
