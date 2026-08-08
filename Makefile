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

.PHONY: all run test demo structs bitwise printf switch clean

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

clean:
	rm -f $(BIN) sample.s sample_prog test.s test_prog \
	      features.s features_prog structs.s structs_prog \
	      bitwise.s bitwise_prog printf.s printf_prog \
	      switch.s switch_prog
