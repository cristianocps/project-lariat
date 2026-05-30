#include "fat32.h"
#include "vfs.h"
#include "block.h"
#include "kapi.h"
#include "serial.h"
#include <string.h>

/* Forward declaration */
static int fat32_readdir_internal(fat32_fs_t *fs, uint32_t cluster,
                                   int index, fat32_dirent_t *out, char *name_out);

/* --------------------------------------------------------------------------
 * FAT32 internal helpers
 * -------------------------------------------------------------------------- */

static int fat32_read_sector(fat32_fs_t *fs, uint32_t lba, void *buf) {
    return block_read(fs->dev, lba, buf, 1);
}

static uint32_t fat32_cluster_to_lba(fat32_fs_t *fs, uint32_t cluster) {
    return fs->data_start + (cluster - 2) * fs->bpb.sectors_per_cluster;
}

static uint32_t fat32_read_fat_entry(fat32_fs_t *fs, uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fs->fat_start + (fat_offset / 512);
    uint32_t ent_offset = fat_offset % 512;

    uint8_t sector[512];
    if (fat32_read_sector(fs, fat_sector, sector) < 0)
        return FAT32_CLUSTER_EOF;

    uint32_t next = *(uint32_t *)(sector + ent_offset);
    return next & 0x0FFFFFFF;
}

static int fat32_read_cluster(fat32_fs_t *fs, uint32_t cluster, void *buf) {
    uint32_t lba = fat32_cluster_to_lba(fs, cluster);
    for (int i = 0; i < fs->bpb.sectors_per_cluster; i++) {
        if (fat32_read_sector(fs, lba + i, (uint8_t *)buf + i * 512) < 0)
            return -1;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * FAT32 write helpers (M7)
 * -------------------------------------------------------------------------- */

static int fat32_write_sector(fat32_fs_t *fs, uint32_t lba, const void *buf) {
    return block_write(fs->dev, lba, buf, 1);
}

static int fat32_write_cluster(fat32_fs_t *fs, uint32_t cluster, const void *buf) {
    uint32_t lba = fat32_cluster_to_lba(fs, cluster);
    for (int i = 0; i < fs->bpb.sectors_per_cluster; i++) {
        if (fat32_write_sector(fs, lba + i, (const uint8_t *)buf + i * 512) < 0)
            return -1;
    }
    return 0;
}

/* Update a single FAT entry in every FAT copy on disk. */
static int fat32_write_fat_entry(fat32_fs_t *fs, uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t ent_offset = fat_offset % 512;
    uint8_t sector[512];

    for (int f = 0; f < fs->bpb.num_fats; f++) {
        uint32_t fat_sector = fs->fat_start + f * fs->bpb.fat_size_32 + (fat_offset / 512);
        if (fat32_read_sector(fs, fat_sector, sector) < 0)
            return -1;
        uint32_t *slot = (uint32_t *)(sector + ent_offset);
        *slot = (*slot & 0xF0000000u) | (value & 0x0FFFFFFFu);
        if (fat32_write_sector(fs, fat_sector, sector) < 0)
            return -1;
    }
    return 0;
}

/* Allocate one free cluster, mark it EOF, zero its data, return it (0 = fail). */
static uint32_t fat32_alloc_cluster(fat32_fs_t *fs) {
    for (uint32_t c = 2; c < fs->total_clusters + 2; c++) {
        if (fat32_read_fat_entry(fs, c) == FAT32_CLUSTER_FREE) {
            if (fat32_write_fat_entry(fs, c, FAT32_CLUSTER_EOF) < 0)
                return 0;
            uint8_t *zero = kzalloc(fs->bytes_per_cluster);
            if (zero) {
                fat32_write_cluster(fs, c, zero);
                kfree(zero);
            }
            return c;
        }
    }
    return 0;  /* disk full */
}

/* Free an entire cluster chain. */
static void fat32_free_chain(fat32_fs_t *fs, uint32_t cluster) {
    while (cluster >= FAT32_CLUSTER_MIN && cluster <= FAT32_CLUSTER_MAX) {
        uint32_t next = fat32_read_fat_entry(fs, cluster);
        fat32_write_fat_entry(fs, cluster, FAT32_CLUSTER_FREE);
        cluster = next;
    }
}

/* Read or write a 32-byte directory entry at (cluster, byte-offset). */
static int fat32_dirent_rw(fat32_fs_t *fs, uint32_t cluster, uint32_t off,
                           fat32_dirent_t *ent, int write) {
    uint32_t lba = fat32_cluster_to_lba(fs, cluster) + off / 512;
    uint32_t so = off % 512;
    uint8_t sec[512];
    if (fat32_read_sector(fs, lba, sec) < 0)
        return -1;
    if (write) {
        memcpy(sec + so, ent, sizeof(*ent));
        return fat32_write_sector(fs, lba, sec);
    }
    memcpy(ent, sec + so, sizeof(*ent));
    return 0;
}

/* Find a free directory slot in `dir_cluster`, extending the directory with a
 * fresh cluster if it is full.  Returns the slot location via out params. */
static int fat32_alloc_dirent(fat32_fs_t *fs, uint32_t dir_cluster,
                              uint32_t *out_cluster, uint32_t *out_off) {
    uint32_t cluster = dir_cluster;
    uint32_t prev = cluster;
    uint8_t *buf = kzalloc(fs->bytes_per_cluster);
    if (!buf) return -1;

    while (cluster >= FAT32_CLUSTER_MIN && cluster <= FAT32_CLUSTER_MAX) {
        if (fat32_read_cluster(fs, cluster, buf) < 0) {
            kfree(buf);
            return -1;
        }
        fat32_dirent_t *entries = (fat32_dirent_t *)buf;
        int count = fs->bytes_per_cluster / sizeof(fat32_dirent_t);
        for (int i = 0; i < count; i++) {
            if (entries[i].name[0] == 0x00 || entries[i].name[0] == 0xE5) {
                *out_cluster = cluster;
                *out_off = (uint32_t)i * sizeof(fat32_dirent_t);
                kfree(buf);
                return 0;
            }
        }
        prev = cluster;
        cluster = fat32_read_fat_entry(fs, cluster);
    }

    /* Directory full: append a new zeroed cluster. */
    kfree(buf);
    uint32_t nc = fat32_alloc_cluster(fs);
    if (!nc) return -1;
    if (fat32_write_fat_entry(fs, prev, nc) < 0) return -1;
    *out_cluster = nc;
    *out_off = 0;
    return 0;
}

/* Build an 8.3 short name (uppercased, space-padded) from a user name.
 * Returns -1 if the name does not fit the 8.3 form. */
static int fat32_make_short_name(const char *name, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int i = 0, n = 0;
    /* base name */
    while (name[i] && name[i] != '.') {
        if (n >= 8) return -1;
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        out[n++] = (uint8_t)c;
        i++;
    }
    if (name[i] == '.') {
        i++;
        int e = 8;
        while (name[i]) {
            if (e >= 11) return -1;
            char c = name[i];
            if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
            out[e++] = (uint8_t)c;
            i++;
        }
    }
    if (n == 0) return -1;
    return 0;
}

/* Convert 8.3 name to a readable form */
static void fat32_parse_short_name(const uint8_t *src, char *dst) {
    int i, j = 0;
    /* Name part (first 8 chars) */
    for (i = 0; i < 8 && src[i] != ' '; i++) {
        dst[j++] = src[i];
    }
    /* Extension */
    if (src[8] != ' ') {
        dst[j++] = '.';
        for (i = 8; i < 11 && src[i] != ' '; i++) {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    /* Lowercase conversion */
    for (i = 0; dst[i]; i++) {
        if (dst[i] >= 'A' && dst[i] <= 'Z')
            dst[i] = dst[i] - 'A' + 'a';
    }
}

/* Compare a short 8.3 name with a user-provided name */
static int fat32_name_match(const uint8_t *entry_name, const char *user_name) {
    char parsed[16];
    fat32_parse_short_name(entry_name, parsed);
    const char *a = parsed;
    const char *b = user_name;
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/* --------------------------------------------------------------------------
 * Directory traversal
 * -------------------------------------------------------------------------- */

typedef struct {
    fat32_fs_t   *fs;
    uint32_t      cluster;
    uint32_t      offset;
} fat32_dir_ctx_t;

static int fat32_find_in_dir_loc(fat32_fs_t *fs, uint32_t cluster,
                                 const char *name, fat32_dirent_t *out,
                                 uint32_t *out_cluster, uint32_t *out_off) {
    uint8_t *cluster_buf = kzalloc(fs->bytes_per_cluster);
    if (!cluster_buf) return -1;

    while (cluster >= FAT32_CLUSTER_MIN && cluster <= FAT32_CLUSTER_MAX) {
        if (fat32_read_cluster(fs, cluster, cluster_buf) < 0) {
            kfree(cluster_buf);
            return -1;
        }

        fat32_dirent_t *entries = (fat32_dirent_t *)cluster_buf;
        int count = fs->bytes_per_cluster / sizeof(fat32_dirent_t);

        for (int i = 0; i < count; i++) {
            fat32_dirent_t *ent = &entries[i];
            if (ent->name[0] == 0x00) {
                kfree(cluster_buf);
                return -1;  /* End of directory */
            }
            if (ent->name[0] == 0xE5) continue;  /* Deleted */
            if (ent->attr == FAT32_ATTR_LFN) continue;  /* LFN entry */
            if (ent->attr & FAT32_ATTR_VOLUME_ID) continue;

            if (fat32_name_match(ent->name, name)) {
                *out = *ent;
                if (out_cluster) *out_cluster = cluster;
                if (out_off) *out_off = (uint32_t)i * sizeof(fat32_dirent_t);
                kfree(cluster_buf);
                return 0;
            }
        }

        cluster = fat32_read_fat_entry(fs, cluster);
    }

    kfree(cluster_buf);
    return -1;
}

static int fat32_find_in_dir(fat32_fs_t *fs, uint32_t cluster,
                              const char *name, fat32_dirent_t *out) {
    return fat32_find_in_dir_loc(fs, cluster, name, out, NULL, NULL);
}

static uint32_t fat32_resolve_path(fat32_fs_t *fs, const char *path,
                                    fat32_dirent_t *out) {
    uint32_t cluster = fs->bpb.root_cluster;

    if (path[0] == '/') path++;
    if (path[0] == '\0') {
        /* Root directory itself */
        out->attr = FAT32_ATTR_DIRECTORY;
        out->fst_clus_hi = (uint16_t)(cluster >> 16);
        out->fst_clus_lo = (uint16_t)(cluster & 0xFFFF);
        out->file_size = 0;
        return cluster;
    }

    char comp[16];
    const char *p = path;

    while (*p) {
        /* Extract next path component */
        int i = 0;
        while (*p && *p != '/' && i < 15) {
            comp[i++] = *p++;
        }
        comp[i] = '\0';
        while (*p == '/') p++;

        fat32_dirent_t ent;
        if (fat32_find_in_dir(fs, cluster, comp, &ent) < 0)
            return 0;

        if (*p == '\0') {
            /* Last component */
            *out = ent;
            return ((uint32_t)ent.fst_clus_hi << 16) | ent.fst_clus_lo;
        }

        if (!(ent.attr & FAT32_ATTR_DIRECTORY))
            return 0;  /* Not a directory but more path follows */

        cluster = ((uint32_t)ent.fst_clus_hi << 16) | ent.fst_clus_lo;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * VFS inode / file operations
 * -------------------------------------------------------------------------- */

typedef struct fat32_inode_priv {
    fat32_fs_t    *fs;
    uint32_t       cluster;     /* first data cluster (0 = empty file) */
    uint32_t       size;
    uint32_t       attr;
    uint32_t       ent_cluster; /* dir cluster holding this file's 8.3 entry */
    uint32_t       ent_off;     /* byte offset of the entry within ent_cluster */
} fat32_inode_priv_t;

/* Forward decls used by write helpers below. */
static struct vfs_inode_ops fat32_inode_ops;

/* Persist the file's first-cluster + size back into its on-disk dirent. */
static void fat32_sync_dirent(fat32_inode_priv_t *priv) {
    if (priv->ent_cluster < FAT32_CLUSTER_MIN) return;  /* e.g. root inode */
    fat32_dirent_t ent;
    if (fat32_dirent_rw(priv->fs, priv->ent_cluster, priv->ent_off, &ent, 0) < 0)
        return;
    ent.fst_clus_hi = (uint16_t)(priv->cluster >> 16);
    ent.fst_clus_lo = (uint16_t)(priv->cluster & 0xFFFF);
    ent.file_size = priv->size;
    fat32_dirent_rw(priv->fs, priv->ent_cluster, priv->ent_off, &ent, 1);
    block_flush(priv->fs->dev);
}

static ssize_t fat32_file_read(struct vfs_file *file, void *buf, size_t count) {
    if (!file || !file->inode) return -1;
    fat32_inode_priv_t *priv = (fat32_inode_priv_t *)file->inode->private_data;
    fat32_fs_t *fs = priv->fs;

    if (file->pos >= (off_t)priv->size) return 0;
    if (file->pos + (off_t)count > (off_t)priv->size)
        count = priv->size - (size_t)file->pos;

    uint8_t *dest = buf;
    size_t bytes_read = 0;
    uint32_t cluster = priv->cluster;
    uint32_t pos = (uint32_t)file->pos;

    /* Skip clusters until we reach the starting position */
    uint32_t cluster_offset = pos / fs->bytes_per_cluster;
    for (uint32_t i = 0; i < cluster_offset; i++) {
        cluster = fat32_read_fat_entry(fs, cluster);
        if (cluster < FAT32_CLUSTER_MIN || cluster > FAT32_CLUSTER_MAX)
            return bytes_read;
    }

    uint32_t offset_in_cluster = pos % fs->bytes_per_cluster;
    uint8_t *cluster_buf = kzalloc(fs->bytes_per_cluster);
    if (!cluster_buf) return -1;

    while (bytes_read < count) {
        if (cluster < FAT32_CLUSTER_MIN || cluster > FAT32_CLUSTER_MAX)
            break;

        if (fat32_read_cluster(fs, cluster, cluster_buf) < 0)
            break;

        size_t avail = fs->bytes_per_cluster - offset_in_cluster;
        size_t to_copy = count - bytes_read;
        if (to_copy > avail) to_copy = avail;

        for (size_t i = 0; i < to_copy; i++) {
            dest[bytes_read + i] = cluster_buf[offset_in_cluster + i];
        }
        bytes_read += to_copy;
        offset_in_cluster = 0;
        cluster = fat32_read_fat_entry(fs, cluster);
    }

    kfree(cluster_buf);
    file->pos += (off_t)bytes_read;
    return (ssize_t)bytes_read;
}

/* Return the cluster holding logical cluster index `idx` of the file,
 * extending the chain (and the file's first cluster) as needed. 0 = failure. */
static uint32_t fat32_nth_cluster_alloc(fat32_fs_t *fs, fat32_inode_priv_t *priv,
                                        uint32_t idx) {
    if (priv->cluster < FAT32_CLUSTER_MIN) {
        uint32_t c = fat32_alloc_cluster(fs);
        if (!c) return 0;
        priv->cluster = c;
    }
    uint32_t cur = priv->cluster;
    for (uint32_t i = 0; i < idx; i++) {
        uint32_t next = fat32_read_fat_entry(fs, cur);
        if (next < FAT32_CLUSTER_MIN || next > FAT32_CLUSTER_MAX) {
            uint32_t c = fat32_alloc_cluster(fs);
            if (!c) return 0;
            if (fat32_write_fat_entry(fs, cur, c) < 0) return 0;
            next = c;
        }
        cur = next;
    }
    return cur;
}

static ssize_t fat32_file_write(struct vfs_file *file, const void *buf, size_t count) {
    if (!file || !file->inode) return -1;
    fat32_inode_priv_t *priv = (fat32_inode_priv_t *)file->inode->private_data;
    fat32_fs_t *fs = priv->fs;
    if (count == 0) return 0;

    if (file->flags & O_APPEND)
        file->pos = (off_t)priv->size;

    const uint8_t *src = buf;
    size_t written = 0;
    uint32_t pos = (uint32_t)file->pos;
    uint8_t *cbuf = kzalloc(fs->bytes_per_cluster);
    if (!cbuf) return -1;

    while (written < count) {
        uint32_t cidx = pos / fs->bytes_per_cluster;
        uint32_t coff = pos % fs->bytes_per_cluster;
        uint32_t cluster = fat32_nth_cluster_alloc(fs, priv, cidx);
        if (!cluster) break;  /* disk full */

        size_t chunk = fs->bytes_per_cluster - coff;
        if (chunk > count - written) chunk = count - written;

        /* Read-modify-write when not overwriting a whole cluster. */
        if (coff != 0 || chunk != (size_t)fs->bytes_per_cluster) {
            if (fat32_read_cluster(fs, cluster, cbuf) < 0) break;
        }
        memcpy(cbuf + coff, src + written, chunk);
        if (fat32_write_cluster(fs, cluster, cbuf) < 0) break;

        written += chunk;
        pos += chunk;
    }
    kfree(cbuf);

    file->pos = (off_t)pos;
    if (pos > priv->size) {
        priv->size = pos;
        file->inode->size = pos;
    }
    fat32_sync_dirent(priv);
    return written ? (ssize_t)written : -1;
}

static int fat32_file_truncate(struct vfs_file *file, off_t length) {
    if (!file || !file->inode) return -1;
    fat32_inode_priv_t *priv = (fat32_inode_priv_t *)file->inode->private_data;
    fat32_fs_t *fs = priv->fs;

    if (length == 0) {
        if (priv->cluster >= FAT32_CLUSTER_MIN)
            fat32_free_chain(fs, priv->cluster);
        priv->cluster = 0;
        priv->size = 0;
        file->inode->size = 0;
        fat32_sync_dirent(priv);
    }
    return 0;
}

static int fat32_file_close(struct vfs_file *file) {
    (void)file;
    return 0;
}

static off_t fat32_file_lseek(struct vfs_file *file, off_t offset, int whence) {
    if (!file || !file->inode) return -1;
    fat32_inode_priv_t *priv = (fat32_inode_priv_t *)file->inode->private_data;

    switch (whence) {
        case 0: file->pos = offset; break;
        case 1: file->pos += offset; break;
        case 2: file->pos = (off_t)priv->size + offset; break;
        default: return -1;
    }
    if (file->pos < 0) file->pos = 0;
    return file->pos;
}

static struct vfs_file_ops fat32_file_ops = {
    .read     = fat32_file_read,
    .write    = fat32_file_write,
    .close    = fat32_file_close,
    .lseek    = fat32_file_lseek,
    .truncate = fat32_file_truncate,
};

/* --------------------------------------------------------------------------
 * VFS inode (directory) operations
 * -------------------------------------------------------------------------- */

static struct vfs_inode_ops fat32_inode_ops;

static struct vfs_inode *fat32_create_inode_at(fat32_fs_t *fs,
                                               fat32_dirent_t *ent,
                                               uint32_t ent_cluster,
                                               uint32_t ent_off) {
    struct vfs_inode *inode = kzalloc(sizeof(struct vfs_inode));
    if (!inode) return NULL;

    fat32_inode_priv_t *priv = kzalloc(sizeof(fat32_inode_priv_t));
    if (!priv) {
        kfree(inode);
        return NULL;
    }

    priv->fs = fs;
    priv->cluster = ((uint32_t)ent->fst_clus_hi << 16) | ent->fst_clus_lo;
    priv->size = ent->file_size;
    priv->attr = ent->attr;
    priv->ent_cluster = ent_cluster;
    priv->ent_off = ent_off;

    inode->inode_no = priv->cluster;  /* Use cluster as inode number */
    inode->mode = (ent->attr & FAT32_ATTR_DIRECTORY) ? S_IFDIR | 0755 : S_IFREG | 0644;
    inode->size = ent->file_size;
    inode->nlink = 1;
    inode->sb = fs->dev ? (struct vfs_superblock *)fs->dev->priv : NULL;
    inode->i_ops = &fat32_inode_ops;
    inode->f_ops = (ent->attr & FAT32_ATTR_DIRECTORY) ? NULL : &fat32_file_ops;
    inode->private_data = priv;
    return inode;
}

static struct vfs_inode *fat32_create_inode(fat32_fs_t *fs, fat32_dirent_t *ent) {
    return fat32_create_inode_at(fs, ent, 0, 0);
}

static struct vfs_dentry *fat32_lookup(struct vfs_inode *dir, const char *name) {
    fat32_inode_priv_t *priv = (fat32_inode_priv_t *)dir->private_data;
    fat32_fs_t *fs = priv->fs;

    fat32_dirent_t ent;
    uint32_t ent_cluster = 0, ent_off = 0;
    if (fat32_find_in_dir_loc(fs, priv->cluster, name, &ent, &ent_cluster, &ent_off) < 0)
        return NULL;

    struct vfs_inode *inode = fat32_create_inode_at(fs, &ent, ent_cluster, ent_off);
    if (!inode) return NULL;

    struct vfs_dentry *dentry = vfs_dentry_create(name, inode, dir->sb ? dir->sb->root : NULL);
    return dentry;
}

/* Write a fresh directory entry into a free slot of `dir_cluster`.
 * `first_cluster` is the entry's starting data cluster (0 for empty). */
static int fat32_add_dirent(fat32_fs_t *fs, uint32_t dir_cluster,
                            const char *name, uint8_t attr,
                            uint32_t first_cluster, uint32_t size,
                            uint32_t *out_cluster, uint32_t *out_off) {
    uint8_t shortname[11];
    if (fat32_make_short_name(name, shortname) < 0)
        return -1;

    uint32_t sc, so;
    if (fat32_alloc_dirent(fs, dir_cluster, &sc, &so) < 0)
        return -1;

    fat32_dirent_t ent;
    memset(&ent, 0, sizeof(ent));
    memcpy(ent.name, shortname, 11);
    ent.attr = attr;
    ent.fst_clus_hi = (uint16_t)(first_cluster >> 16);
    ent.fst_clus_lo = (uint16_t)(first_cluster & 0xFFFF);
    ent.file_size = size;
    if (fat32_dirent_rw(fs, sc, so, &ent, 1) < 0)
        return -1;
    block_flush(fs->dev);

    if (out_cluster) *out_cluster = sc;
    if (out_off) *out_off = so;
    return 0;
}

static int fat32_create(struct vfs_inode *dir, const char *name, uint32_t mode) {
    (void)mode;
    fat32_inode_priv_t *dpriv = (fat32_inode_priv_t *)dir->private_data;
    fat32_fs_t *fs = dpriv->fs;

    fat32_dirent_t existing;
    if (fat32_find_in_dir(fs, dpriv->cluster, name, &existing) == 0)
        return -1;  /* already exists */

    return fat32_add_dirent(fs, dpriv->cluster, name, FAT32_ATTR_ARCHIVE,
                            0, 0, NULL, NULL);
}

static int fat32_mkdir(struct vfs_inode *dir, const char *name, uint32_t mode) {
    (void)mode;
    fat32_inode_priv_t *dpriv = (fat32_inode_priv_t *)dir->private_data;
    fat32_fs_t *fs = dpriv->fs;

    fat32_dirent_t existing;
    if (fat32_find_in_dir(fs, dpriv->cluster, name, &existing) == 0)
        return -1;

    /* Allocate the new directory's first cluster and seed it with . and .. */
    uint32_t newc = fat32_alloc_cluster(fs);
    if (!newc) return -1;

    uint32_t parent = dpriv->cluster;
    if (parent == fs->bpb.root_cluster) parent = 0;  /* FAT convention */

    uint8_t *cbuf = kzalloc(fs->bytes_per_cluster);
    if (!cbuf) { fat32_free_chain(fs, newc); return -1; }
    fat32_dirent_t *e = (fat32_dirent_t *)cbuf;
    memset(&e[0], 0, sizeof(e[0]));
    memcpy(e[0].name, ".          ", 11);
    e[0].attr = FAT32_ATTR_DIRECTORY;
    e[0].fst_clus_hi = (uint16_t)(newc >> 16);
    e[0].fst_clus_lo = (uint16_t)(newc & 0xFFFF);
    memset(&e[1], 0, sizeof(e[1]));
    memcpy(e[1].name, "..         ", 11);
    e[1].attr = FAT32_ATTR_DIRECTORY;
    e[1].fst_clus_hi = (uint16_t)(parent >> 16);
    e[1].fst_clus_lo = (uint16_t)(parent & 0xFFFF);
    fat32_write_cluster(fs, newc, cbuf);
    kfree(cbuf);

    if (fat32_add_dirent(fs, dpriv->cluster, name, FAT32_ATTR_DIRECTORY,
                         newc, 0, NULL, NULL) < 0) {
        fat32_free_chain(fs, newc);
        return -1;
    }
    return 0;
}

static int fat32_unlink(struct vfs_inode *dir, const char *name) {
    fat32_inode_priv_t *dpriv = (fat32_inode_priv_t *)dir->private_data;
    fat32_fs_t *fs = dpriv->fs;

    fat32_dirent_t ent;
    uint32_t ec, eo;
    if (fat32_find_in_dir_loc(fs, dpriv->cluster, name, &ent, &ec, &eo) < 0)
        return -1;
    if (ent.attr & FAT32_ATTR_DIRECTORY)
        return -1;  /* use rmdir */

    uint32_t first = ((uint32_t)ent.fst_clus_hi << 16) | ent.fst_clus_lo;
    if (first >= FAT32_CLUSTER_MIN)
        fat32_free_chain(fs, first);

    ent.name[0] = 0xE5;
    fat32_dirent_rw(fs, ec, eo, &ent, 1);
    block_flush(fs->dev);
    return 0;
}

/* Returns 1 if a directory cluster chain contains only . and .. entries. */
static int fat32_dir_empty(fat32_fs_t *fs, uint32_t cluster) {
    uint8_t *buf = kzalloc(fs->bytes_per_cluster);
    if (!buf) return 0;
    while (cluster >= FAT32_CLUSTER_MIN && cluster <= FAT32_CLUSTER_MAX) {
        if (fat32_read_cluster(fs, cluster, buf) < 0) { kfree(buf); return 0; }
        fat32_dirent_t *entries = (fat32_dirent_t *)buf;
        int count = fs->bytes_per_cluster / sizeof(fat32_dirent_t);
        for (int i = 0; i < count; i++) {
            uint8_t c0 = entries[i].name[0];
            if (c0 == 0x00) { kfree(buf); return 1; }
            if (c0 == 0xE5) continue;
            if (entries[i].attr == FAT32_ATTR_LFN) continue;
            char nm[16];
            fat32_parse_short_name(entries[i].name, nm);
            if (nm[0] == '.' && (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
                continue;
            kfree(buf);
            return 0;  /* a real entry */
        }
        cluster = fat32_read_fat_entry(fs, cluster);
    }
    kfree(buf);
    return 1;
}

static int fat32_rmdir(struct vfs_inode *dir, const char *name) {
    fat32_inode_priv_t *dpriv = (fat32_inode_priv_t *)dir->private_data;
    fat32_fs_t *fs = dpriv->fs;

    fat32_dirent_t ent;
    uint32_t ec, eo;
    if (fat32_find_in_dir_loc(fs, dpriv->cluster, name, &ent, &ec, &eo) < 0)
        return -1;
    if (!(ent.attr & FAT32_ATTR_DIRECTORY))
        return -1;  /* use unlink */

    uint32_t first = ((uint32_t)ent.fst_clus_hi << 16) | ent.fst_clus_lo;
    if (first >= FAT32_CLUSTER_MIN && !fat32_dir_empty(fs, first))
        return -1;  /* not empty */

    if (first >= FAT32_CLUSTER_MIN)
        fat32_free_chain(fs, first);

    ent.name[0] = 0xE5;
    fat32_dirent_rw(fs, ec, eo, &ent, 1);
    block_flush(fs->dev);
    return 0;
}

static int fat32_readdir(struct vfs_inode *dir, int index, struct vfs_dir_entry *entry) {
    fat32_inode_priv_t *priv = (fat32_inode_priv_t *)dir->private_data;
    fat32_fs_t *fs = priv->fs;

    fat32_dirent_t ent;
    char name_buf[16];
    if (fat32_readdir_internal(fs, priv->cluster, index, &ent, name_buf) < 0)
        return 0;

    size_t i = 0;
    while (name_buf[i] && i < sizeof(entry->name) - 1) {
        entry->name[i] = name_buf[i];
        i++;
    }
    entry->name[i] = '\0';
    entry->inode_no = ((uint32_t)ent.fst_clus_hi << 16) | ent.fst_clus_lo;
    entry->mode = (ent.attr & FAT32_ATTR_DIRECTORY) ? S_IFDIR | 0755 : S_IFREG | 0644;
    return 1;
}

static struct vfs_inode_ops fat32_inode_ops = {
    .lookup = fat32_lookup,
    .create = fat32_create,
    .mkdir  = fat32_mkdir,
    .unlink = fat32_unlink,
    .rmdir  = fat32_rmdir,
    .readdir = fat32_readdir,
};

/* --------------------------------------------------------------------------
 * Directory iteration for VFS
 * -------------------------------------------------------------------------- */

typedef struct fat32_dir_priv {
    fat32_fs_t   *fs;
    uint32_t      cluster;
    uint32_t      index;
} fat32_dir_priv_t;

static int fat32_readdir_internal(fat32_fs_t *fs, uint32_t cluster,
                                   int index, fat32_dirent_t *out, char *name_out) {
    uint8_t *cluster_buf = kzalloc(fs->bytes_per_cluster);
    if (!cluster_buf) return -1;

    int current = 0;
    while (cluster >= FAT32_CLUSTER_MIN && cluster <= FAT32_CLUSTER_MAX) {
        if (fat32_read_cluster(fs, cluster, cluster_buf) < 0) {
            kfree(cluster_buf);
            return -1;
        }

        fat32_dirent_t *entries = (fat32_dirent_t *)cluster_buf;
        int count = fs->bytes_per_cluster / sizeof(fat32_dirent_t);

        for (int i = 0; i < count; i++) {
            fat32_dirent_t *ent = &entries[i];
            if (ent->name[0] == 0x00) {
                kfree(cluster_buf);
                return -1;
            }
            if (ent->name[0] == 0xE5) continue;
            if (ent->attr == FAT32_ATTR_LFN) continue;
            if (ent->attr & FAT32_ATTR_VOLUME_ID) continue;

            if (current == index) {
                *out = *ent;
                fat32_parse_short_name(ent->name, name_out);
                kfree(cluster_buf);
                return 0;
            }
            current++;
        }

        cluster = fat32_read_fat_entry(fs, cluster);
    }

    kfree(cluster_buf);
    return -1;
}

/* --------------------------------------------------------------------------
 * FAT32 mount / unmount
 * -------------------------------------------------------------------------- */

static struct vfs_superblock *fat32_mount(const char *dev_name) {
    device_t *dev = block_find(dev_name);
    if (!dev) {
        serial_printf(SERIAL_COM1, "[FAT32] Device not found: %s\n", dev_name);
        return NULL;
    }

    /* Read boot sector */
    uint8_t boot_sector[512];
    if (block_read(dev, 0, boot_sector, 1) < 0) {
        serial_printf(SERIAL_COM1, "[FAT32] Failed to read boot sector\n");
        return NULL;
    }

    fat32_bpb_t *bpb = (fat32_bpb_t *)boot_sector;

    /* Validate */
    if (bpb->bytes_per_sector != 512) {
        serial_printf(SERIAL_COM1, "[FAT32] Unsupported sector size: %d\n", bpb->bytes_per_sector);
        return NULL;
    }
    if (bpb->fat_size_32 == 0) {
        serial_printf(SERIAL_COM1, "[FAT32] Not a FAT32 volume\n");
        return NULL;
    }

    fat32_fs_t *fs = kzalloc(sizeof(fat32_fs_t));
    if (!fs) return NULL;

    fs->dev = dev;
    fs->bpb = *bpb;
    fs->fat_start = bpb->reserved_sectors;
    fs->data_start = bpb->reserved_sectors + (bpb->num_fats * bpb->fat_size_32);

    uint32_t total_sectors = bpb->total_sectors_32;
    if (total_sectors == 0)
        total_sectors = bpb->total_sectors_16;

    fs->total_clusters = (total_sectors - fs->data_start) / bpb->sectors_per_cluster;
    fs->bytes_per_cluster = bpb->bytes_per_sector * bpb->sectors_per_cluster;

    serial_printf(SERIAL_COM1,
        "[FAT32] Mounted %s: %d sectors/cluster, %d total clusters, root_cluster=%d, data_start=%d\n",
        dev_name, bpb->sectors_per_cluster, fs->total_clusters, bpb->root_cluster, fs->data_start);

    /* Debug: read root cluster */
    uint8_t *test_buf = kzalloc(fs->bytes_per_cluster);
    if (test_buf) {
        if (fat32_read_cluster(fs, bpb->root_cluster, test_buf) == 0) {
            serial_printf(SERIAL_COM1, "[FAT32] Root cluster first bytes: %02x %02x %02x %02x\n",
                test_buf[0], test_buf[1], test_buf[2], test_buf[3]);
        } else {
            serial_printf(SERIAL_COM1, "[FAT32] Failed to read root cluster\n");
        }
        kfree(test_buf);
    }

    /* Create root inode */
    fat32_dirent_t root_ent = {0};
    root_ent.attr = FAT32_ATTR_DIRECTORY;
    root_ent.fst_clus_hi = (uint16_t)(bpb->root_cluster >> 16);
    root_ent.fst_clus_lo = (uint16_t)(bpb->root_cluster & 0xFFFF);

    struct vfs_inode *root_inode = fat32_create_inode(fs, &root_ent);
    if (!root_inode) {
        kfree(fs);
        return NULL;
    }
    root_inode->i_ops = &fat32_inode_ops;

    struct vfs_superblock *sb = kzalloc(sizeof(struct vfs_superblock));
    if (!sb) {
        kfree(root_inode->private_data);
        kfree(root_inode);
        kfree(fs);
        return NULL;
    }

    sb->fs_type = NULL;  /* Set by caller */
    sb->root = vfs_dentry_create("/", root_inode, NULL);
    sb->private_data = fs;

    /* Link inode to superblock */
    root_inode->sb = sb;

    return sb;
}

static int fat32_unmount(struct vfs_superblock *sb) {
    if (!sb) return -1;
    /* TODO: free all resources */
    kfree(sb->private_data);
    kfree(sb);
    return 0;
}

static struct vfs_fs_type fat32_fs_type = {
    .name     = "fat32",
    .mount    = fat32_mount,
    .unmount  = fat32_unmount,
    .next     = NULL,
};

/* --------------------------------------------------------------------------
 * Registration
 * -------------------------------------------------------------------------- */
void fat32_init(void) {
    vfs_register_fs(&fat32_fs_type);
}
