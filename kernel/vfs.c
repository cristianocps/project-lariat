#include "vfs.h"
#include "kapi.h"
#include "serial.h"
#include <string.h>

/* Current root filesystem */
static struct vfs_dentry *vfs_root = NULL;
static struct vfs_fs_type *fs_types = NULL;

/* --------------------------------------------------------------------------
 * Utility functions
 * -------------------------------------------------------------------------- */
static int path_component(const char **path, char *buf, size_t buflen) {
    const char *p = *path;
    while (*p == '/') p++;
    if (*p == '\0') return 0;

    size_t i = 0;
    while (*p && *p != '/' && i < buflen - 1) {
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    *path = p;
    return 1;
}

static int is_absolute(const char *path) {
    return path[0] == '/';
}

/* --------------------------------------------------------------------------
 * Dentry helpers
 * -------------------------------------------------------------------------- */
struct vfs_dentry *vfs_dentry_create(const char *name, struct vfs_inode *inode,
                                      struct vfs_dentry *parent) {
    struct vfs_dentry *d = kzalloc(sizeof(struct vfs_dentry));
    if (!d) return NULL;

    size_t len = 0;
    while (name[len] && len < sizeof(d->name) - 1) len++;
    for (size_t i = 0; i < len; i++) d->name[i] = name[i];
    d->name[len] = '\0';

    d->inode = inode;
    d->parent = parent;
    d->child_list = NULL;
    d->next_sibling = NULL;
    d->mount = NULL;
    return d;
}

void vfs_dentry_add_child(struct vfs_dentry *parent, struct vfs_dentry *child) {
    if (!parent || !child) return;
    child->next_sibling = parent->child_list;
    parent->child_list = child;
    child->parent = parent;
}

struct vfs_dentry *vfs_dentry_find_child(struct vfs_dentry *parent, const char *name) {
    if (!parent || !parent->inode || !S_ISDIR(parent->inode->mode))
        return NULL;

    struct vfs_dentry *child = parent->child_list;
    while (child) {
        const char *a = child->name;
        const char *b = name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0')
            return child;
        child = child->next_sibling;
    }
    return NULL;
}

void vfs_dentry_remove_child(struct vfs_dentry *parent, struct vfs_dentry *child) {
    if (!parent || !child) return;
    struct vfs_dentry **pp = &parent->child_list;
    while (*pp) {
        if (*pp == child) {
            *pp = child->next_sibling;
            child->next_sibling = NULL;
            return;
        }
        pp = &(*pp)->next_sibling;
    }
}

/* --------------------------------------------------------------------------
 * Path resolution
 * -------------------------------------------------------------------------- */
struct vfs_dentry *vfs_lookup_path(const char *path) {
    if (!vfs_root) return NULL;
    if (!path || path[0] == '\0') return NULL;

    struct vfs_dentry *current = vfs_root;

    if (!is_absolute(path)) return NULL;

    const char *p = path;
    char comp[64];

    while (path_component(&p, comp, sizeof(comp))) {
        if (comp[0] == '.' && comp[1] == '\0') continue;
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
            if (current->parent) current = current->parent;
            continue;
        }

        if (!current->inode || !S_ISDIR(current->inode->mode))
            return NULL;

        /* Follow mount points */
        while (current && current->mount) {
            current = current->mount->root;
        }

        if (current->inode->i_ops && current->inode->i_ops->lookup) {
            current = current->inode->i_ops->lookup(current->inode, comp);
        } else {
            current = vfs_dentry_find_child(current, comp);
        }

        if (!current) return NULL;
    }

    /* Follow final mount point */
    while (current && current->mount) {
        current = current->mount->root;
    }

    return current;
}

struct vfs_dentry *vfs_lookup_parent(const char *path, char *name_out, size_t name_len) {
    if (!path || path[0] != '/') return NULL;

    /* Find last component */
    const char *p = path;
    const char *last_slash = NULL;
    while (*p) {
        if (*p == '/') last_slash = p;
        p++;
    }

    if (!last_slash || last_slash == path) {
        /* Parent is root */
        if (name_out && name_len > 0) {
            size_t i = 0;
            while (path[i] && path[i] == '/') i++;
            size_t j = 0;
            while (path[i] && j < name_len - 1) {
                name_out[j++] = path[i++];
            }
            name_out[j] = '\0';
        }
        return vfs_root;
    }

    /* Copy parent path */
    char parent_path[256];
    size_t plen = (size_t)(last_slash - path);
    if (plen >= sizeof(parent_path)) plen = sizeof(parent_path) - 1;
    for (size_t i = 0; i < plen; i++) parent_path[i] = path[i];
    parent_path[plen] = '\0';
    if (plen == 0) { parent_path[0] = '/'; parent_path[1] = '\0'; }

    struct vfs_dentry *parent = vfs_lookup_path(parent_path);
    if (!parent) return NULL;

    if (name_out && name_len > 0) {
        const char *name = last_slash + 1;
        while (*name == '/') name++;
        size_t i = 0;
        while (name[i] && i < name_len - 1) {
            name_out[i] = name[i];
            i++;
        }
        name_out[i] = '\0';
    }

    return parent;
}

/* --------------------------------------------------------------------------
 * VFS registration and mount
 * -------------------------------------------------------------------------- */
void vfs_init(void) {
    vfs_root = NULL;
    fs_types = NULL;
}

int vfs_register_fs(struct vfs_fs_type *fs) {
    if (!fs || !fs->name) return -1;
    fs->next = fs_types;
    fs_types = fs;
    return 0;
}

int vfs_mount(const char *fs_name, const char *dev_name, const char *path) {
    struct vfs_fs_type *fs = fs_types;
    while (fs) {
        const char *a = fs->name;
        const char *b = fs_name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') break;
        fs = fs->next;
    }
    if (!fs) return -1;

    struct vfs_superblock *sb = fs->mount(dev_name);
    if (!sb) return -1;

    if (!vfs_root || (path && path[0] == '/' && path[1] == '\0')) {
        vfs_root = sb->root;
        return 0;
    }

    /* Mount at non-root path */
    struct vfs_dentry *mnt = vfs_lookup_path(path);
    if (!mnt) return -1;
    mnt->mount = sb;
    return 0;
}

struct vfs_dentry *vfs_get_root(void) {
    return vfs_root;
}

/* --------------------------------------------------------------------------
 * File operations
 * -------------------------------------------------------------------------- */
struct vfs_file *vfs_open(const char *path, int flags) {
    struct vfs_dentry *dentry = vfs_lookup_path(path);

    if (!dentry && (flags & O_CREAT)) {
        /* Create the file */
        char name[64];
        struct vfs_dentry *parent = vfs_lookup_parent(path, name, sizeof(name));
        if (!parent || !parent->inode || !parent->inode->i_ops ||
            !parent->inode->i_ops->create)
            return NULL;

        int err = parent->inode->i_ops->create(parent->inode, name, S_IFREG | 0644);
        if (err < 0) return NULL;
        dentry = vfs_lookup_path(path);
    }

    if (!dentry || !dentry->inode) return NULL;

    struct vfs_file *file = kzalloc(sizeof(struct vfs_file));
    if (!file) return NULL;

    file->dentry = dentry;
    file->inode = dentry->inode;
    file->pos = 0;
    file->flags = flags;

    if ((flags & O_TRUNC) && file->inode->f_ops && file->inode->f_ops->write) {
        file->inode->size = 0;
    }

    return file;
}

int vfs_close(struct vfs_file *file) {
    if (!file) return -1;
    if (file->inode && file->inode->f_ops && file->inode->f_ops->close) {
        file->inode->f_ops->close(file);
    }
    kfree(file);
    return 0;
}

ssize_t vfs_read(struct vfs_file *file, void *buf, size_t count) {
    if (!file || !file->inode || !file->inode->f_ops || !file->inode->f_ops->read)
        return -1;
    return file->inode->f_ops->read(file, buf, count);
}

ssize_t vfs_write(struct vfs_file *file, const void *buf, size_t count) {
    if (!file || !file->inode || !file->inode->f_ops || !file->inode->f_ops->write)
        return -1;
    return file->inode->f_ops->write(file, buf, count);
}

off_t vfs_lseek(struct vfs_file *file, off_t offset, int whence) {
    if (!file || !file->inode || !file->inode->f_ops || !file->inode->f_ops->lseek)
        return -1;
    return file->inode->f_ops->lseek(file, offset, whence);
}

/* --------------------------------------------------------------------------
 * Directory operations
 * -------------------------------------------------------------------------- */
int vfs_mkdir(const char *path, uint32_t mode) {
    char name[64];
    struct vfs_dentry *parent = vfs_lookup_parent(path, name, sizeof(name));
    if (!parent || !parent->inode || !parent->inode->i_ops ||
        !parent->inode->i_ops->mkdir)
        return -1;
    return parent->inode->i_ops->mkdir(parent->inode, name, mode | S_IFDIR);
}

int vfs_unlink(const char *path) {
    char name[64];
    struct vfs_dentry *parent = vfs_lookup_parent(path, name, sizeof(name));
    if (!parent || !parent->inode || !parent->inode->i_ops ||
        !parent->inode->i_ops->unlink)
        return -1;
    return parent->inode->i_ops->unlink(parent->inode, name);
}

int vfs_rmdir(const char *path) {
    char name[64];
    struct vfs_dentry *parent = vfs_lookup_parent(path, name, sizeof(name));
    if (!parent || !parent->inode || !parent->inode->i_ops ||
        !parent->inode->i_ops->rmdir)
        return -1;
    return parent->inode->i_ops->rmdir(parent->inode, name);
}

/* --------------------------------------------------------------------------
 * Directory iteration
 * -------------------------------------------------------------------------- */
struct vfs_dir *vfs_opendir(const char *path) {
    struct vfs_dentry *dentry = vfs_lookup_path(path);
    if (!dentry || !dentry->inode || !S_ISDIR(dentry->inode->mode))
        return NULL;

    struct vfs_dir *dir = kzalloc(sizeof(struct vfs_dir));
    if (!dir) return NULL;

    dir->dentry = dentry;
    dir->inode = dentry->inode;
    dir->index = 0;
    return dir;
}

int vfs_readdir(struct vfs_dir *dir, struct vfs_dir_entry *entry) {
    if (!dir || !entry) return 0;

    /* If the inode provides readdir, use it */
    if (dir->inode && dir->inode->i_ops && dir->inode->i_ops->readdir) {
        int rc = dir->inode->i_ops->readdir(dir->inode, dir->index, entry);
        if (rc > 0) {
            dir->index++;
            return 1;
        }
        return 0;
    }

    /* Fallback: iterate cached dentries (ramfs) */
    struct vfs_dentry *d = dir->dentry->child_list;
    int idx = 0;
    while (d) {
        if (idx >= dir->index) {
            /* Skip . and .. */
            if ((d->name[0] == '.' && d->name[1] == '\0') ||
                (d->name[0] == '.' && d->name[1] == '.' && d->name[2] == '\0')) {
                dir->index++;
                d = d->next_sibling;
                continue;
            }
            size_t i = 0;
            while (d->name[i] && i < sizeof(entry->name) - 1) {
                entry->name[i] = d->name[i];
                i++;
            }
            entry->name[i] = '\0';
            entry->inode_no = d->inode ? d->inode->inode_no : 0;
            entry->mode = d->inode ? d->inode->mode : 0;
            dir->index++;
            return 1;
        }
        idx++;
        d = d->next_sibling;
    }
    return 0;
}

void vfs_closedir(struct vfs_dir *dir) {
    if (dir) kfree(dir);
}
