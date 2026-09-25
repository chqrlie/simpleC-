// shell.c — the "mini-OS" interactive shell, compiled by nano_cc and booted
// bare-metal under QEMU.  Proves keyboard_getchar() is a real hardware read,
// that the bitwise operators generate correct code, and that variadic
// functions work — printf() below is compiled by nano_cc.
//
//   ../nano_cc --kernel shell.c shell.s
//   as --64 shell.s -o shell.o ; as --64 boot64.s -o boot64.o
//   ... (see Makefile)
//   qemu-system-x86_64 -kernel kernel.elf
#include "nano-kernel.h"

char cmd_buf[256];
int  buf_pos;

int starts_with(char *s, char *p) {
    while (*p) {
        if (*s != *p) return 0;
        s = s + 1; p = p + 1;
    }
    return 1;
}

int main() {
    kernel_init();
    printf("Welcome to %s (v%d.%d)\n", "simpleC++ Mini-OS", 1, 0);
    printf("Type 'help' for commands.\n");

    for (;;) {
        printf("shell> ");
        buf_pos = 0;

        // read one line from the keyboard
        for (;;) {
            char c;
            c = keyboard_getchar();
            if (c == '\n') { putc('\n'); break; }
            if (c == '\b') {
                if (buf_pos > 0) { buf_pos = buf_pos - 1; puts("\b \b"); }
            } else {
                cmd_buf[buf_pos] = c;
                buf_pos = buf_pos + 1;
                putc(c);                       // echo the key
            }
        }
        cmd_buf[buf_pos] = 0;

        // dispatch
        if (strcmp(cmd_buf, "help") == 0) {
            printf("Commands: help, clear, bits, ver, echo <text>\n");
        } else if (strcmp(cmd_buf, "clear") == 0) {
            vga_clear();
        } else if (strcmp(cmd_buf, "ver") == 0) {
            printf("nano_cc mini-OS, %d-bit long mode, %d cmds\n", 64, 5);
        } else if (strcmp(cmd_buf, "bits") == 0) {
            // exercises the bitwise-operator codegen, printed with printf
            int flags;
            int mask;
            flags = 0x0F;
            mask  = 0x05;
            printf("  0x%x & 0x%x = %d\n", flags, mask, flags & mask);
            printf("  0x%x | 0x30 = %d\n", flags, flags | 0x30);
            printf("  0xF0 ^ 0x0F = %d\n", 0xF0 ^ 0x0F);
            printf("  1 << 4      = %d\n", 1 << 4);
            printf("  255 >> 2    = %d\n", 255 >> 2);
        } else if (starts_with(cmd_buf, "echo ")) {
            printf("%s\n", cmd_buf + 5);
        } else if (cmd_buf[0] != 0) {
            printf("Unknown command: %s\n", cmd_buf);
        }
    }
    return 0;
}
