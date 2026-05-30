#ifndef SYS_SELECT_H
#define SYS_SELECT_H

#include "libc/unistd.h"
#include "libc/time.h"

#define FD_SETSIZE 256

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

static inline void FD_ZERO(fd_set *s) {
    for (unsigned i = 0; i < sizeof(s->fds_bits) / sizeof(s->fds_bits[0]); i++)
        s->fds_bits[i] = 0;
}
static inline void FD_SET(int fd, fd_set *s) {
    s->fds_bits[fd / (8 * sizeof(unsigned long))] |=
        (1ul << (fd % (8 * sizeof(unsigned long))));
}
static inline void FD_CLR(int fd, fd_set *s) {
    s->fds_bits[fd / (8 * sizeof(unsigned long))] &=
        ~(1ul << (fd % (8 * sizeof(unsigned long))));
}
static inline int FD_ISSET(int fd, fd_set *s) {
    return (s->fds_bits[fd / (8 * sizeof(unsigned long))] >>
            (fd % (8 * sizeof(unsigned long)))) & 1ul;
}

static inline int select(int nfds, fd_set *r, fd_set *w, fd_set *e,
                         struct timeval *timeout) {
    return (int)__syscall_ret(syscall5(SYS_SELECT, nfds, (long)r, (long)w,
                                       (long)e, (long)timeout));
}

#endif /* SYS_SELECT_H */
