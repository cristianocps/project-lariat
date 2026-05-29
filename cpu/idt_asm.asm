; Project Lariat - IDT and ISR/IRQ stubs (x86_64)

[bits 64]

extern isr_handler

; ---------------------------------------------------------------------------
; Macros
; ---------------------------------------------------------------------------
%macro ISR_NOERRCODE 1
    global isr_%1
isr_%1:
    push 0
    push %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERRCODE 1
    global isr_%1
isr_%1:
    push %1
    jmp isr_common_stub
%endmacro

%macro IRQ 1
    global irq_%1
irq_%1:
    push 0
    push %1 + 32
    jmp isr_common_stub
%endmacro

; ---------------------------------------------------------------------------
; CPU Exceptions (0-31)
; ---------------------------------------------------------------------------
ISR_NOERRCODE 0     ; Divide-by-zero
ISR_NOERRCODE 1     ; Debug
ISR_NOERRCODE 2     ; NMI
ISR_NOERRCODE 3     ; Breakpoint
ISR_NOERRCODE 4     ; Overflow
ISR_NOERRCODE 5     ; Bound Range Exceeded
ISR_NOERRCODE 6     ; Invalid Opcode
ISR_NOERRCODE 7     ; Device Not Available
ISR_ERRCODE   8     ; Double Fault
ISR_NOERRCODE 9     ; Coprocessor Segment Overrun
ISR_ERRCODE   10    ; Invalid TSS
ISR_ERRCODE   11    ; Segment Not Present
ISR_ERRCODE   12    ; Stack-Segment Fault
ISR_ERRCODE   13    ; General Protection Fault
ISR_ERRCODE   14    ; Page Fault
ISR_NOERRCODE 15    ; Reserved
ISR_NOERRCODE 16    ; x87 FPU Floating-Point Error
ISR_ERRCODE   17    ; Alignment Check
ISR_NOERRCODE 18    ; Machine Check
ISR_NOERRCODE 19    ; SIMD Floating-Point Exception
ISR_NOERRCODE 20    ; Virtualization Exception
ISR_ERRCODE   21    ; Control Protection Exception
ISR_NOERRCODE 22    ; Reserved
ISR_NOERRCODE 23    ; Reserved
ISR_NOERRCODE 24    ; Reserved
ISR_NOERRCODE 25    ; Reserved
ISR_NOERRCODE 26    ; Reserved
ISR_NOERRCODE 27    ; Reserved
ISR_NOERRCODE 28    ; Reserved
ISR_ERRCODE   29    ; Reserved
ISR_ERRCODE   30    ; Reserved
ISR_NOERRCODE 31    ; Reserved

; ---------------------------------------------------------------------------
; Hardware IRQs (0-15 -> mapped to 32-47)
; ---------------------------------------------------------------------------
IRQ 0
IRQ 1
IRQ 2
IRQ 3
IRQ 4
IRQ 5
IRQ 6
IRQ 7
IRQ 8
IRQ 9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15

; ---------------------------------------------------------------------------
; Common stub
; ---------------------------------------------------------------------------
isr_common_stub:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    call isr_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    add rsp, 16
    iretq

; ---------------------------------------------------------------------------
; End of IDT stubs
; ---------------------------------------------------------------------------
