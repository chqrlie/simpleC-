#include <stdio.h>

int rax = 1, rcx = 1, rdx = 1, rbx = 1, rsp = 1, rbp = 1, rsi = 1, rdi = 1, r8 = 1, r9 = 1, r10 = 1, r11 = 1, r12 = 1, r13 = 1, r14 = 1, r15 = 1;
int eax = 1, ecx = 1, edx = 1, ebx = 1, esp = 1, ebp = 1, esi = 1, edi = 1, r8d = 1, r9d = 1, r10d = 1, r11d = 1, r12d = 1, r13d = 1, r14d = 1, r15d = 1;
int ax = 1, cx = 1, dx = 1, bx = 1, sp = 1, bp = 1, si = 1, di = 1, r8w = 1, r9w = 1, r10w = 1, r11w = 1, r12w = 1, r13w = 1, r14w = 1, r15w = 1;
int al = 1, cl = 1, dl = 1, bl = 1, spl = 1, bpl = 1, sil = 1, dil = 1, r8b = 1, r9b = 1, r10b = 1, r11b = 1, r12b = 1, r13b = 1, r14b = 1, r15b = 1;

int main(void) {
    printf("%d\n", rax + rcx + rdx + rbx + rsp + rbp + rsi + rdi + r8 + r9 + r10 + r11 + r12 + r13 + r14 + r15);
    printf("%d\n", eax + ecx + edx + ebx + esp + ebp + esi + edi + r8d + r9d + r10d + r11d + r12d + r13d + r14d + r15d);
    printf("%d\n", ax + cx + dx + bx + sp + bp + si + di + r8w + r9w + r10w + r11w + r12w + r13w + r14w + r15w);
    printf("%d\n", al + cl + dl + bl + spl + bpl + sil + dil + r8b + r9b + r10b + r11b + r12b + r13b + r14b + r15b);
}
