; Project Lariat - Context switch (x86_64)

[bits 64]

section .text

global switch_thread

; ---------------------------------------------------------------------------
; switch_thread(uint64_t *old_rsp, uint64_t new_rsp)
;
; Save callee-saved registers on current stack, save RSP to *old_rsp,
; load new_rsp, restore callee-saved registers, and return.
; This effectively switches execution to another thread.
; ---------------------------------------------------------------------------
switch_thread:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp      ; *old_rsp = current RSP
    mov rsp, rsi        ; RSP = new_rsp

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx

    ret
