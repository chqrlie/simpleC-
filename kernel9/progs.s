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

.section .text

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
