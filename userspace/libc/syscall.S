/* Userspace syscall stubs for x86_64 */

.text
.global _start
.global syscall0
.global syscall1
.global syscall2
.global syscall3
.global syscall4
.global syscall5
.global syscall6

_start:
    call main
    mov %rax, %rdi
    mov $0, %rax        /* SYS_EXIT */
    syscall

syscall0:
    mov %rdi, %rax
    syscall
    ret

syscall1:
    mov %rdi, %rax
    mov %rsi, %rdi
    syscall
    ret

syscall2:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    syscall
    ret

syscall3:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    syscall
    ret

syscall4:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    syscall
    ret

syscall5:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    mov %r9, %r8
    syscall
    ret

syscall6:
    mov %rdi, %rax
    mov %rsi, %rdi
    mov %rdx, %rsi
    mov %rcx, %rdx
    mov %r8, %r10
    mov %r9, %r8
    mov 8(%rsp), %r9
    syscall
    ret
