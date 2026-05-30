; C runtime startup: parse the SysV initial stack and call main(argc, argv, envp).
;
; On entry (set up by the kernel ELF loader):
;   [rsp]            = argc
;   [rsp+8]          = argv[0]
;   ...
;   [rsp+8+argc*8]   = NULL
;   then envp[], NULL.

section .text
global _start
extern main
extern environ

_start:
    mov     rdi, [rsp]              ; argc
    lea     rsi, [rsp + 8]          ; argv
    lea     rdx, [rsi + rdi*8 + 8]  ; envp = argv + (argc + 1) * 8
    mov     [environ], rdx          ; publish envp for getenv/setenv
    xor     rbp, rbp                ; mark outermost frame
    and     rsp, -16                ; 16-byte align the stack
    call    main
    mov     edi, eax                ; exit code = main()'s return value
    mov     eax, 60                 ; SYS_EXIT
    syscall
.hang:
    jmp     .hang

; Signal-return trampoline: a signal handler returns here, which invokes
; rt_sigreturn(15) to have the kernel restore the pre-signal context.
global __lariat_sigreturn
__lariat_sigreturn:
    mov     eax, 15                 ; SYS_RT_SIGRETURN
    syscall
