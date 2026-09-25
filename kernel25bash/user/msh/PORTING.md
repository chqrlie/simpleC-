# miniShell on nanoOS

Your `miniShell-fixed/main.c`, 5554 lines, compiled by `nano_cc` and running as
a process inside the OS. Not a rewrite and not the small parser in
`user/sh.c` — this is your source, with **13 changed hunks**: 269 lines added,
76 removed. Everything else is byte for byte what you wrote, so it still diffs
cleanly against upstream.

```
user/msh/minishell.c   your main.c, with the port changes marked /* PORT: */
user/msh/msh-compat.h  the POSIX surface the OS does not have (~480 lines)
user/msh/build.sh      stages a copy with the POSIX #includes swapped out
```

The build never edits your includes in place. It copies `minishell.c`, strips
`<unistd.h>`, `<dirent.h>`, `<sys/stat.h>` and the rest, and puts
`msh-compat.h` there instead — the same trick `user/cc.elf` uses to build the
compiler from `simpleC++.c` unmodified.

## What works

| | |
|---|---|
| tokeniser, parser, quoting, escaping | your code, untouched |
| `;`, `&&`, `\|\|`, `!` | untouched |
| `if`, `while`, `for`, `case`, functions | untouched |
| variables, arrays, `${...}` expansions | untouched |
| builtins | untouched |
| `>`, `>>`, `<` on a builtin | works — needed new `dup`/`dup2` syscalls |
| `>`, `>>`, `<` on a program | works — resolved into the spawn |
| `a \| b \| c` | works, stages run concurrently |
| here-documents `<< EOF` | works up to 4096 bytes — see below |

## What does not, and why

Four things need **real `fork`** — a child that keeps running the shell's own
code, with the parent's variables and functions, and no program to `exec`.
This kernel has no copy-on-write, so that means duplicating every mapped page.

- subshells `( ... )`
- command substitution `$(...)` and backticks
- background jobs `&`
- a **builtin** as a pipeline stage — `echo hi | cat` needs a second copy of
  the shell to run `echo` in. Two *programs* pipe fine.

Each reports itself **by name**:

```
minishell: subshells ( ... ) need fork, which this build does not have
minishell: $(...) and `...` need fork, which this build does not have
minishell: background jobs (&) need fork, which this build does not have
minishell: only external commands can be used in a pipeline in this build
```

That is deliberate. Your parser still *accepts* all of this syntax, so a
silent failure would look like a bug in the script rather than a missing
feature.

Three smaller gaps, same reasoning:

- `2>file` — spawn takes the child's stdout and stdin and has no third slot.
  Works for a builtin, where `dup2` does the job.
- `>&` and `<&` between descriptors of a *program*, for the same reason.
- more than 8 arguments to one command, which is `SYS_SPAWN`'s limit. Refused
  rather than truncated: running a command with its arguments silently cut off
  is worse than not running it.

## Here-documents

Your `heredoc_fd` already avoided forking for bodies under 32768 bytes — it
writes straight into the pipe. That is why here-documents work here at all.

The limit had to come down to **4096**, the kernel's pipe buffer. Write one
byte more than the buffer holds with no reader started yet and the shell
blocks forever on a drain that cannot happen. So the size is checked and
refused with a message rather than hanging.

## Changes to your source, by category

1. **Two `for`-increment comma operators** → the advance moved to the bottom of
   the loop. `nano_cc` has no comma operator.
2. **Five shadowed locals renamed.** `nano_cc` gives every local the whole
   function as its scope, so `char **av` and a later `Var *av` in the same
   function are the same variable. One of them silently won, and
   `av->nitems` then failed to compile — which is the *good* outcome; the
   dangerous version of this is a shadow that compiles.
3. **The execution backend**, which is the real work — `fork`+`dup2`+`execvp`
   became `spawn`, at two sites. Marked `/* PORT: */` with the reasoning.
4. **Four fork sites** given messages naming the feature instead of
   `perror("fork")`.

## One behavioural difference you should know about

`FOO=1 cmd` sets `FOO` in **this shell** as well as for the command. Spawn
carries no environment block, so the assignment is applied before the spawn
rather than in a child that would have thrown it away afterwards. A Unix shell
scopes it to the command. This is a consequence of having no fork, not an
oversight — and it disappears when fork lands.

## What the fork milestone would take

`fork` here means: allocate a new address space, copy every mapped page,
duplicate the fd table with the reference counts bumped, and return twice. The
kernel already has private address spaces per process (`CR3` each) and the fd
table with pipe reference counting, so the missing piece is the page copy and
the double return. Copy-on-write would be the faster version and a larger job;
an eager copy is simpler and would make all four features above work.
