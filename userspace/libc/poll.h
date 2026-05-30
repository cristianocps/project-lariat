#ifndef LIBC_POLL_H
#define LIBC_POLL_H

#include "libc/unistd.h"

#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

struct pollfd {
    int   fd;
    short events;
    short revents;
};

typedef unsigned long nfds_t;

static inline int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    return (int)__syscall_ret(syscall3(SYS_POLL, (long)fds, (long)nfds, timeout));
}

#endif /* LIBC_POLL_H */
