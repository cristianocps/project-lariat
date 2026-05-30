#ifndef LIBC_DIRENT_H
#define LIBC_DIRENT_H

#include <stddef.h>
#include "unistd.h"
#include "fcntl.h"

struct dirent {
    unsigned long  d_ino;
    unsigned char  d_type;
    char           d_name[256];
};

/* A directory stream built on top of getdents64(2). */
typedef struct DIR {
    int   fd;
    long  bufpos;   /* current parse offset into buf */
    long  buflen;   /* valid bytes in buf */
    char  buf[2048];
    struct dirent ent;
} DIR;

static inline DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return (DIR *)0;
    /* Heap-allocate so the stream survives the caller's frame. */
    extern void *malloc(size_t);
    DIR *d = (DIR *)malloc(sizeof(DIR));
    if (!d) { close(fd); return (DIR *)0; }
    d->fd = fd;
    d->bufpos = 0;
    d->buflen = 0;
    return d;
}

static inline struct dirent *readdir(DIR *d) {
    if (!d) return (struct dirent *)0;
    if (d->bufpos >= d->buflen) {
        long n = getdents64(d->fd, d->buf, sizeof(d->buf));
        if (n <= 0) return (struct dirent *)0;
        d->buflen = n;
        d->bufpos = 0;
    }
    struct dirent64 *de = (struct dirent64 *)(d->buf + d->bufpos);
    d->bufpos += de->d_reclen;
    d->ent.d_ino = de->d_ino;
    d->ent.d_type = de->d_type;
    size_t i = 0;
    while (de->d_name[i] && i < sizeof(d->ent.d_name) - 1) {
        d->ent.d_name[i] = de->d_name[i];
        i++;
    }
    d->ent.d_name[i] = '\0';
    return &d->ent;
}

static inline int closedir(DIR *d) {
    if (!d) return -1;
    int fd = d->fd;
    extern void free(void *);
    free(d);
    return close(fd);
}

#endif /* LIBC_DIRENT_H */
