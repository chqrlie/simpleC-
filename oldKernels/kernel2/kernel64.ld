/* kernel64.ld — link the 64-bit kernel at 0x101000, long_mode_start first. */
ENTRY(long_mode_start)
SECTIONS {
    . = 0x101000;
    .text   : { *(.text.entry) *(.text) }
    .rodata : { *(.rodata) }
    .data   : { *(.data) }
    .bss    : { *(.bss) *(COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}
