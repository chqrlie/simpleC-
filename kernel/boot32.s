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
.set PD,   0x3000
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

    /* zero the PML4 and PDPT pages (only entry 0 is used; rest must be 0) */
    xor %eax, %eax
    mov $PML4, %edi
    mov $1024, %ecx         /* 2 pages * 512 dwords */
    rep stosl

    /* PML4[0] = PDPT | present|write */
    movl $(PDPT | 0x3), PML4
    /* PDPT[0] = PD | present|write */
    movl $(PD | 0x3), PDPT

    /* PD[i] = (i*2MiB) | present|write|PS   for i = 0..511 */
    mov $0, %ecx
    mov $PD, %edi
.fill_pd:
    mov %ecx, %eax
    shl $21, %eax
    or  $0x83, %eax
    mov %eax, (%edi)
    movl $0, 4(%edi)
    add $8, %edi
    inc %ecx
    cmp $512, %ecx
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
