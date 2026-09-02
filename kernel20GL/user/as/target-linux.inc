; ==== TARGET BEGIN ====
; ==============================================================================
; TARGET: LINUX
; ==============================================================================
; Everything the assembler needs from the operating system it RUNS on lives in
; this block, and nothing outside it issues a syscall or knows a syscall
; number. `tools/retarget.py` swaps everything between the two TARGET markers
; for another block -- fixed/target-nanoos.inc -- and produces the same
; assembler, from the same body, running on nano-os instead.
;
; That is a split worth being deliberate about, because the alternative is two
; copies of a 1,900-line assembler that drift apart -- and a divergence between
; them would show up as a miscompilation on one platform and not the other,
; which is close to the worst shape a bug can have.
;
; Note the OTHER axis, which is independent of this one: which OS the assembler
; runs on has nothing to do with which base address it EMITS for. That is the
; -b flag, and it is why building a nano-os assembler on Linux is possible at
; all -- the Linux build, told -b 0x8000000000, produces the nano-os binary.
;
; The four routines and their contracts:
;   os_read_input   rdi = path                  -> rax = bytes read into in_buf,
;                                                  or negative if unreadable
;   os_write_output rdi = path, rsi = buf,
;                   rdx = length                -> rax = 0, or negative
;   os_err          rsi = buf, rdx = length     -> writes to stderr
;   os_exit         rdi = status                -> does not return
;
; and one constant, def_base: where output goes when -b is not given. That is a
; property of the machine you would normally be assembling FOR, so it belongs
; with the target rather than with the assembler.
; ==============================================================================

def_base equ 0x400000

_start:
    ; Linux puts argc at [rsp] and argv immediately above it. This has to be
    ; the very first thing that runs: a `call` would put a return address
    ; exactly where argc is.
    mov rax, [rsp]
    mov [g_argc], rax
    mov rax, rsp
    add rax, 8
    mov [g_argv], rax
    jmp main_flow

os_read_input:
    mov [t_path], rdi
    mov rax, 2                  ; sys_open
    mov rdi, [t_path]
    xor rsi, rsi                ; O_RDONLY
    xor rdx, rdx
    syscall
    test rax, rax
    js .fail
    mov [t_fd], rax
    mov rax, 0                  ; sys_read
    mov rdi, [t_fd]
    lea rsi, [rip + in_buf]
    mov rdx, buf_size
    syscall
    mov [t_n], rax
    mov rax, 3                  ; sys_close
    mov rdi, [t_fd]
    syscall
    mov rax, [t_n]
    ret
.fail:
    mov rax, -1
    ret

os_write_output:
    mov [t_path], rdi
    mov [t_buf], rsi
    mov [t_n], rdx
    mov rax, 2                  ; sys_open
    mov rdi, [t_path]
    mov rsi, 577                ; O_WRONLY | O_CREAT | O_TRUNC
    mov rdx, 420                ; 0644
    syscall
    test rax, rax
    js .fail
    mov [t_fd], rax
    mov rax, 1                  ; sys_write
    mov rdi, [t_fd]
    mov rsi, [t_buf]
    mov rdx, [t_n]
    syscall
    mov [t_n], rax
    mov rax, 3                  ; sys_close
    mov rdi, [t_fd]
    syscall
    xor rax, rax
    ret
.fail:
    mov rax, -1
    ret

os_err:
    mov rax, 1                  ; sys_write
    mov rdi, 2                  ; stderr
    syscall
    ret

os_exit:
    mov rax, 60
    syscall
; ==== TARGET END ====
