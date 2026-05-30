#ifndef LIBC_SIGNAL_H
#define LIBC_SIGNAL_H

#include "libc/unistd.h"

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGKILL  9
#define SIGSEGV  11
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20

#define SIG_DFL  ((void (*)(int))0)
#define SIG_IGN  ((void (*)(int))1)

#define SA_RESTORER 0x04000000

typedef void (*sighandler_t)(int);

struct k_sigaction {
    unsigned long sa_handler;
    unsigned long sa_flags;
    unsigned long sa_restorer;
    unsigned long sa_mask;
};

extern void __lariat_sigreturn(void);

static inline sighandler_t signal(int signum, sighandler_t handler) {
    struct k_sigaction act, old;
    act.sa_handler = (unsigned long)handler;
    act.sa_flags = SA_RESTORER;
    act.sa_restorer = (unsigned long)&__lariat_sigreturn;
    act.sa_mask = 0;
    old.sa_handler = 0;
    long r = syscall3(SYS_RT_SIGACTION, signum, (long)&act, (long)&old);
    if (r < 0) { errno = (int)(-r); return SIG_DFL; }
    return (sighandler_t)old.sa_handler;
}

#endif /* LIBC_SIGNAL_H */
