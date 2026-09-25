#!/usr/bin/env python3
"""Rename one block-scoped declaration so it stops shadowing an outer one.

nano_cc gives every local the whole function as its scope, so two declarations
of the same name at different types inside one function are an error rather
than a shadow. The fix is mechanical -- rename the INNER one and every use of
it within its own block -- but doing it by hand across a 5554-line file is how
you rename one occurrence too many and change what the code means.

So: find the innermost braces containing the declaration, and rewrite whole-word
occurrences of the name only between them. A use of the same name outside those
braces belongs to the other declaration and is left alone, which is exactly the
distinction that makes the edit safe.

Usage:  unshadow.py <file> <line-number-of-decl> <old-name> <new-name>
"""
import re
import sys


def block_bounds(text, pos):
    """Character range of the innermost {...} containing `pos`."""
    depth = 0
    start = None
    i = pos
    while i >= 0:                       # walk back to the opening brace
        c = text[i]
        if c == '}':
            depth += 1
        elif c == '{':
            if depth == 0:
                start = i
                break
            depth -= 1
        i -= 1
    if start is None:
        return None
    depth = 0
    i = start
    while i < len(text):                # and forward to its partner
        c = text[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return start, i
        i += 1
    return None


def main():
    path, lineno, old, new = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    text = open(path).read()
    lines = text.split('\n')
    pos = sum(len(l) + 1 for l in lines[:lineno - 1])

    bounds = block_bounds(text, pos)
    if not bounds:
        sys.exit('could not find the enclosing block')
    a, b = bounds

    seg = text[a:b + 1]
    pat = re.compile(r'\b%s\b' % re.escape(old))
    n = len(pat.findall(seg))
    if n == 0:
        sys.exit('name %r does not occur in that block' % old)
    text = text[:a] + pat.sub(new, seg) + text[b + 1:]
    open(path, 'w').write(text)
    print('renamed %d occurrence(s) of %r -> %r within lines %d..%d'
          % (n, old, new,
             text[:a].count('\n') + 1, text[:b].count('\n') + 1))


if __name__ == '__main__':
    main()
