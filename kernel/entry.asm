; Project Lariat - 64-bit Kernel Entry

[bits 64]
[extern kmain]
[extern pmm_init_e820]

section .text

global _start
_start:
    ; RDI = e820 entry count
    ; RSI = e820 buffer physical address
    ; Save them before zeroing BSS
    mov r12, rdi
    mov r13, rsi

    ; Zero BSS section
    extern __bss_start
    extern __bss_end
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    xor rax, rax
    rep stosb

    ; Restore e820 params
    mov rdi, r12
    mov rsi, r13

    ; Initialize physical memory manager from e820 map
    call pmm_init_e820

    ; Set up kernel stack
    mov rsp, 0x300000

    ; Call kernel main
    call kmain

    ; Halt if kernel returns
    cli
.halt:
    hlt
    jmp .halt
