# Makefile for simpleC++ (the "nano_cc" toy C/C++ -> x86_64 compiler)
#
#   make            # build ./nano_cc with GCC
#   make run        # build, then compile the bundled sample.c to sample.s
#   make test       # build nano_cc, compile+assemble+link+RUN test.c
#   make clean      # remove build artifacts
#
# The compiler itself is a single, self-contained C translation unit
# (simpleC++.c). It only needs a hosted C library and a C11 compiler.

# to force a different assembler, append AS=gcc on the make command line
# to prevent assembly and run, add NOAS=1 on the make command line
# to prevent running the executables, add NORUN=1 on the make command line

CC       ?= gcc
CFLAGS   ?= -std=c11 -O2 -Wall -Wextra
AS	  = $(CC) -no-pie
RUN	  = @
SRC       = simpleC++.c
BIN       = nano_cc
FLAGS	  = -O

ifneq (,$(NOAS))
AS	= @\#
RUN	= \#
endif
ifneq (,$(NORUN))
RUN	= \#
endif

.PHONY: all run test demo structs bitwise printf switch clean nano

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ '$(SRC)'

# Compile a small program to assembly with the built compiler.
run: $(BIN)
	./$(BIN) $(FLAGS) sample.c sample.s
	@echo "---- sample.s ----"
	@cat sample.s

# Full end-to-end demo: nano_cc compiles test.c, GNU as assembles it,
# the linker produces a freestanding binary, and we run it.
test: $(BIN)
	./$(BIN) $(FLAGS) test.c test.s
	$(AS) -nostdlib test.s -o test_prog
	@$(RUN) echo "---- ./test_prog output ----"
	@$(RUN) ./test_prog

# Control-flow / operator feature demo (for, do/while, break, continue,
# prefix ++/--, ternary).
demo: $(BIN)
	./$(BIN) $(FLAGS) features.c features.s
	$(AS) -nostdlib features.s -o features_prog
	@$(RUN) echo "---- ./features_prog output ----"
	@$(RUN) ./features_prog

# struct/union + member access + sizeof + string-return demo.
structs: $(BIN)
	./$(BIN) $(FLAGS) structs.c structs.s
	$(AS) -nostdlib structs.s -o structs_prog
	@$(RUN) echo "---- ./structs_prog output ----"
	@$(RUN) ./structs_prog

# bitwise operators + function-like macro demo.
bitwise: $(BIN)
	./$(BIN) $(FLAGS) bitwise.c bitwise.s
	$(AS) -nostdlib bitwise.s -o bitwise_prog
	@$(RUN) echo "---- ./bitwise_prog output ----"
	@$(RUN) ./bitwise_prog

# variadic functions -> a real printf() written in nano-nolibc.h.
printf: $(BIN)
	./$(BIN) $(FLAGS) printf.c printf.s
	$(AS) -nostdlib printf.s -o printf_prog
	@$(RUN) echo "---- ./printf_prog output ----"
	@$(RUN) ./printf_prog

# switch / case / default (dispatch, fall-through, break, nested, in a loop).
switch: $(BIN)
	./$(BIN) $(FLAGS) switch.c switch.s
	$(AS) -nostdlib switch.s -o switch_prog
	@$(RUN) echo "---- ./switch_prog output ----"
	@$(RUN) ./switch_prog

nano: $(BIN)
	./$(BIN) $(FLAGS) --libc $(SRC) -o nano.s
	$(AS) nano.s -o nano_prog
	@$(RUN) echo "---- ./nano_prog output ----"
	@$(RUN) ./nano_prog

hello: $(BIN)
	./$(BIN) $(FLAGS) hello.c
	$(AS) -nostdlib hello.s -o hello_prog
	$(RUN) ./hello_prog

test_all: test demo structs bitwise printf switch nano hello

clean:
	rm -f $(BIN) sample.s sample_prog test.s test_prog \
	      features.s features_prog structs.s structs_prog \
	      bitwise.s bitwise_prog printf.s printf_prog \
	      switch.s switch_prog nano.s nano_prog hello.s hello_prog
