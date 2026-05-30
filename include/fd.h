#ifndef FD_H
#define FD_H

#include "vfs.h"

#define FD_MAX 256

struct fd_table {
    struct vfs_file *files[FD_MAX];
    int ref_count;
};

struct fd_table *fd_table_alloc(void);
void fd_table_free(struct fd_table *fdt);
struct fd_table *fd_table_clone(struct fd_table *fdt);
int fd_alloc(struct fd_table *fdt, struct vfs_file *file);
int fd_install_at(struct fd_table *fdt, int fd, struct vfs_file *file);
int fd_dup(struct fd_table *fdt, int oldfd);
int fd_dup2(struct fd_table *fdt, int oldfd, int newfd);
void fd_close(struct fd_table *fdt, int fd);

#endif /* FD_H */
