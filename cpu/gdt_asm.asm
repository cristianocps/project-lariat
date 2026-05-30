; Project Lariat - GDT/TSS flush routines (x86_64)

[bits 64]

section .text

global gdt_flush
global tss_flush

; ---------------------------------------------------------------------------
; gdt_flush(uint64_t gdt_ptr)
; Load new GDT and reload segment registers.
; ---------------------------------------------------------------------------
gdt_flush:
    lgdt [rdi]

    ; Reload CS via far return
    push 0x08           ; kernel code segment
    lea rax, [rel .reload_cs]
    push rax
    retfq

.reload_cs:
    ; Reload data segments
    mov ax, 0x10        ; kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

; ---------------------------------------------------------------------------
; tss_flush(uint16_t sel)
; Load task register with the TSS selector.
; ---------------------------------------------------------------------------
tss_flush:
    ltr di
    ret
