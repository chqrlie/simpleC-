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
        gshell gshellrun gshelltest gshellshot \
        intr intrrun intrtest acpi acpirun acpitest \
        mm mmrun mmtest threads threadsrun threadstest \
        srv srvrun srvtest fs fsrun fstest clean

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
gfxshell.s: gfxshell.c nano-kernel.h nano-fb.h nano-font.h nano-int.h \
            nano-acpi.h nano-mm.h nano-thread.h nano-srv.h nano-fs.h $(NANO)
	$(NANO) --kernel gfxshell.c gfxshell.s

gshell64.bin: boot64.s isr.s gfxshell.s kernel64.ld
	as --64 boot64.s   -o boot64.o
	as --64 isr.s      -o isr.o
	as --64 gfxshell.s -o gfxshell.o
	ld -T kernel64.ld -o gshell64.elf boot64.o isr.o gfxshell.o
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
	   sleep 0.4; for k in a c p i ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in i d l e ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 1.8; for k in m e m ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in h e a p t e s t ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.6; for k in m a p t e s t ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.5; for k in s p a w n ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in s p a w n ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in p s ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in s r v ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in c r a s h ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 1.2; for k in l s ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in c a t spc slash d o c slash r e a d m e ret; \
	     do echo "sendkey $$k"; sleep 0.15; done; \
	   sleep 0.5; for k in c p spc slash d o c slash r e a d m e spc slash r 2 ret; \
	     do echo "sendkey $$k"; sleep 0.15; done; \
	   sleep 0.5; for k in m v spc slash r 2 spc slash r 3 ret; \
	     do echo "sendkey $$k"; sleep 0.15; done; \
	   sleep 0.5; for k in l s ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in d f ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.5; for k in b a r s ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in e c h o spc h i ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in f a u l t ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 1.2; echo quit ) | \
	 timeout 60 $(QEMU) -kernel gshell.elf -no-reboot -display none \
	   -serial file:gshell-serial.log -monitor stdio >/dev/null 2>&1; \
	 echo "---- serial output ----"; cat gshell-serial.log; echo "-----------------------"; \
	 tr -d '\r' < gshell-serial.log > gshell-serial.clean; \
	 grep -q "resolution 1024x768 at 32 bpp" gshell-serial.clean \
	   && grep -q "class 3" gshell-serial.clean \
	   && grep -q "FACP" gshell-serial.clean \
	   && grep -q "a spinning core would show" gshell-serial.clean \
	   && grep -q "drew colour bars" gshell-serial.clean \
	   && grep -q "contents intact" gshell-serial.clean \
	   && grep -q "same block reused" gshell-serial.clean \
	   && grep -q "wrote through it" gshell-serial.clean \
	   && grep -q "the shell stays usable" gshell-serial.clean \
	   && grep -q "context switches so far" gshell-serial.clean \
	   && grep -q "this shell never stopped" gshell-serial.clean \
	   && grep -q "everything here was compiled by nano_cc" gshell-serial.clean \
	   && grep -q "^copied 22[0-9] bytes" gshell-serial.clean \
	   && grep -q "^r3  22[0-9]$$" gshell-serial.clean \
	   && grep -q "blocks of 512 bytes" gshell-serial.clean \
	   && grep -qx "hi" gshell-serial.clean \
	   && grep -q "EXCEPTION 14: page fault" gshell-serial.clean \
	   && echo "PASS: graphics, keyboard, PCI, ACPI, idle, memory and faults" \
	   || { echo "FAIL"; exit 1; }

# Boot it, type a few commands, and photograph the screen.
gshellshot: gshell.elf
	@rm -f gshell.ppm gshell.png
	@( sleep 3; for k in a c p i ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.5; for k in i d l e ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 2; for k in u p t i m e ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.5; for k in m e m ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.5; for k in s p a w n ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.3; for k in s p a w n ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.3; for k in s p a w n ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.3; for k in l s ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in c a t spc slash d o c slash r e a d m e ret; \
	     do echo "sendkey $$k"; sleep 0.15; done; \
	   sleep 0.6; for k in c p spc slash d o c slash r e a d m e spc slash n o t e s ret; \
	     do echo "sendkey $$k"; sleep 0.15; done; \
	   sleep 0.6; for k in l s ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in d f ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 1.2; echo "screendump gshell.ppm"; sleep 1; echo quit ) | \
	 timeout 45 $(QEMU) -kernel gshell.elf -no-reboot -display none \
	   -serial file:gshell-serial.log -monitor stdio >/dev/null 2>&1
	@convert gshell.ppm gshell.png && echo "wrote gshell.png"

# ---------------------------------------------------------------------------
# Interrupts: IDT, exception reporting, PIC remap, PIT timer, IRQ keyboard and
# a real idle loop. intr.c is the headless bring-up test.
# ---------------------------------------------------------------------------
intr.s: intr.c nano-kernel.h nano-int.h $(NANO)
	$(NANO) --kernel intr.c intr.s

intr64.bin: boot64.s isr.s intr.s kernel64.ld
	as --64 boot64.s -o boot64.o
	as --64 isr.s    -o isr.o
	as --64 intr.s   -o intr.o
	ld -T kernel64.ld -o intr64.elf boot64.o isr.o intr.o
	objcopy -O binary intr64.elf intr64.bin

blob-intr.s:
	@printf '.section .blob, "a"\n.incbin "intr64.bin"\n' > $@

intr.elf: boot32.s blob-intr.s intr64.bin final.ld
	as --32 boot32.s    -o boot32.o
	as --32 blob-intr.s -o blob-intr.o
	ld -m elf_i386 -T final.ld -o intr.elf boot32.o blob-intr.o

intr: intr.elf

intrrun: intr.elf
	$(QEMU) -kernel intr.elf -no-reboot

# Every claim here is checked: the timer really ticked, the core really halted
# rather than spun, and a deliberate page fault produced a report instead of a
# silent reboot.
intrtest: intr.elf
	@rm -f intr-serial.log
	@( sleep 6; echo quit ) | \
	 timeout 40 $(QEMU) -kernel intr.elf -no-reboot -display none \
	   -serial file:intr-serial.log -monitor stdio >/dev/null 2>&1; \
	 echo "---- serial output ----"; cat intr-serial.log; echo "-----------------------"; \
	 tr -d '\r' < intr-serial.log > intr-serial.clean; \
	 grep -q "timer ok" intr-serial.clean \
	   && grep -q "idle ok" intr-serial.clean \
	   && grep -q "EXCEPTION 14: page fault" intr-serial.clean \
	   && grep -q "page not present, on a write, from kernel mode" intr-serial.clean \
	   && ! grep -q "UNREACHABLE" intr-serial.clean \
	   && echo "PASS: timer ticks, core idles, faults are reported not silent" \
	   || { echo "FAIL"; exit 1; }

# ---------------------------------------------------------------------------
# ACPI: find the firmware tables, read the FADT and MADT, and idle the CPU.
# ---------------------------------------------------------------------------
acpi.s: acpi.c nano-kernel.h nano-int.h nano-acpi.h $(NANO)
	$(NANO) --kernel acpi.c acpi.s

acpi64.bin: boot64.s isr.s acpi.s kernel64.ld
	as --64 boot64.s -o boot64.o
	as --64 isr.s    -o isr.o
	as --64 acpi.s   -o acpi.o
	ld -T kernel64.ld -o acpi64.elf boot64.o isr.o acpi.o
	objcopy -O binary acpi64.elf acpi64.bin

blob-acpi.s:
	@printf '.section .blob, "a"\n.incbin "acpi64.bin"\n' > $@

acpi.elf: boot32.s blob-acpi.s acpi64.bin final.ld
	as --32 boot32.s    -o boot32.o
	as --32 blob-acpi.s -o blob-acpi.o
	ld -m elf_i386 -T final.ld -o acpi.elf boot32.o blob-acpi.o

acpi: acpi.elf

acpirun: acpi.elf
	$(QEMU) -kernel acpi.elf -no-reboot

acpitest: acpi.elf
	@rm -f acpi-serial.log
	@( sleep 8; echo quit ) | \
	 timeout 40 $(QEMU) -kernel acpi.elf -no-reboot -display none \
	   -serial file:acpi-serial.log -monitor stdio >/dev/null 2>&1; \
	 echo "---- serial output ----"; cat acpi-serial.log; echo "-----------------------"; \
	 tr -d '\r' < acpi-serial.log > acpi-serial.clean; \
	 grep -q "FACP" acpi-serial.clean \
	   && grep -q "pm timer ok" acpi-serial.clean \
	   && grep -q "idle ok" acpi-serial.clean \
	   && grep -q "acpi bring-up complete" acpi-serial.clean \
	   && echo "PASS: ACPI tables read, PM timer runs, core idles" \
	   || { echo "FAIL"; exit 1; }

# ---------------------------------------------------------------------------
# Memory: the Multiboot map, a physical frame allocator, 4 KiB paging over the
# boot-time 2 MiB identity map, and a kernel heap that really frees.
# ---------------------------------------------------------------------------
mm.s: mm.c nano-kernel.h nano-int.h nano-mm.h $(NANO)
	$(NANO) --kernel mm.c mm.s

mm64.bin: boot64.s isr.s mm.s kernel64.ld
	as --64 boot64.s -o boot64.o
	as --64 isr.s    -o isr.o
	as --64 mm.s     -o mm.o
	ld -T kernel64.ld -o mm64.elf boot64.o isr.o mm.o
	objcopy -O binary mm64.elf mm64.bin

blob-mm.s:
	@printf '.section .blob, "a"\n.incbin "mm64.bin"\n' > $@

mm.elf: boot32.s blob-mm.s mm64.bin final.ld
	as --32 boot32.s  -o boot32.o
	as --32 blob-mm.s -o blob-mm.o
	ld -m elf_i386 -T final.ld -o mm.elf boot32.o blob-mm.o

mm: mm.elf

mmrun: mm.elf
	$(QEMU) -kernel mm.elf -no-reboot

mmtest: mm.elf
	@rm -f mm-serial.log
	@( sleep 6; echo quit ) | \
	 timeout 40 $(QEMU) -kernel mm.elf -no-reboot -display none \
	   -serial file:mm-serial.log -monitor stdio >/dev/null 2>&1; \
	 echo "---- serial output ----"; cat mm-serial.log; echo "-----------------------"; \
	 tr -d '\r' < mm-serial.log > mm-serial.clean; \
	 grep -q "distinct ok" mm-serial.clean \
	   && grep -q "reservation ok" mm-serial.clean \
	   && grep -q "map ok" mm-serial.clean \
	   && grep -q "write-through ok" mm-serial.clean \
	   && grep -q "split ok" mm-serial.clean \
	   && grep -q "reuse ok" mm-serial.clean \
	   && grep -q "coalesce ok" mm-serial.clean \
	   && grep -q "heap growth ok" mm-serial.clean \
	   && grep -q "unmap ok" mm-serial.clean \
	   && grep -q "EXCEPTION 14: page fault" mm-serial.clean \
	   && ! grep -q "UNREACHABLE" mm-serial.clean \
	   && echo "PASS: frames, paging, heap, and unmapping really unmaps" \
	   || { echo "FAIL"; exit 1; }

# ---------------------------------------------------------------------------
# Threads: a preemptive round-robin scheduler, real context switching on the
# timer, locking, and a heap that survives being used from several threads.
# ---------------------------------------------------------------------------
threads.s: threads.c nano-kernel.h nano-mm.h nano-thread.h nano-int.h $(NANO)
	$(NANO) --kernel threads.c threads.s

threads64.bin: boot64.s isr.s threads.s kernel64.ld
	as --64 boot64.s  -o boot64.o
	as --64 isr.s     -o isr.o
	as --64 threads.s -o threads.o
	ld -T kernel64.ld -o threads64.elf boot64.o isr.o threads.o
	objcopy -O binary threads64.elf threads64.bin

blob-threads.s:
	@printf '.section .blob, "a"\n.incbin "threads64.bin"\n' > $@

threads.elf: boot32.s blob-threads.s threads64.bin final.ld
	as --32 boot32.s       -o boot32.o
	as --32 blob-threads.s -o blob-threads.o
	ld -m elf_i386 -T final.ld -o threads.elf boot32.o blob-threads.o

threads: threads.elf

threadsrun: threads.elf
	$(QEMU) -kernel threads.elf -no-reboot

threadstest: threads.elf
	@rm -f threads-serial.log
	@( sleep 10; echo quit ) | \
	 timeout 60 $(QEMU) -kernel threads.elf -no-reboot -display none \
	   -serial file:threads-serial.log -monitor stdio >/dev/null 2>&1; \
	 echo "---- serial output ----"; cat threads-serial.log; echo "-----------------------"; \
	 tr -d '\r' < threads-serial.log > threads-serial.clean; \
	 grep -q "interleave ok" threads-serial.clean \
	   && grep -q "join ok" threads-serial.clean \
	   && grep -q "race ok" threads-serial.clean \
	   && grep -q "mutex ok" threads-serial.clean \
	   && grep -q "heap ok" threads-serial.clean \
	   && grep -q "preemption ok" threads-serial.clean \
	   && grep -q "thread bring-up complete" threads-serial.clean \
	   && echo "PASS: preemption, joins, a real race, a mutex that stops it" \
	   || { echo "FAIL"; exit 1; }

# ---------------------------------------------------------------------------
# Servers: a registry, heartbeats, a supervisor, and faults that kill only the
# server that caused them.
# ---------------------------------------------------------------------------
srv.s: srv.c nano-kernel.h nano-mm.h nano-thread.h nano-int.h nano-srv.h $(NANO)
	$(NANO) --kernel srv.c srv.s

srv64.bin: boot64.s isr.s srv.s kernel64.ld
	as --64 boot64.s -o boot64.o
	as --64 isr.s    -o isr.o
	as --64 srv.s    -o srv.o
	ld -T kernel64.ld -o srv64.elf boot64.o isr.o srv.o
	objcopy -O binary srv64.elf srv64.bin

blob-srv.s:
	@printf '.section .blob, "a"\n.incbin "srv64.bin"\n' > $@

srv.elf: boot32.s blob-srv.s srv64.bin final.ld
	as --32 boot32.s   -o boot32.o
	as --32 blob-srv.s -o blob-srv.o
	ld -m elf_i386 -T final.ld -o srv.elf boot32.o blob-srv.o

srv: srv.elf

srvrun: srv.elf
	$(QEMU) -kernel srv.elf -no-reboot

srvtest: srv.elf
	@rm -f srv-serial.log
	@( sleep 14; echo quit ) | \
	 timeout 60 $(QEMU) -kernel srv.elf -no-reboot -display none \
	   -serial file:srv-serial.log -monitor stdio >/dev/null 2>&1; \
	 echo "---- serial output ----"; cat srv-serial.log; echo "-----------------------"; \
	 tr -d '\r' < srv-serial.log > srv-serial.clean; \
	 grep -q "crash recovery ok" srv-serial.clean \
	   && grep -q "isolation ok" srv-serial.clean \
	   && grep -q "hang detected" srv-serial.clean \
	   && grep -q "hang recovery ok" srv-serial.clean \
	   && grep -q "repeat recovery ok" srv-serial.clean \
	   && grep -q "healthy server still counting" srv-serial.clean \
	   && grep -q "server bring-up complete" srv-serial.clean \
	   && echo "PASS: crashes and hangs both recovered, the rest kept running" \
	   || { echo "FAIL"; exit 1; }

# ---------------------------------------------------------------------------
# Filesystem: a RAM disk, a Unix-shaped layout (superblock, bitmaps, inodes,
# directory entries), and the operations the shell tools need.
# ---------------------------------------------------------------------------
fs.s: fs.c nano-kernel.h nano-mm.h nano-thread.h nano-int.h nano-fs.h $(NANO)
	$(NANO) --kernel fs.c fs.s

fs64.bin: boot64.s isr.s fs.s kernel64.ld
	as --64 boot64.s -o boot64.o
	as --64 isr.s    -o isr.o
	as --64 fs.s     -o fs.o
	ld -T kernel64.ld -o fs64.elf boot64.o isr.o fs.o
	objcopy -O binary fs64.elf fs64.bin

blob-fs.s:
	@printf '.section .blob, "a"\n.incbin "fs64.bin"\n' > $@

fs.elf: boot32.s blob-fs.s fs64.bin final.ld
	as --32 boot32.s  -o boot32.o
	as --32 blob-fs.s -o blob-fs.o
	ld -m elf_i386 -T final.ld -o fs.elf boot32.o blob-fs.o

fs: fs.elf

fsrun: fs.elf
	$(QEMU) -kernel fs.elf -no-reboot

fstest: fs.elf
	@rm -f fs-serial.log
	@( sleep 10; echo quit ) | \
	 timeout 60 $(QEMU) -kernel fs.elf -no-reboot -display none \
	   -serial file:fs-serial.log -monitor stdio >/dev/null 2>&1; \
	 echo "---- serial output ----"; cat fs-serial.log; echo "-----------------------"; \
	 tr -d '\r' < fs-serial.log > fs-serial.clean; \
	 grep -q "read/write ok" fs-serial.clean \
	   && grep -q "indirect block ok" fs-serial.clean \
	   && grep -q "short read at EOF ok" fs-serial.clean \
	   && grep -q "nested path lookup ok" fs-serial.clean \
	   && grep -q "unlink freed the blocks ok" fs-serial.clean \
	   && grep -q "reused space ok" fs-serial.clean \
	   && grep -q "rename moved the name, not the data" fs-serial.clean \
	   && grep -q "refused to remove a non-empty directory ok" fs-serial.clean \
	   && grep -q "concurrent fs ok" fs-serial.clean \
	   && grep -q "each file kept its own data ok" fs-serial.clean \
	   && echo "PASS: files, directories, indirect blocks, rename, concurrency" \
	   || { echo "FAIL"; exit 1; }

clean:
	rm -f *.o shell.s gfx.s gfxshell.s intr.s acpi.s mm.s threads.s srv.s fs.s \
	      blob-gfx.s blob-gshell.s blob-intr.s blob-acpi.s blob-mm.s \
	      blob-threads.s blob-srv.s blob-fs.s \
	      kernel64.elf kernel64.bin kernel.elf \
	      gfx64.elf gfx64.bin gfx.elf \
	      gshell64.elf gshell64.bin gshell.elf \
	      serial.log serial.clean gfx-serial.log gfx-serial.clean \
	      gshell-serial.log gshell-serial.clean \
	      intr64.elf intr64.bin intr.elf intr-serial.log intr-serial.clean \
	      acpi64.elf acpi64.bin acpi.elf acpi-serial.log acpi-serial.clean \
	      mm64.elf mm64.bin mm.elf mm-serial.log mm-serial.clean \
	      threads64.elf threads64.bin threads.elf \
	      srv64.elf srv64.bin srv.elf srv-serial.log srv-serial.clean \
	      fs64.elf fs64.bin fs.elf fs-serial.log fs-serial.clean \
	      threads-serial.log threads-serial.clean \
	      vga.ppm gfx.ppm gshell.ppm
