/* final.ld — 32-bit Multiboot image: boot32 stub below 0x101000, then the
 * 64-bit kernel blob at 0x101000. */
ENTRY(_start)
SECTIONS {
    . = 0x100000;
    .boot : { *(.multiboot) *(.text) *(.rodata) }
    . = 0x101000;
    .blob : { *(.blob) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}
