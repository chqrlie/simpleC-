# kernel/Makefile — build and boot the nano_cc "mini-OS" bare-metal.
#
#   make          # build kernel.elf (a 32-bit Multiboot image)
#   make run      # boot it in QEMU with a graphical VGA window
#   make test     # headless: drive real keystrokes, check the serial output
#   make shot     # boot, type into it, save a VGA screenshot (vga.png)
#
# The shell is compiled by nano_cc (../nano_cc). boot32.s brings the CPU into
# 64-bit long mode and jumps into the 64-bit kernel embedded at 0x101000.

NANO   := ../nano_cc
QEMU   := qemu-system-x86_64
QFLAGS := -kernel kernel.elf -no-reboot

.PHONY: all run test shot clean

all: kernel.elf

# nano_cc lives one level up; build it if needed.
$(NANO):
	$(MAKE) -C .. nano_cc

# 1) compile the shell with our own compiler (kernel mode: no _start, .data globals)
shell.s: shell.c nano-kernel.h $(NANO)
	$(NANO) --kernel shell.c shell.s

# 2) assemble + link the 64-bit kernel at 0x101000 (long_mode_start first)
kernel64.bin: boot64.s shell.s kernel64.ld
	as --64 boot64.s -o boot64.o
	as --64 shell.s  -o shell.o
	ld -T kernel64.ld -o kernel64.elf boot64.o shell.o
	objcopy -O binary kernel64.elf kernel64.bin

# 3) wrap it in a 32-bit Multiboot stub QEMU can load
kernel.elf: boot32.s blob.s kernel64.bin final.ld
	as --32 boot32.s -o boot32.o
	as --32 blob.s   -o blob.o
	ld -m elf_i386 -T final.ld -o kernel.elf boot32.o blob.o
	@grub-file --is-x86-multiboot kernel.elf && echo "kernel.elf: valid Multiboot image" || true

# Interactive boot with a VGA window (needs a display).
run: kernel.elf
	$(QEMU) $(QFLAGS)

# Headless self-test: type "help" then "bits" via emulated keystrokes and
# confirm the shell responded on the serial port.
test: kernel.elf
	@rm -f serial.log
	@( sleep 2; for k in h e l p ret; do echo "sendkey $$k"; sleep 0.25; done; \
	   sleep 0.5; for k in b i t s ret; do echo "sendkey $$k"; sleep 0.25; done; \
	   sleep 0.6; echo quit ) | \
	 timeout 20 $(QEMU) $(QFLAGS) -display none -serial file:serial.log -monitor stdio >/dev/null 2>&1; \
	 echo "---- serial output ----"; cat serial.log; echo "-----------------------"; \
	 grep -q "Commands: help, clear, bits" serial.log && grep -q "0x0F & 0x05 = 5" serial.log \
	   && echo "PASS: keyboard + shell + bitwise all working" \
	   || { echo "FAIL"; exit 1; }

# Boot, type into it, and save a VGA screenshot to vga.png.
shot: kernel.elf
	@rm -f vga.ppm vga.png
	@( sleep 2; for k in h e l p ret; do echo "sendkey $$k"; sleep 0.25; done; \
	   sleep 0.4; for k in b i t s ret; do echo "sendkey $$k"; sleep 0.25; done; \
	   sleep 0.5; echo "screendump vga.ppm"; sleep 0.8; echo quit ) | \
	 timeout 20 $(QEMU) $(QFLAGS) -display none -serial file:serial.log -monitor stdio >/dev/null 2>&1
	@convert vga.ppm vga.png && echo "wrote vga.png"

clean:
	rm -f *.o shell.s kernel64.elf kernel64.bin kernel.elf serial.log vga.ppm vga.png
