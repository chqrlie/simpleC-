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
# temporary files are created in the build directory

CC     ?= gcc
CFLAGS += -std=gnu11 -O2 -Wall -Wextra
SIZE   ?= size
STRIP  ?= strip
NOPIE  ?= -no-pie
AS	= gcc
RUN	=
SRC     = simpleC++.c
BIN     = ./nano_cc
FLAGS	= -O
TMP     = build

ifneq (,$(NOAS))
AS	= @\#
RUN	= @\#
endif
ifneq (,$(NORUN))
RUN	= @\#
endif

.PHONY: all build extra run test demo structs bitwise printf switch clean \
        distclean nano test-printf nano-printf examples test-all

INC = lib/nano-nolibc.h lib/nano-libc.h lib/nano-malloc.h lib/nano-printf.h

all: $(BIN)

$(BIN): $(SRC) $(INC) Makefile
	$(CC) $(CFLAGS) '$(SRC)' -o $@
	$(CC) $(CFLAGS) -O0 -g '$(SRC)' -o $@_g

build:
	@mkdir -p $(TMP)

extra: $(BIN) build
	$(CC) $(CFLAGS) -S -O2    '$(SRC)' -o $(TMP)/nano_gcc_O2.s
	$(CC) $(CFLAGS) -S -O0    '$(SRC)' -o $(TMP)/nano_gcc_O0.s
	$(CC) $(CFLAGS) -S -O2 -g '$(SRC)' -o $(TMP)/nano_gcc_O2_g.s
	$(CC) $(CFLAGS) -S -O0 -g '$(SRC)' -o $(TMP)/nano_gcc_O0_g.s

# Full end-to-end demo: nano_cc compiles the C source, GNU as assembles it,
# the linker produces a freestanding binary, and we run it.
$(TMP)/%_prog: examples/%.c $(BIN) build Makefile
	$(BIN) $(FLAGS) $< $@.s
	$(AS) $(NOPIE) -nostdlib -static $@.s -o $@
	$(RUN) ./$@

%: %.c $(BIN) Makefile
	$(BIN) $(FLAGS) $< -o $@.s
	$(AS) $(NOPIE) -nostdlib -static $@.s -o $@
	$(RUN) ./$@

test: $(TMP)/test_prog
hello: $(TMP)/hello_prog
structs: $(TMP)/structs_prog
bitwise: $(TMP)/bitwise_prog
printf: $(TMP)/printf_prog
switch: $(TMP)/switch_prog

nano: $(BIN) build Makefile
	$(BIN) $(FLAGS) $(SRC) -time -memory -o $(TMP)/nano.s
	$(BIN) $(FLAGS) $(SRC) -time -g -o $(TMP)/nano_g.s
	$(AS) $(NOPIE) -nostdlib -static $(TMP)/nano.s   -o $(TMP)/nano_prog
	$(AS) $(NOPIE) -nostdlib -static $(TMP)/nano_g.s -o $(TMP)/nano_prog_g -g
	$(STRIP) $(BIN) $(TMP)/nano_prog
	$(SIZE) $(BIN) $(wildcard $(TMP)/nano_prog)
	$(RUN) $(TMP)/nano_prog $(FLAGS) $(SRC) -time -memory -o $(TMP)/nano2.s
	$(RUN) $(TMP)/nano_prog $(FLAGS) $(SRC) -time -g -o $(TMP)/nano2_g.s
	$(RUN) diff $(TMP)/nano.s $(TMP)/nano2.s | head -50
	$(RUN) diff $(TMP)/nano_g.s $(TMP)/nano2_g.s | head -50
	@if [ '!' -f STATS.csv ] ; then echo "Source lines,Source bytes,Library lines,Library bytes,nano.s lines,nano.s bytes,nano_cc bytes,nano_prog bytes" > STATS.csv ; fi
	@if [ -f $(TMP)/nano_prog ] ; then \
	    echo `wc -l < simpleC++.c`,`wc -c < simpleC++.c`,`cat lib/nano-*.h | wc -l`,`cat lib/nano-*.h | wc -c`,`wc -l < $(TMP)/nano.s`,`wc -c < $(TMP)/nano.s`,`wc -c < nano_cc`,`wc -c < $(TMP)/nano_prog`   >> STATS.csv ; \
        else \
	    echo `wc -l < simpleC++.c`,`wc -c < simpleC++.c`,`cat lib/nano-*.h | wc -l`,`cat lib/nano-*.h | wc -c`,`wc -l < $(TMP)/nano.s`,`wc -c < $(TMP)/nano.s`,`wc -c < nano_cc`   >> STATS.csv ; \
        fi
	@if [ `wc -l < STATS.csv` -gt 4 ] ; then head -1 < STATS.csv ; fi
	@tail -4 < STATS.csv

# test the host libc and nano-printf
test-printf: build Makefile
	$(CC) $(CFLAGS) test/printf-test.c -o $(TMP)/printf-test_g -g
	$(TMP)/printf-test_g

# test nano-printf compiled by nano as part of its C library
nano-printf: $(BIN) build Makefile
	$(BIN) $(FLAGS) test/printf-test.c -o $(TMP)/printf-test_prog_g.s -g
	$(AS) $(NOPIE) -nostdlib -static $(TMP)/printf-test_prog_g.s -o $(TMP)/printf-test_prog_g
	$(RUN) $(TMP)/printf-test_prog_g

examples: test demo structs bitwise printf switch hello
test-all: examples test-printf nano-printf nano

clean:
	rm -rf build $(BIN) *_g *_g.s *_prog *_prog.s nano*.s a.out *.dSYM

distclean: clean
	rm -f $(BIN) $(BIN)_g
