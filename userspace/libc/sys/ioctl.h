#ifndef SYS_IOCTL_H
#define SYS_IOCTL_H

#include "libc/unistd.h"

/* ioctl request numbers */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCGWINSZ 0x5413

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

static inline int ioctl(int fd, unsigned long req, long arg) {
    return (int)__syscall_ret(syscall3(SYS_IOCTL, fd, (long)req, arg));
}

static inline int tcsetpgrp(int fd, int pgrp) {
    return ioctl(fd, TIOCSPGRP, (long)&pgrp);
}
static inline int tcgetpgrp(int fd) {
    int pgrp = 0;
    if (ioctl(fd, TIOCGPGRP, (long)&pgrp) < 0) return -1;
    return pgrp;
}

#endif /* SYS_IOCTL_H */
