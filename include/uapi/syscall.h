#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>

/* --------------------------------------------------------------------------
 * Syscall numbers.
 *
 * These match the Linux x86_64 ABI so that the door stays open to running
 * Linux-leaning userland later.  We do not implement the full Linux ABI, only
 * this subset, but keeping the numbers identical is free.
 * -------------------------------------------------------------------------- */
#define SYS_READ          0
#define SYS_WRITE         1
#define SYS_OPEN          2
#define SYS_CLOSE         3
#define SYS_STAT          4
#define SYS_FSTAT         5
#define SYS_POLL          7
#define SYS_LSEEK         8
#define SYS_MMAP          9
#define SYS_BRK           12
#define SYS_RT_SIGACTION  13
#define SYS_RT_SIGRETURN  15
#define SYS_IOCTL         16
#define SYS_PIPE          22
#define SYS_SELECT        23
#define SYS_SCHED_YIELD   24
#define SYS_DUP           32
#define SYS_CLONE         56
#define SYS_ARCH_PRCTL    158
#define SYS_GETTID        186
#define SYS_FUTEX         202
#define SYS_SET_TID_ADDRESS 218
#define SYS_EXIT_GROUP    231
#define SYS_DUP2          33
#define SYS_NANOSLEEP     35
#define SYS_GETPID        39
#define SYS_GETUID        102
#define SYS_GETGID        104
#define SYS_SETUID        105
#define SYS_SETGID        106
#define SYS_GETEUID       107
#define SYS_GETEGID       108
#define SYS_SETREUID      113
#define SYS_SETREGID      114
#define SYS_GETGROUPS     115
#define SYS_SETGROUPS     116
#define SYS_SETRESUID     117
#define SYS_GETRESUID     118
#define SYS_SETRESGID     119
#define SYS_GETRESGID     120
#define SYS_SOCKET        41
#define SYS_CONNECT       42
#define SYS_ACCEPT        43
#define SYS_SENDTO        44
#define SYS_RECVFROM      45
#define SYS_BIND          49
#define SYS_LISTEN        50
#define SYS_GETSOCKNAME   51
#define SYS_SETSOCKOPT    54
#define SYS_GETSOCKOPT    55
#define SYS_FORK          57
#define SYS_VFORK         58
#define SYS_EXECVE        59
#define SYS_EXIT          60
#define SYS_WAIT4         61
#define SYS_KILL          62
#define SYS_UNAME         63
#define SYS_FCNTL         72
#define SYS_GETCWD        79
#define SYS_CHDIR         80
#define SYS_MKDIR         83
#define SYS_RMDIR         84
#define SYS_UNLINK        87
#define SYS_GETTIMEOFDAY  96
#define SYS_GETPPID       110
#define SYS_SETPGID       109
#define SYS_GETPGRP       111
#define SYS_SETSID        112
#define SYS_GETPGID       121
#define SYS_GETSID        124
#define SYS_CLOCK_GETTIME 228
#define SYS_GETDENTS64    217

/* Phase 0: memory protection, vectored I/O, positional I/O, and the *at family
 * needed by ported libc (musl) and the dynamic linker. Numbers match Linux. */
#define SYS_MPROTECT      10
#define SYS_MUNMAP        11
#define SYS_RT_SIGPROCMASK 14
#define SYS_PREAD64       17
#define SYS_PWRITE64      18
#define SYS_READV         19
#define SYS_WRITEV        20
#define SYS_ACCESS        21
#define SYS_FSYNC         74
#define SYS_FDATASYNC     75
#define SYS_FTRUNCATE     77
#define SYS_RENAME        82
#define SYS_SYMLINK       88
#define SYS_READLINK      89
#define SYS_CHMOD         90
#define SYS_FCHMOD        91
#define SYS_UMASK         95
#define SYS_GETRLIMIT     97
#define SYS_SETRLIMIT     160
#define SYS_OPENAT        257
#define SYS_NEWFSTATAT    262
#define SYS_FACCESSAT     269
#define SYS_DUP3          292
#define SYS_PIPE2         293
#define SYS_PRLIMIT64     302
#define SYS_GETRANDOM     318

/* Lariat-specific (non-Linux) syscalls live above the Linux range. */
#define SYS_LARIAT_PS     400   /* enumerate processes for ps(1) */

/* Phase M: IPC message ports (Mach-port-like). See include/uapi/lipc.h. */
#define SYS_LARIAT_PORT_CREATE 401  /* register a named port -> id */
#define SYS_LARIAT_PORT_OPEN   402  /* look up a named port -> id */
#define SYS_LARIAT_PORT_SEND   403  /* send datagram to a port */
#define SYS_LARIAT_PORT_RECV   404  /* receive datagram from a port */

/* Legacy aliases kept for in-kernel call sites. */
#define SYS_YIELD    SYS_SCHED_YIELD
#define SYS_SLEEP    SYS_NANOSLEEP
#define SYS_WAITPID  SYS_WAIT4

#define MAX_SYSCALLS 512

void syscall_init(void);

/* Program the per-CPU SYSCALL/SYSRET MSRs on the calling core (called once per
 * CPU: by syscall_init() on the BSP and from ap_main() on each AP). */
void syscall_init_cpu(void);
uint64_t syscall_handler(uint64_t nr, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5,
                         uint64_t arg6);

#endif
