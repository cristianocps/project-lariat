#include "fd.h"
#include "kapi.h"
#include <string.h>

struct fd_table *fd_table_alloc(void) {
    struct fd_table *fdt = kzalloc(sizeof(struct fd_table));
    if (!fdt) return NULL;
    fdt->ref_count = 1;
    for (int i = 0; i < FD_MAX; i++) {
        fdt->files[i] = NULL;
    }
    return fdt;
}

void fd_table_free(struct fd_table *fdt) {
    if (!fdt) return;
    fdt->ref_count--;
    if (fdt->ref_count <= 0) {
        for (int i = 0; i < FD_MAX; i++) {
            if (fdt->files[i]) {
                vfs_close(fdt->files[i]);
                fdt->files[i] = NULL;
            }
        }
        kfree(fdt);
    }
}

struct fd_table *fd_table_clone(struct fd_table *fdt) {
    if (!fdt) return NULL;
    struct fd_table *new_fdt = fd_table_alloc();
    if (!new_fdt) return NULL;
    for (int i = 0; i < FD_MAX; i++) {
        if (fdt->files[i]) {
            new_fdt->files[i] = fdt->files[i];
            /* Shared open-file: bump the reference count so the underlying
             * file is only torn down when the last fd referencing it closes. */
            fdt->files[i]->ref_count++;
        }
    }
    return new_fdt;
}

int fd_alloc(struct fd_table *fdt, struct vfs_file *file) {
    if (!fdt || !file) return -1;
    for (int i = 0; i < FD_MAX; i++) {
        if (fdt->files[i] == NULL) {
            fdt->files[i] = file;
            return i;
        }
    }
    return -1;
}

/* Install a file at a specific descriptor, closing whatever was there. */
int fd_install_at(struct fd_table *fdt, int fd, struct vfs_file *file) {
    if (!fdt || fd < 0 || fd >= FD_MAX) return -1;
    if (fdt->files[fd]) vfs_close(fdt->files[fd]);
    fdt->files[fd] = file;
    return fd;
}

/* dup(): lowest available fd sharing the same open file. */
int fd_dup(struct fd_table *fdt, int oldfd) {
    if (!fdt || oldfd < 0 || oldfd >= FD_MAX || !fdt->files[oldfd]) return -1;
    int newfd = fd_alloc(fdt, fdt->files[oldfd]);
    if (newfd < 0) return -1;
    fdt->files[oldfd]->ref_count++;
    return newfd;
}

/* dup2(): force a specific new fd. */
int fd_dup2(struct fd_table *fdt, int oldfd, int newfd) {
    if (!fdt || oldfd < 0 || oldfd >= FD_MAX || !fdt->files[oldfd]) return -1;
    if (newfd < 0 || newfd >= FD_MAX) return -1;
    if (oldfd == newfd) return newfd;
    if (fdt->files[newfd]) vfs_close(fdt->files[newfd]);
    fdt->files[newfd] = fdt->files[oldfd];
    fdt->files[oldfd]->ref_count++;
    return newfd;
}

void fd_close(struct fd_table *fdt, int fd) {
    if (!fdt || fd < 0 || fd >= FD_MAX) return;
    if (fdt->files[fd]) {
        vfs_close(fdt->files[fd]);
        fdt->files[fd] = NULL;
    }
}
