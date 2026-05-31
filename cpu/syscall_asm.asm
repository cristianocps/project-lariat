; Project Lariat - SYSCALL entry via sysretq (x86_64)
; SYSCALL saves user RIP to RCX and user RFLAGS to R11.
; We save them plus user RSP, swap to the thread's kernel stack, call C handler,
; restore everything, then sysretq back to ring 3.

[bits 64]

section .text

global syscall_entry
extern syscall_handler
extern current_thread

; Offsets into struct thread (must match include/sched.h)
TH_KERNEL_STACK equ 72
TH_TMP_RIP      equ 160
TH_TMP_RFLAGS   equ 168
TH_TMP_RSP      equ 176
TH_SYSCALL_NR   equ 184
TH_USR_RDI      equ 192
TH_USR_RSI      equ 200
TH_USR_RDX      equ 208
TH_USR_R10      equ 216
TH_USR_R8       equ 224
TH_USR_R9       equ 232
TH_USR_RBX      equ 240
TH_USR_RBP      equ 248
TH_USR_R12      equ 256
TH_USR_R13      equ 264
TH_USR_R14      equ 272
TH_USR_R15      equ 280

; ---------------------------------------------------------------------------
; syscall_entry
; ---------------------------------------------------------------------------
syscall_entry:
    ; On entry: rax = syscall nr, rcx = user RIP, r11 = user RFLAGS,
    ; rsp = user RSP, and rdi/rsi/rdx/r10/r8/r9 = the six syscall arguments.
    ;
    ; current_thread() is a C call that may clobber ANY caller-saved register -
    ; in particular the argument registers and the syscall-clobbered RIP/RFLAGS.
    ; (It reads the LAPIC to find the per-CPU `current`, so it is no longer a
    ; trivial load.)  Stash everything we still need on the user stack across the
    ; call.  Callee-saved user regs (rbx, rbp, r12-r15) are preserved by the C
    ; call itself, so only the caller-saved set needs saving here.
    push r11                     ; user RFLAGS
    push rcx                     ; user RIP
    push r9                      ; a6
    push r8                      ; a5
    push r10                     ; a4
    push rdx                     ; a3
    push rsi                     ; a2
    push rdi                     ; a1
    push r15                     ; user r15
    push rax                     ; syscall nr
    call current_thread          ; rax = current thread
    mov  r15, rax                ; r15 = current

    pop  rax                     ; syscall nr
    mov  [r15 + TH_SYSCALL_NR], rax
    pop  rax                     ; user r15
    mov  [r15 + TH_USR_R15], rax
    pop  rdi                     ; a1 (restored into rdi)
    mov  [r15 + TH_USR_RDI], rdi
    pop  rsi                     ; a2
    mov  [r15 + TH_USR_RSI], rsi
    pop  rdx                     ; a3
    mov  [r15 + TH_USR_RDX], rdx
    pop  r10                     ; a4
    mov  [r15 + TH_USR_R10], r10
    pop  r8                      ; a5
    mov  [r15 + TH_USR_R8],  r8
    pop  r9                      ; a6
    mov  [r15 + TH_USR_R9],  r9
    pop  rcx                     ; user RIP
    mov  [r15 + TH_TMP_RIP], rcx
    pop  r11                     ; user RFLAGS
    mov  [r15 + TH_TMP_RFLAGS], r11
    mov  [r15 + TH_TMP_RSP], rsp ; rsp is now back to the original user RSP

    ; Callee-saved user registers survived the C call untouched.
    mov  [r15 + TH_USR_RBX], rbx
    mov  [r15 + TH_USR_RBP], rbp
    mov  [r15 + TH_USR_R12], r12
    mov  [r15 + TH_USR_R13], r13
    mov  [r15 + TH_USR_R14], r14

    ; Swap to the thread's private kernel stack
    mov  rsp, [r15 + TH_KERNEL_STACK]

    ; Save all user registers on kernel stack for restore after handler
    push r9
    push r8
    push r10
    push rdx
    push rsi
    push rdi
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Rearrange syscall args (Linux x86_64 convention -> SysV ABI).
    ; User passed: RAX=nr, RDI=a1, RSI=a2, RDX=a3, R10=a4, R8=a5, R9=a6
    ; C handler:  RDI=nr, RSI=a1, RDX=a2, RCX=a3, R8=a4, R9=a5, stack=a6
    ; The handler has SEVEN parameters, so a6 must be passed on the stack as the
    ; 7th argument.  r9 (which held a6) gets reused for a5, so source a6 from its
    ; saved slot.  An 8-byte pad keeps rsp 16-byte aligned across the call.
    mov  r9, r8                 ; a5 -> r9
    mov  r8, r10                ; a4 -> r8
    mov  rcx, rdx               ; a3 -> rcx
    mov  rdx, rsi               ; a2 -> rdx
    mov  rsi, rdi               ; a1 -> rsi
    mov  rdi, [r15 + TH_SYSCALL_NR] ; nr -> rdi
    sub  rsp, 8                  ; alignment pad (keep 16-byte alignment)
    push qword [r15 + TH_USR_R9] ; a6 -> 7th arg on the stack
    call syscall_handler
    add  rsp, 16                 ; drop a6 + pad

    ; Restore all saved registers (reverse order)
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop rdi
    pop rsi
    pop rdx
    pop r10
    pop r8
    pop r9

    ; At this point r15 = current thread pointer (it was pushed/popped as such),
    ; and every other GPR holds the user's value (rax = syscall return value).

    ; Load user RIP / RFLAGS for sysret (rcx, r11 are clobbered by syscall anyway).
    mov rcx, [r15 + TH_TMP_RIP]
    mov r11, [r15 + TH_TMP_RFLAGS]

    ; Clear DS/ES selectors without clobbering any user GPR.  rax (the return
    ; value) is saved on the kernel stack across the segment loads instead of in
    ; a user register (the previous code used r12 here, corrupting the user's
    ; r12 across every syscall).
    ;
    ; We deliberately do NOT touch FS/GS here: in long mode, loading a selector
    ; into FS/GS zeroes that segment's hidden base, which would wipe the user's
    ; TLS pointer (set via arch_prctl(ARCH_SET_FS)) on every syscall return and
    ; fault the next %fs:0 access.  The kernel never loads kernel data into FS/GS
    ; during syscall handling (per-CPU state is found via the LAPIC id, not GS),
    ; so the user's FS/GS bases are already intact - and the scheduler reloads
    ; FS_BASE from the thread on every context switch (sched_finish_switch).
    push rax
    xor eax, eax
    mov ds, ax
    mov es, ax
    pop rax

    ; Switch to the user stack, then restore the user's real r15 (until now r15
    ; held the thread pointer).  This must be the last use of the thread pointer.
    mov rsp, [r15 + TH_TMP_RSP]
    mov r15, [r15 + TH_USR_R15]

    o64 sysret
