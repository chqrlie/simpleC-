// nano-kernel.h — a tiny freestanding runtime for the nano_cc "mini-OS" target.
//
// Compiled by nano_cc itself (with --kernel).  Provides VGA text output, a
// serial mirror (so the shell is testable headlessly), and a real PS/2
// keyboard driver.  The only external symbols are inb/outb, which are the
// port-I/O instructions provided by boot.s.
//
// Everything here stays inside the language subset nano_cc supports:
// no globals initialisers (globals live in .bss, zeroed), no switch, no
// brace initialisers — just functions, locals, loops, if/else and pointers.

extern int  inb(int port);
extern void outb(int port, int val);

// ---------- reading physical memory a field at a time ----------
// Everything below 4 GiB is identity-mapped, so a physical address can be
// dereferenced directly. These exist because nano_cc has no 16- or 32-bit
// integer type: firmware tables are laid out in 1, 2, 4 and 8-byte fields and
// the only way to read them correctly is a byte at a time.
long mem8(long addr) {
    char *p;
    p = (char *)addr;
    return p[0] & 255;
}
long mem16(long addr) { return mem8(addr) | (mem8(addr + 1) << 8); }
long mem32(long addr) { return mem16(addr) | (mem16(addr + 2) << 16); }
long mem64(long addr) { return mem32(addr) | (mem32(addr + 4) << 32); }

// ---------- VGA text mode: 80x25 cells at physical 0xB8000 ----------
int vga_row;
int vga_col;

void vga_put(int c) {
    char *vga;
    vga = (char *)0xB8000;
    if (c == '\n') {
        vga_col = 0;
        vga_row = vga_row + 1;
    } else {
        int idx;
        idx = (vga_row * 80 + vga_col) * 2;
        vga[idx] = c;
        vga[idx + 1] = 0x0F;              // bright white on black
        vga_col = vga_col + 1;
        if (vga_col >= 80) { vga_col = 0; vga_row = vga_row + 1; }
    }
    if (vga_row >= 25) vga_row = 0;       // wrap to top (no scroll — keep it small)
}

void vga_clear() {
    char *vga;
    int i;
    vga = (char *)0xB8000;
    i = 0;
    while (i < 80 * 25) {
        vga[i * 2] = ' ';
        vga[i * 2 + 1] = 0x0F;
        i = i + 1;
    }
    vga_row = 0;
    vga_col = 0;
}

// ---------- COM1 serial (0x3F8): mirrors output for headless testing ----------
void serial_init() {
    outb(0x3F8 + 1, 0x00);               // disable interrupts
    outb(0x3F8 + 3, 0x80);               // enable DLAB
    outb(0x3F8 + 0, 0x03);               // divisor low  (38400 baud)
    outb(0x3F8 + 1, 0x00);               // divisor high
    outb(0x3F8 + 3, 0x03);               // 8 bits, no parity, 1 stop
    outb(0x3F8 + 2, 0xC7);               // enable + clear FIFO
    outb(0x3F8 + 4, 0x0B);               // RTS/DSR set
}

void serial_put(int c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0) { }   // wait: transmit holding empty
    outb(0x3F8, c);
}

// ---------- combined character / string output ----------
//
// Where the characters go depends on whether a framebuffer console exists.
// Include nano-fb.h BEFORE this file and text goes to the framebuffer; include
// this file alone and it goes to VGA text mode at 0xB8000. The test for that
// is the include guard, so the order matters and is checked at compile time
// rather than producing a link error.
//
// The serial mirror is unconditional either way, because that is what the
// headless tests read.
int g_have_fb;      // set to 1 once a framebuffer console is up

void putc(int c) {
    if (c == '\n') serial_put('\r');     // CR so serial terminals advance
    serial_put(c);
#ifdef NANO_FB_H
    if (g_have_fb) fb_putc(c); else vga_put(c);
#else
    vga_put(c);
#endif
}

void puts(char *s) {
    while (*s) { putc(*s); s = s + 1; }
}

void print_int(long n) {
    char buf[24];
    int i;
    if (n == 0) { putc('0'); return; }
    if (n < 0) { putc('-'); n = -n; }
    i = 0;
    while (n > 0) { buf[i] = '0' + (n % 10); i = i + 1; n = n / 10; }
    while (i > 0) { i = i - 1; putc(buf[i]); }
}

int strcmp(char *a, char *b) {
    while (*a && (*a == *b)) { a = a + 1; b = b + 1; }
    return *a - *b;
}

// ---------- stdarg + printf (variadic functions) ----------
#define va_list         long
#define va_start(ap, l) __builtin_va_start(ap)
#define va_arg(ap, t)   __builtin_va_arg(ap)
#define va_end(ap)      __builtin_va_end(ap)

void _put_uint(long n, int base) {
    char buf[32]; int i; char *digits;
    digits = "0123456789abcdef";
    if (n == 0) { putc('0'); return; }
    i = 0;
    while (n > 0) { buf[i] = digits[n % base]; i = i + 1; n = n / base; }
    while (i > 0) { i = i - 1; putc(buf[i]); }
}

// printf straight to the framebuffer or VGA screen (and the serial mirror).
//
// %d %x %c %s and %% ONLY -- no flags, no field width, no precision. Writing
// "%-9s" here does not pad, it prints "%-9s" literally and then reads the NEXT
// argument for the following conversion, so every column after it is one
// argument out. If you want columns, pad them yourself. nano-libc.h has the
// full formatter; this one is deliberately small because it is what the kernel
// uses before there is a heap.
void printf(char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    while (*fmt) {
        if (*fmt == '%') {
            fmt = fmt + 1;
            if (*fmt == 'd') {
                long v; v = va_arg(ap, int);
                if (v < 0) { putc('-'); v = -v; }
                _put_uint(v, 10);
            } else if (*fmt == 'x') {
                _put_uint(va_arg(ap, int), 16);
            } else if (*fmt == 's') {
                char *s; s = (char *)va_arg(ap, char *);
                puts(s);
            } else if (*fmt == 'c') {
                putc(va_arg(ap, int));
            } else if (*fmt == '%') {
                putc('%');
            } else {
                putc('%'); putc(*fmt);
            }
        } else {
            putc(*fmt);
        }
        fmt = fmt + 1;
    }
    va_end(ap);
}

// ---------- PS/2 keyboard ----------
// US-QWERTY scancode set 1 -> ASCII, built once into a .bss table.
char g_keymap[128];

void kbd_init() {
    int i;
    i = 0;
    while (i < 128) { g_keymap[i] = 0; i = i + 1; }
    g_keymap[0x02] = '1'; g_keymap[0x03] = '2'; g_keymap[0x04] = '3';
    g_keymap[0x05] = '4'; g_keymap[0x06] = '5'; g_keymap[0x07] = '6';
    g_keymap[0x08] = '7'; g_keymap[0x09] = '8'; g_keymap[0x0A] = '9';
    g_keymap[0x0B] = '0';
    g_keymap[0x10] = 'q'; g_keymap[0x11] = 'w'; g_keymap[0x12] = 'e';
    g_keymap[0x13] = 'r'; g_keymap[0x14] = 't'; g_keymap[0x15] = 'y';
    g_keymap[0x16] = 'u'; g_keymap[0x17] = 'i'; g_keymap[0x18] = 'o';
    g_keymap[0x19] = 'p';
    g_keymap[0x1E] = 'a'; g_keymap[0x1F] = 's'; g_keymap[0x20] = 'd';
    g_keymap[0x21] = 'f'; g_keymap[0x22] = 'g'; g_keymap[0x23] = 'h';
    g_keymap[0x24] = 'j'; g_keymap[0x25] = 'k'; g_keymap[0x26] = 'l';
    g_keymap[0x2C] = 'z'; g_keymap[0x2D] = 'x'; g_keymap[0x2E] = 'c';
    g_keymap[0x2F] = 'v'; g_keymap[0x30] = 'b'; g_keymap[0x31] = 'n';
    g_keymap[0x32] = 'm';
    g_keymap[0x39] = ' ';       // space
    g_keymap[0x1C] = '\n';      // enter
    g_keymap[0x0E] = '\b';      // backspace
}

// Block until a key is pressed; return its ASCII value.  A genuine hardware
// read: poll the i8042 status port (0x64) for "output buffer full", then read
// the scancode from the data port (0x60).  Key-release codes (>= 0x80) are
// ignored.
char keyboard_getchar() {
    int sc;
    for (;;) {
        while ((inb(0x64) & 1) == 0) { }    // wait for a byte
        sc = inb(0x60);
        if (sc < 128) {                     // press (not release)
            char ch;
            ch = g_keymap[sc];
            if (ch != 0) return ch;
        }
    }
}

void kernel_init() {
    serial_init();
    vga_clear();
    kbd_init();
}
