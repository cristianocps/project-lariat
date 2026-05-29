; Project Lariat - 64-bit Boot Sector
; Transitions: 16-bit real -> 32-bit protected -> 64-bit long mode
; Passes e820 memory map to kernel at 0x5000

[bits 16]
[org 0x7C00]

start:
    jmp 0x0000:.set_cs
.set_cs:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    ; Save boot drive
    mov [boot_drive], dl

    ; Print loading message
    mov si, msg_loading
    call print_string

    ; Query e820 memory map
    call query_e820

    ; Load kernel from disk (sectors 2-129 = 128 sectors = 64KB) to 0x7E00
    mov ah, 0x02
    mov al, KERNEL_SECTORS
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov dl, [boot_drive]
    mov bx, KERNEL_OFFSET
    int 0x13
    jc disk_error

    ; Switch to 32-bit protected mode
    cli
    lgdt [gdt32_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp CODE32_SEG:protected_mode_32

; ---------------------------------------------------------------------------
; 16-bit routines
; ---------------------------------------------------------------------------
print_string:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp print_string
.done:
    ret

disk_error:
    mov si, msg_disk_error
    call print_string
    jmp $

; ---------------------------------------------------------------------------
; e820 memory map query (INT 0x15, AX=0xE820)
; Stores entries at E820_BUFFER, count at E820_COUNT_ADDR
; Each entry: base(8) + length(8) + type(4) = 20 bytes
; ---------------------------------------------------------------------------
query_e820:
    xor ebx, ebx                ; Clear continuation value
    mov edi, E820_BUFFER        ; Destination buffer
    mov edx, 0x534D4150         ; 'SMAP' signature
    mov ecx, 24                 ; Request 24 bytes per entry

.e820_loop:
    mov eax, 0xE820
    int 0x15
    jc .e820_done               ; CF set = error or end
    cmp eax, 0x534D4150         ; Verify signature
    jne .e820_done

    ; Store entry (24 bytes: base 8, length 8, type 4, extended 4)
    ; We only need 20 bytes but BIOS may return 24
    add edi, 24
    cmp ebx, 0
    je .e820_done
    cmp edi, E820_BUFFER + (E820_MAX_ENTRIES * 24)
    jb .e820_loop

.e820_done:
    ; Calculate and store entry count
    mov eax, edi
    sub eax, E820_BUFFER
    xor edx, edx
    mov ecx, 24
    div ecx
    mov [E820_COUNT_ADDR], eax
    ret

; ---------------------------------------------------------------------------
; 32-bit protected mode
; ---------------------------------------------------------------------------
[bits 32]
protected_mode_32:
    mov ax, DATA32_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    ; Copy kernel from 0x7E00 to 0x100000 (1MB)
    mov esi, KERNEL_OFFSET
    mov edi, KERNEL_TARGET
    mov ecx, KERNEL_SIZE
    rep movsb

    ; Set up identity paging with 2MB huge pages (first 4MB)
    ; Clear page table area (0x1000 - 0x4FFF)
    mov edi, 0x1000
    mov cr3, edi
    xor eax, eax
    mov ecx, 4096
    rep stosd
    mov edi, 0x1000

    ; PML4[0] -> PDPT at 0x2000
    mov dword [edi], 0x2003
    add edi, 0x1000

    ; PDPT[0] -> PD at 0x3000
    mov dword [edi], 0x3003
    add edi, 0x1000

    ; PD[0] -> 2MB huge page at 0x000000 (PS=1, Present, R/W)
    mov dword [edi], 0x83
    add edi, 8
    ; PD[1] -> 2MB huge page at 0x200000
    mov dword [edi], 0x200083

    ; Enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Enable long mode (EFER.LME)
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; Enable paging
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; Far jump to 64-bit code
    jmp CODE64_SEG:long_mode

; ---------------------------------------------------------------------------
; 64-bit long mode
; ---------------------------------------------------------------------------
[bits 64]
long_mode:
    mov ax, DATA64_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, 0x200000

    ; Pass e820 info to kernel via registers:
    ; RDI = entry count (32-bit, zero-extended), RSI = buffer physical address
    mov edi, [E820_COUNT_ADDR]
    mov rsi, E820_BUFFER

    ; Jump to kernel
    mov rax, KERNEL_TARGET
    call rax

    cli
.halt:
    hlt
    jmp .halt

; ---------------------------------------------------------------------------
; Data
; ---------------------------------------------------------------------------
[bits 16]
msg_loading:    db "Lariat: loading kernel...", 0x0D, 0x0A, 0
msg_disk_error: db "Lariat: disk error!", 0
boot_drive:     db 0

E820_COUNT_ADDR  equ 0x6000
E820_BUFFER      equ 0x6010
E820_MAX_ENTRIES equ 64

; ---------------------------------------------------------------------------
; GDT
; ---------------------------------------------------------------------------
align 8
gdt32_start:
    dq 0x0000000000000000          ; Null descriptor
gdt32_code:
    dq 0x00CF9A000000FFFF          ; 32-bit code: base=0, limit=4GB, execute/read
gdt32_data:
    dq 0x00CF92000000FFFF          ; 32-bit data: base=0, limit=4GB, read/write
gdt64_code:
    dq 0x00209A0000000000          ; 64-bit code: long mode, execute/read, present
gdt64_data:
    dq 0x0020920000000000          ; 64-bit data: read/write, present
gdt32_end:

gdt32_descriptor:
    dw gdt32_end - gdt32_start - 1
    dd gdt32_start

; Segment selectors
CODE32_SEG equ gdt32_code - gdt32_start
DATA32_SEG equ gdt32_data - gdt32_start
CODE64_SEG equ gdt64_code - gdt32_start
DATA64_SEG equ gdt64_data - gdt32_start

; ---------------------------------------------------------------------------
; Constants
; ---------------------------------------------------------------------------
KERNEL_OFFSET  equ 0x7E00
KERNEL_TARGET  equ 0x100000
KERNEL_SIZE    equ 65536           ; 64KB max
KERNEL_SECTORS equ 127             ; 127 * 512 = 65024 (some BIOS limit)

; Boot signature
 times 510-($-$$) db 0
dw 0xAA55
