#include "vfs.h"
#include "kapi.h"
#include "vga.h"
#include "serial.h"
#include "keyboard.h"
#include "uapi.h"
#include "errno.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * /dev/console - the kernel console as a character device exposed through the
 * VFS so that stdin/stdout/stderr are ordinary file descriptors instead of a
 * hardcoded fd==0/1/2 special case in the syscall layer.
 *
 * The console implements a small TTY line discipline: in canonical (cooked)
 * mode it buffers a whole line, echoes typed characters, and supports erase
 * (backspace/DEL); in raw mode it returns bytes as they arrive without echo.
 * Terminal-signal characters (Ctrl-C/Ctrl-\/Ctrl-Z) are turned into signals by
 * the keyboard/serial IRQ before they ever reach the ring buffer.
 * -------------------------------------------------------------------------- */

static void console_echo(char c) {
    uint64_t f = serial_lock_acquire();
    vga_putchar(c);
    serial_putc(SERIAL_COM1, c);
    serial_lock_release(f);
}

static void console_echo_erase(void) {
    uint64_t f = serial_lock_acquire();
    const char *s = "\b \b";
    for (int i = 0; i < 3; i++) { vga_putchar(s[i]); serial_putc(SERIAL_COM1, s[i]); }
    serial_lock_release(f);
}

/* Canonical-mode line buffer.  A completed line is assembled here (with echo
 * and editing) and then handed to read() callers a chunk at a time, so reading
 * one byte at a time works the same as reading a whole line at once. */
#define CANON_MAX 1024
static char   canon_buf[CANON_MAX];
static size_t canon_len = 0;   /* bytes of a completed line */
static size_t canon_pos = 0;   /* next byte to hand out */

/* Assemble one input line into canon_buf with echo + erase.  Returns the line
 * length (>=0, includes the terminating '\n'), 0 for EOF on an empty line, or
 * -EINTR if interrupted by a signal. */
static int canon_fill(const struct termios *tio) {
    size_t n = 0;
    int echo = (tio->c_lflag & ECHO) != 0;
    for (;;) {
        int ci = tty_getc_intr();
        if (ci < 0) return -EINTR;
        char c = (char)ci;
        if ((tio->c_iflag & ICRNL) && c == '\r') c = '\n';

        if (c == '\n') {
            if (echo) console_echo('\n');
            if (n < CANON_MAX) canon_buf[n++] = '\n';
            break;
        }
        if ((unsigned char)c == tio->c_cc[VEOF]) {   /* Ctrl-D */
            if (n == 0) return 0;                     /* EOF */
            break;                                    /* deliver partial line */
        }
        if ((unsigned char)c == tio->c_cc[VERASE] || c == '\b') {
            if (n > 0) { n--; if (echo && (tio->c_lflag & ECHOE)) console_echo_erase(); }
            continue;
        }
        if (n < CANON_MAX - 1) {
            canon_buf[n++] = c;
            if (echo) console_echo(c);
        }
    }
    canon_len = n;
    canon_pos = 0;
    return (int)n;
}

static ssize_t console_read(struct vfs_file *file, void *buf, size_t count) {
    (void)file;
    char *dest = (char *)buf;
    if (count == 0) return 0;

    struct termios tio;
    tty_termios_get(&tio);

    if (!(tio.c_lflag & ICANON)) {
        /* Raw mode: return at least one byte, then drain whatever is buffered. */
        int c = tty_getc_intr();
        if (c < 0) return -EINTR;
        size_t n = 0;
        dest[n++] = (char)c;
        n += (size_t)tty_read(dest + n, count - n);
        return (ssize_t)n;
    }

    /* Canonical mode: refill the line buffer if it has been fully consumed. */
    if (canon_pos >= canon_len) {
        int r = canon_fill(&tio);
        if (r < 0) return -EINTR;
        if (r == 0) return 0;   /* EOF */
    }
    size_t avail = canon_len - canon_pos;
    size_t n = (count < avail) ? count : avail;
    memcpy(dest, canon_buf + canon_pos, n);
    canon_pos += n;
    return (ssize_t)n;
}

static ssize_t console_write(struct vfs_file *file, const void *buf, size_t count) {
    (void)file;
    const char *str = (const char *)buf;
    uint64_t f = serial_lock_acquire();
    for (size_t i = 0; i < count; i++) {
        vga_putchar(str[i]);
        serial_putc(SERIAL_COM1, str[i]);
    }
    serial_lock_release(f);
    return (ssize_t)count;
}

static int console_close(struct vfs_file *file) {
    (void)file;
    return 0;
}

static off_t console_lseek(struct vfs_file *file, off_t offset, int whence) {
    (void)file; (void)offset; (void)whence;
    return 0;  /* character device: not seekable, report 0 */
}

static short console_poll(struct vfs_file *file, short events) {
    (void)file;
    short r = 0;
    if ((events & POLLIN) && tty_poll()) r |= POLLIN;
    if (events & POLLOUT) r |= POLLOUT;   /* console writes never block */
    return r;
}

static int console_ioctl(struct vfs_file *file, unsigned long req, unsigned long arg) {
    (void)file;
    void *p = (void *)(uintptr_t)arg;
    switch (req) {
        case TCGETS:
            if (!p) return -EFAULT;
            tty_termios_get((struct termios *)p);
            return 0;
        case TCSETS:
            if (!p) return -EFAULT;
            tty_termios_set((const struct termios *)p);
            return 0;
        case TIOCGPGRP:
            if (!p) return -EFAULT;
            *(int *)p = tty_get_fg_pgrp();
            return 0;
        case TIOCSPGRP:
            if (!p) return -EFAULT;
            tty_set_fg_pgrp(*(int *)p);
            return 0;
        case TIOCGWINSZ: {
            if (!p) return -EFAULT;
            struct winsize *ws = (struct winsize *)p;
            ws->ws_row = 25; ws->ws_col = 80;
            ws->ws_xpixel = 0; ws->ws_ypixel = 0;
            return 0;
        }
        default:
            return -ENOTTY;
    }
}

static struct vfs_file_ops console_fops = {
    .read  = console_read,
    .write = console_write,
    .close = console_close,
    .lseek = console_lseek,
    .poll  = console_poll,
    .ioctl = console_ioctl,
};

static struct vfs_inode console_inode;
static int console_ready = 0;

void console_init(void) {
    memset(&console_inode, 0, sizeof(console_inode));
    console_inode.mode = S_IFCHR | 0666;
    console_inode.f_ops = &console_fops;
    console_ready = 1;
}

/* Allocate a fresh open-file handle for the console. */
struct vfs_file *console_open(void) {
    if (!console_ready) console_init();
    struct vfs_file *f = kzalloc(sizeof(struct vfs_file));
    if (!f) return NULL;
    f->inode = &console_inode;
    f->pos = 0;
    f->flags = O_RDWR;
    f->ref_count = 1;
    return f;
}
