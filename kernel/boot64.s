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
