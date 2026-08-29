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

# The user programs. Defined here rather than next to their own rules further
# down: make expands a rule's prerequisites when it READS the rule, and this is
# a simply-expanded variable, so a definition below the first use expands to
# nothing. The symptom was gshell64.bin assembling progs.s before the binaries
# it .incbin's had been built -- "file not found: user/hello.elf" on a clean
# tree, and a perfectly good build on the second attempt.
UPROGS := user/hello.elf user/twin.elf user/wild.elf user/cc.elf user/as.elf
QFLAGS := -kernel kernel.elf -no-reboot

.PHONY: all run test shot gfx gfxrun gfxtest gfxshot \
        gshell gshellrun gshelltest gshellshot \
        intr intrrun intrtest acpi acpirun acpitest \
        mm mmrun mmtest threads threadsrun threadstest \
        srv srvrun srvtest fs fsrun fstest \
        progs elf elfrun elftest cc ccrun cctest check-miniasm \
        chain chainrun chaintest clean

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
            nano-acpi.h nano-mm.h nano-thread.h nano-srv.h nano-fs.h nano-proc.h $(NANO)
	$(NANO) --kernel gfxshell.c gfxshell.s

gshell64.bin: boot64.s isr.s gfxshell.s progs.s $(UPROGS) src/demo.c src/util.h src/prog.c kernel64.ld
	as --64 boot64.s   -o boot64.o
	as --64 isr.s      -o isr.o
	as --64 gfxshell.s -o gfxshell.o
	as --64 progs.s    -o progs.o
	ld -T kernel64.ld -o gshell64.elf boot64.o isr.o gfxshell.o progs.o
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
	   sleep 0.4; for k in e x e c spc slash b i n slash h e l l o ret; \
	     do echo "sendkey $$k"; sleep 0.15; done; \
	   sleep 1.2; for k in r u n spc slash b i n slash t w i n spc 1 ret; \
	     do echo "sendkey $$k"; sleep 0.15; done; \
	   sleep 0.3; for k in r u n spc slash b i n slash t w i n spc 2 ret; \
	     do echo "sendkey $$k"; sleep 0.15; done; \
	   sleep 0.4; for k in p r o c s ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 2.5; for k in e x e c spc slash b i n slash w i l d spc 1 ret; \
	     do echo "sendkey $$k"; sleep 0.15; done; \
	   sleep 1.2; for k in p r o c s ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in c a t spc slash h e l l o dot o u t ret; \
	     do echo "sendkey $$k"; sleep 0.15; done; \
	   sleep 0.4; for k in e c h o spc h i ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 0.4; for k in f a u l t ret; do echo "sendkey $$k"; sleep 0.18; done; \
	   sleep 1.2; echo quit ) | \
	 timeout 90 $(QEMU) -kernel gshell.elf -no-reboot -display none \
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
	   && grep -q "of them across address spaces" gshell-serial.clean \
	   && grep -q "this shell never stopped" gshell-serial.clean \
	   && grep -q "everything here was compiled by nano_cc" gshell-serial.clean \
	   && grep -q "^copied 22[0-9] bytes" gshell-serial.clean \
	   && grep -q "^r3  22[0-9]$$" gshell-serial.clean \
	   && grep -q "blocks of 512 bytes" gshell-serial.clean \
	   && grep -qx "hi" gshell-serial.clean \
	   && grep -q "EXCEPTION 14: page fault" gshell-serial.clean \
	   && grep -q "hello from a user program" gshell-serial.clean \
	   && grep -q "exited with 7" gshell-serial.clean \
	   && grep -q "written by a user process" gshell-serial.clean \
	   && grep -q "twin 1: 0 of" gshell-serial.clean \
	   && grep -q "twin 2: 0 of" gshell-serial.clean \
	   && grep -q "killed by a fault" gshell-serial.clean \
	   && echo "PASS: graphics, keyboard, PCI, ACPI, idle, memory, faults, and processes" \
	   || { echo "FAIL"; exit 1; }

# Boot it, type a few commands, and photograph the screen.
#
# The sequence is the whole toolchain: compile a C file, assemble the result,
# and run what comes out. Every one of those three steps is a program on the
# RAM disk, in its own address space.
gshellshot: gshell.elf
	@rm -f gshell.ppm gshell.png
	@( sleep 3; for k in c l e a r ret; do echo "sendkey $$k"; sleep 0.15; done; \
	   sleep 0.4; for k in l s spc slash b i n ret; do echo "sendkey $$k"; sleep 0.15; done; \
	   sleep 0.5; for k in c d spc slash s r c ret; do echo "sendkey $$k"; sleep 0.15; done; \
	   sleep 0.5; for k in c c spc minus minus m i n i m a l spc minus minus n a s m spc \
	     minus minus b s s spc minus minus k e r n e l spc p r o g dot c spc \
	     p r o g dot a s m ret; do echo "sendkey $$k"; sleep 0.14; done; \
	   sleep 8; for k in a s spc p r o g dot a s m spc slash b i n slash p r o g ret; \
	     do echo "sendkey $$k"; sleep 0.14; done; \
	   sleep 5; for k in e x e c spc slash b i n slash p r o g ret; \
	     do echo "sendkey $$k"; sleep 0.14; done; \
	   sleep 3; echo "screendump gshell.ppm"; sleep 1.5; echo quit ) | \
	 timeout 120 $(QEMU) -kernel gshell.elf -no-reboot -display none \
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

# ---------------------------------------------------------------------------
# Programs: user-space binaries, compiled by nano_cc, linked at 512 GiB, and
# embedded in the kernel image so the RAM disk has something to hold.
#
# -z max-page-size=0x1000 matters. Without it ld aligns each segment to 2 MiB,
# and a 4 KiB program asks the loader for six megabytes of frames.
# ---------------------------------------------------------------------------
user/ustart.o: user/ustart.s
	as --64 user/ustart.s -o user/ustart.o

# One recipe from .c to .elf rather than a .s and a .o step.
#
# Splitting it looks tidier and does not work: make has a built-in rule for
# %.o from %.c that runs GCC, and it wins over a pattern rule of ours whose
# prerequisite is the .s. The first build compiled these with GCC instead of
# nano_cc and failed inside nano-user.h, on __builtin_va_start -- which was a
# confusing way to be told the wrong compiler had been chosen.
#
# The `cd user` is needed too: nano_cc resolves #include against the working
# directory, not against the file doing the including, so nano-user.h is only
# findable from inside that directory.
user/%.elf: user/%.c user/nano-user.h user/ustart.o user/user.ld $(NANO)
	cd user && ../$(NANO) --kernel --bss $*.c $*.s
	as --64 user/$*.s -o user/$*.o
	ld -T user/user.ld -z max-page-size=0x1000 -o $@ user/ustart.o user/$*.o

# The compiler itself, as a program for this OS.
#
# Same source as ../nano_cc, same libc (../nano-libc.h), and the same awk
# transform selfhost.sh uses to swap the five system includes for it. The ONLY
# thing that differs is which file lands as nano-base.h: on Linux it is the one
# with `syscall` in it, here it is user/os-base.h with `int $$0x80`.
#
# --bss is not optional here. Without it the uninitialised globals are written
# out as zero bytes and the binary is 19.6 MB, which is larger than the loader
# accepts, larger than the filesystem's biggest file, and larger than the
# kernel image it has to be embedded in.
user/cc.elf: ../simpleC++.c ../nano-libc.h user/os-base.h user/ustart.o user/user.ld $(NANO)
	@mkdir -p user/ccbuild
	@awk '/^#include <(stdio|stdlib|string|ctype|stdarg)\.h>$$/ { next } \
	      /^static FILE \*fout;$$/ && !done { print "#include \"nano-libc.h\""; done=1 } \
	      { print }' ../simpleC++.c > user/ccbuild/cc.c
	@cp ../nano-libc.h user/ccbuild/
	@cp user/os-base.h user/ccbuild/nano-base.h
	cd user/ccbuild && ../../$(NANO) --kernel --bss cc.c cc.s
	as --64 user/ccbuild/cc.s -o user/ccbuild/cc.o
	ld -T user/user.ld -z max-page-size=0x1000 -o $@ user/ustart.o user/ccbuild/cc.o
	@echo "   $$(stat -c%s $@) bytes on disk, $$(readelf -lW $@ | awk '/RW /{print strtonum($$6)}') bytes of memory"

# The assembler, as a program for this OS.
#
# An EXPLICIT rule, not the user/%.elf pattern above: the pattern would go
# looking for user/as.c, and an explicit rule wins over a pattern rule.
#
# user/as/miniasm.asm is generated -- see user/as/README.md. It is the same
# assembler the Linux build uses with one block swapped, and `make check-miniasm`
# proves the vendored copy has not drifted.
#
# user.ld is the right linker script unchanged: it puts .text at 512 GiB, which
# is where nano-os user memory starts, and names _start as the entry point.
user/as.elf: user/as/miniasm.asm tools/nasm2gas.py user/user.ld
	python3 tools/nasm2gas.py user/as/miniasm.asm > user/as/miniasm.s
	as --64 -o user/as/miniasm.o user/as/miniasm.s
	ld -T user/user.ld -z max-page-size=0x1000 -o $@ user/as/miniasm.o
	@echo "   $$(stat -c%s $@) bytes on disk, $$(readelf -lW $@ | awk '/RW /{print strtonum($$6)}') bytes of memory"

# Regenerate the vendored assembler and require it to match, so the copy in
# this tree cannot quietly fall behind the one it came from.
check-miniasm:
	@[ -n "$(MINIASM_SRC)" ] || { \
	   echo "pass MINIASM_SRC=/path/to/SelfHostedAssembler-audit"; exit 1; }
	@python3 $(MINIASM_SRC)/tools/retarget.py \
	   $(MINIASM_SRC)/fixed/selfContained.asm \
	   $(MINIASM_SRC)/fixed/target-nanoos.inc > user/as/.regen.asm
	@if cmp -s user/as/.regen.asm user/as/miniasm.asm; then \
	   rm -f user/as/.regen.asm; \
	   echo "PASS the vendored assembler matches $(MINIASM_SRC)"; \
	 else \
	   echo "FAIL user/as/miniasm.asm has drifted:"; \
	   diff user/as/miniasm.asm user/as/.regen.asm | head -30; \
	   rm -f user/as/.regen.asm; exit 1; \
	 fi

progs: $(UPROGS)

# ---------------------------------------------------------------------------
# ELF loader: per-process address spaces, a syscall vector, and processes.
# ---------------------------------------------------------------------------
elf.s: elf.c nano-kernel.h nano-mm.h nano-thread.h nano-fs.h nano-proc.h nano-int.h $(NANO)
	$(NANO) --kernel elf.c elf.s

elf64.bin: boot64.s isr.s elf.s progs.s $(UPROGS) src/demo.c src/util.h src/prog.c kernel64.ld
	as --64 boot64.s -o boot64.o
	as --64 isr.s    -o isr.o
	as --64 elf.s    -o elf.o
	as --64 progs.s  -o progs.o
	ld -T kernel64.ld -o elf64.elf boot64.o isr.o elf.o progs.o
	objcopy -O binary elf64.elf elf64.bin

blob-elf.s:
	@printf '.section .blob, "a"\n.incbin "elf64.bin"\n' > $@

elfk.elf: boot32.s blob-elf.s elf64.bin final.ld
	as --32 boot32.s  -o boot32.o
	as --32 blob-elf.s -o blob-elf.o
	ld -m elf_i386 -T final.ld -o elfk.elf boot32.o blob-elf.o

elf: elfk.elf

elfrun: elfk.elf
	$(QEMU) -kernel elfk.elf -no-reboot

elftest: elfk.elf
	@rm -f elf-serial.log
	@( sleep 14; echo quit ) | \
	 timeout 70 $(QEMU) -kernel elfk.elf -no-reboot -display none \
	   -serial file:elf-serial.log -monitor stdio >/dev/null 2>&1; \
	 echo "---- serial output ----"; cat elf-serial.log; echo "-----------------------"; \
	 tr -d '\r' < elf-serial.log > elf-serial.clean; \
	 grep -q "exit code crossed the boundary ok" elf-serial.clean \
	   && grep -q "process wrote a file the kernel can read ok" elf-serial.clean \
	   && grep -q "separate address spaces ok" elf-serial.clean \
	   && grep -q "all four faults contained ok" elf-serial.clean \
	   && grep -q "NX is on: a page marked no-execute really is" elf-serial.clean \
	   && grep -q "kernel survived every fault ok" elf-serial.clean \
	   && grep -q "every address space was reclaimed ok" elf-serial.clean \
	   && grep -q "refused /bad.elf" elf-serial.clean \
	   && grep -q "refused /wx.elf: segment is both writable and executable" elf-serial.clean \
	   && grep -q "refused /zx.elf: executable segment wants zero-filled pages" elf-serial.clean \
	   && grep -q "refused /ne.elf: entry point is not in an executable segment" elf-serial.clean \
	   && echo "PASS: ELF loading, syscalls, address-space isolation, fault containment" \
	   || { echo "FAIL"; exit 1; }

# ---------------------------------------------------------------------------
# The compiler, running inside the OS. cc.c is the headless test.
# ---------------------------------------------------------------------------
cc.s: cc.c nano-kernel.h nano-mm.h nano-thread.h nano-fs.h nano-proc.h nano-int.h $(NANO)
	$(NANO) --kernel cc.c cc.s

cc64.bin: boot64.s isr.s cc.s progs.s $(UPROGS) src/demo.c src/util.h src/prog.c kernel64.ld
	as --64 boot64.s -o boot64.o
	as --64 isr.s    -o isr.o
	as --64 cc.s     -o cck.o
	as --64 progs.s  -o progs.o
	ld -T kernel64.ld -o cc64.elf boot64.o isr.o cck.o progs.o
	objcopy -O binary cc64.elf cc64.bin

blob-cc.s:
	@printf '.section .blob, "a"\n.incbin "cc64.bin"\n' > $@

cck.elf: boot32.s blob-cc.s cc64.bin final.ld
	as --32 boot32.s -o boot32.o
	as --32 blob-cc.s -o blob-cc.o
	ld -m elf_i386 -T final.ld -o cck.elf boot32.o blob-cc.o

cc: cck.elf

ccrun: cck.elf
	$(QEMU) -m 512 -kernel cck.elf -no-reboot

# The test that means something: the assembly produced INSIDE the OS, compared
# byte for byte with the assembly the same compiler produces on Linux from the
# same source file.
#
# -m 512 because the compiler's globals are 19 MB, every page of them is mapped
# up front, and six runs plus a 2 MiB RAM disk do not fit in QEMU's default 128.
cctest: cck.elf $(NANO)
	@rm -f cc-serial.log cc-host.s cc-os.s
	@cd src && ../$(NANO) demo.c ../cc-host.s > /dev/null
	@( sleep 40; echo quit ) | \
	 timeout 120 $(QEMU) -m 512 -kernel cck.elf -no-reboot -display none \
	   -serial file:cc-serial.log -monitor stdio >/dev/null 2>&1; \
	 tr -d '\r' < cc-serial.log > cc-serial.clean; \
	 sed -n '/^---8<--- demo.s begins$$/,/^---8<--- demo.s ends$$/p' cc-serial.clean \
	   | sed '1d;$$d' > cc-os.s; \
	 echo "---- serial output (assembly elided) ----"; \
	 sed '/^---8<--- demo.s begins$$/,/^---8<--- demo.s ends$$/{//!d}' cc-serial.clean; \
	 echo "-----------------------------------------"; \
	 echo "host: $$(wc -l < cc-host.s) lines, in the OS: $$(wc -l < cc-os.s) lines"; \
	 grep -q "the last byte of it reads back correctly" cc-serial.clean \
	   && grep -q "^cc exited with 0$$" cc-serial.clean \
	   && grep -q "the old output was truncated" cc-serial.clean \
	   && grep -q "^missing input: exit 1$$" cc-serial.clean \
	   && grep -q "^broken input: exit 1$$" cc-serial.clean \
	   && grep -q "^relative include with the wrong cwd: exit 1$$" cc-serial.clean \
	   && grep -q "every frame came back" cc-serial.clean \
	   && grep -q "CCTEST DONE" cc-serial.clean \
	   && [ -s cc-os.s ] \
	   && cmp cc-host.s cc-os.s \
	   && echo "PASS: the compiler ran inside the OS and produced byte-identical assembly" \
	   || { echo "FAIL"; diff cc-host.s cc-os.s | head -20; exit 1; }

# ---------------------------------------------------------------------------
# The whole chain, inside the machine: C source -> cc -> as -> a process.
# chain.c is the headless test.
# ---------------------------------------------------------------------------
chain.s: chain.c nano-kernel.h nano-mm.h nano-thread.h nano-fs.h nano-proc.h nano-int.h $(NANO)
	$(NANO) --kernel chain.c chain.s

chain64.bin: boot64.s isr.s chain.s progs.s $(UPROGS) src/prog.c kernel64.ld
	as --64 boot64.s -o boot64.o
	as --64 isr.s    -o isr.o
	as --64 chain.s  -o chaink.o
	as --64 progs.s  -o progs.o
	ld -T kernel64.ld -o chain64.elf boot64.o isr.o chaink.o progs.o
	objcopy -O binary chain64.elf chain64.bin

blob-chain.s:
	@printf '.section .blob, "a"\n.incbin "chain64.bin"\n' > $@

chaink.elf: boot32.s blob-chain.s chain64.bin final.ld
	as --32 boot32.s -o boot32.o
	as --32 blob-chain.s -o blob-chain.o
	ld -m elf_i386 -T final.ld -o chaink.elf boot32.o blob-chain.o

chain: chaink.elf

chainrun: chaink.elf
	$(QEMU) -m 512 -kernel chaink.elf -no-reboot

# The binary the OS assembles is compared byte for byte against the one the
# same assembler produces on Linux from the same input. Two machines agreeing
# on 1,907 bytes of x86-64 is a much narrower claim than a program that runs.
# The SAME assembler, retargeted back to Linux and built to run here.
#
# It exists so the byte comparison below has something to compare against
# without needing a second checkout, and it is worth having for its own sake:
# retargeting the vendored nano-os source back to Linux reproduces the audit
# repository's file exactly, which is the tightest possible check that the two
# have not drifted.
user/as/hostas: user/as/miniasm.asm user/as/target-linux.inc tools/retarget.py tools/nasm2gas.py
	python3 tools/retarget.py user/as/miniasm.asm user/as/target-linux.inc > user/as/hostas.asm
	python3 tools/nasm2gas.py user/as/hostas.asm > user/as/hostas.s
	as --64 -o user/as/hostas.o user/as/hostas.s
	ld -o $@ user/as/hostas.o

chaintest: chaink.elf $(NANO) user/as.elf user/as/hostas
	@rm -f chain-serial.log chain-host.bin chain-host.hex chain-os.hex
	@cd src && ../$(NANO) --minimal --nasm --bss --kernel prog.c ../chain-host.asm >/dev/null
	@# the same input, assembled by the same assembler, running on Linux
	@d=$$(mktemp -d); cp user/as/hostas $$d/mini_asm; cp chain-host.asm $$d/in.asm; \
	 ( cd $$d && ./mini_asm in.asm out.bin -b 0x8000000000 ); \
	 cp $$d/out.bin chain-host.bin; rm -rf $$d
	@python3 -c "d=open('chain-host.bin','rb').read();\
	print('\n'.join(d[i:i+32].hex() for i in range(0,len(d),32)))" > chain-host.hex
	@( sleep 30; echo quit ) | \
	 timeout 90 $(QEMU) -m 512 -kernel chaink.elf -no-reboot -display none \
	   -serial file:chain-serial.log -monitor stdio >/dev/null 2>&1; \
	 tr -d '\r' < chain-serial.log > chain-serial.clean; \
	 sed -n '/^---8<--- prog begins$$/,/^---8<--- prog ends$$/p' chain-serial.clean \
	   | sed '1d;$$d' > chain-os.hex; \
	 echo "---- serial output (the binary elided) ----"; \
	 sed '/^---8<--- prog begins$$/,/^---8<--- prog ends$$/{//!d}' chain-serial.clean; \
	 echo "-------------------------------------------"; \
	 echo "host: $$(wc -l < chain-host.hex) lines, in the OS: $$(wc -l < chain-os.hex) lines"; \
	 cmp chain-host.hex chain-os.hex || { echo "FAIL: the two binaries differ"; exit 1; }; \
	 grep -q "^cc exited with 0$$" chain-serial.clean \
	   && grep -q "^as exited with 0$$" chain-serial.clean \
	   && grep -q "it is an ELF file" chain-serial.clean \
	   && grep -q "hello from a program this machine compiled and assembled itself" chain-serial.clean \
	   && grep -q "^prog exited with 33$$" chain-serial.clean \
	   && grep -q "^unknown mnemonic: exit 1$$" chain-serial.clean \
	   && grep -q "^missing input: exit 1$$" chain-serial.clean \
	   && grep -q "^resb with no count: exit 1$$" chain-serial.clean \
	   && grep -q "CHAINTEST DONE" chain-serial.clean \
	   && [ -s chain-os.hex ] \
	   && echo "PASS: the machine compiled, assembled and ran a program by itself" \
	   || { echo "FAIL"; exit 1; }

clean:
	@# NOT user/*.s -- ustart.s is a source file, and the generated ones are
	@# named after their .c. Deleting by wildcard here would remove the entry
	@# point and leave a build that fails on a missing symbol.
	rm -f user/hello.s user/twin.s user/wild.s user/*.o user/*.elf \
	      progs.o elf.s blob-elf.s elf64.bin elf64.elf elfk.elf \
	      elf-serial.log elf-serial.clean
	rm -rf user/ccbuild
	rm -f user/as/miniasm.s user/as/miniasm.o user/as/.regen.asm \
	      user/as/hostas user/as/hostas.asm user/as/hostas.s user/as/hostas.o
	rm -f chain.s chaink.o blob-chain.s chain64.bin chain64.elf chaink.elf \
	      chain-serial.log chain-serial.clean
	@# cck.o, not cc.o: cc.o is what make's built-in rule would call the
	@# object it builds from cc.c with GCC, and naming ours the same invites
	@# exactly that mix-up back.
	rm -f cc.s cck.o blob-cc.s cc64.bin cc64.elf cck.elf \
	      cc-serial.log cc-serial.clean cc-host.s cc-os.s \
	      chain-host.asm chain-host.bin chain-host.hex chain-os.hex
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
