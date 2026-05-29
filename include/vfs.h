#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>

typedef int64_t off_t;
typedef long ssize_t;

/* File types */
#define S_IFREG  0x8000
#define S_IFDIR  0x4000
#define S_IFCHR  0x2000
#define S_IFBLK  0x6000
#define S_IFIFO  0x1000
#define S_IFLNK  0xA000
#define S_IFSOCK 0xC000

#define S_ISREG(m)  (((m) & S_IFREG) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFDIR) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFCHR) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFBLK) == S_IFBLK)

/* File open flags */
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_CREAT   0x0040
#define O_TRUNC   0x0200
#define O_APPEND  0x0400
#define O_DIRECTORY 0x010000

/* Forward declarations */
struct vfs_inode;
struct vfs_dentry;
struct vfs_file;
struct vfs_superblock;
struct vfs_fs_type;

/* File operations (per-open-file) */
struct vfs_file_ops {
    ssize_t (*read)(struct vfs_file *file, void *buf, size_t count);
    ssize_t (*write)(struct vfs_file *file, const void *buf, size_t count);
    int     (*close)(struct vfs_file *file);
    off_t   (*lseek)(struct vfs_file *file, off_t offset, int whence);
};

/* Directory entry for readdir */
struct vfs_dir_entry {
    char     name[64];
    uint32_t inode_no;
    uint32_t mode;
};

/* Inode operations (per-inode, for directory manipulation) */
struct vfs_inode_ops {
    struct vfs_dentry *(*lookup)(struct vfs_inode *dir, const char *name);
    int (*create)(struct vfs_inode *dir, const char *name, uint32_t mode);
    int (*mkdir)(struct vfs_inode *dir, const char *name, uint32_t mode);
    int (*unlink)(struct vfs_inode *dir, const char *name);
    int (*rmdir)(struct vfs_inode *dir, const char *name);
    int (*readdir)(struct vfs_inode *dir, int index, struct vfs_dir_entry *entry);
};

/* Inode - represents a file or directory on a filesystem */
struct vfs_inode {
    uint32_t               inode_no;
    uint32_t               mode;        /* file type + permissions */
    uint32_t               size;
    uint32_t               nlink;
    struct vfs_superblock *sb;
    struct vfs_inode_ops  *i_ops;
    struct vfs_file_ops   *f_ops;
    void                  *private_data;
};

/* Dentry - directory entry, maps a name to an inode */
struct vfs_dentry {
    char                   name[64];
    struct vfs_inode      *inode;
    struct vfs_dentry     *parent;
    struct vfs_dentry     *child_list;   /* first child if directory */
    struct vfs_dentry     *next_sibling; /* next in parent's child_list */
    struct vfs_superblock *mount;        /* mounted fs root, or NULL */
};

/* File - open file handle */
struct vfs_file {
    struct vfs_dentry     *dentry;
    struct vfs_inode      *inode;
    off_t                  pos;
    uint32_t               flags;
};

/* Superblock - represents a mounted filesystem instance */
struct vfs_superblock {
    struct vfs_fs_type    *fs_type;
    struct vfs_dentry     *root;
    void                  *private_data;
};

/* Filesystem type registration */
struct vfs_fs_type {
    const char            *name;
    struct vfs_superblock *(*mount)(const char *dev_name);
    int                    (*unmount)(struct vfs_superblock *sb);
    struct vfs_fs_type    *next;
};

/* --------------------------------------------------------------------------
 * VFS core API
 * -------------------------------------------------------------------------- */
void vfs_init(void);

int vfs_register_fs(struct vfs_fs_type *fs);
int vfs_mount(const char *fs_name, const char *dev_name, const char *path);

struct vfs_file *vfs_open(const char *path, int flags);
int vfs_close(struct vfs_file *file);
ssize_t vfs_read(struct vfs_file *file, void *buf, size_t count);
ssize_t vfs_write(struct vfs_file *file, const void *buf, size_t count);
off_t vfs_lseek(struct vfs_file *file, off_t offset, int whence);

int vfs_mkdir(const char *path, uint32_t mode);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);

/* Directory handle */
struct vfs_dir {
    struct vfs_dentry *dentry;
    struct vfs_inode  *inode;
    int                index;
};

struct vfs_dir *vfs_opendir(const char *path);
int vfs_readdir(struct vfs_dir *dir, struct vfs_dir_entry *entry);
void vfs_closedir(struct vfs_dir *dir);

/* Path resolution helpers */
struct vfs_dentry *vfs_lookup_path(const char *path);
struct vfs_dentry *vfs_lookup_parent(const char *path, char *name_out, size_t name_len);

/* Root dentry accessor */
struct vfs_dentry *vfs_get_root(void);

/* Filesystem initializers */
void ramfs_init(void);

/* Dentry helpers */
struct vfs_dentry *vfs_dentry_create(const char *name, struct vfs_inode *inode,
                                      struct vfs_dentry *parent);
void vfs_dentry_add_child(struct vfs_dentry *parent, struct vfs_dentry *child);
struct vfs_dentry *vfs_dentry_find_child(struct vfs_dentry *parent, const char *name);
void vfs_dentry_remove_child(struct vfs_dentry *parent, struct vfs_dentry *child);

#endif /* VFS_H */
