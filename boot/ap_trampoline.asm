; Project Lariat - Application Processor (AP) trampoline.
;
; Copied to physical 0x8000 at runtime.  An AP starts here in 16-bit real mode
; after a SIPI (CS=0x0800, IP=0).  It transitions real -> protected -> long mode
; reusing the kernel's PML4, then jumps to the 64-bit C entry (ap_main).
;
; A small parameter block lives at a fixed physical address (0x9000) that the
; BSP fills in before sending the SIPI:
;   0x9000 : u32  pml4 physical address (CR3)
;   0x9008 : u64  per-AP stack top
;   0x9010 : u64  64-bit C entry (ap_main)
;   0x9018 : u32  cpu index argument
;   0x901C : u32  "online" flag, set by the AP once it reaches C

%define AP_PARAMS   0x9000
%define P_PML4      (AP_PARAMS + 0x00)
%define P_STACK     (AP_PARAMS + 0x08)
%define P_ENTRY     (AP_PARAMS + 0x10)
%define P_CPUARG    (AP_PARAMS + 0x18)
%define P_ONLINE    (AP_PARAMS + 0x1C)

[bits 16]
[org 0x8000]

ap_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    lgdt [ap_gdt_desc]

    mov eax, cr0
    or  eax, 1
    mov cr0, eax

    jmp dword 0x08:ap_pm32

; ---------------------------------------------------------------------------
[bits 32]
ap_pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov eax, [P_PML4]
    mov cr3, eax

    mov eax, cr4
    or  eax, 1 << 5            ; CR4.PAE
    mov cr4, eax

    mov ecx, 0xC0000080        ; EFER
    rdmsr
    or  eax, 1 << 8            ; LME
    wrmsr

    mov eax, cr0
    or  eax, 1 << 31           ; PG
    mov cr0, eax

    jmp 0x18:ap_lm64

; ---------------------------------------------------------------------------
[bits 64]
ap_lm64:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, [P_STACK]

    ; NOTE: we do NOT signal P_ONLINE here.  The BSP reuses this shared
    ; parameter block (including the stack pointer) for the next AP as soon as
    ; the online flag is set, so the AP must consume every shared field first.
    ; ap_main() sets P_ONLINE only after it is safely on its own stack.
    mov edi, [P_CPUARG]        ; arg0 = cpu index (consumed here)
    mov rax, [P_ENTRY]
    call rax

.hang:
    cli
    hlt
    jmp .hang

; ---------------------------------------------------------------------------
align 8
ap_gdt:
    dq 0x0000000000000000      ; null
    dq 0x00CF9A000000FFFF      ; 0x08 32-bit code
    dq 0x00CF92000000FFFF      ; 0x10 32-bit data
    dq 0x00209A0000000000      ; 0x18 64-bit code
    dq 0x0020920000000000      ; 0x20 64-bit data
ap_gdt_end:

ap_gdt_desc:
    dw ap_gdt_end - ap_gdt - 1
    dd ap_gdt

ap_end:
