#ifndef LIBC_TERMIOS_H
#define LIBC_TERMIOS_H

#include "libc/unistd.h"
#include "libc/sys/ioctl.h"

typedef unsigned int  tcflag_t;
typedef unsigned char cc_t;

#define NCCS 19
struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
};

/* c_cc subscripts */
#define VINTR   0
#define VQUIT   1
#define VERASE  2
#define VKILL   3
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define VSUSP  10

/* c_iflag */
#define ICRNL  0x0100
/* c_oflag */
#define OPOST  0x0001
#define ONLCR  0x0004
/* c_lflag */
#define ISIG   0x0001
#define ICANON 0x0002
#define ECHO   0x0008
#define ECHOE  0x0010

/* tcsetattr actions (accepted, applied immediately) */
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

static inline int tcgetattr(int fd, struct termios *t) {
    return ioctl(fd, TCGETS, (long)t);
}
static inline int tcsetattr(int fd, int actions, const struct termios *t) {
    (void)actions;
    return ioctl(fd, TCSETS, (long)t);
}

#endif /* LIBC_TERMIOS_H */
