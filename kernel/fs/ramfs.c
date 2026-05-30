#include "vfs.h"
#include "kapi.h"
#include "serial.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * RAMFS data structures
 * -------------------------------------------------------------------------- */

typedef struct ramfs_inode {
    uint32_t         inode_no;
    uint32_t         mode;
    uint32_t         size;
    uint8_t         *data;       /* file contents, NULL for directories */
    uint32_t         capacity;   /* allocated buffer size */
} ramfs_inode_t;

typedef struct ramfs_sb {
    uint32_t         next_inode_no;
} ramfs_sb_t;

static struct vfs_file_ops  ramfs_file_ops;
static struct vfs_inode_ops ramfs_inode_ops;

/* --------------------------------------------------------------------------
 * RAMFS inode helpers
 * -------------------------------------------------------------------------- */
static struct vfs_inode *ramfs_create_vinode(struct vfs_superblock *sb,
                                               uint32_t mode) {
    ramfs_sb_t *rsb = (ramfs_sb_t *)sb->private_data;

    ramfs_inode_t *ri = kzalloc(sizeof(ramfs_inode_t));
    if (!ri) return NULL;

    ri->inode_no = rsb->next_inode_no++;
    ri->mode = mode;
    ri->size = 0;
    ri->data = NULL;
    ri->capacity = 0;

    struct vfs_inode *inode = kzalloc(sizeof(struct vfs_inode));
    if (!inode) {
        kfree(ri);
        return NULL;
    }

    inode->inode_no = ri->inode_no;
    inode->mode = mode;
    inode->size = 0;
    inode->nlink = 1;
    inode->sb = sb;
    inode->i_ops = &ramfs_inode_ops;
    inode->f_ops = S_ISDIR(mode) ? NULL : &ramfs_file_ops;
    inode->private_data = ri;
    return inode;
}

static void ramfs_destroy_vinode(struct vfs_inode *inode) {
    if (!inode) return;
    ramfs_inode_t *ri = (ramfs_inode_t *)inode->private_data;
    if (ri) {
        if (ri->data) kfree(ri->data);
        kfree(ri);
    }
    kfree(inode);
}

/* --------------------------------------------------------------------------
 * RAMFS file operations
 * -------------------------------------------------------------------------- */
static ssize_t ramfs_read(struct vfs_file *file, void *buf, size_t count) {
    if (!file || !file->inode) return -1;
    ramfs_inode_t *ri = (ramfs_inode_t *)file->inode->private_data;
    if (!ri || !ri->data) return 0;

    if (file->pos >= (off_t)ri->size) return 0;

    size_t avail = ri->size - (size_t)file->pos;
    if (count > avail) count = avail;

    uint8_t *src = ri->data + file->pos;
    uint8_t *dst = buf;
    for (size_t i = 0; i < count; i++) {
        dst[i] = src[i];
    }
    file->pos += (off_t)count;
    return (ssize_t)count;
}

static ssize_t ramfs_write(struct vfs_file *file, const void *buf, size_t count) {
    if (!file || !file->inode) return -1;
    ramfs_inode_t *ri = (ramfs_inode_t *)file->inode->private_data;
    if (!ri) return -1;

    size_t need = (size_t)file->pos + count;
    if (need > ri->capacity) {
        size_t new_cap = ri->capacity ? ri->capacity * 2 : 4096;
        while (new_cap < need) new_cap *= 2;

        uint8_t *new_data = kzalloc(new_cap);
        if (!new_data) return -1;

        if (ri->data) {
            for (size_t i = 0; i < ri->size; i++) {
                new_data[i] = ri->data[i];
            }
            kfree(ri->data);
        }
        ri->data = new_data;
        ri->capacity = new_cap;
    }

    const uint8_t *src = buf;
    uint8_t *dst = ri->data + file->pos;
    for (size_t i = 0; i < count; i++) {
        dst[i] = src[i];
    }
    file->pos += (off_t)count;
    if ((size_t)file->pos > ri->size) {
        ri->size = (size_t)file->pos;
        file->inode->size = ri->size;
    }
    return (ssize_t)count;
}

static int ramfs_close(struct vfs_file *file) {
    (void)file;
    return 0;
}

static off_t ramfs_lseek(struct vfs_file *file, off_t offset, int whence) {
    if (!file || !file->inode) return -1;
    ramfs_inode_t *ri = (ramfs_inode_t *)file->inode->private_data;

    switch (whence) {
        case 0: /* SEEK_SET */
            file->pos = offset;
            break;
        case 1: /* SEEK_CUR */
            file->pos += offset;
            break;
        case 2: /* SEEK_END */
            file->pos = (off_t)ri->size + offset;
            break;
        default:
            return -1;
    }
    if (file->pos < 0) file->pos = 0;
    return file->pos;
}

static struct vfs_file_ops ramfs_file_ops = {
    .read   = ramfs_read,
    .write  = ramfs_write,
    .close  = ramfs_close,
    .lseek  = ramfs_lseek,
};

/* --------------------------------------------------------------------------
 * RAMFS inode (directory) operations
 * -------------------------------------------------------------------------- */
/* Locate the dentry that owns `inode` by walking the dentry tree from the
 * superblock root.  Lookups, creates and directory ops must all resolve the
 * parent *by identity* - searching globally by name (as an earlier version did)
 * makes same-named files in different directories (e.g. /etc/passwd vs
 * /bin/passwd) alias to whichever the search happened to hit first. */
static struct vfs_dentry *ramfs_dentry_for(struct vfs_dentry *d,
                                           struct vfs_inode *inode) {
    if (!d) return NULL;
    if (d->inode == inode) return d;
    for (struct vfs_dentry *c = d->child_list; c; c = c->next_sibling) {
        struct vfs_dentry *r = ramfs_dentry_for(c, inode);
        if (r) return r;
    }
    return NULL;
}

static struct vfs_dentry *ramfs_parent_dentry(struct vfs_inode *dir) {
    if (!dir || !dir->sb || !dir->sb->root) return NULL;
    return ramfs_dentry_for(dir->sb->root, dir);
}

static struct vfs_dentry *ramfs_lookup(struct vfs_inode *dir, const char *name) {
    struct vfs_dentry *parent = ramfs_parent_dentry(dir);
    if (!parent) return NULL;
    return vfs_dentry_find_child(parent, name);
}

static int ramfs_create(struct vfs_inode *dir, const char *name, uint32_t mode) {
    if (!dir || !dir->sb) return -1;

    struct vfs_inode *inode = ramfs_create_vinode(dir->sb, mode);
    if (!inode) return -1;

    struct vfs_dentry *parent = ramfs_parent_dentry(dir);
    if (!parent) parent = dir->sb->root;

    struct vfs_dentry *dentry = vfs_dentry_create(name, inode, parent);
    if (!dentry) {
        ramfs_destroy_vinode(inode);
        return -1;
    }
    vfs_dentry_add_child(parent, dentry);
    return 0;
}

static int ramfs_mkdir(struct vfs_inode *dir, const char *name, uint32_t mode) {
    return ramfs_create(dir, name, mode);
}

static int ramfs_unlink(struct vfs_inode *dir, const char *name) {
    struct vfs_dentry *parent = ramfs_parent_dentry(dir);
    if (!parent) return -1;

    struct vfs_dentry *child = vfs_dentry_find_child(parent, name);
    if (!child || !child->inode) return -1;
    if (S_ISDIR(child->inode->mode)) return -1; /* use rmdir */

    vfs_dentry_remove_child(parent, child);
    ramfs_destroy_vinode(child->inode);
    kfree(child);
    return 0;
}

static int ramfs_rmdir(struct vfs_inode *dir, const char *name) {
    struct vfs_dentry *parent = ramfs_parent_dentry(dir);
    if (!parent) return -1;

    struct vfs_dentry *child = vfs_dentry_find_child(parent, name);
    if (!child || !child->inode) return -1;
    if (!S_ISDIR(child->inode->mode)) return -1; /* use unlink */
    if (child->child_list) return -1; /* not empty */

    vfs_dentry_remove_child(parent, child);
    ramfs_destroy_vinode(child->inode);
    kfree(child);
    return 0;
}

static int ramfs_readdir(struct vfs_inode *dir, int index, struct vfs_dir_entry *entry) {
    struct vfs_dentry *parent = ramfs_parent_dentry(dir);
    if (!parent) return -1;

    struct vfs_dentry *child = parent->child_list;
    int idx = 0;
    while (child) {
        if ((child->name[0] == '.' && child->name[1] == '\0') ||
            (child->name[0] == '.' && child->name[1] == '.' && child->name[2] == '\0')) {
            child = child->next_sibling;
            continue;
        }
        if (idx == index) {
            size_t i = 0;
            while (child->name[i] && i < sizeof(entry->name) - 1) {
                entry->name[i] = child->name[i];
                i++;
            }
            entry->name[i] = '\0';
            entry->inode_no = child->inode ? child->inode->inode_no : 0;
            entry->mode = child->inode ? child->inode->mode : 0;
            return 1;
        }
        idx++;
        child = child->next_sibling;
    }
    return 0;
}

static struct vfs_inode_ops ramfs_inode_ops = {
    .lookup = ramfs_lookup,
    .create = ramfs_create,
    .mkdir  = ramfs_mkdir,
    .unlink = ramfs_unlink,
    .rmdir  = ramfs_rmdir,
    .readdir = ramfs_readdir,
};

/* --------------------------------------------------------------------------
 * RAMFS mount / unmount
 * -------------------------------------------------------------------------- */
static struct vfs_superblock *ramfs_mount(const char *dev_name) {
    (void)dev_name;

    ramfs_sb_t *rsb = kzalloc(sizeof(ramfs_sb_t));
    if (!rsb) return NULL;
    rsb->next_inode_no = 1;

    struct vfs_superblock *sb = kzalloc(sizeof(struct vfs_superblock));
    if (!sb) {
        kfree(rsb);
        return NULL;
    }
    sb->fs_type = NULL;  /* set by caller */
    sb->private_data = rsb;

    /* Create root inode */
    struct vfs_inode *root_inode = ramfs_create_vinode(sb, S_IFDIR | 0755);
    if (!root_inode) {
        kfree(sb);
        kfree(rsb);
        return NULL;
    }

    sb->root = vfs_dentry_create("/", root_inode, NULL);
    if (!sb->root) {
        ramfs_destroy_vinode(root_inode);
        kfree(sb);
        kfree(rsb);
        return NULL;
    }

    return sb;
}

static int ramfs_unmount(struct vfs_superblock *sb) {
    if (!sb) return -1;
    /* TODO: recursively free all dentries and inodes */
    kfree(sb->private_data);
    kfree(sb);
    return 0;
}

static struct vfs_fs_type ramfs_fs_type = {
    .name     = "ramfs",
    .mount    = ramfs_mount,
    .unmount  = ramfs_unmount,
    .next     = NULL,
};

/* --------------------------------------------------------------------------
 * Registration
 * -------------------------------------------------------------------------- */
void ramfs_init(void) {
    vfs_register_fs(&ramfs_fs_type);
}
