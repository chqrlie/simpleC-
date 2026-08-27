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

.PHONY: all run test shot gfx gfxrun gfxtest gfxshot \
        gshell gshellrun gshelltest gshellshot clean

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

# Headless self-test: type "help", "bits" and "echo hi" via emulated
# keystrokes and confirm the shell (and its printf) responded on serial.
test: kernel.elf
	@rm -f serial.log
	@( sleep 2; for k in h e l p ret; do echo "sendkey $$k"; sleep 0.22; done; \
	   sleep 0.3; for k in b i t s ret; do echo "sendkey $$k"; sleep 0.22; done; \
	   sleep 0.3; for k in e c h o spc h i ret; do echo "sendkey $$k"; sleep 0.22; done; \
	   sleep 0.6; echo quit ) | \
	 timeout 25 $(QEMU) $(QFLAGS) -display none -serial file:serial.log -monitor stdio >/dev/null 2>&1; \
	 echo "---- serial output ----"; cat serial.log; echo "-----------------------"; \
	 tr -d '\r' < serial.log > serial.clean; \
	 grep -q "Commands: help, clear, bits" serial.clean && grep -q "0xf & 0x5 = 5" serial.clean \
	   && grep -qx "hi" serial.clean \
	   && echo "PASS: keyboard + shell + bitwise + printf all working" \
	   || { echo "FAIL"; exit 1; }

# Boot, type into it, and save a VGA screenshot to vga.png.
shot: kernel.elf
	@rm -f vga.ppm vga.png
	@( sleep 2; for k in h e l p ret; do echo "sendkey $$k"; sleep 0.22; done; \
	   sleep 0.3; for k in v e r ret; do echo "sendkey $$k"; sleep 0.22; done; \
	   sleep 0.3; for k in b i t s ret; do echo "sendkey $$k"; sleep 0.22; done; \
	   sleep 0.3; for k in e c h o spc h i ret; do echo "sendkey $$k"; sleep 0.22; done; \
	   sleep 0.5; echo "screendump vga.ppm"; sleep 0.8; echo quit ) | \
	 timeout 25 $(QEMU) $(QFLAGS) -display none -serial file:serial.log -monitor stdio >/dev/null 2>&1
	@convert vga.ppm vga.png && echo "wrote vga.png"

# ---------------------------------------------------------------------------
# Graphics build: the same boot path, but the 64-bit kernel is gfx.c, which
# programs a real graphics mode and draws into a linear framebuffer.
# ---------------------------------------------------------------------------
gfx.s: gfx.c nano-kernel.h nano-fb.h $(NANO)
	$(NANO) --kernel gfx.c gfx.s

gfx64.bin: boot64.s gfx.s kernel64.ld
	as --64 boot64.s -o boot64.o
	as --64 gfx.s    -o gfx.o
	ld -T kernel64.ld -o gfx64.elf boot64.o gfx.o
	objcopy -O binary gfx64.elf gfx64.bin

# .incbin needs a literal file name, so each image gets its own one-line
# wrapper. They used to share kernel64.bin, which meant `make test` and
# `make gshelltest` quietly overwrote each other's kernel.
blob-gfx.s:
	@printf '.section .blob, "a"\n.incbin "gfx64.bin"\n' > $@

gfx.elf: boot32.s blob-gfx.s gfx64.bin final.ld
	as --32 boot32.s   -o boot32.o
	as --32 blob-gfx.s -o blob-gfx.o
	ld -m elf_i386 -T final.ld -o gfx.elf boot32.o blob-gfx.o

gfx: gfx.elf

gfxrun: gfx.elf
	$(QEMU) -kernel gfx.elf -no-reboot

# Headless: boot, read the serial commentary, and require the framebuffer to
# have been found and the read-back to have come back correct.
gfxtest: gfx.elf
	@rm -f gfx-serial.log
	@( sleep 3; echo quit ) | \
	 timeout 25 $(QEMU) -kernel gfx.elf -no-reboot -display none \
	   -serial file:gfx-serial.log -monitor stdio >/dev/null 2>&1; \
	 echo "---- serial output ----"; cat gfx-serial.log; echo "-----------------------"; \
	 tr -d '\r' < gfx-serial.log > gfx-serial.clean; \
	 grep -q "FB: 1024x768 at 32 bpp" gfx-serial.clean \
	   && grep -q "readback 0x10203 (want 0x10203)" gfx-serial.clean \
	   && grep -q "drawing complete" gfx-serial.clean \
	   && echo "PASS: mode set, framebuffer located, pixels read back correctly" \
	   || { echo "FAIL"; exit 1; }

gfxshot: gfx.elf
	@rm -f gfx.ppm gfx.png
	@( sleep 3; echo "screendump gfx.ppm"; sleep 1; echo quit ) | \
	 timeout 25 $(QEMU) -kernel gfx.elf -no-reboot -display none \
	   -serial file:gfx-serial.log -monitor stdio >/dev/null 2>&1
	@convert gfx.ppm gfx.png && echo "wrote gfx.png"

# ---------------------------------------------------------------------------
# The graphics shell: same keyboard-driven shell, drawn into the framebuffer,
# with commands that draw.
# ---------------------------------------------------------------------------
gfxshell.s: gfxshell.c nano-kernel.h nano-fb.h nano-font.h $(NANO)
	$(NANO) --kernel gfxshell.c gfxshell.s

gshell64.bin: boot64.s gfxshell.s kernel64.ld
	as --64 boot64.s   -o boot64.o
	as --64 gfxshell.s -o gfxshell.o
	ld -T kernel64.ld -o gshell64.elf boot64.o gfxshell.o
	objcopy -O binary gshell64.elf gshell64.bin

blob-gshell.s:
	@printf '.section .blob, "a"\n.incbin "gshell64.bin"\n' > $@

gshell.elf: boot32.s blob-gshell.s gshell64.bin final.ld
	as --32 boot32.s      -o boot32.o
	as --32 blob-gshell.s -o blob-gshell.o
	ld -m elf_i386 -T final.ld -o gshell.elf boot32.o blob-gshell.o

gshell: gshell.elf

gshellrun: gshell.elf
	$(QEMU) -kernel gshell.elf -no-reboot

# Headless: type real keystrokes at it and check what came back on serial.
gshelltest: gshell.elf
	@rm -f gshell-serial.log
	@( sleep 3; for k in f b i n f o ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in p c i ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in b a r s ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in e c h o spc h i ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.8; echo quit ) | \
	 timeout 45 $(QEMU) -kernel gshell.elf -no-reboot -display none \
	   -serial file:gshell-serial.log -monitor stdio >/dev/null 2>&1; \
	 echo "---- serial output ----"; cat gshell-serial.log; echo "-----------------------"; \
	 tr -d '\r' < gshell-serial.log > gshell-serial.clean; \
	 grep -q "resolution 1024x768 at 32 bpp" gshell-serial.clean \
	   && grep -q "class 3" gshell-serial.clean \
	   && grep -q "drew colour bars" gshell-serial.clean \
	   && grep -qx "hi" gshell-serial.clean \
	   && echo "PASS: graphics shell, keyboard, PCI scan and drawing all working" \
	   || { echo "FAIL"; exit 1; }

# Boot it, type a few commands, and photograph the screen.
gshellshot: gshell.elf
	@rm -f gshell.ppm gshell.png
	@( sleep 3; for k in f b i n f o ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in p c i ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in d e m o ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 1.5; for k in e c h o spc h e l l o ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 1; echo "screendump gshell.ppm"; sleep 1; echo quit ) | \
	 timeout 45 $(QEMU) -kernel gshell.elf -no-reboot -display none \
	   -serial file:gshell-serial.log -monitor stdio >/dev/null 2>&1
	@convert gshell.ppm gshell.png && echo "wrote gshell.png"

clean:
	rm -f *.o shell.s gfx.s gfxshell.s blob-gfx.s blob-gshell.s \
	      kernel64.elf kernel64.bin kernel.elf \
	      gfx64.elf gfx64.bin gfx.elf \
	      gshell64.elf gshell64.bin gshell.elf \
	      serial.log serial.clean gfx-serial.log gfx-serial.clean \
	      gshell-serial.log gshell-serial.clean \
	      vga.ppm gfx.ppm gshell.ppm
