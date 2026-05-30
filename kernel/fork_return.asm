; Project Lariat - fork_return: first entry into a forked child process
; Called by switch_thread when a newly-forked child is scheduled for the
; first time.  Restores all user registers and returns to ring-3 via iretq.

[bits 64]

section .text

global fork_return_asm
extern current_thread
extern fork_return_log
extern sched_finish_switch

; Offsets into struct thread (must match include/sched.h)
TH_CR3        equ 64
TH_FORK_RIP   equ 128
TH_FORK_RSP   equ 144
TH_FORK_RAX   equ 152
TH_SYSCALL_NR equ 184
TH_TMP_RIP    equ 160
TH_TMP_RFLAGS equ 168
TH_TMP_RSP    equ 176
TH_USR_RDI    equ 192
TH_USR_RSI    equ 200
TH_USR_RDX    equ 208
TH_USR_R10    equ 216
TH_USR_R8     equ 224
TH_USR_R9     equ 232
TH_USR_RBX    equ 240
TH_USR_RBP    equ 248
TH_USR_R12    equ 256
TH_USR_R13    equ 264
TH_USR_R14    equ 272
TH_USR_R15    equ 280

; ---------------------------------------------------------------------------
; fork_return_asm(void)
; ---------------------------------------------------------------------------
fork_return_asm:
    ; The CPU that scheduled us left sched_lock held; release it first.  This
    ; restores the child's baseline RFLAGS (IF=0), then we keep IRQs masked
    ; until the iretq below loads RFLAGS=0x202 for ring 3.
    call sched_finish_switch
    cli
    call fork_return_log

    ; Get current thread pointer into r15 (per-CPU, found via LAPIC id).
    call current_thread
    mov  r15, rax

    ; Build the iretq frame first, using the saved fork RIP/RSP.  Doing this
    ; before restoring the GPRs means every general-purpose register can be set
    ; to its saved user value right before iretq (fork must preserve ALL of the
    ; parent's registers in the child, except rax which becomes 0).
    mov  rax, [r15 + TH_FORK_RSP]
    mov  rcx, [r15 + TH_FORK_RIP]
    push qword 0x23             ; SS = user data + RPL3
    push qword rax              ; RSP = fork_rsp
    push qword 0x202            ; RFLAGS (IF=1)
    push qword 0x2B             ; CS = user code64 + RPL3
    push qword rcx              ; RIP = fork_rip

    ; Restore every user GPR from current->usr_* (r15 loaded last).  rcx/r11 are
    ; clobbered by the SYSCALL convention anyway, so they need no restore.
    mov  rdi, [r15 + TH_USR_RDI]
    mov  rsi, [r15 + TH_USR_RSI]
    mov  rdx, [r15 + TH_USR_RDX]
    mov  r8,  [r15 + TH_USR_R8]
    mov  r9,  [r15 + TH_USR_R9]
    mov  r10, [r15 + TH_USR_R10]
    mov  rbx, [r15 + TH_USR_RBX]
    mov  rbp, [r15 + TH_USR_RBP]
    mov  r12, [r15 + TH_USR_R12]
    mov  r13, [r15 + TH_USR_R13]
    mov  r14, [r15 + TH_USR_R14]
    xor  rax, rax               ; child sees fork() == 0
    mov  r15, [r15 + TH_USR_R15]
    iretq
