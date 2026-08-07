// shell.c — the "mini-OS" interactive shell, compiled by nano_cc and booted
// bare-metal under QEMU.  Proves keyboard_getchar() is a real hardware read
// and that the bitwise operators generate correct code.
//
//   ../nano_cc --kernel shell.c shell.s
//   as --64 shell.s -o shell.o ; as --64 boot.s -o boot.o
//   ld -T kernel.ld -o kernel.elf boot.o shell.o
//   qemu-system-x86_64 -kernel kernel.elf
#include "nano-kernel.h"

char cmd_buf[256];
int  buf_pos;

int main() {
    kernel_init();
    puts("Welcome to simpleC++ Mini-OS!\n");
    puts("Type 'help' for commands.\n");

    for (;;) {
        puts("shell> ");
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
            puts("Commands: help, clear, bits\n");
        } else if (strcmp(cmd_buf, "clear") == 0) {
            vga_clear();
        } else if (strcmp(cmd_buf, "bits") == 0) {
            // exercises the bitwise-operator codegen at runtime
            int flags;
            int mask;
            flags = 0x0F;
            mask  = 0x05;
            puts("0x0F & 0x05 = "); print_int(flags & mask);  putc('\n');
            puts("0x0F | 0x30 = "); print_int(flags | 0x30);  putc('\n');
            puts("0xF0 ^ 0x0F = "); print_int(0xF0 ^ 0x0F);   putc('\n');
            puts("1 << 4      = "); print_int(1 << 4);        putc('\n');
            puts("255 >> 2    = "); print_int(255 >> 2);      putc('\n');
        } else if (cmd_buf[0] != 0) {
            puts("Unknown command: ");
            puts(cmd_buf);
            putc('\n');
        }
    }
    return 0;
}
