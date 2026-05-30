#include "vfs.h"
#include "kapi.h"
#include "sched.h"
#include "errno.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Anonymous pipes.
 *
 * A pipe is a fixed-size ring buffer shared by a read-end and a write-end
 * vfs_file.  Blocking is implemented cooperatively by yielding until progress
 * is possible (the preemptive timer guarantees forward progress).
 * -------------------------------------------------------------------------- */

#define PIPE_SIZE 4096

struct pipe {
    uint8_t  buf[PIPE_SIZE];
    size_t   head;       /* read position */
    size_t   tail;       /* write position */
    size_t   count;      /* bytes currently buffered */
    int      readers;    /* number of open read ends */
    int      writers;    /* number of open write ends */
};

static ssize_t pipe_read(struct vfs_file *file, void *buf, size_t count) {
    struct pipe *p = (struct pipe *)file->private_data;
    if (!p) return -EINVAL;
    uint8_t *dst = (uint8_t *)buf;

    while (p->count == 0) {
        if (p->writers == 0) return 0;  /* EOF: no writers left */
        thread_yield();
    }

    size_t n = 0;
    while (n < count && p->count > 0) {
        dst[n++] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_SIZE;
        p->count--;
    }
    return (ssize_t)n;
}

static ssize_t pipe_write(struct vfs_file *file, const void *buf, size_t count) {
    struct pipe *p = (struct pipe *)file->private_data;
    if (!p) return -EINVAL;
    const uint8_t *src = (const uint8_t *)buf;

    size_t n = 0;
    while (n < count) {
        if (p->readers == 0) return -EPIPE;  /* nobody to read */
        while (p->count == PIPE_SIZE) {
            if (p->readers == 0) return -EPIPE;
            thread_yield();
        }
        while (n < count && p->count < PIPE_SIZE) {
            p->buf[p->tail] = src[n++];
            p->tail = (p->tail + 1) % PIPE_SIZE;
            p->count++;
        }
    }
    return (ssize_t)n;
}

static int pipe_close_read(struct vfs_file *file) {
    struct pipe *p = (struct pipe *)file->private_data;
    if (p && --p->readers <= 0 && p->writers <= 0) kfree(p);
    return 0;
}

static int pipe_close_write(struct vfs_file *file) {
    struct pipe *p = (struct pipe *)file->private_data;
    if (p && --p->writers <= 0 && p->readers <= 0) kfree(p);
    return 0;
}

static short pipe_read_poll(struct vfs_file *file, short events) {
    struct pipe *p = (struct pipe *)file->private_data;
    if (!p) return POLLNVAL;
    short r = 0;
    if (events & POLLIN) {
        if (p->count > 0) r |= POLLIN;
        else if (p->writers == 0) r |= POLLHUP;  /* EOF */
    }
    return r;
}

static short pipe_write_poll(struct vfs_file *file, short events) {
    struct pipe *p = (struct pipe *)file->private_data;
    if (!p) return POLLNVAL;
    short r = 0;
    if (p->readers == 0) r |= POLLERR;
    if ((events & POLLOUT) && p->count < PIPE_SIZE) r |= POLLOUT;
    return r;
}

static struct vfs_file_ops pipe_read_ops = {
    .read = pipe_read, .write = NULL, .close = pipe_close_read, .lseek = NULL,
    .poll = pipe_read_poll,
};
static struct vfs_file_ops pipe_write_ops = {
    .read = NULL, .write = pipe_write, .close = pipe_close_write, .lseek = NULL,
    .poll = pipe_write_poll,
};

static struct vfs_inode pipe_read_inode  = { .mode = S_IFIFO, .f_ops = &pipe_read_ops };
static struct vfs_inode pipe_write_inode = { .mode = S_IFIFO, .f_ops = &pipe_write_ops };

int pipe_create(struct vfs_file **files) {
    struct pipe *p = kzalloc(sizeof(struct pipe));
    if (!p) return -ENOMEM;
    p->readers = 1;
    p->writers = 1;

    struct vfs_file *rd = kzalloc(sizeof(struct vfs_file));
    struct vfs_file *wr = kzalloc(sizeof(struct vfs_file));
    if (!rd || !wr) {
        if (rd) kfree(rd);
        if (wr) kfree(wr);
        kfree(p);
        return -ENOMEM;
    }

    rd->inode = &pipe_read_inode;
    rd->private_data = p;
    rd->ref_count = 1;
    rd->flags = O_RDONLY;

    wr->inode = &pipe_write_inode;
    wr->private_data = p;
    wr->ref_count = 1;
    wr->flags = O_WRONLY;

    files[0] = rd;
    files[1] = wr;
    return 0;
}
