#ifndef UAPI_H
#define UAPI_H

#include <stdint.h>

/* --------------------------------------------------------------------------
 * Userspace/kernel shared ABI structures.
 *
 * The syscall ABI is byte-for-byte Linux x86_64 so that binaries produced by
 * the x86_64-linux-musl toolchain (and any other Linux-ABI userland we import)
 * run unmodified. struct kstat below is the exact layout the stat/fstat/
 * newfstatat syscalls must fill on Linux x86_64.
 * -------------------------------------------------------------------------- */

struct kstat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;

    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;

    int64_t  st_atime;
    int64_t  st_atime_nsec;
    int64_t  st_mtime;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime;
    int64_t  st_ctime_nsec;
    int64_t  __unused[3];
};

struct dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

/* d_type values */
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

/* clockid_t values */
#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1

/* poll(2) */
struct pollfd {
    int   fd;
    short events;
    short revents;
};

/* mmap protection / flags (subset) */
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* --------------------------------------------------------------------------
 * Framebuffer (/dev/fb0) and input (/dev/input) ABI - M11
 * -------------------------------------------------------------------------- */
struct fb_var_info {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;     /* bytes per scanline */
    uint64_t size;      /* total framebuffer bytes */
};

#define FBIOGET_INFO 0x4600

/* Unified input event (keyboard + mouse), read from /dev/input. */
struct input_event {
    uint32_t type;      /* EV_* */
    uint32_t code;      /* key/button code or axis */
    int32_t  value;     /* 1=press 0=release, or relative delta */
};

#define EV_KEY  1       /* keyboard key or mouse button */
#define EV_REL  2       /* relative axis (mouse motion) */

#define REL_X 0
#define REL_Y 1

/* Mouse button codes (avoid clashing with ASCII key codes). */
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

/* signals (subset) */
#define SIGHUP   1
#define SIGINT   2
#define SIGKILL  9
#define SIGSEGV  11
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGQUIT   3
#define SIG_DFL  0
#define SIG_IGN  1

struct ksigaction {
    uint64_t sa_handler;
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;
};

/* uname(2) */
struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

/* --------------------------------------------------------------------------
 * termios / TTY (subset of the Linux ABI)
 * -------------------------------------------------------------------------- */
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
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSUSP   10

/* c_iflag bits */
#define ICRNL   0x0100
/* c_oflag bits */
#define OPOST   0x0001
#define ONLCR   0x0004
/* c_lflag bits */
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0008
#define ECHOE   0x0010

/* ioctl request numbers */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCGWINSZ  0x5413

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

/* fcntl(2) commands (subset) */
#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4

/* --------------------------------------------------------------------------
 * BSD sockets ABI
 * -------------------------------------------------------------------------- */
#define AF_INET      2
#define SOCK_STREAM  1
#define SOCK_DGRAM   2
#define SOL_SOCKET   1
#define SO_REUSEADDR 2

typedef uint32_t socklen_t;

struct sockaddr {
    uint16_t sa_family;
    uint8_t  sa_data[14];
};

struct in_addr { uint32_t s_addr; };   /* network byte order */

struct sockaddr_in {
    uint16_t       sin_family;
    uint16_t       sin_port;   /* network byte order */
    struct in_addr sin_addr;   /* network byte order */
    uint8_t        sin_zero[8];
};

#endif /* UAPI_H */
