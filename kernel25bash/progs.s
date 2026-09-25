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

/* The GUI apps, which are the point of the apps-not-demos milestone: the same
 * nano-ui.h the kernel images compile, running as ordinary processes. */
.align 16
p_uidemo:
    .incbin "user/uidemo.elf"
p_uidemo_end:

.align 16
p_edit:
    .incbin "user/edit.elf"
p_edit_end:

.align 16
p_files:
    .incbin "user/files.elf"
p_files_end:

.align 16
p_sh:
    .incbin "user/sh.elf"
p_sh_end:

.align 16
p_cat:
    .incbin "user/cat.elf"
p_cat_end:

/* A bulk producer, used to push several buffers' worth through a pipe so the
 * writer actually runs out of space and has to wait for the reader. */
.align 16
p_bulk:
    .incbin "user/bulk.elf"
p_bulk_end:

/* The client's miniShell, ported. 194 KB -- the second largest thing in the
 * image after the C compiler. */
.align 16
p_msh:
    .incbin "user/msh.elf"
p_msh_end:

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

/* src/gears.c and the two renderer headers it includes. This is the whole of
 * nano-gl.h and nano-glapi.h going onto the RAM disk so that the compiler
 * INSIDE the machine can #include them -- the same two files the kernel
 * images compile against, not a copy and not a subset. */
.align 16
p_ugears:
    .incbin "src/gears.c"
p_ugears_end:

.align 16
p_hgl:
    .incbin "nano-gl.h"
p_hgl_end:

.align 16
p_hglapi:
    .incbin "nano-glapi.h"
p_hglapi_end:

.section .text

.globl prog_ugears_addr
prog_ugears_addr:
    lea p_ugears(%rip), %rax
    ret

.globl prog_ugears_size
prog_ugears_size:
    mov $(p_ugears_end - p_ugears), %rax
    ret

.globl prog_hgl_addr
prog_hgl_addr:
    lea p_hgl(%rip), %rax
    ret

.globl prog_hgl_size
prog_hgl_size:
    mov $(p_hgl_end - p_hgl), %rax
    ret

.globl prog_hglapi_addr
prog_hglapi_addr:
    lea p_hglapi(%rip), %rax
    ret

.globl prog_hglapi_size
prog_hglapi_size:
    mov $(p_hglapi_end - p_hglapi), %rax
    ret

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

.globl prog_uidemo_addr
prog_uidemo_addr:
    lea p_uidemo(%rip), %rax
    ret

.globl prog_wild_size
prog_wild_size:
    mov $(p_wild_end - p_wild), %rax
    ret

.globl prog_uidemo_size
prog_uidemo_size:
    mov $(p_uidemo_end - p_uidemo), %rax
    ret

.globl prog_edit_addr
prog_edit_addr:
    lea p_edit(%rip), %rax
    ret

.globl prog_edit_size
prog_edit_size:
    mov $(p_edit_end - p_edit), %rax
    ret

.globl prog_files_addr
prog_files_addr:
    lea p_files(%rip), %rax
    ret

.globl prog_files_size
prog_files_size:
    mov $(p_files_end - p_files), %rax
    ret

.globl prog_sh_addr
prog_sh_addr:
    lea p_sh(%rip), %rax
    ret

.globl prog_sh_size
prog_sh_size:
    mov $(p_sh_end - p_sh), %rax
    ret

.globl prog_cat_addr
prog_cat_addr:
    lea p_cat(%rip), %rax
    ret

.globl prog_cat_size
prog_cat_size:
    mov $(p_cat_end - p_cat), %rax
    ret

.globl prog_bulk_addr
prog_bulk_addr:
    lea p_bulk(%rip), %rax
    ret

.globl prog_bulk_size
prog_bulk_size:
    mov $(p_bulk_end - p_bulk), %rax
    ret

.globl prog_msh_addr
prog_msh_addr:
    lea p_msh(%rip), %rax
    ret

.globl prog_msh_size
prog_msh_size:
    mov $(p_msh_end - p_msh), %rax
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
