; Project Lariat - Enter ring-3 via iretq (x86_64)

[bits 64]

section .text

global enter_userspace

; ---------------------------------------------------------------------------
; enter_userspace(uint64_t rip, uint64_t rsp)
;
; Build an interrupt frame on the current kernel stack and iretq into ring 3.
; This function does NOT return.
; ---------------------------------------------------------------------------
enter_userspace:
    ; Debug: print 'E' to serial
    mov dx, 0x3FD
.wait1:
    in al, dx
    test al, 0x20
    jz .wait1
    mov al, 'E'
    mov dx, 0x3F8
    out dx, al

    ; SS = user data segment | RPL=3
    push 0x20 + 3
    ; RSP = user stack pointer
    push rsi
    ; RFLAGS = interrupts enabled
    push 0x202
    ; CS = user code 64-bit segment | RPL=3
    push 0x28 + 3
    ; RIP = user entry point
    push rdi

    ; Debug: print 'I' to serial
    mov dx, 0x3FD
.wait2:
    in al, dx
    test al, 0x20
    jz .wait2
    mov al, 'I'
    mov dx, 0x3F8
    out dx, al

    ; Clear kernel segment registers so user space doesn't inherit them
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Zero general-purpose registers so userspace doesn't inherit kernel state
    xor rbx, rbx
    xor rbp, rbp
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15
    xor rax, rax
    xor rcx, rcx
    xor rdx, rdx
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor rdi, rdi
    xor rsi, rsi

    iretq
