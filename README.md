# simpleC++

gcc -o nano_cc nano_cc.c          # build the compiler<br>
./nano_cc kernel.c kernel.s       # compile your kernel<br>
gcc -nostdlib -T linker.ld kernel.s -o kernel.elf   # link for bare metal<br>

i was hoping to compile this with chibicc ,8cc or lcc minimal kind of compiler <br>

<<<<<<< HEAD
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
=======
https://github.com/R077A6r1an/stdlib/tree/main for stdlib <br>

<br>
Feature,Status
Full C expression parsing with correct precedence,✅
if / else / while / return,✅
Local + global variables,✅
Functions with up to 6 register args (System V ABI),✅
struct definitions,✅
class with public: / private:,✅
new / delete (C++ sugar),✅
"Pointers, "&", "*,✅
#define and #include preprocessor,✅
"__asm__(""..."")" inline assembly,✅
Macro expansion inside asm strings (e.g. #define VGA 0xB8000 → "__asm__(""mov rdi, VGA"")"),✅
Real x86_64 System V output,✅
>>>>>>> 818220209677321765b956fd615fd5b4cad1359d
