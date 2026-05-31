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
#define S_ISLNK(m)  (((m) & S_IFMT) == S_IFLNK)
#define S_IFMT      0xF000

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

/* poll(2)/select(2) readiness event bits (Linux-compatible). */
#define POLLIN   0x0001
#define POLLPRI  0x0002
#define POLLOUT  0x0004
#define POLLERR  0x0008
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

/* File operations (per-open-file) */
struct vfs_file_ops {
    ssize_t (*read)(struct vfs_file *file, void *buf, size_t count);
    ssize_t (*write)(struct vfs_file *file, const void *buf, size_t count);
    int     (*close)(struct vfs_file *file);
    off_t   (*lseek)(struct vfs_file *file, off_t offset, int whence);
    /* Return the subset of `events` (plus POLLHUP/POLLERR) that are ready
     * right now without blocking.  NULL means "always readable+writable". */
    short   (*poll)(struct vfs_file *file, short events);
    /* Device control; returns 0/positive or negative errno. */
    int     (*ioctl)(struct vfs_file *file, unsigned long req, unsigned long arg);
    /* Truncate the file to `length` bytes (optional). */
    int     (*truncate)(struct vfs_file *file, off_t length);
    /* Map the device's pages into the calling process at user_va for length
     * bytes (used by /dev/fb0).  Returns 0 or negative errno.  NULL means the
     * file is not mmappable. */
    int     (*mmap)(struct vfs_file *file, uint64_t user_va, size_t length,
                    uint64_t prot);
};

/* Query readiness of an open file (returns ready event mask). */
short vfs_poll(struct vfs_file *file, short events);
int   vfs_ioctl(struct vfs_file *file, unsigned long req, unsigned long arg);

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
    /* Create a symbolic link `name` in `dir` pointing at `target` (optional). */
    int (*symlink)(struct vfs_inode *dir, const char *name, const char *target);
    /* Copy this symlink's target into buf (no NUL); returns length or <0
     * (required for any inode that can carry S_IFLNK). */
    int (*readlink)(struct vfs_inode *inode, char *buf, size_t bufsiz);
    /* Flush this inode's attributes (mode/uid/gid) to the backing store so a
     * chmod/chown/creator-ownership change survives a re-read or remount.
     * In-memory filesystems (ramfs) may treat this as a no-op.  Optional. */
    int (*setattr)(struct vfs_inode *inode);
};

/* Persist an inode's mode/uid/gid via its filesystem (no-op if unsupported). */
int vfs_setattr(struct vfs_inode *inode);

/* Inode - represents a file or directory on a filesystem */
struct vfs_inode {
    uint32_t               inode_no;
    uint32_t               mode;        /* file type + permissions */
    uint32_t               size;
    uint32_t               nlink;
    uint32_t               uid;         /* owning user  (M10) */
    uint32_t               gid;         /* owning group (M10) */
    struct vfs_superblock *sb;
    struct vfs_inode_ops  *i_ops;
    struct vfs_file_ops   *f_ops;
    void                  *private_data;
};

/* Permission bits (mode & 0777) and the set-user/group-ID bits. */
#define S_ISUID 0x800
#define S_ISGID 0x400
#define S_IRWXU 0x1C0
#define S_IRUSR 0x100
#define S_IWUSR 0x080
#define S_IXUSR 0x040
#define S_IRWXG 0x038
#define S_IRGRP 0x020
#define S_IWGRP 0x010
#define S_IXGRP 0x008
#define S_IRWXO 0x007
#define S_IROTH 0x004
#define S_IWOTH 0x002
#define S_IXOTH 0x001

/* Access modes for vfs_permission(). */
#define MAY_EXEC  0x1
#define MAY_WRITE 0x2
#define MAY_READ  0x4

/* Check the current thread's credentials against an inode (root bypasses).
 * Returns 0 if allowed, negative errno otherwise. */
int vfs_permission(struct vfs_inode *inode, int mask);
int vfs_access_check(const char *path, int flags);

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
    int                    ref_count;   /* shared across dup()/fork() */
    void                  *private_data; /* e.g. pipe state */
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

/* Mount table (for /proc/mounts and fstab idempotency). */
#define VFS_MAX_MOUNTS 16
struct vfs_mount_rec {
    char dev[32];      /* backing device, or "none" for synthetic fs */
    char path[64];     /* mount point */
    char fstype[16];   /* filesystem type name */
};
int vfs_mounts_count(void);
const struct vfs_mount_rec *vfs_mounts_get(int i);
int vfs_is_mounted(const char *path);

struct vfs_file *vfs_open(const char *path, int flags);
int vfs_close(struct vfs_file *file);
ssize_t vfs_read(struct vfs_file *file, void *buf, size_t count);
ssize_t vfs_write(struct vfs_file *file, const void *buf, size_t count);
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
off_t vfs_lseek(struct vfs_file *file, off_t offset, int whence);

int vfs_mkdir(const char *path, uint32_t mode);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);

/* Create a symlink at `linkpath` pointing at `target`. Returns 0 or <0. */
int vfs_symlink(const char *target, const char *linkpath);
/* Read a symlink's target WITHOUT following it; returns length or <0. */
int vfs_readlink(const char *path, char *buf, size_t bufsiz);

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

/* Console device (/dev/console) backing stdin/stdout/stderr */
void console_init(void);
struct vfs_file *console_open(void);

/* Anonymous pipe: fills files[0]=read end, files[1]=write end */
int pipe_create(struct vfs_file **files);

/* Install a static char-device inode at /dev/<name> (creating /dev on first
 * use).  Used by the framebuffer and input drivers. */
int vfs_devfs_register(const char *name, struct vfs_inode *inode);

/* Dentry helpers */
struct vfs_dentry *vfs_dentry_create(const char *name, struct vfs_inode *inode,
                                      struct vfs_dentry *parent);
void vfs_dentry_add_child(struct vfs_dentry *parent, struct vfs_dentry *child);
struct vfs_dentry *vfs_dentry_find_child(struct vfs_dentry *parent, const char *name);
void vfs_dentry_remove_child(struct vfs_dentry *parent, struct vfs_dentry *child);

#endif /* VFS_H */
