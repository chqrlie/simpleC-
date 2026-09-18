# TODO list

* merge upstream
* support function pointers
* add analysis pass:
  - late constant folding
  - automatic library fetch
  - register allocation, stack usage
  - set discard information
  - [90%] detect unreachable code
  - [90%] dead code elimination
* add `N_MEMBER` variant for `pointer->member`
* add `gen_addr_type` to optimize `N_MEMBER` case (simpler with analyser)
* optimize `var->member` access
* optimize `var[const_val]` address computations
* optimize expressions with commutativity and associativity
* optim `+=`, `*=`, `&=`, `|=`, `^=` with var lhs -> swap
* optim `+`, `*`, `&`, `|`, `^` with const lhs -> swap
* optim bin/compare operations with fullwidth sym rhs -> skip RCX load_var
* optim comparisons with const lhs -> swap and transpose
* [20%] optimize <expr> = <const> and <expr> = <sym>
* use common type for `N_TERNARY`
* direct load and store
* improve inc / dec
* `gen_expr` should return flow state and flags state
* elide register reload:
* use generic save mask
* use spill area and stop `push`/`pop` method
* use unpromoted ABI and promote if needed
* merge `parse_toplevel` and `parse_decl` or `parse_stmt`
* [95%] optimize code gen for condition expressions
* [50%] complete support for short/int/long semantics
* [50%] complete support of unsigned semantics
* check and convert function arguments according to prototype
* fix bogus 2D array handling
* accept more than 6 function arguments
* simplify varargs API: all optional arguments on the stack
* add builtins for `memcpy`, `memset`, `strcpy`, `strlen`
* compound literals
* fix unstructured initializers
* fix complex designated initializers
* structure assignment
* structure passing and returning
* better macro processing (single pass, token based)
* accept more than 8 macro arguments
* accept varargs macros
* save locations as offsets with full range capabilities
* output column in error messages
* pass integer offset to `gen_expr()`
* pass integer rhs argument to `gen_expr()`
* add `get_loc` to generate better load/store/update code
* `stdin`, `stdout` and `stderr` should be defined as pointers
* should support `__func__`, a pre-defined static local `char` array
* support `extern`, `static`, `typedef` correctly
* accept flexible arrays at end of `struct`/`union` definition, and flag recursively
* check if strings with embedded null bytes can be output as `.string`
* split long strings on `\n` in assembly, using `.ascii`
* further reduce `Node` size, distinguish `SNode` and `ENode`?
* [/] proper structure alignment, layout and size (mostly done)
* support bit-field syntax
* support bit-field semantics
* support bit-field proper layout
* `expect_id()`, `expect_string()`: check kind, increment `P`, return data
* [50%] add timings and other stats using helper program
* emit static data initializers for address expressions (eg: `char a, *p = &a;`)
* more optimizations
* complete library
* more intrinsics
* add more tests
* support floating point:
  - accept floating point literals
  - support floating point arithmetics (``float` and `double`)
  - parse floating point literals
  - extend `fprintf` for floating point conversions
  - make the whole thing optional
* add C backend for bootstrapping
* x86_64 binary backend
* Arm64 mac backend (source and binary)
* Intel 32-bit backend (source and binary)
* Wasm backend
* LLVM backend
* add built-in assembler to generate binaries from .s files (Intel/ATT syntax, x86/arm
* improve hashing:
```c
for (int i = 0; i < len; i++) hash = __builtin_rotate_left(hash, 5) + (str[i] & 255);
```
* optimize member load and assignments:
```c
    if (lhs->kind == N_MEMBER && lhs->lhs->kind === N_VAR) {
        gen_expr(n->rhs, r, save_rax);
        resolve_name(lhs->lhs);
        lt = store_var(lhs->decl, 0, lhs->decl->type, r);
        if (!(n->flags & DISCARD)) promote_reg(lt, r);
        return lt;
    }
```
* += should not push previous value
* support `\uxxxx`, `\Uxxxxxxxx`, `\u{x+}`, `\U{x+}`,
  `\x{x+}`, `\o{o+}` in strings and character constants
* `bool` type, `true` and `false` predefined constants
* `nullptr_t` type and `nullptr` predefined constant
# C grammar
* support _generic-selection_ (a _primary-expression_)
* support _compound-literal_ (a _postfix-expression_)
* parse _call-expression_ as a _postfix-expression_
* parse _alignment-specifier_:
  - `alignas` `(` _type-name_ `)`
  - `alignas` `(` _constant-expression_ `)`
* support initialized `const` variables as _constant-expression_
* support minimal `constexpr` semantics
* use correct terms:
  - _type-qualifier_: `const`, `restrict`, `volatile`,
  - _function-specifier_: `inline`, `_Noreturn`
* support `typeof` and `typeof_unqual`
