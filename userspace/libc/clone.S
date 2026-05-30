; __clone - low-level thread creation used by pthread_create.
;
;   long __clone(unsigned long flags, void *child_stack_top,
;                int *ptid, int *ctid, void *tls, void (*entry)(void));
;     rdi = flags
;     rsi = child_stack_top  (highest address of the new stack)
;     rdx = ptid
;     rcx = ctid
;     r8  = tls              (FS base for the new thread)
;     r9  = entry            (called in the child; must not return)
;
; In the parent this behaves like a normal syscall and returns the new tid.
; In the child it switches to the brand-new stack (so it must NOT touch the
; parent's stack) and jumps to `entry`, which never comes back.

section .text
global __clone

__clone:
    ; Stash the entry point on the child's stack so the child can recover it
    ; after the syscall (every other register is clobbered by the syscall ABI).
    and     rsi, -16                ; 16-byte align the child stack
    sub     rsi, 16
    mov     [rsi], r9               ; child_stack[0] = entry

    ; Marshal the SYSCALL arguments: clone(flags, stack, ptid, ctid, tls).
    ; Kernel ABI: rdi=flags, rsi=stack, rdx=ptid, r10=ctid, r8=tls.
    mov     r10, rcx                ; ctid -> r10
    ; rdi (flags), rsi (stack), rdx (ptid), r8 (tls) are already in place.
    mov     eax, 56                 ; SYS_CLONE
    syscall

    test    rax, rax
    jz      .child
    ret                             ; parent: rax = child tid or -errno

.child:
    xor     rbp, rbp                ; outermost frame
    pop     r9                      ; recover entry; rsp == child_top-8 (ABI: %16==8)
    jmp     r9                      ; run the thread; never returns
