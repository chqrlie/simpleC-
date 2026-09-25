// prog.c — a program for the OS to compile and assemble by itself.
//
// It is written for the `cc --minimal --nasm --bss` path, which is a narrower
// world than the ordinary one: the output goes to the bootstrap assembler,
// which produces a single flat image with no linker behind it. Two consequences
// shape this file.
//
// _start MUST BE THE FIRST FUNCTION. The assembler makes the first byte it
// emits the entry point, and nano_cc emits functions in source order. Put
// anything above this and the machine starts executing that instead.
//
// AND THERE IS NO C LIBRARY. Not a small one -- none. Everything below is what
// this program needs and nothing more: a syscall trampoline, strlen, and a
// write. The trampoline is inline assembly because `int 0x80` has no spelling
// in C, and it passes its arguments through globals because nano_cc's __asm__
// has no operand constraints -- the body is handed to the assembler verbatim.

void _start() {
    // No prologue is needed and nano_cc's does no harm: it touches rsp and rbp
    // and leaves rdi and rsi alone, which is what matters, because nano-os put
    // argc and argv in them before the iretq that landed here.
    __asm__(
        "call main\n"
        "mov rdi, rax\n"
        "mov rax, 0\n"         // SYS_EXIT
        "int 0x80\n"
    );
}

// The arguments travel through globals rather than registers so the asm body
// needs no operand constraints. These names must be ones the compiler does not
// rename on the way out.
long _n, _a1, _a2, _a3, _ret;

void _do_syscall() {
    __asm__(
        "mov rax, [rip + _n]\n"
        "mov rdi, [rip + _a1]\n"
        "mov rsi, [rip + _a2]\n"
        "mov rdx, [rip + _a3]\n"
        "int 0x80\n"
        "mov [rip + _ret], rax\n"
    );
}

long sys3(long n, long a, long b, long c) {
    _n = n; _a1 = a; _a2 = b; _a3 = c;
    _do_syscall();
    return _ret;
}

long slen(char *s) {
    long n;
    n = 0;
    while (s[n]) n = n + 1;
    return n;
}

void say(char *s) {
    sys3(1, 1, (long)s, slen(s));      // SYS_WRITE, fd 1
}

char digits[4];

int main(int argc, char **argv) {
    long i;
    long sum;

    say("hello from a program this machine compiled and assembled itself\n");

    // Do some arithmetic, so the exit code is a value that had to be computed
    // rather than a constant sitting in the binary. A loader that ran the
    // wrong bytes would not arrive at 33.
    sum = 0;
    i = 1;
    while (i <= 10) { sum = sum + i; i = i + 1; }   // 55
    sum = sum - 22;                                  // 33

    // argv reached it too: argc is at least 1, and argv[0] is a string.
    if (argc < 1) return 1;
    if (slen(argv[0]) == 0) return 2;

    digits[0] = 'o';
    digits[1] = 'k';
    digits[2] = 10;
    digits[3] = 0;
    say(digits);

    return (int)sum;
}
