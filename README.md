# simpleC++

gcc -o nano_cc nano_cc.c          # build the compiler
./nano_cc kernel.c kernel.s       # compile your kernel
gcc -nostdlib -T linker.ld kernel.s -o kernel.elf   # link for bare metal


## What the compiler supports

Enough of C to compile `nano-nolibc.h` + `test.c`:

- **Preprocessor:** `#include "..."`, object-like `#define`, `#ifndef` /
  `#ifdef` / `#else` / `#endif` include guards, `//` and `/* */` comments.
- **Types:** `int`, `long`, `char`, `void`, pointers, `char` arrays, and the
  `const` / `unsigned` / `static` / `inline` qualifiers (parsed and ignored).
- **Expressions:** `+ - * / %`, `< > <= >= == !=`, `&& ||`, unary `- ! * &`,
  prefix and postfix `++` / `--`, the ternary `?:` operator, casts, function
  calls, array indexing `a[i]`, assignment and compound assignment
  (`+= -= *= /= %=`), string/char literals with escapes.
- **Statements:** `if/else`, `while`, `for`, `do/while`, `break`, `continue`,
  `return`, blocks, and `__asm__("...")` pass-through inline assembly.
- **Codegen:** x86_64 System V, values in `rax`, up to 6 register arguments,
  a freestanding `_start` that calls `main` and exits with its return value.

---