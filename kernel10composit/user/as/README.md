# `miniasm.asm` — the assembler, targeted at nano-os

`miniasm.asm` is **generated**, and vendored here on purpose. It is

    tools/retarget.py fixed/selfContained.asm fixed/target-nanoos.inc

run in [SelfHostedAssembler-audit](https://github.com/anirudhatalmale6-alt/SelfHostedAssembler-audit).

Only the block between the two `; ==== TARGET ... ====` marker lines differs
from the Linux build — the trap instruction, the syscall numbers, how argc and
argv arrive, and the default output base. Everything below those markers is the
same assembler, byte for byte. That is the point of the split: two copies of a
2,300-line assembler drift, and a divergence between them surfaces as a
miscompilation on one platform and not the other.

Vendored rather than fetched because a clone of *this* repository has to build
and boot on its own. `make gshellrun` should not need a second checkout to
produce an OS with an assembler in it.

The copy cannot drift quietly:

    make check-miniasm MINIASM_SRC=/path/to/SelfHostedAssembler-audit

regenerates it and diffs. Generated at `ad82ee6`.
