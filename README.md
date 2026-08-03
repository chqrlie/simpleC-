# simpleC++

gcc -o nano_cc nano_cc.c          # build the compiler<br>
./nano_cc kernel.c kernel.s       # compile your kernel<br>
gcc -nostdlib -T linker.ld kernel.s -o kernel.elf   # link for bare metal<br>

i was hoping to compile this with chibicc ,8cc or lcc minimal kind of compiler <br>

https://github.com/R077A6r1an/stdlib/tree/main for stdlib <br>

<br>
Feature,Status
Full C expression parsing with correct precedence,✅
if / else / while / return,✅
Local + global variables,✅
Functions with up to 6 register args (System V ABI),✅
struct definitions,✅
class with public: / private:,✅
new / delete (C++ sugar),✅
"Pointers, "&", "*,✅
#define and #include preprocessor,✅
"__asm__(""..."")" inline assembly,✅
Macro expansion inside asm strings (e.g. #define VGA 0xB8000 → "__asm__(""mov rdi, VGA"")"),✅
Real x86_64 System V output,✅
