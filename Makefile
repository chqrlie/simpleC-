# Makefile for simpleC++ (the "nano_cc" toy C/C++ -> x86_64 compiler)
#
#   make            # build ./nano_cc with GCC
#   make nano       # build, then self compile to produce nano_prog
#   make examples   # build nano_cc and compile the examples
#   make test-all   # build nano_cc and run tests
#   make clean      # remove build artifacts
#   make clobber    # remove build artifacts and targets
#
# The compiler itself is a single, self-contained C translation unit
# (simpleC++.c). It only needs a hosted C library and a C11 compiler.

# to force a different assembler, append AS=gcc on the make command line
# to prevent assembly and run, add NOAS=1 on the make command line
# to prevent running the executables, add NORUN=1 on the make command line

CC       ?= gcc
CFLAGS   ?= -std=gnu11 -O2 -Wall -Wextra
SIZE	 ?= size
AS	  = $(CC) -no-pie
RUN	  =
SRC       = simpleC++.c
BIN       = ./nano_cc
FLAGS	  = -O

ifneq (,$(NOAS))
AS	= @\#
RUN	= @\#
endif
ifneq (,$(NORUN))
RUN	= @\#
endif

.PHONY: all extra run test demo structs bitwise printf switch clean distclean nano test-printf nano-printf examples test-all

INC = lib/nano-nolibc.h lib/nano-libc.h lib/nano-malloc.h lib/nano-printf.h

all: $(BIN)

$(BIN): $(SRC) $(INC)
	$(CC) $(CFLAGS)    '$(SRC)' -o $@
	$(CC) $(CFLAGS) -g '$(SRC)' -o $@_g

extra: $(BIN)
	$(CC) $(CFLAGS) -S -O2    '$(SRC)' -o nano_gcc_O2.s
	$(CC) $(CFLAGS) -S -O0    '$(SRC)' -o nano_gcc_O0.s
	$(CC) $(CFLAGS) -S -O2 -g '$(SRC)' -o nano_gcc_O2_g.s
	$(CC) $(CFLAGS) -S -O0 -g '$(SRC)' -o nano_gcc_O0_g.s

# Full end-to-end demo: nano_cc compiles the C source, GNU as assembles it,
# the linker produces a freestanding binary, and we run it.
%_prog: examples/%.c $(BIN) Makefile
	$(BIN) $(FLAGS) $< $@.s
	$(AS) -nostdlib $@.s -o $@
	$(RUN) ./$@

test: test_prog
hello: hello_prog
structs: structs_prog
bitwise: bitwise_prog
printf: printf_prog
switch: switch_prog

nano: $(BIN) Makefile
	$(BIN) $(FLAGS) $(SRC) -time -memory -o nano.s
	$(BIN) $(FLAGS) $(SRC) -time -g -o nano_g.s
	$(AS) -nostdlib nano.s -o nano_prog
	$(AS) -nostdlib nano.s -o nano_prog_g -g
	$(SIZE) nano_cc nano_prog
	$(RUN) ./nano_prog $(FLAGS) $(SRC) -time -memory -o nano2.s
	$(RUN) ./nano_prog $(FLAGS) $(SRC) -time -g -o nano2_g.s
	$(RUN) diff nano.s nano2.s | head -50
	$(RUN) diff nano_g.s nano2_g.s | head -50
	@if [ '!' -f STATS.csv ] ; then echo "Source lines,Source bytes,Library lines,Library bytes,nano.s lines,nano.s bytes,nano_cc bytes,nano_prog bytes" > STATS.csv ; fi
	@if [ -f nano_prog ] ; then \
	    echo `wc -l < simpleC++.c`,`wc -c < simpleC++.c`,`cat lib/nano-*.h | wc -l`,`cat lib/nano-*.h | wc -c`,`wc -l < nano.s`,`wc -c < nano.s`,`wc -c < nano_cc`,`wc -c < nano_prog`   >> STATS.csv ; \
        else \
	    echo `wc -l < simpleC++.c`,`wc -c < simpleC++.c`,`cat lib/nano-*.h | wc -l`,`cat lib/nano-*.h | wc -c`,`wc -l < nano.s`,`wc -c < nano.s`,`wc -c < nano_cc`   >> STATS.csv ; \
        fi
	@if [ `wc -l < STATS.csv` -gt 4 ] ; then head -1 < STATS.csv ; fi
	@tail -4 < STATS.csv

# test the host libc and nano-printf
test-printf: Makefile
	$(CC) $(CFLAGS) test/printf-test.c -o printf-test_g -g
	./printf-test_g

# test nano-printf compiled by nano as part of its C library
nano-printf: $(BIN) Makefile
	$(BIN) $(FLAGS) test/printf-test.c -o printf-test_prog_g.s -g
	$(AS) -nostdlib printf-test_prog_g.s -o printf-test_prog_g
	./printf-test_prog_g

examples: test demo structs bitwise printf switch hello
test-all: examples test-printf nano-printf nano

clean:
	rm -f $(BIN) sample.s *_g *_g.s *_prog *_prog.s nano*.s

distclean: clean
	rm -f $(BIN) $(BIN)_g
