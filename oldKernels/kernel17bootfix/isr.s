/* isr.s — 256 interrupt stubs, and the machinery C cannot express.
 *
 * An IDT entry holds the ADDRESS of a handler, and the CPU arrives there with
 * a hardware stack frame it expects to leave with `iretq` — neither of which a
 * C function can do. So every vector gets a two-instruction stub here that
 * normalises the frame and jumps to one common path, which saves the register
 * file and calls a single C dispatcher with a pointer to it.
 *
 * Two things the stubs exist to even out:
 *
 *   - Only vectors 8, 10..14 and 17, 21, 29, 30 push an error code. The rest do
 *     not. If the handler is going to treat every frame the same way, the ones
 *     without have to push a zero in its place, or the stack layout differs by
 *     eight bytes depending on which fault happened.
 *
 *   - The vector number is nowhere in the hardware frame. The only way to know
 *     which interrupt fired is for the stub that was entered to say so.
 *
 * The dispatcher is chosen by NUMBER, not by a function pointer table, because
 * nano_cc has no function pointers. `isr_table` below is a plain array of
 * addresses that C reads to fill in the IDT.
 */
.code64
.section .text

.extern isr_dispatch

/* The frame the dispatcher sees, from its pointer downwards:
 *
 *   +0   r15 r14 r13 r12 r11 r10 r9 r8              (8 * 8 bytes)
 *   +64  rbp rdi rsi rdx rcx rbx rax                (7 * 8)
 *   +120 vector
 *   +128 error code
 *   +136 rip cs rflags rsp ss                       (the CPU's own frame)
 *
 * The C side has the same layout written out as a struct; if one changes the
 * other has to, and the fault report is the thing that would go quietly wrong.
 */
isr_common:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15

    mov %rsp, %rdi              /* one argument: a pointer to the frame */
    cld                         /* the ABI wants DF clear on entry to C */
    call isr_dispatch

    /* isr_dispatch RETURNS the stack pointer to resume on. Normally that is
     * the one it was handed and this changes nothing. When the scheduler
     * decides to switch task, it returns the OTHER task's saved stack instead,
     * and the pops below unwind that task rather than this one -- so a whole
     * context switch is this single instruction. Every register, the
     * instruction pointer and the flags all live on the stack at this point,
     * so moving the stack moves all of them at once. */

    /* If the scheduler picked a task in a DIFFERENT address space, it left the
     * new page-table root in g_switch_cr3 rather than installing it itself.
     * It could not install it: sched_switch runs on the outgoing task's stack,
     * and changing CR3 unmaps that stack out from under the function that is
     * still running on it.
     *
     * Here is the one window where that is safe. Between the call returning and
     * the mov below, nothing touches memory at all -- the new stack pointer is
     * in %rax, the new root is in %rcx, and the instructions themselves are in
     * the kernel image, which every address space maps identically.
     *
     * Zero means "no change", which is also what the field holds on any kernel
     * that never creates a second address space. */
    mov g_switch_cr3(%rip), %rcx
    test %rcx, %rcx
    jz .Lsame_space
    mov %rcx, %cr3
.Lsame_space:
    mov %rax, %rsp

    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax

    add $16, %rsp               /* drop the vector and the error code */
    iretq

/* A fallback definition of g_switch_cr3, for the kernels that have no scheduler.
 *
 * isr.s is one file shared by every image here, and three of them -- the
 * interrupt, ACPI and memory-manager bring-ups -- do not include nano-thread.h
 * and so never define this variable. The reference above would leave them
 * failing to link over a feature they do not use.
 *
 * `.weak` is exactly the right tool: an image that defines the symbol in C gets
 * its own, and the one here is discarded; an image that does not gets this
 * eight bytes of zero, which reads as "never change CR3".
 *
 * .data, NOT .bss. These images are flattened with `objcopy -O binary` and
 * loaded by a stub that zeroes nothing, so a .bss variable would come up
 * holding whatever was in memory -- and this one is loaded straight into CR3.
 */
.section .data
.align 8
.weak g_switch_cr3
g_switch_cr3:
    .quad 0

.section .text

/* isr_noerr N: the CPU pushed no error code, so push a zero in its place. */
.macro isr_noerr n
.globl isr\n
isr\n:
    push $0
    push $\n
    jmp isr_common
.endm

/* isr_err N: the CPU already pushed an error code. */
.macro isr_err n
.globl isr\n
isr\n:
    push $\n
    jmp isr_common
.endm

/* Exceptions 0..31: only these push an error code. */
isr_noerr 0
isr_noerr 1
isr_noerr 2
isr_noerr 3
isr_noerr 4
isr_noerr 5
isr_noerr 6
isr_noerr 7
isr_err   8
isr_noerr 9
isr_err   10
isr_err   11
isr_err   12
isr_err   13
isr_err   14
isr_noerr 15
isr_noerr 16
isr_err   17
isr_noerr 18
isr_noerr 19
isr_noerr 20
isr_err   21
isr_noerr 22
isr_noerr 23
isr_noerr 24
isr_noerr 25
isr_noerr 26
isr_noerr 27
isr_noerr 28
isr_err   29
isr_err   30
isr_noerr 31

/* Everything above 31 is a device or software interrupt: no error code. */
.altmacro
.set vec, 32
.rept 224
    isr_noerr %vec
    .set vec, vec + 1
.endr

/* A plain array of the 256 stub addresses, so the C side can fill in the IDT
 * without needing function pointers. */
.section .rodata
.align 8
.globl isr_table
isr_table:
/* `.quad isr%vec` does not work directly: %-substitution only happens on macro
 * ARGUMENTS, so the address has to be emitted from inside a macro. */
.macro tblent n
    .quad isr\n
.endm
.set vec, 0
.rept 256
    tblent %vec
    .set vec, vec + 1
.endr

.section .text

/* long isr_table_addr(void) — hand C the address of that array. */
.globl isr_table_addr
isr_table_addr:
    lea isr_table(%rip), %rax
    ret

/* void idt_load(long descriptor_address) */
.globl idt_load
idt_load:
    lidt (%rdi)
    ret

.globl cli_
cli_:
    cli
    ret

/* long irq_save(void) — disable interrupts, return whether they were on.
 * This is the uniprocessor lock: with preemption coming only from the timer,
 * nothing else can be running to contend, so masking interrupts IS mutual
 * exclusion. On more than one core it stops being true and needs a real
 * atomic test-and-set; the shape here is the same either way. */
.globl irq_save
irq_save:
    pushfq
    pop %rax
    and $0x200, %rax            /* the interrupt-enable flag */
    cli
    ret

/* void irq_restore(long were_enabled) */
.globl irq_restore
irq_restore:
    test %rdi, %rdi
    jz 1f
    sti
1:  ret

/* void switch_to_first(long rsp) — start the very first thread.
 *
 * There is no task to switch away from yet, so this drops onto the new stack
 * and unwinds it exactly the way isr_common would. The stack was built by
 * thread_create to look like a complete interrupt frame, which is what makes
 * a brand-new thread and a preempted one indistinguishable from here. */
.globl switch_to_first
switch_to_first:
    mov %rdi, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rdx
    pop %rcx
    pop %rbx
    pop %rax
    add $16, %rsp
    iretq

.globl sti_
sti_:
    sti
    ret

/* void cpu_idle(void) — enable interrupts and stop the core until one
 * arrives. `sti` does not take effect until after the NEXT instruction, which
 * is what makes this pair atomic: an interrupt cannot slip in between the two
 * and leave the CPU halted with nothing left to wake it. Writing it the other
 * way round is the classic lost-wakeup hang. */
.globl cpu_idle
cpu_idle:
    sti
    hlt
    ret

/* void cpu_halt_forever(void) */
.globl cpu_halt_forever
cpu_halt_forever:
    cli
1:  hlt
    jmp 1b

/* long read_eflags(void) — so the kernel can check its own machine state.
 *
 * Exists for one assertion: that the direction flag is CLEAR by the time C
 * runs. Multiboot leaves DF undefined on entry, the System V ABI requires it
 * clear on entry to every function, and a string instruction that runs
 * backwards is the kind of fault that leaves no output at all. boot32.s clears
 * it; this is how a test says so. */
.globl read_eflags
read_eflags:
    pushfq
    pop %rax
    ret

/* long read_cr2(void) — the faulting address, for a page fault. */
.globl read_cr2
read_cr2:
    mov %cr2, %rax
    ret

/* long read_cr3(void) */
.globl read_cr3
read_cr3:
    mov %cr3, %rax
    ret

/* long read_rsp(void) */
.globl read_rsp
read_rsp:
    mov %rsp, %rax
    ret

/* void io_wait(void) — a throwaway write to an unused port, the traditional
 * short delay the 8259s need between initialisation words on old hardware. */
.globl io_wait
io_wait:
    mov $0x80, %dx
    xor %eax, %eax
    out %al, %dx
    ret

/* void yield_now(void) — give up the rest of this time slice.
 *
 * A software interrupt on a vector of its own, rather than calling the
 * scheduler directly. That way a voluntary yield and a timer preemption arrive
 * at the scheduler through exactly the same path, with the same stack layout,
 * so there is one code path to get right instead of two. */
.globl yield_now
yield_now:
    int $0x81
    ret
