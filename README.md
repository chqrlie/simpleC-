# simpleC++

```
make                    # build the compiler
make -C kernel          # compile the kernel
```

## The compiler supports a large subset of C.  Here is a list of limitations:

- **Preprocessor:**
  - no recursive macros
  - no token pasting or stringization
  - including from a relative path
  - only 8 macro arguments supported
  - no varargs macros
  - macro invocations cannot span multiple unescaped lines
- **Types:**
  - no function pointers
  - no anonymous sub structures
  - no designated initializers
  - no compound literals
  - no bit-fields
  - 2D arrays are defective
  - no initializers for local structures
  - no non trivial initializers (eg: `FILE *stdin = &_iob[0];`)
  - `static`, `extern`, `const`, `volatile` are ignored
  - no floating point types
- **Expressions:**
  - signed / unsigned promotions and arithmetics are broken
  - noo check and convert function arguments according to prototype
  - vararg ABI is proprietary
  - only 6 function arguments
  - no structure assignment, passing and returning
- **Statements:**
  - no `static_assert`
  - no case ranges
  - local scoping is partially broken
  - `__asm__` syntax is direct pass-through, no argument processing
  **Library**
  - bare bone library is minimalistic in **lib/nano-nolibc.h**
  - `printf` is complete except floating point conversions
  - `malloc` uses a simplistic mark and release approach with a fixed arena
  - standard C library is sufficient for the compiler to compile itself, but it is missing 90% of the standard functions (WIP)
- **Codegen:**
  - only x86_64 System V Intel syntax is supported
  - need more backends: x86_64 binary backend, Arm64 mac backend (source and binary), Intel 32-bit backend (source and binary), Wasm backend, LLVM backend
  - need a built-in assembler to generate binaries from .s files and inline assembly

# Authors:

- Clay Shippy - original idea using AI
- Charlie Gordon - extensive rewrite, the compiler compile itself

---
=======
https://github.com/R077A6r1an/stdlib/tree/main for stdlib <br>
