#ifndef UNISTD_H
#define UNISTD_H

#include <stdint.h>
#include <stddef.h>

typedef long ssize_t;
typedef long off_t;

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Syscall numbers (Linux x86_64 ABI subset) */
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
#define SYS_EXECVE        59
#define SYS_EXIT          60
#define SYS_WAIT4         61
#define SYS_KILL          62
#define SYS_UNAME         63
#define SYS_FCNTL         72
#define SYS_GETTIMEOFDAY  96
#define SYS_CLOCK_GETTIME 228
#define SYS_GETCWD        79
#define SYS_CHDIR         80
#define SYS_MKDIR         83
#define SYS_RMDIR         84
#define SYS_UNLINK        87
#define SYS_GETPPID       110
#define SYS_SETPGID       109
#define SYS_GETPGRP       111
#define SYS_SETSID        112
#define SYS_GETPGID       121
#define SYS_GETSID        124
#define SYS_GETDENTS64    217
#define SYS_GETUID        102
#define SYS_GETGID        104
#define SYS_SETUID        105
#define SYS_SETGID        106
#define SYS_GETEUID       107
#define SYS_GETEGID       108
#define SYS_GETGROUPS     115
#define SYS_SETGROUPS     116
#define SYS_SETRESUID     117
#define SYS_SETRESGID     119
#define SYS_LARIAT_PS     400

/* Legacy aliases */
#define SYS_YIELD    SYS_SCHED_YIELD
#define SYS_WAITPID  SYS_WAIT4

static inline long syscall0(long n) {
    long ret;
    __asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n) : "rcx","r11","r8","r9","r10","memory");
    return ret;
}

static inline long syscall1(long n, long a1) {
    long ret;
    __asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n), "D"(a1) : "rcx","r11","r8","r9","r10","memory");
    return ret;
}

static inline long syscall2(long n, long a1, long a2) {
    long ret;
    __asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2) : "rcx","r11","r8","r9","r10","memory");
    return ret;
}

static inline long syscall3(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx","r11","r8","r9","r10","memory");
    return ret;
}

static inline long syscall4(long n, long a1, long a2, long a3, long a4) {
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx","r11","r8","r9","memory");
    return ret;
}

static inline long syscall5(long n, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    __asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8) : "rcx","r11","r9","memory");
    return ret;
}

static inline long syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ __volatile__ ("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9) : "rcx","r11","memory");
    return ret;
}

/* errno translation: kernel returns -errno in [-4095, -1] on failure. */
extern int errno;

static inline long __syscall_ret(long r) {
    if (r < 0 && r > -4096) {
        errno = (int)(-r);
        return -1;
    }
    return r;
}

/* Wrappers */
static inline ssize_t write(int fd, const void *buf, size_t count) {
    return (ssize_t)__syscall_ret(syscall3(SYS_WRITE, fd, (long)buf, count));
}

static inline ssize_t read(int fd, void *buf, size_t count) {
    return (ssize_t)__syscall_ret(syscall3(SYS_READ, fd, (long)buf, count));
}

static inline int open(const char *pathname, int flags) {
    return (int)__syscall_ret(syscall2(SYS_OPEN, (long)pathname, flags));
}

static inline int close(int fd) {
    return (int)__syscall_ret(syscall1(SYS_CLOSE, fd));
}

static inline off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)__syscall_ret(syscall3(SYS_LSEEK, fd, offset, whence));
}

static inline void _exit(int code) {
    syscall1(SYS_EXIT, code);
}

static inline void yield(void) {
    syscall0(SYS_SCHED_YIELD);
}

static inline int fork(void) {
    return (int)__syscall_ret(syscall0(SYS_FORK));
}

static inline int waitpid(int pid, int *status, int options) {
    return (int)__syscall_ret(syscall4(SYS_WAIT4, pid, (long)status, options, 0));
}

static inline int execve(const char *pathname, char *const argv[], char *const envp[]) {
    return (int)__syscall_ret(syscall3(SYS_EXECVE, (long)pathname, (long)argv, (long)envp));
}

static inline int getpid(void)  { return (int)syscall0(SYS_GETPID); }
static inline int gettid(void)  { return (int)syscall0(SYS_GETTID); }
static inline int getppid(void) { return (int)syscall0(SYS_GETPPID); }

/* futex(2): only FUTEX_WAIT/FUTEX_WAKE are implemented by the kernel. */
#define FUTEX_WAIT         0
#define FUTEX_WAKE         1
#define FUTEX_PRIVATE_FLAG 128
static inline int futex(int *uaddr, int op, int val, void *timeout) {
    return (int)syscall4(SYS_FUTEX, (long)uaddr, op, val, (long)timeout);
}

/* arch_prctl(2): set/get the FS base used for thread-local storage. */
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
static inline int arch_prctl(int code, unsigned long addr) {
    return (int)__syscall_ret(syscall2(SYS_ARCH_PRCTL, code, (long)addr));
}

static inline int set_tid_address(int *tidptr) {
    return (int)syscall1(SYS_SET_TID_ADDRESS, (long)tidptr);
}
static inline int setpgid(int pid, int pgid) { return (int)__syscall_ret(syscall2(SYS_SETPGID, pid, pgid)); }
static inline int getpgid(int pid) { return (int)__syscall_ret(syscall1(SYS_GETPGID, pid)); }
static inline int getpgrp(void) { return (int)syscall0(SYS_GETPGRP); }
static inline int setsid(void) { return (int)__syscall_ret(syscall0(SYS_SETSID)); }
static inline int getsid(int pid) { return (int)__syscall_ret(syscall1(SYS_GETSID, pid)); }
static inline int chdir(const char *path) { return (int)__syscall_ret(syscall1(SYS_CHDIR, (long)path)); }
static inline char *getcwd(char *buf, size_t size) {
    long r = __syscall_ret(syscall2(SYS_GETCWD, (long)buf, (long)size));
    return (r < 0) ? (char *)0 : buf;
}
static inline int dup(int fd)   { return (int)__syscall_ret(syscall1(SYS_DUP, fd)); }
static inline int dup2(int o, int n) { return (int)__syscall_ret(syscall2(SYS_DUP2, o, n)); }
static inline int pipe(int fds[2]) { return (int)__syscall_ret(syscall1(SYS_PIPE, (long)fds)); }
static inline int mkdir(const char *p, int mode) { return (int)__syscall_ret(syscall2(SYS_MKDIR, (long)p, mode)); }
static inline int rmdir(const char *p) { return (int)__syscall_ret(syscall1(SYS_RMDIR, (long)p)); }
static inline int unlink(const char *p) { return (int)__syscall_ret(syscall1(SYS_UNLINK, (long)p)); }
static inline int kill(int pid, int sig) { return (int)__syscall_ret(syscall2(SYS_KILL, pid, sig)); }
static inline void *sbrk_set(unsigned long addr) { return (void *)syscall1(SYS_BRK, (long)addr); }

/* mmap (anonymous memory only in this kernel). */
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

static inline void *mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
    return (void *)syscall6(SYS_MMAP, (long)addr, (long)len, prot, flags, fd, off);
}

static inline int fcntl(int fd, int cmd, long arg) {
    return (int)__syscall_ret(syscall3(SYS_FCNTL, fd, cmd, arg));
}

/* uname(2) */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

static inline int uname(struct utsname *u) {
    return (int)__syscall_ret(syscall1(SYS_UNAME, (long)u));
}

/* getdents64(2) directory entry */
struct dirent64 {
    unsigned long  d_ino;
    long           d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

static inline long getdents64(int fd, void *dirp, size_t count) {
    return __syscall_ret(syscall3(SYS_GETDENTS64, fd, (long)dirp, count));
}

/* --------------------------------------------------------------------------
 * Credentials (M10)
 * -------------------------------------------------------------------------- */
typedef unsigned int uid_t;
typedef unsigned int gid_t;

static inline uid_t getuid(void)  { return (uid_t)syscall0(SYS_GETUID); }
static inline uid_t geteuid(void) { return (uid_t)syscall0(SYS_GETEUID); }
static inline gid_t getgid(void)  { return (gid_t)syscall0(SYS_GETGID); }
static inline gid_t getegid(void) { return (gid_t)syscall0(SYS_GETEGID); }

static inline int setuid(uid_t uid) {
    return (int)__syscall_ret(syscall1(SYS_SETUID, (long)uid));
}
static inline int setgid(gid_t gid) {
    return (int)__syscall_ret(syscall1(SYS_SETGID, (long)gid));
}
static inline int setresuid(uid_t r, uid_t e, uid_t s) {
    return (int)__syscall_ret(syscall3(SYS_SETRESUID, (long)r, (long)e, (long)s));
}
static inline int setresgid(gid_t r, gid_t e, gid_t s) {
    return (int)__syscall_ret(syscall3(SYS_SETRESGID, (long)r, (long)e, (long)s));
}
static inline int setgroups(int n, const gid_t *list) {
    return (int)__syscall_ret(syscall2(SYS_SETGROUPS, n, (long)list));
}
static inline int getgroups(int n, gid_t *list) {
    return (int)__syscall_ret(syscall2(SYS_GETGROUPS, n, (long)list));
}

#endif /* UNISTD_H */
