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
