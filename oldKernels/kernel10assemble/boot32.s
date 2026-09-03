/* boot32.s — 32-bit Multiboot entry.  QEMU's -kernel loader enters here in
 * 32-bit protected mode.  We build identity-mapped page tables (first 1 GiB,
 * 2 MiB pages), enable PAE + long mode + paging, load a 64-bit GDT, and
 * far-jump into the 64-bit kernel at 0x101000 (long_mode_start).
 *
 * Page tables live at fixed free low RAM (0x1000/0x2000/0x3000); the stack at
 * 0x90000.  Assembled with `as --32`.
 */
.set MB_MAGIC,  0x1BADB002
.set MB_FLAGS,  0x00000003
.set MB_CHECK,  -(MB_MAGIC + MB_FLAGS)

.set PML4, 0x1000
.set PDPT, 0x2000
.set PD,   0x3000          /* four consecutive PDs: 0x3000..0x6fff */
.set MBINFO, 0x7000        /* where we park the Multiboot info pointer */
.set KERNEL64, 0x101000

.section .multiboot, "a"
.align 4
    .long MB_MAGIC
    .long MB_FLAGS
    .long MB_CHECK

.section .text
.code32
.globl _start
_start:
    cli
    mov $0x90000, %esp

    /* The Multiboot loader leaves the address of its information structure in
       EBX. That structure carries the memory map, which is the only way to
       know which physical RAM actually exists -- everything else here would be
       guessing. EBX is a scratch register in the page-table loops below, so
       save it before anything can touch it. */
    movl %ebx, MBINFO

    /* zero the PML4 and PDPT pages (only the low entries are used; the rest
       must be zero or the CPU will follow garbage) */
    xor %eax, %eax
    mov $PML4, %edi
    mov $1024, %ecx         /* 2 pages * 512 dwords */
    rep stosl

    /* PML4[0] = PDPT | present|write */
    movl $(PDPT | 0x3), PML4

    /* PDPT[0..3] -> four page directories, i.e. identity-map the first FOUR
       GiB rather than one. The PCI framebuffer aperture on QEMU's stdvga sits
       around 0xFD000000, so a 1 GiB map cannot reach it at all: writing a
       pixel would page-fault, and with no IDT that is a triple fault and a
       silent reboot loop rather than an error. Everything below 4 GiB is
       MMIO-or-RAM on this machine, so mapping all of it is the simple
       answer. */
    mov $PDPT, %edi
    mov $PD, %eax
    mov $0, %ecx
.fill_pdpt:
    mov %eax, %ebx
    or  $0x3, %ebx
    mov %ebx, (%edi)
    movl $0, 4(%edi)
    add $0x1000, %eax       /* next page directory */
    add $8, %edi
    inc %ecx
    cmp $4, %ecx
    jne .fill_pdpt

    /* PD[i] = (i*2MiB) | present|write|PS  for i = 0..2047 (4 GiB of 2 MiB
       pages, laid out consecutively across the four directories) */
    mov $0, %ecx
    mov $PD, %edi
.fill_pd:
    mov %ecx, %eax
    shl $21, %eax           /* low 32 bits of i * 2 MiB */
    or  $0x83, %eax
    mov %eax, (%edi)
    mov %ecx, %eax
    shr $11, %eax           /* high 32 bits: i * 2 MiB >> 32 */
    mov %eax, 4(%edi)
    add $8, %edi
    inc %ecx
    cmp $2048, %ecx
    jne .fill_pd

    /* CR3 = PML4 */
    mov $PML4, %eax
    mov %eax, %cr3

    /* CR4.PAE = 1 */
    mov %cr4, %eax
    or  $(1 << 5), %eax
    mov %eax, %cr4

    /* EFER.LME = 1 */
    mov $0xC0000080, %ecx
    rdmsr
    or  $(1 << 8), %eax
    wrmsr

    /* CR0.PG = 1 (also keep PE) */
    mov %cr0, %eax
    or  $(1 << 31), %eax
    mov %eax, %cr0

    lgdt gdt64_pointer
    ljmp $0x08, $KERNEL64

.section .rodata
.align 8
gdt64:
    .quad 0
    .quad 0x00AF9A000000FFFF    /* 0x08: 64-bit code, L=1 */
    .quad 0x00AF92000000FFFF    /* 0x10: data */
gdt64_pointer:
    .word gdt64_pointer - gdt64 - 1
    .long gdt64
