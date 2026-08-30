// sse.c — what floating point actually costs on this machine.
//
// The question this image answers is a licensing and effort question dressed
// up as a technical one: do we need a software floating-point library?
//
// A soft-float library exists because some CPUs cannot add two reals. The
// 6502 cannot. The original ARMs could not. On those machines `a * b` has to
// become a call into several thousand lines of shifting and normalising, and
// Berkeley SoftFloat is the good answer.
//
// x86-64 is not one of those machines. SSE2 is not an extension you check for
// on x86-64; it is part of the architecture definition, so `mulsd` is present
// on every CPU that can run this kernel at all. There is nothing to emulate.
//
// But "the instruction exists" is not the same as "the instruction works".
// The CPU boots with SSE switched OFF: CR0.EM says "there is no FPU, trap
// instead", and CR4.OSFXSR says "the OS has not agreed to save the SSE
// registers on a context switch". Until the kernel clears one and sets the
// other, the first `addsd` is an invalid-opcode fault, not an addition.
//
// So this image measures three things in order:
//
//   1. the actual CR0 and CR4 bits this kernel boots with;
//   2. whether `addsd xmm0, xmm1` faults BEFORE the bits are changed --
//      caught with our own #UD handler so the machine survives to report it;
//   3. whether it stops faulting afterwards, and then whether the results are
//      really IEEE-754 and not merely plausible.
//
// Point 3 is the one worth being careful about. "The number printed looks
// about right" is how you ship an FPU that rounds wrongly in the last place.
// The checks below are bit-exact: 0.1 + 0.2 must come out as
// 0x3FD3333333333334, which is one unit above the double nearest to 0.3. Any
// implementation that is merely approximately correct gets that wrong, and
// any implementation that is genuinely IEEE-754 cannot.

#include "nano-kernel.h"
#include "nano-int.h"

long g_fail;

void fail(char *msg) {
    printf("FAIL: %s\n", msg);
    g_fail = g_fail + 1;
}

// ---------------------------------------------------------------------------
// Reading and writing the control registers.
//
// nano_cc has no operand constraints on __asm__ -- the body goes to the
// assembler verbatim -- so everything travels through globals with names the
// compiler will not rename on the way out.
// ---------------------------------------------------------------------------

long _cr0;
long _cr4;

void read_control_regs() {
    __asm__(
        "mov rax, cr0\n"
        "mov qword ptr [rip + _cr0], rax\n"
        "mov rax, cr4\n"
        "mov qword ptr [rip + _cr4], rax\n"
    );
}

// CR0.MP = 1, CR0.EM = 0  -- there is a real FPU, do not trap on its opcodes.
// CR4.OSFXSR = 1          -- this OS will use fxsave/fxrstor, so SSE is legal.
// CR4.OSXMMEXCPT = 1      -- report SSE numeric errors as #XM (vector 19)
//                            rather than as an invalid opcode, which is what
//                            you get otherwise and which is deeply confusing.
//
// The 16-bit forms leave the upper 48 bits of the register untouched, which is
// what we want: the paging and long-mode bits already in CR0/CR4 got us here.
void enable_sse() {
    __asm__(
        "mov rax, cr0\n"
        "and ax, 0xFFFB\n"      // clear EM (bit 2)
        "or  ax, 0x0002\n"      // set MP (bit 1)
        "mov cr0, rax\n"
        "mov rax, cr4\n"
        "or  ax, 0x0600\n"      // set OSFXSR (bit 9) and OSXMMEXCPT (bit 10)
        "mov cr4, rax\n"
    );
}

void print_bits(char *when) {
    printf("  %s: CR0 = %x  CR4 = %x\n", when, _cr0, _cr4);
    printf("    CR0.MP(1)=%d  CR0.EM(2)=%d  CR0.TS(3)=%d\n",
           (_cr0 >> 1) & 1, (_cr0 >> 2) & 1, (_cr0 >> 3) & 1);
    printf("    CR4.OSFXSR(9)=%d  CR4.OSXMMEXCPT(10)=%d\n",
           (_cr4 >> 9) & 1, (_cr4 >> 10) & 1);
}

// ---------------------------------------------------------------------------
// Surviving the fault.
//
// nano-int.h's dispatcher halts the machine on any CPU exception, which is the
// right default and useless here: the whole point is to provoke one and then
// carry on to report it. So vector 6 (#UD, invalid opcode) and vector 7 (#NM,
// device not available -- what you get if CR0.TS is set rather than CR0.EM)
// are pointed at handlers of our own for the duration of the experiment.
//
// The handler steps the saved instruction pointer over the faulting
// instruction. That is only safe because we know exactly which instruction it
// is and exactly how long it is: `addsd xmm0, xmm1` assembles to F2 0F 58 C1,
// four bytes. The build checks that with objdump rather than trusting this
// comment, because a comment that drifts from the encoding would turn this
// handler into a jump into the middle of an instruction.
//
// The interrupt frame's RIP sits at [rsp+8], not [rsp], because nano_cc's
// prologue pushed rbp first. Confirmed by reading the generated assembly, not
// assumed -- the emitted prologue is exactly `push rbp; mov rbp, rsp` with no
// local frame, so the offset is 8 and stays 8.
// ---------------------------------------------------------------------------

long _trap_vec;
#define ADDSD_LEN 4

void ud_handler() {
    __asm__(
        "mov qword ptr [rip + _trap_vec], 6\n"
        "add qword ptr [rsp + 8], 4\n"
        "pop rbp\n"
        "iretq\n"
    );
}

void nm_handler() {
    __asm__(
        "mov qword ptr [rip + _trap_vec], 7\n"
        "add qword ptr [rsp + 8], 4\n"
        "pop rbp\n"
        "iretq\n"
    );
}

// nano_cc has no function pointers, so the address of a function can only be
// obtained the same way isr.s does it: with a lea in assembly.
long ud_handler_addr() { __asm__("lea rax, [rip + ud_handler]\n"); }
long nm_handler_addr() { __asm__("lea rax, [rip + nm_handler]\n"); }

// The instruction under test, on its own, so its encoding is the only thing
// between the two labels the fault handler steps over.
void try_addsd() {
    __asm__("addsd xmm0, xmm1\n");
}

// ---------------------------------------------------------------------------
// The arithmetic itself. Every one of these is a single hardware instruction
// wrapped in the moves needed to get operands in and results out.
// ---------------------------------------------------------------------------

long _a;
long _b;
long _out;
long _scale;

// _out = (long)((double)_a / (double)_b * _scale)
void hw_div_scaled() {
    __asm__(
        "cvtsi2sd xmm0, qword ptr [rip + _a]\n"
        "cvtsi2sd xmm1, qword ptr [rip + _b]\n"
        "divsd xmm0, xmm1\n"
        "cvtsi2sd xmm2, qword ptr [rip + _scale]\n"
        "mulsd xmm0, xmm2\n"
        "cvttsd2si rax, xmm0\n"
        "mov qword ptr [rip + _out], rax\n"
    );
}

// _out = (long)(sqrt((double)_a) * _scale). sqrtsd is one instruction and is
// correctly rounded by the hardware -- in a soft-float world this alone is a
// few hundred lines and a table.
void hw_sqrt_scaled() {
    __asm__(
        "cvtsi2sd xmm0, qword ptr [rip + _a]\n"
        "sqrtsd xmm0, xmm0\n"
        "cvtsi2sd xmm2, qword ptr [rip + _scale]\n"
        "mulsd xmm0, xmm2\n"
        "cvttsd2si rax, xmm0\n"
        "mov qword ptr [rip + _out], rax\n"
    );
}

// _out = the raw 64-bit pattern of ((double)_a/(double)_scale + (double)_b/(double)_scale).
// Used to assert IEEE-754 bit patterns rather than "the number looked right".
void hw_add_bits() {
    __asm__(
        "cvtsi2sd xmm3, qword ptr [rip + _scale]\n"
        "cvtsi2sd xmm0, qword ptr [rip + _a]\n"
        "divsd xmm0, xmm3\n"
        "cvtsi2sd xmm1, qword ptr [rip + _b]\n"
        "divsd xmm1, xmm3\n"
        "addsd xmm0, xmm1\n"
        "movq rax, xmm0\n"
        "mov qword ptr [rip + _out], rax\n"
    );
}

// _out = the raw pattern of (double)_a / (double)_b, no scaling.
void hw_div_bits() {
    __asm__(
        "cvtsi2sd xmm0, qword ptr [rip + _a]\n"
        "cvtsi2sd xmm1, qword ptr [rip + _b]\n"
        "divsd xmm0, xmm1\n"
        "movq rax, xmm0\n"
        "mov qword ptr [rip + _out], rax\n"
    );
}

// The same division in 32-bit single precision, returned as its 32-bit
// pattern. Proves cvtss/divss work too, and that single and double really are
// different formats rather than the same one printed twice.
void hw_div_bits32() {
    __asm__(
        "cvtsi2ss xmm0, qword ptr [rip + _a]\n"
        "cvtsi2ss xmm1, qword ptr [rip + _b]\n"
        "divss xmm0, xmm1\n"
        "movd eax, xmm0\n"
        "mov qword ptr [rip + _out], rax\n"
    );
}

void print_scaled(char *label, long v, long digits) {
    long div;
    long i;
    div = 1;
    i = 0;
    while (i < digits) { div = div * 10; i = i + 1; }
    printf("  %s = %d.%d\n", label, v / div, v % div);
}

void expect(char *what, long got, long want) {
    if (got == want) printf("  %s: %x  as specified\n", what, got);
    else { printf("  %s: got %x, IEEE-754 requires %x\n", what, got, want); fail(what); }
}

// ---------------------------------------------------------------------------

void run_tests() {
    long trapped_before;

    puts("-- 1. the control-register bits this kernel boots with --\n");
    read_control_regs();
    print_bits("at boot");
    if ((_cr0 >> 2) & 1) puts("    CR0.EM is set: SSE opcodes trap. Nothing can be added yet.\n");
    if (!((_cr4 >> 9) & 1)) puts("    CR4.OSFXSR is clear: SSE opcodes trap for a second, separate reason.\n");

    puts("\n-- 2. execute addsd with SSE still switched off --\n");
    idt_set(6, ud_handler_addr());
    idt_set(7, nm_handler_addr());
    _trap_vec = 0;
    try_addsd();
    trapped_before = _trap_vec;
    if (trapped_before == 6) puts("  faulted: #UD, invalid opcode. The hardware is there; permission is not.\n");
    else if (trapped_before == 7) puts("  faulted: #NM, device not available.\n");
    else {
        puts("  addsd executed with SSE apparently disabled\n");
        fail("expected a fault before enabling SSE and did not get one");
    }

    puts("\n-- 3. enable SSE: six instructions, no library --\n");
    enable_sse();
    read_control_regs();
    print_bits("after");
    if ((_cr0 >> 2) & 1) fail("CR0.EM still set after enable_sse");
    if (!((_cr4 >> 9) & 1)) fail("CR4.OSFXSR still clear after enable_sse");

    _trap_vec = 0;
    try_addsd();
    if (_trap_vec) { printf("  addsd still faults, vector %d\n", _trap_vec); fail("addsd faults after enabling SSE"); }
    else puts("  addsd executes. Same instruction, same silicon, different two bits.\n");

    // Put the dispatcher's own handlers back, so a genuine fault from here on
    // is reported and halts rather than being silently stepped over. A
    // recovery handler left installed past the experiment it was written for
    // turns every later bug into a wrong answer instead of a crash.
    idt_init();
    puts("  fault handlers restored to the default (halt-and-report)\n");

    puts("\n-- 4. is it arithmetic, or does it merely look like arithmetic --\n");

    _a = 355; _b = 113; _scale = 10000000;
    hw_div_scaled();
    print_scaled("355 / 113", _out, 7);
    if (_out != 31415929) fail("355/113 is wrong in the 7th decimal place");

    _a = 2; _scale = 10000000;
    hw_sqrt_scaled();
    print_scaled("sqrt(2)  ", _out, 7);
    if (_out != 14142135) fail("sqrt(2) is wrong in the 7th decimal place");

    puts("\n-- 5. bit-exact IEEE-754, not approximately right --\n");

    // The classic. 0.1 and 0.2 are not representable in binary, so their sum
    // is not the double nearest to 0.3 -- it is the next one up. A library
    // that is "close enough" gets 0x...3333 here. The hardware gets 0x...3334
    // because that is what the standard requires, and so must we.
    _a = 1; _b = 2; _scale = 10;
    hw_add_bits();
    expect("0.1 + 0.2", _out, 0x3FD3333333333334);

    _a = 3; _b = 10; _scale = 0;
    hw_div_bits();
    expect("0.3      ", _out, 0x3FD3333333333333);
    if (_out == 0x3FD3333333333334) fail("0.1+0.2 and 0.3 came out identical, so nothing is being rounded");

    _a = 1; _b = 10;
    hw_div_bits32();
    expect("0.1f (32)", _out, 0x3DCCCCCD);

    _a = 1; _b = 10;
    hw_div_bits();
    expect("0.1  (64)", _out, 0x3FB999999999999A);

    puts("\n-- 6. what this replaces --\n");
    puts("  Berkeley SoftFloat 2c, 64-bit build: softfloat.c 5,165 lines plus\n");
    puts("  softfloat-macros 713 and softfloat-specialize 457, for operations\n");
    puts("  this CPU does in one instruction each. Right answer for a 6502.\n");
    puts("  Not for a machine where addsd is already in the silicon.\n");

    if (g_fail) printf("\n%d CHECKS FAILED\n", g_fail);
    else puts("\nPASS: hardware IEEE-754 doubles, bit-exact, after a six-instruction change to boot\n");

    puts("\nSSETEST DONE\n");
}

int main() {
    serial_init();
    g_fail = 0;

    puts("\nnano-os: floating point, measured\n\n");

    interrupts_init(100);
    run_tests();
    for (;;) { }
    return 0;
}
