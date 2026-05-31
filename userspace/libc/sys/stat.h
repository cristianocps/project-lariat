#ifndef LIBC_SYS_STAT_H
#define LIBC_SYS_STAT_H

#include <stdint.h>
#include <stddef.h>
#include "../unistd.h"

/* Mirrors the kernel struct kstat (include/uapi/uapi.h): Linux x86_64 layout. */
struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;

    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;

    int64_t  st_atime;
    int64_t  st_atime_nsec;
    int64_t  st_mtime;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime;
    int64_t  st_ctime_nsec;
    int64_t  __unused[3];
};

/* File type bits (match include/vfs.h). */
#define S_IFMT   0xF000
#define S_IFREG  0x8000
#define S_IFDIR  0x4000
#define S_IFCHR  0x2000
#define S_IFBLK  0x6000
#define S_IFIFO  0x1000
#define S_IFLNK  0xA000
#define S_IFSOCK 0xC000

#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISCHR(m)  (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)  (((m) & S_IFMT) == S_IFBLK)

static inline int stat(const char *path, struct stat *st) {
    return (int)__syscall_ret(syscall2(SYS_STAT, (long)path, (long)st));
}

static inline int fstat(int fd, struct stat *st) {
    return (int)__syscall_ret(syscall2(SYS_FSTAT, fd, (long)st));
}

static inline int chmod(const char *path, unsigned int mode) {
    return (int)__syscall_ret(syscall2(SYS_CHMOD, (long)path, (long)mode));
}

#endif /* LIBC_SYS_STAT_H */
