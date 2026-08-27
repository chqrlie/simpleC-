# simpleC++

gcc -o nano_cc simpleC++.c          # build the compiler<br>
./nano_cc kernel.c kernel.s       # compile your kernel<br>
gcc -nostdlib -T linker.ld kernel.s -o kernel.elf   # link for bare metal<br>

<br>
v2 designed to work with this selfHostingAssembler https://github.com/netpipe/SelfHostedAssembler/ with --minimal flag
<br>
$ ./nano_cc --minimal --nasm printf.c prog.asm $ ./mini_asm # reads prog.asm, writes a.out $ ./a.out
<br>





i was hoping to compile this with chibicc ,8cc or lcc minimal kind of compiler <br>

## What the compiler supports

- **Preprocessor:** `#include "..."`, object-like `#define`, **function-like
  `#define` macros** (e.g. `#define MAX(a,b) ((a)>(b)?(a):(b))` — argument
  substitution with nested-paren handling), `#ifndef` / `#ifdef` / `#else` /
  `#endif` include guards, `//` and `/* */` comments.
- **Types:** `int`, `long`, `char`, `void`, pointers, arrays (of any element
  type), `struct` and `union`, and the `const` / `unsigned` / `static` /
  `inline` / `extern` qualifiers (parsed and ignored). Function prototypes /
  `extern` declarations (`char getc();`) are accepted. *(Note: `int` and `long` are both
  64-bit internally for now — a real 32-bit `int` is a planned next step.)*
- **Expressions:** `+ - * / %`, `< > <= >= == !=`, `&& ||`, **bitwise
  `& | ^ ~ << >>`** (full C precedence), unary `- ! ~ * &`, prefix and postfix
  `++` / `--`, the ternary `?:` operator, `sizeof`, casts, function calls
  (including functions that return pointers/strings), array indexing `a[i]`,
  struct member access `.` and `->`, assignment and compound assignment
  (`+= -= *= /= %=`), string/char literals with escapes.
- **Variadic functions:** `type f(args, ...)` with `__builtin_va_start` /
  `__builtin_va_arg` / `__builtin_va_end` (wrapped as `va_start`/`va_arg`/
  `va_end`), enough to write a real `printf()` — see `printf.c`.
- **Statements:** `if/else`, `while`, `for`, `do/while`, `break`, `continue`,
  `return`, blocks, and `__asm__("...")` pass-through inline assembly.
- **Codegen:** x86_64 System V, values in `rax`, up to 6 register arguments,
  a freestanding `_start` that calls `main` and exits with its return value.

---
=======
https://github.com/R077A6r1an/stdlib/tree/main for stdlib <br>

