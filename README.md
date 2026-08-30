# simpleC++

```
make                    # build the compiler
make -C kernel          # compile the kernel
```

## The compiler supports a large subset of C.  Here is a list of limitations:

- **Preprocessor:**
  - no recursive macros
  - no token pasting or stringization
  - only 8 macro arguments supported
  - no varargs macros
  - macro invocations cannot span multiple unescaped lines
- **Types:**
  - no function pointers
  - no designated initializers
  - no compound literals
  - no bit-fields
  - 2D arrays are defective
  - no initializers for local structures / unions
  - no non trivial initializers (eg: `FILE *stdin = &_iob[0];`)
  - `static`, `extern`, `const`, `volatile` are ignored
  - no floating point types
- **Expressions:**
  - signed / unsigned promotions and arithmetics may be incomplete
  - no check and convert function arguments according to prototype
  - vararg ABI is proprietary (or will be)
  - only 6 function arguments
  - no structure assignment, passing and returning
- **Statements:**
  - no `static_assert`
  - `__asm__` syntax is direct pass-through, no argument processing
  **Library**
  - bare bone library is minimalistic in **lib/nano-nolibc.h**
  - `printf` is complete except floating point conversions
  - `malloc` uses a simplistic mark and release approach with a fixed 3.2MB arena
  - standard C library is sufficient for the compiler to compile itself, but it is missing 90% of the standard functions (WIP)
- **Codegen:**
  - only x86_64 System V Intel syntax is supported
  - need more backends: x86_64 binary backend, Arm64 mac backend (source and binary), Intel 32-bit backend (source and binary), Wasm backend, LLVM backend
  - need a built-in assembler to generate binaries from .s files and inline assembly

# Authors:

- Clay Shippy - original idea using AI
- Charlie Gordon - extensive rewrite, self compilation, confirmance

# Who I am - Charlie Gordon

- I am based in Paris, France
- I love C programming, never tired of it since Feb of 1983 :)
- I want to share this passion by making C programming more enjoyable.

# Project goals

- make a mostly c23 compliant compiler that can be used for educational purposes.
- keep it small and fast, which requires it to produce efficient code :)
- _small_ means less than 5K lines and less than 128KB binary
- _fast_ means the executables produced should be faster than if compiled with `gcc -O2`.
- incorporate a number of extensions to simplify the language and make it safer
- at the user option, produce:
  - preprocessor output
  - assembly source code
  - standard object files
  - static binary executable.
  - standard C code for other compilers (use as a transpiler)
