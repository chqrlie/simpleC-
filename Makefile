# Makefile for simpleC++ (the "nano_cc" toy C/C++ -> x86_64 compiler)
#
#   make            # build ./nano_cc with GCC
#   make run        # build, then compile the bundled sample.c to sample.s
#   make test       # build nano_cc, compile+assemble+link+RUN test.c
#   make clean      # remove build artifacts
#
# The compiler itself is a single, self-contained C translation unit
# (simpleC++.c). It only needs a hosted C library and a C11 compiler.

CC       ?= gcc
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra
SRC       = simpleC++.c
BIN       = nano_cc

.PHONY: all run test demo structs bitwise printf switch minimal \
        initializers typedefs gotos functions reserved libcheck casts checkall \
        selfhost clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ '$(SRC)'

# Compile a small program to assembly with the built compiler.
run: $(BIN)
	./$(BIN) sample.c sample.s
	@echo "---- sample.s ----"
	@cat sample.s

# Full end-to-end demo: nano_cc compiles test.c, GNU as assembles it,
# the linker produces a freestanding binary, and we run it.
test: $(BIN)
	./$(BIN) test.c test.s
	$(CC) -nostdlib -no-pie test.s -o test_prog
	@echo "---- ./test_prog output ----"
	@./test_prog

# Control-flow / operator feature demo (for, do/while, break, continue,
# prefix ++/--, ternary).
demo: $(BIN)
	./$(BIN) features.c features.s
	$(CC) -nostdlib -no-pie features.s -o features_prog
	@echo "---- ./features_prog output ----"
	@./features_prog

# struct/union + member access + sizeof + string-return demo.
structs: $(BIN)
	./$(BIN) structs.c structs.s
	$(CC) -nostdlib -no-pie structs.s -o structs_prog
	@echo "---- ./structs_prog output ----"
	@./structs_prog

# bitwise operators + function-like macro demo.
bitwise: $(BIN)
	./$(BIN) bitwise.c bitwise.s
	$(CC) -nostdlib -no-pie bitwise.s -o bitwise_prog
	@echo "---- ./bitwise_prog output ----"
	@./bitwise_prog

# variadic functions -> a real printf() written in nano-nolibc.h.
printf: $(BIN)
	./$(BIN) printf.c printf.s
	$(CC) -nostdlib -no-pie printf.s -o printf_prog
	@echo "---- ./printf_prog output ----"
	@./printf_prog

# switch / case / default (dispatch, fall-through, break, nested, in a loop).
switch: $(BIN)
	./$(BIN) switch.c switch.s
	$(CC) -nostdlib -no-pie switch.s -o switch_prog
	@echo "---- ./switch_prog output ----"
	@./switch_prog

# --minimal: emit only the instruction set the bootstrap assembler implements
# (mov add or and sub xor cmp shl shr sar jcc call ret syscall, plus 8-bit mov).
# Checks every demo still behaves identically and uses nothing outside that set.
minimal: $(BIN)
	@sh minimal-check.sh

# Checked against gcc compiling the same source, in normal and --minimal mode.
# Pass MINIASM=/path/to/mini-asm to include the no-binutils leg:
#   make initializers MINIASM=../sha-audit/build/fixed
MINIASM ?=

# Brace initialisers, local and global.
initializers: $(BIN)
	@sh gcc-check.sh initializers.c $(MINIASM)

# typedef and enum: typedef'd builtins, structs, pointers and arrays, the
# self-referential idiom, enumerator auto-increment, enums as array sizes and
# case labels, and block-scope typedefs.
typedefs: $(BIN)
	@sh gcc-check.sh typedefs.c $(MINIASM)

# goto and labels: forward, backward, out of nested loops, inside a switch.
gotos: $(BIN)
	@sh gcc-check.sh gotos.c $(MINIASM)

# array parameters (long m[4][3] is long (*m)[3]) and function return types.
functions: $(BIN)
	@sh gcc-check.sh functions.c $(MINIASM)

# C names that are assembler keywords (sp, ax, ch, gs, flat, ptr, word).
reserved: $(BIN)
	@sh gcc-check.sh reserved.c $(MINIASM)

# nano-libc.h: the freestanding C library, checked against glibc.
libcheck: $(BIN)
	@sh gcc-check.sh libcheck.c $(MINIASM)

# Cast precedence: a cast binds to a unary-expression, not to what follows.
casts: $(BIN)
	@sh gcc-check.sh casts.c $(MINIASM)

# Every gcc-checked suite in one go.
checkall: $(BIN)
	@for f in initializers typedefs gotos functions reserved libcheck casts; do \
	    echo "== $$f.c"; sh gcc-check.sh $$f.c $(MINIASM) || exit 1; \
	done

# The three-stage bootstrap: nano_cc builds itself, then that builds itself,
# and the two have to come out byte-identical. Uses nano-libc.h, not glibc.
selfhost: $(BIN)
	@sh selfhost.sh

clean:
	rm -f $(BIN) sample.s sample_prog test.s test_prog \
	      features.s features_prog structs.s structs_prog \
	      bitwise.s bitwise_prog printf.s printf_prog \
	      switch.s switch_prog initializers.s initializers_prog \
	      typedefs.s typedefs_prog gotos.s gotos_prog \
	      functions.s functions_prog reserved.s reserved_prog \
	      libcheck.s libcheck_prog libcheck.tmp casts.s casts_prog
