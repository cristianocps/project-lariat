#ifndef LIBC_STDIO_H
#define LIBC_STDIO_H

#include <stddef.h>
#include <stdarg.h>

int  putchar(int c);
int  puts(const char *s);                 /* writes s + newline to stdout */
int  fputs(const char *s, int fd);        /* writes s (no newline) to fd */

int  printf(const char *fmt, ...);
int  fprintf(int fd, const char *fmt, ...);
int  snprintf(char *buf, size_t n, const char *fmt, ...);
int  vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

/* Read a line from fd into buf (up to size-1 chars), with echo and basic
 * backspace handling.  Returns the number of chars read, or -1 on EOF/error. */
int  read_line(int fd, char *buf, size_t size);

/* Buffered byte-stream reader over a raw fd (so line-oriented tools don't issue
 * one syscall per byte, which is very slow on block-backed files). */
typedef struct {
    int    fd;
    int    pos;
    int    len;
    char   buf[4096];
} freader;

void freader_init(freader *fr, int fd);
/* Returns next byte (0..255) or -1 at EOF/error. */
int  freader_getc(freader *fr);
/* Reads one line (without the trailing newline) into out (size bytes incl NUL).
 * Returns line length, or -1 at EOF with no data. */
int  freader_getline(freader *fr, char *out, int size);

#endif /* LIBC_STDIO_H */
