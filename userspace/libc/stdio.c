#include "stdio.h"
#include "string.h"
#include "unistd.h"

/* --------------------------------------------------------------------------
 * Number formatting helpers
 * -------------------------------------------------------------------------- */
static int put_str(char *buf, size_t n, size_t pos, const char *s) {
    while (*s) {
        if (pos < n - 1) buf[pos] = *s;
        pos++; s++;
    }
    return (int)pos;
}

static int put_uint(char *buf, size_t n, size_t pos, unsigned long v,
                    int base, int upper) {
    char tmp[32];
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = digs[v % base]; v /= base; }
    while (i > 0) {
        if (pos < n - 1) buf[pos] = tmp[--i]; else --i;
        pos++;
    }
    return (int)pos;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    if (n == 0) return 0;
    size_t pos = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            if (pos < n - 1) buf[pos] = *p;
            pos++;
            continue;
        }
        p++;
        int lng = 0;
        while (*p == 'l') { lng++; p++; }
        switch (*p) {
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                pos = (size_t)put_str(buf, n, pos, s);
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                if (pos < n - 1) buf[pos] = c;
                pos++;
                break;
            }
            case 'd':
            case 'i': {
                long v = lng ? va_arg(ap, long) : (long)va_arg(ap, int);
                if (v < 0) {
                    if (pos < n - 1) buf[pos] = '-';
                    pos++;
                    v = -v;
                }
                pos = (size_t)put_uint(buf, n, pos, (unsigned long)v, 10, 0);
                break;
            }
            case 'u': {
                unsigned long v = lng ? va_arg(ap, unsigned long)
                                      : (unsigned long)va_arg(ap, unsigned int);
                pos = (size_t)put_uint(buf, n, pos, v, 10, 0);
                break;
            }
            case 'x':
            case 'X': {
                unsigned long v = lng ? va_arg(ap, unsigned long)
                                      : (unsigned long)va_arg(ap, unsigned int);
                pos = (size_t)put_uint(buf, n, pos, v, 16, *p == 'X');
                break;
            }
            case 'p': {
                unsigned long v = (unsigned long)va_arg(ap, void *);
                pos = (size_t)put_str(buf, n, pos, "0x");
                pos = (size_t)put_uint(buf, n, pos, v, 16, 0);
                break;
            }
            case '%':
                if (pos < n - 1) buf[pos] = '%';
                pos++;
                break;
            case '\0':
                p--;
                break;
            default:
                if (pos < n - 1) buf[pos] = '%';
                pos++;
                if (pos < n - 1) buf[pos] = *p;
                pos++;
                break;
        }
    }
    buf[pos < n ? pos : n - 1] = '\0';
    return (int)pos;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int fprintf(int fd, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t len = (size_t)r;
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    write(fd, buf, len);
    return r;
}

int printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t len = (size_t)r;
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    write(STDOUT_FILENO, buf, len);
    return r;
}

int putchar(int c) {
    char ch = (char)c;
    write(STDOUT_FILENO, &ch, 1);
    return c;
}

int fputs(const char *s, int fd) {
    return (int)write(fd, s, strlen(s));
}

int puts(const char *s) {
    write(STDOUT_FILENO, s, strlen(s));
    char nl = '\n';
    write(STDOUT_FILENO, &nl, 1);
    return 0;
}

void freader_init(freader *fr, int fd) {
    fr->fd = fd;
    fr->pos = 0;
    fr->len = 0;
}

int freader_getc(freader *fr) {
    if (fr->pos >= fr->len) {
        int n = (int)read(fr->fd, fr->buf, sizeof(fr->buf));
        if (n <= 0) return -1;
        fr->len = n;
        fr->pos = 0;
    }
    return (unsigned char)fr->buf[fr->pos++];
}

int freader_getline(freader *fr, char *out, int size) {
    int n = 0;
    int c = freader_getc(fr);
    if (c < 0) return -1;
    while (c >= 0 && c != '\n') {
        if (n < size - 1) out[n++] = (char)c;
        c = freader_getc(fr);
    }
    out[n] = '\0';
    return n;
}

int read_line(int fd, char *buf, size_t size) {
    /* The kernel TTY line discipline handles echo and line editing in canonical
     * mode and returns whole lines, so here we just collect bytes up to the
     * newline (or EOF). */
    size_t n = 0;
    for (;;) {
        char c;
        int r = read(fd, &c, 1);
        if (r <= 0) {
            if (n == 0) return -1;       /* EOF / error with nothing read */
            break;
        }
        if (c == '\r') c = '\n';
        if (c == '\n') break;            /* end of line (newline not stored) */
        if (n < size - 1) buf[n++] = c;
    }
    buf[n] = '\0';
    return (int)n;
}
