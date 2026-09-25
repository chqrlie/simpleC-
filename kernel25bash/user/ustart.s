/* ustart.s — the entry point and the syscall instruction for a nano-os program.
 *
 * Two things C cannot say. `int $0x80` is one instruction with no C spelling,
 * and _start is not a function: it is entered with a stack the kernel built and
 * it must never return, because there is nowhere to return to.
 *
 * The kernel starts a process by building a stack that looks exactly like a
 * thread caught mid-interrupt (see proc_build_stack), with argc in rdi and argv
 * in rsi. `iretq` drops into _start with both registers already set, which is
 * why nothing here loads them -- `call main` passes them straight through, and
 * a main() declared (int, char **) is already looking in the right place.
 */
.code64
.section .text

.globl _start
_start:
    xor %rbp, %rbp              /* end of the frame chain: nothing above us */
    call main
    mov %rax, %rdi              /* main's return value becomes the exit code */
    xor %eax, %eax              /* SYS_EXIT */
    int $0x80

    /* SYS_EXIT does not return -- the kernel marks the thread finished and
     * reschedules instead of resuming here. If it ever did come back, spinning
     * is better than falling into whatever bytes follow. */
.Lstopped:
    jmp .Lstopped

/* long syscall4(long nr, long a, long b, long c)
 *
 * The C calling convention hands us (nr, a, b, c) in rdi, rsi, rdx, rcx. The
 * kernel wants nr in rax and the arguments in rdi, rsi, rdx. The shuffle has to
 * go left to right or each move would clobber the next one's source. */
.globl syscall4
syscall4:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    int $0x80
    ret

/* long syscall6(long nr, long a, long b, long c, long d, long e)
 *
 * Five arguments, for SYS_WINOPEN(x, y, w, h, title). C hands us
 * (nr, a, b, c, d, e) in rdi, rsi, rdx, rcx, r8, r9; the kernel wants nr in
 * rax and the arguments in rdi, rsi, rdx, r10, r8.
 *
 * ORDER MATTERS, and not in the obvious direction. The left-to-right shuffle
 * that syscall4 uses works because each destination has already been read;
 * here r8 is BOTH a source (argument d) and a destination (argument d), so it
 * is moved through r10 first and r9 lands in r8 afterwards. Written the tidy
 * way round, the fifth argument arrives as a copy of the fourth. */
.globl syscall6
syscall6:
    mov %rdi, %rax              /* nr */
    mov %rsi, %rdi              /* a */
    mov %rdx, %rsi              /* b */
    mov %rcx, %rdx              /* c */
    mov %r8,  %r10              /* d -> r10, before r8 is overwritten */
    mov %r9,  %r8               /* e */
    int $0x80
    ret
