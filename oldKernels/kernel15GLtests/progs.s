/* progs.s — the user programs, embedded in the kernel image.
 *
 * There is no disk yet, so the programs the OS runs have to arrive inside the
 * kernel and be copied onto the RAM disk at boot. `.incbin` puts the linked ELF
 * files straight into .rodata; the accessors below hand C their address and
 * length, because nano_cc has no way to declare `extern char blob[]` and take
 * its address.
 *
 * The sizes are label differences rather than numbers anybody has to keep in
 * step with the build. A hardcoded length here would be wrong the first time a
 * program grew, and the symptom would be a truncated ELF file rather than an
 * error -- the loader would refuse it, correctly, and the reason would look
 * like a loader bug.
 */
.code64

.section .rodata
.align 16

p_hello:
    .incbin "user/hello.elf"
p_hello_end:

.align 16
p_twin:
    .incbin "user/twin.elf"
p_twin_end:

.align 16
p_wild:
    .incbin "user/wild.elf"
p_wild_end:

/* The C compiler. 131 KB, which is most of this image -- and 19.6 MB before
 * nano_cc learned to leave uninitialised globals in .bss instead of writing
 * their zero bytes into the object. */
.align 16
p_cc:
    .incbin "user/cc.elf"
p_cc_end:

/* The assembler. Same file the Linux build uses, with its OS block swapped --
 * see user/as/README.md. */
.align 16
p_as:
    .incbin "user/as.elf"
p_as_end:

/* The C source the OS compiles, and the header it includes. These are real
 * files in the tree rather than string literals in the kernel, so the host
 * build and the OS compile the SAME BYTES -- which is the only thing that
 * makes comparing the two outputs mean anything. */
.align 16
p_demo:
    .incbin "src/demo.c"
p_demo_end:

.align 16
p_util:
    .incbin "src/util.h"
p_util_end:

/* And the program the machine compiles, assembles and then runs, all by
 * itself. It is written for the --minimal --nasm path, which has no C library
 * at all behind it -- see the note at the top of src/prog.c. */
.align 16
p_prog:
    .incbin "src/prog.c"
p_prog_end:

/* The graphical programs. Same idea as src/prog.c -- real files, compiled
 * inside the machine -- but these ask the window manager for a window and blit
 * their own pixels into it. src/winbad.c is the one that misbehaves on
 * purpose: it blits out of bounds and at a handle it does not own, and reports
 * what the kernel said. */
.align 16
p_wingl:
    .incbin "src/wingl.c"
p_wingl_end:

.align 16
p_winbad:
    .incbin "src/winbad.c"
p_winbad_end:

.section .text

.globl prog_wingl_addr
prog_wingl_addr:
    lea p_wingl(%rip), %rax
    ret

.globl prog_wingl_size
prog_wingl_size:
    mov $(p_wingl_end - p_wingl), %rax
    ret

.globl prog_winbad_addr
prog_winbad_addr:
    lea p_winbad(%rip), %rax
    ret

.globl prog_winbad_size
prog_winbad_size:
    mov $(p_winbad_end - p_winbad), %rax
    ret

.globl prog_hello_addr
prog_hello_addr:
    lea p_hello(%rip), %rax
    ret

.globl prog_hello_size
prog_hello_size:
    mov $(p_hello_end - p_hello), %rax
    ret

.globl prog_twin_addr
prog_twin_addr:
    lea p_twin(%rip), %rax
    ret

.globl prog_twin_size
prog_twin_size:
    mov $(p_twin_end - p_twin), %rax
    ret

.globl prog_wild_addr
prog_wild_addr:
    lea p_wild(%rip), %rax
    ret

.globl prog_wild_size
prog_wild_size:
    mov $(p_wild_end - p_wild), %rax
    ret

.globl prog_cc_addr
prog_cc_addr:
    lea p_cc(%rip), %rax
    ret

.globl prog_cc_size
prog_cc_size:
    mov $(p_cc_end - p_cc), %rax
    ret

.globl prog_demo_addr
prog_demo_addr:
    lea p_demo(%rip), %rax
    ret

.globl prog_demo_size
prog_demo_size:
    mov $(p_demo_end - p_demo), %rax
    ret

.globl prog_util_addr
prog_util_addr:
    lea p_util(%rip), %rax
    ret

.globl prog_util_size
prog_util_size:
    mov $(p_util_end - p_util), %rax
    ret

.globl prog_as_addr
prog_as_addr:
    lea p_as(%rip), %rax
    ret

.globl prog_as_size
prog_as_size:
    mov $(p_as_end - p_as), %rax
    ret

.globl prog_prog_addr
prog_prog_addr:
    lea p_prog(%rip), %rax
    ret

.globl prog_prog_size
prog_prog_size:
    mov $(p_prog_end - p_prog), %rax
    ret
