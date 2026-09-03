// nano-int.h — interrupts: an IDT, exception reporting, the 8259s, a timer,
// and an idle loop that actually stops the core.
//
// Compiled by nano_cc with --kernel. The parts C cannot express — the stubs,
// `lidt`, `sti`/`hlt`, reading CR2 — live in isr.s.
//
// Before this existed, any fault was a triple fault: the CPU could not find a
// handler, could not fault on that either, and reset. On QEMU that looks like
// the machine silently rebooting, with no message and nothing to go on. Half
// the value here is turning that into a page of text.
//
// Include AFTER nano-fb.h and nano-kernel.h, since the fault report prints.

#ifndef NANO_INT_H
#define NANO_INT_H

extern long isr_table_addr();
extern void idt_load(long descriptor);
extern void cli_();
extern void sti_();
extern void cpu_idle();
extern void cpu_halt_forever();
extern long read_cr2();
extern long read_cr3();
extern long read_rsp();
extern void io_wait();

// ---------- the register frame isr.s builds ----------
// This must match the push order in isr_common exactly. If the two ever
// disagree the fault report prints plausible-looking nonsense, which is worse
// than printing nothing.
struct Regs {
    long r15; long r14; long r13; long r12;
    long r11; long r10; long r9;  long r8;
    long rbp; long rdi; long rsi; long rdx;
    long rcx; long rbx; long rax;
    long vec; long err;
    long rip; long cs; long rflags; long rsp; long ss;
};

// ---------- the IDT ----------
// 256 entries of 16 bytes. Each is built as two 64-bit words rather than the
// seven fields the manual describes, because nano_cc has no 16- or 32-bit
// store and would need four writes and a lot of masking to do it the other
// way.
//
//   word 0: offset[15:0] | selector<<16 | ist<<32 | type_attr<<40 | offset[31:16]<<48
//   word 1: offset[63:32]
long g_idt[512];            // 256 entries x 2 words
long g_idtr[2];             // limit in the low 16 bits of word 0, then base

#define IDT_INTERRUPT 0x8E  // present, ring 0, 64-bit interrupt gate
#define KCODE_SEL     0x08  // the 64-bit code selector boot32.s installed

void idt_set(long vec, long handler) {
    long lo;
    long hi;
    lo = handler & 0xFFFF;                        // offset[15:0]
    lo = lo | (KCODE_SEL << 16);                  // selector
    lo = lo | (IDT_INTERRUPT << 40);              // present, ring 0, int gate
    lo = lo | (((handler >> 16) & 0xFFFF) << 48); // offset[31:16]
    hi = (handler >> 32) & 0xFFFFFFFF;            // offset[63:32]
    g_idt[vec * 2] = lo;
    g_idt[vec * 2 + 1] = hi;
}

// ---------- the 8259 pair ----------
// The PICs come up delivering IRQ0..7 on vectors 8..15, which are the CPU's
// own exception numbers -- a timer tick would arrive looking exactly like a
// double fault. Remapping them is not a nicety, it is the difference between
// an interrupt and a lie about which one happened.
#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

void pic_remap(int off1, int off2) {
    int m1;
    int m2;
    m1 = inb(PIC1_DATA);
    m2 = inb(PIC2_DATA);

    outb(PIC1_CMD, 0x11); io_wait();      // start init, expect ICW4
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, off1); io_wait();     // vector offsets
    outb(PIC2_DATA, off2); io_wait();
    outb(PIC1_DATA, 0x04); io_wait();     // secondary is on IRQ2
    outb(PIC2_DATA, 0x02); io_wait();     // ...and this is its identity
    outb(PIC1_DATA, 0x01); io_wait();     // 8086 mode
    outb(PIC2_DATA, 0x01); io_wait();

    outb(PIC1_DATA, m1);                  // restore the masks
    outb(PIC2_DATA, m2);
}

void pic_mask(int irq, int masked) {
    int port;
    int val;
    if (irq < 8) { port = PIC1_DATA; }
    else { port = PIC2_DATA; irq = irq - 8; }
    val = inb(port);
    if (masked) val = val | (1 << irq);
    else        val = val & ~(1 << irq);
    outb(port, val);
}

// Tell the PIC the interrupt has been handled. The secondary has to be told
// too, or IRQ8..15 fire exactly once and then never again.
void pic_eoi(long irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

// ---------- the 8253/8254 timer ----------
// Channel 0, mode 3 (square wave), from the fixed 1.193182 MHz input.
#define PIT_CH0  0x40
#define PIT_CMD  0x43

long g_ticks;
long g_hz;

void pit_init(long hz) {
    long div;
    div = 1193182 / hz;
    if (div < 1) div = 1;
    if (div > 65535) div = 65535;
    g_hz = 1193182 / div;                 // the rate we actually got
    outb(PIT_CMD, 0x36);                  // channel 0, lo/hi, mode 3, binary
    outb(PIT_CH0, div & 0xFF);
    outb(PIT_CH0, (div >> 8) & 0xFF);
}

// ---------- the keyboard, on an interrupt ----------
// A ring buffer, filled by the IRQ and drained by whoever is reading. The
// polling version could only ever be doing one thing at a time; this one lets
// the CPU sleep between keys.
#define KBD_RING 256
char g_kbd_ring[KBD_RING];
long g_kbd_head;
long g_kbd_tail;
long g_kbd_dropped;

void kbd_push(char c) {
    long next;
    next = (g_kbd_head + 1) % KBD_RING;
    if (next == g_kbd_tail) { g_kbd_dropped = g_kbd_dropped + 1; return; }
    g_kbd_ring[g_kbd_head] = c;
    g_kbd_head = next;
}

long kbd_available() { return g_kbd_head != g_kbd_tail; }

// ---------- exception reporting ----------
char *exc_name(long v) {
    if (v == 0)  return "divide error";
    if (v == 1)  return "debug";
    if (v == 2)  return "NMI";
    if (v == 3)  return "breakpoint";
    if (v == 4)  return "overflow";
    if (v == 5)  return "bound range exceeded";
    if (v == 6)  return "invalid opcode";
    if (v == 7)  return "device not available";
    if (v == 8)  return "double fault";
    if (v == 10) return "invalid TSS";
    if (v == 11) return "segment not present";
    if (v == 12) return "stack-segment fault";
    if (v == 13) return "general protection fault";
    if (v == 14) return "page fault";
    if (v == 16) return "x87 floating-point";
    if (v == 17) return "alignment check";
    if (v == 18) return "machine check";
    if (v == 19) return "SIMD floating-point";
    if (v == 21) return "control protection";
    return "reserved/unknown";
}

void dump_regs(struct Regs *r) {
    printf("\n*** EXCEPTION %d: %s\n", r->vec, exc_name(r->vec));
    printf("error 0x%x  rip 0x%x  cs 0x%x\n", r->err, r->rip, r->cs);
    printf("rflags 0x%x  rsp 0x%x  ss 0x%x\n", r->rflags, r->rsp, r->ss);
    if (r->vec == 14) {
        long cr2;
        cr2 = read_cr2();
        printf("faulting address 0x%x\n", cr2);
        // The page-fault error code is a bitfield, and reading it as a number
        // tells you almost nothing.
        printf("cause: ");
        if (r->err & 1) puts("protection violation"); else puts("page not present");
        if (r->err & 2) puts(", on a write"); else puts(", on a read");
        if (r->err & 4) puts(", from user mode"); else puts(", from kernel mode");
        if (r->err & 16) puts(", instruction fetch");
        putc('\n');
    }
    printf("rax 0x%x  rbx 0x%x  rcx 0x%x\n", r->rax, r->rbx, r->rcx);
    printf("rdx 0x%x  rsi 0x%x  rdi 0x%x\n", r->rdx, r->rsi, r->rdi);
    printf("rbp 0x%x  r8  0x%x  r9  0x%x\n", r->rbp, r->r8, r->r9);
    printf("r10 0x%x  r11 0x%x  r12 0x%x\n", r->r10, r->r11, r->r12);
    printf("r13 0x%x  r14 0x%x  r15 0x%x\n", r->r13, r->r14, r->r15);
}

// ---------- the one dispatcher ----------
// isr.s calls this for every vector. There is no table of handlers because
// there are no function pointers; a small number of interrupts matter and they
// are named here.
#define IRQ_BASE 32

long g_spurious;

void isr_dispatch(struct Regs *r) {
    long v;
    v = r->vec;

    if (v < 32) {
        // A CPU exception. Nothing here can meaningfully continue, so report
        // it fully and stop, rather than returning into the same instruction
        // and faulting forever.
        dump_regs(r);
        puts("\nhalted.\n");
        cpu_halt_forever();
        return;
    }

    if (v == IRQ_BASE + 0) {                    // timer
        g_ticks = g_ticks + 1;
        pic_eoi(0);
        return;
    }

    if (v == IRQ_BASE + 1) {                    // keyboard
        int sc;
        sc = inb(0x60);
        if (sc < 128) {
            char ch;
            ch = g_keymap[sc];
            if (ch != 0) kbd_push(ch);
        }
        pic_eoi(1);
        return;
    }

    // IRQ7 and IRQ15 can fire with nothing behind them -- a spurious interrupt
    // from line noise or a race in the PIC. The primary's must NOT be
    // acknowledged, or a real interrupt gets its EOI consumed by this one.
    if (v == IRQ_BASE + 7) { g_spurious = g_spurious + 1; return; }
    if (v == IRQ_BASE + 15) { g_spurious = g_spurious + 1; outb(PIC2_CMD, PIC_EOI); return; }

    if (v >= IRQ_BASE && v < IRQ_BASE + 16) { pic_eoi(v - IRQ_BASE); return; }
}

// ---------- bring-up ----------
void idt_init() {
    long tbl;
    long i;
    tbl = isr_table_addr();
    i = 0;
    while (i < 256) {
        long *entry;
        entry = (long *)tbl;
        idt_set(i, entry[i]);
        i = i + 1;
    }
    // The IDTR is a PACKED 10-byte structure: a 16-bit limit immediately
    // followed by a 64-bit base, with no padding between them. Two `long`
    // fields would put the base at offset 8 and `lidt` would read six bytes of
    // nothing. Writing the bytes one at a time is the only way to say this in
    // a language with no 16-bit type.
    {
        char *p;
        long base;
        long lim;
        long k;
        p = (char *)g_idtr;
        lim = 256 * 16 - 1;
        p[0] = lim & 0xFF;
        p[1] = (lim >> 8) & 0xFF;
        base = (long)g_idt;
        k = 0;
        while (k < 8) { p[2 + k] = (base >> (k * 8)) & 0xFF; k = k + 1; }
    }
    idt_load((long)g_idtr);
}

void interrupts_init(long hz) {
    cli_();
    g_ticks = 0;
    g_kbd_head = 0;
    g_kbd_tail = 0;
    g_kbd_dropped = 0;
    g_spurious = 0;
    idt_init();
    pic_remap(IRQ_BASE, IRQ_BASE + 8);
    // Everything masked except the timer and the keyboard. An unmasked line
    // with no handler is an interrupt storm the moment its device says
    // anything.
    outb(PIC1_DATA, 0xFC);            // IRQ0 and IRQ1 enabled
    outb(PIC2_DATA, 0xFF);
    pit_init(hz);
    sti_();
}

// Block until a key arrives, sleeping in between rather than spinning. This is
// the idle: `cpu_idle` is sti+hlt, so the core stops until the next interrupt.
long g_idle_wakeups;

char keyboard_getchar_irq() {
    for (;;) {
        if (kbd_available()) {
            char c;
            c = g_kbd_ring[g_kbd_tail];
            g_kbd_tail = (g_kbd_tail + 1) % KBD_RING;
            return c;
        }
        g_idle_wakeups = g_idle_wakeups + 1;
        cpu_idle();
    }
}

// Sleep for roughly n milliseconds, using the timer rather than a spin.
void sleep_ms(long ms) {
    long target;
    target = g_ticks + (ms * g_hz) / 1000;
    while (g_ticks < target) cpu_idle();
}

#endif
