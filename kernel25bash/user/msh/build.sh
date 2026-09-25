#!/bin/sh
# Stage miniShell for nano_cc and compile it. Same shape as the cc.elf rule:
# the client's source keeps its POSIX #includes, and they are stripped here.
cd "$(dirname "$0")/../.."
mkdir -p user/mshbuild
cp ../nano-libc.h user/mshbuild/
cp user/os-base.h user/mshbuild/nano-base.h
cp user/msh/msh-compat.h user/mshbuild/
{ echo '#define NANO_CC_INOS 1'
  echo '#include "nano-libc.h"'
  echo '#include "msh-compat.h"'
  awk '/^#include <(stdio|stdlib|string|unistd|fcntl|ctype|signal|dirent|errno)\.h>$/ { next }
       /^#include <sys\/(wait|types|stat)\.h>$/ { next }
       { print }' user/msh/minishell.c
} > user/mshbuild/msh.c
cd user/mshbuild && exec ../../../nano_cc --kernel --bss msh.c msh.s
