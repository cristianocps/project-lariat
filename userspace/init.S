; Userspace entry point (_start)
; Calls main() and then exits with its return code.

section .text
global _start
extern main
_start:
    call main
    ; main return value in rax → exit code
    mov rdi, rax
    mov rax, 60         ; SYS_EXIT (Linux x86_64)
    syscall
