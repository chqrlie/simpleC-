#!/bin/sh
# selfhost.sh — how far is nano_cc from compiling itself?
#
# Rewrites simpleC++.c's five system includes to selfhost-shim.h (declarations
# only, no code), then asks nano_cc to compile the result and GNU as to
# assemble it. Anything nano_cc cannot handle stops it with a line of source;
# anything the freestanding world does not have yet shows up as an undefined
# symbol in the object file.
#
# This is a measurement, not a bootstrap: the shim is declarations only, so a
# clean run means "the compiler handles all of its own source", NOT "it links".
#
#   sh selfhost.sh

set -u
SRC='simpleC++.c'
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

[ -x ./nano_cc ] || { echo "build nano_cc first (make)"; exit 1; }

# Swap the hosted headers for the shim, placed just before the first use of a
# library type so it is in scope everywhere it is needed.
awk '
  /^#include <(stdio|stdlib|string|ctype|stdarg)\.h>$/ { next }
  /^static FILE \*fout;$/ && !done { print "#include \"selfhost-shim.h\""; done=1 }
  { print }
' "$SRC" > "$W/selfsrc.c"
cp selfhost-shim.h "$W/"

if ! ./nano_cc "$W/selfsrc.c" "$W/self.s" >/dev/null 2>"$W/err"; then
    echo "BLOCKED: nano_cc cannot compile its own source yet"
    cat "$W/err"
    exit 1
fi
echo "PASS compile: nano_cc compiled all $(wc -l < "$SRC") lines of its own source"
echo "              -> $(wc -l < "$W/self.s") lines of assembly"

if ! as --64 -o "$W/self.o" "$W/self.s" 2>"$W/aserr"; then
    echo "FAIL assemble: GNU as rejected the output"
    head -10 "$W/aserr"
    exit 1
fi
echo "PASS assemble: GNU as accepted it ($(wc -c < "$W/self.o") bytes of object)"

MISSING=$(nm -u "$W/self.o" | sed 's/^ *U *//' | sort)
N=$(printf '%s\n' "$MISSING" | grep -c .)
if [ "$N" -eq 0 ]; then
    echo "PASS link: nothing undefined -- the freestanding library is complete"
    exit 0
fi
echo
echo "REMAINING: $N C library functions have no freestanding implementation."
echo "These are the whole gap between here and a real stage-1 bootstrap:"
printf '%s\n' "$MISSING" | tr '\n' ' ' | fold -s -w 68 | sed 's/^/  /'
echo
exit 0
