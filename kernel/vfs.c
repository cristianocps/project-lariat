#include "vfs.h"
#include "kapi.h"
#include "serial.h"
#include "sched.h"
#include "errno.h"
#include <string.h>

/* Current root filesystem */
static struct vfs_dentry *vfs_root = NULL;
static struct vfs_fs_type *fs_types = NULL;

/* Mount table: a record of each successful mount, for /proc/mounts and for
 * fstab idempotency (so the parser does not re-mount what boot already did). */
static struct vfs_mount_rec g_mounts[VFS_MAX_MOUNTS];
static int g_mount_count = 0;

static void mt_copy(char *dst, const char *src, size_t cap) {
    size_t i = 0;
    if (cap == 0) return;
    if (src) for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void vfs_mount_record(const char *fstype, const char *dev, const char *path) {
    if (g_mount_count >= VFS_MAX_MOUNTS) return;
    struct vfs_mount_rec *r = &g_mounts[g_mount_count++];
    mt_copy(r->dev, (dev && dev[0]) ? dev : "none", sizeof(r->dev));
    mt_copy(r->path, path ? path : "/", sizeof(r->path));
    mt_copy(r->fstype, fstype, sizeof(r->fstype));
}

int vfs_mounts_count(void) { return g_mount_count; }

const struct vfs_mount_rec *vfs_mounts_get(int i) {
    if (i < 0 || i >= g_mount_count) return NULL;
    return &g_mounts[i];
}

int vfs_is_mounted(const char *path) {
    if (!path) return 0;
    for (int i = 0; i < g_mount_count; i++)
        if (strcmp(g_mounts[i].path, path) == 0) return 1;
    return 0;
}

/* --------------------------------------------------------------------------
 * M10: permission enforcement.  Compares the calling thread's effective
 * credentials against an inode's owner/group/other rwx bits.  uid 0 (root)
 * bypasses every check.  Returns 0 if allowed, -EACCES otherwise.
 * -------------------------------------------------------------------------- */
int vfs_permission(struct vfs_inode *inode, int mask) {
    if (!inode) return -ENOENT;
    if (mask == 0) return 0;

    struct thread *t = current_thread();
    /* No user context (early boot / kernel threads) acts as root. */
    if (!t || t->euid == 0) {
        /* Root may read/write anything; for exec at least one x bit must be
         * set (mirrors POSIX so non-executable files still fail for root). */
        if (mask & MAY_EXEC) {
            if (inode->mode & (S_IXUSR | S_IXGRP | S_IXOTH)) return 0;
            return -EACCES;
        }
        return 0;
    }

    uint32_t mode = inode->mode;
    uint32_t perms;
    if (t->euid == inode->uid) {
        perms = (mode & S_IRWXU) >> 6;
    } else {
        int in_group = (t->egid == inode->gid);
        for (int i = 0; !in_group && i < t->ngroups; i++)
            if (t->groups[i] == inode->gid) in_group = 1;
        if (in_group) perms = (mode & S_IRWXG) >> 3;
        else          perms = (mode & S_IRWXO);
    }

    int want = 0;
    if (mask & MAY_READ)  want |= 4;
    if (mask & MAY_WRITE) want |= 2;
    if (mask & MAY_EXEC)  want |= 1;
    return ((perms & want) == want) ? 0 : -EACCES;
}

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
#define VFS_SYMLINK_MAX_DEPTH 8

/* Read a symlink inode's target into buf (NUL-terminated); returns 0 or -1. */
static int vfs_read_link_target(struct vfs_inode *inode, char *buf, size_t buflen) {
    if (!inode || !inode->i_ops || !inode->i_ops->readlink || buflen == 0)
        return -1;
    int n = inode->i_ops->readlink(inode, buf, buflen - 1);
    if (n < 0) return -1;
    if ((size_t)n >= buflen) n = (int)buflen - 1;
    buf[n] = '\0';
    return 0;
}

/* Walk `path` (its components, leading slashes ignored) starting at `start`,
 * following symbolic links encountered along the way.  `depth` bounds symlink
 * recursion.  Returns the resolved dentry (mount points crossed) or NULL. */
static struct vfs_dentry *vfs_walk(struct vfs_dentry *start, const char *path,
                                   int depth) {
    if (!start || depth > VFS_SYMLINK_MAX_DEPTH) return NULL;

    struct vfs_dentry *current = start;
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

        /* Follow mount points before resolving inside this directory. */
        while (current && current->mount)
            current = current->mount->root;

        struct vfs_dentry *next;
        if (current->inode->i_ops && current->inode->i_ops->lookup)
            next = current->inode->i_ops->lookup(current->inode, comp);
        else
            next = vfs_dentry_find_child(current, comp);

        if (!next) return NULL;

        /* Anchor the resolved child to the directory we looked it up in, so a
         * later ".." ascends to the real parent.  Disk filesystems (ext4) mint
         * fresh dentries per lookup and cannot know the parent dentry, so they
         * leave it pointing at the volume root; correct it here where the walk
         * holds the true parent.  For cached ramfs children this is a no-op. */
        next->parent = current;

        /* Symbolic link: resolve its target, then continue with the rest of
         * the path from there (absolute targets restart at the root). */
        if (next->inode && S_ISLNK(next->inode->mode)) {
            char target[256];
            if (vfs_read_link_target(next->inode, target, sizeof(target)) < 0)
                return NULL;
            struct vfs_dentry *base = (target[0] == '/') ? vfs_root : current;
            current = vfs_walk(base, target, depth + 1);
            if (!current) return NULL;
            continue;
        }

        current = next;
    }

    /* Follow a final mount point so callers see the mounted fs root. */
    while (current && current->mount)
        current = current->mount->root;

    return current;
}

struct vfs_dentry *vfs_lookup_path(const char *path) {
    if (!vfs_root) return NULL;
    if (!path || path[0] == '\0') return NULL;
    if (!is_absolute(path)) return NULL;
    return vfs_walk(vfs_root, path, 0);
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
        vfs_mount_record(fs_name, dev_name, "/");
        return 0;
    }

    /* Mount at non-root path */
    struct vfs_dentry *mnt = vfs_lookup_path(path);
    if (!mnt) return -1;
    mnt->mount = sb;
    vfs_mount_record(fs_name, dev_name, path);
    return 0;
}

struct vfs_dentry *vfs_get_root(void) {
    return vfs_root;
}

/* --------------------------------------------------------------------------
 * File operations
 * -------------------------------------------------------------------------- */
/* Resolve a path the way vfs_open() will and check the calling thread's
 * credentials against the requested access.  Returns 0 if the open should be
 * permitted, or a negative errno (-EACCES / -ENOENT) otherwise.  Kept separate
 * from vfs_open() so in-kernel opens (ELF loader, console, config files) are
 * not subject to userspace permission checks. */
int vfs_access_check(const char *path, int flags) {
    int acc = flags & 0x3;
    int want = 0;
    if (acc == O_RDONLY || acc == O_RDWR) want |= MAY_READ;
    if (acc == O_WRONLY || acc == O_RDWR) want |= MAY_WRITE;
    if (flags & O_TRUNC) want |= MAY_WRITE;

    struct vfs_dentry *dentry = vfs_lookup_path(path);
    if (dentry && dentry->inode)
        return vfs_permission(dentry->inode, want);

    if (flags & O_CREAT) {
        /* Creating a new file needs write+search on the parent directory. */
        char name[64];
        struct vfs_dentry *parent = vfs_lookup_parent(path, name, sizeof(name));
        if (!parent || !parent->inode) return -ENOENT;
        return vfs_permission(parent->inode, MAY_WRITE | MAY_EXEC);
    }
    return -ENOENT;
}

int vfs_devfs_register(const char *name, struct vfs_inode *inode) {
    struct vfs_dentry *dev = vfs_lookup_path("/dev");
    if (!dev) {
        vfs_mkdir("/dev", 0755);
        dev = vfs_lookup_path("/dev");
    }
    if (!dev) return -1;
    struct vfs_dentry *d = vfs_dentry_create(name, inode, dev);
    if (!d) return -1;
    vfs_dentry_add_child(dev, d);
    return 0;
}

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
    file->ref_count = 1;

    if ((flags & O_TRUNC) && file->inode->f_ops) {
        if (file->inode->f_ops->truncate)
            file->inode->f_ops->truncate(file, 0);
        else if (file->inode->f_ops->write)
            file->inode->size = 0;
    }

    /* O_APPEND: start writing at end-of-file. */
    if (flags & O_APPEND)
        file->pos = (off_t)file->inode->size;

    return file;
}

int vfs_close(struct vfs_file *file) {
    if (!file) return -1;
    /* Shared handles (dup/fork) are only torn down once the last reference is
     * dropped. */
    if (file->ref_count > 1) {
        file->ref_count--;
        return 0;
    }
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

int vfs_ioctl(struct vfs_file *file, unsigned long req, unsigned long arg) {
    if (!file || !file->inode) return -9 /* EBADF */;
    if (file->inode->f_ops && file->inode->f_ops->ioctl)
        return file->inode->f_ops->ioctl(file, req, arg);
    return -25 /* ENOTTY */;
}

short vfs_poll(struct vfs_file *file, short events) {
    if (!file || !file->inode) return POLLNVAL;
    if (file->inode->f_ops && file->inode->f_ops->poll)
        return file->inode->f_ops->poll(file, events);
    /* Regular files / devices without a poll op are always ready. */
    return events & (POLLIN | POLLOUT);
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

int vfs_symlink(const char *target, const char *linkpath) {
    if (!target || !linkpath) return -1;
    char name[64];
    struct vfs_dentry *parent = vfs_lookup_parent(linkpath, name, sizeof(name));
    if (!parent) return -1;
    /* Cross a mount point at the parent (e.g. linkpath under /var). */
    while (parent->mount) parent = parent->mount->root;
    if (!parent->inode || !parent->inode->i_ops ||
        !parent->inode->i_ops->symlink)
        return -1;
    return parent->inode->i_ops->symlink(parent->inode, name, target);
}

int vfs_readlink(const char *path, char *buf, size_t bufsiz) {
    if (!path || !buf || bufsiz == 0) return -1;
    /* Resolve the parent and the final component WITHOUT following a final
     * symlink, so we can read the link itself rather than its target. */
    char name[64];
    struct vfs_dentry *parent = vfs_lookup_parent(path, name, sizeof(name));
    if (!parent) return -1;
    while (parent->mount) parent = parent->mount->root;
    if (!parent->inode) return -1;
    struct vfs_dentry *d;
    if (parent->inode->i_ops && parent->inode->i_ops->lookup)
        d = parent->inode->i_ops->lookup(parent->inode, name);
    else
        d = vfs_dentry_find_child(parent, name);
    if (!d || !d->inode || !S_ISLNK(d->inode->mode)) return -1;
    if (!d->inode->i_ops || !d->inode->i_ops->readlink) return -1;
    return d->inode->i_ops->readlink(d->inode, buf, bufsiz);
}

int vfs_setattr(struct vfs_inode *inode) {
    if (!inode) return -1;
    if (inode->i_ops && inode->i_ops->setattr)
        return inode->i_ops->setattr(inode);
    return 0;   /* in-memory fs: the in-core inode is the source of truth */
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
