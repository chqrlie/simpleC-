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
void putc(int c) {
    if (c == '\n') serial_put('\r');     // CR so serial terminals advance
    serial_put(c);
    vga_put(c);
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
