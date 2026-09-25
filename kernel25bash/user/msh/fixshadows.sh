#!/bin/sh
# Drive build.sh until it stops reporting shadowed locals.
#
# nano_cc reports one shadow per run, and a 5554-line shell has a lot of them:
# every `for (int i = ...)` in a function that also has an `i` of another type.
# The rename itself is mechanical and unshadow.py does it safely, so the only
# thing left is the loop. Anything that is NOT a shadowing error stops this
# immediately and is printed for a human to look at -- the point is to clear
# the boring ones, not to keep going until something compiles by accident.
cd "$(dirname "$0")"
n=0
while [ $n -lt 300 ]; do
    err=$(./build.sh 2>&1)
    case "$err" in
        '') echo "COMPILED CLEAN after $n renames"; exit 0 ;;
    esac
    name=$(printf '%s\n' "$err" | sed -n "s/.*local '\([a-zA-Z_][a-zA-Z_0-9]*\)' is declared twice.*/\1/p")
    if [ -z "$name" ]; then
        echo "--- not a shadowing error, stopping after $n renames ---"
        printf '%s\n' "$err" | head -4
        exit 1
    fi
    # The compiler prints the offending SOURCE LINE; find it in the file.
    text=$(printf '%s\n' "$err" | sed -n 's/^  line [0-9]* after preprocessing:  //p')
    hits=$(grep -Fxn "    $text" minishell.c 2>/dev/null | wc -l)
    line=$(grep -Fn "$text" minishell.c | head -1 | cut -d: -f1)
    if [ -z "$line" ]; then
        echo "--- could not locate the line in minishell.c, stopping ---"
        printf '%s\n' "$err" | head -4
        exit 1
    fi
    n=$((n + 1))
    python3 unshadow.py minishell.c "$line" "$name" "${name}_p$n" >/dev/null || exit 1
    echo "$n: $name -> ${name}_p$n   (line $line)"
done
echo "hit the 300-rename guard; something is not converging"
exit 1
