/* user.ld — where a nano-os program lives.
 *
 * 0x8000000000 is 512 GiB: the base of PML4 entry 1, the one slot each process
 * gets a private copy of. Every program is linked at the same address on
 * purpose -- that is what makes two running copies a test of isolation rather
 * than a test of the linker.
 *
 * Link with -z max-page-size=0x1000. Without it ld aligns segments to 2 MiB and
 * a 4 KiB program claims two megabytes of frames, most of them zero.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x8000000000;

    .text : {
        *(.text*)
    }

    /* Read-only data gets its own page so that the text segment can be mapped
     * without PTE_WRITE and stay that way. Sharing a page with .data would make
     * the loader take the union of the permissions and quietly hand the program
     * a writable code segment. */
    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata*)
    }

    . = ALIGN(0x1000);
    .data : {
        *(.data*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    _end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
