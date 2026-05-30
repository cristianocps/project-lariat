#ifndef SYS_SOCKET_H
#define SYS_SOCKET_H

#include <stddef.h>
#include "libc/unistd.h"
#include "libc/netinet/in.h"

#define SOCK_STREAM  1
#define SOCK_DGRAM   2

#define SOL_SOCKET   1
#define SO_REUSEADDR 2

static inline int socket(int domain, int type, int proto) {
    return (int)__syscall_ret(syscall3(SYS_SOCKET, domain, type, proto));
}

static inline int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    return (int)__syscall_ret(syscall3(SYS_BIND, fd, (long)addr, len));
}

static inline int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    return (int)__syscall_ret(syscall3(SYS_CONNECT, fd, (long)addr, len));
}

static inline int listen(int fd, int backlog) {
    return (int)__syscall_ret(syscall2(SYS_LISTEN, fd, backlog));
}

static inline int accept(int fd, struct sockaddr *addr, socklen_t *len) {
    return (int)__syscall_ret(syscall3(SYS_ACCEPT, fd, (long)addr, (long)len));
}

static inline ssize_t sendto(int fd, const void *buf, size_t len, int flags,
                             const struct sockaddr *addr, socklen_t alen) {
    return (ssize_t)__syscall_ret(syscall6(SYS_SENDTO, fd, (long)buf, len,
                                           flags, (long)addr, alen));
}

static inline ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                               struct sockaddr *addr, socklen_t *alen) {
    return (ssize_t)__syscall_ret(syscall6(SYS_RECVFROM, fd, (long)buf, len,
                                           flags, (long)addr, (long)alen));
}

static inline ssize_t send(int fd, const void *buf, size_t len, int flags) {
    return sendto(fd, buf, len, flags, (const struct sockaddr *)0, 0);
}

static inline ssize_t recv(int fd, void *buf, size_t len, int flags) {
    return recvfrom(fd, buf, len, flags, (struct sockaddr *)0, (socklen_t *)0);
}

static inline int getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
    return (int)__syscall_ret(syscall3(SYS_GETSOCKNAME, fd, (long)addr, (long)len));
}

static inline int setsockopt(int fd, int level, int opt, const void *val, socklen_t len) {
    return (int)__syscall_ret(syscall5(SYS_SETSOCKOPT, fd, level, opt, (long)val, len));
}

static inline int getsockopt(int fd, int level, int opt, void *val, socklen_t *len) {
    return (int)__syscall_ret(syscall5(SYS_GETSOCKOPT, fd, level, opt, (long)val, (long)len));
}

#endif /* SYS_SOCKET_H */
