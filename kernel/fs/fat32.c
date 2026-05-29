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

static int fat32_find_in_dir(fat32_fs_t *fs, uint32_t cluster,
                              const char *name, fat32_dirent_t *out) {
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
                kfree(cluster_buf);
                return 0;
            }
        }

        cluster = fat32_read_fat_entry(fs, cluster);
    }

    kfree(cluster_buf);
    return -1;
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
    uint32_t       cluster;
    uint32_t       size;
    uint32_t       attr;
} fat32_inode_priv_t;

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

static ssize_t fat32_file_write(struct vfs_file *file, const void *buf, size_t count) {
    (void)file; (void)buf; (void)count;
    return -1;  /* Read-only for now */
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
    .read  = fat32_file_read,
    .write = fat32_file_write,
    .close = fat32_file_close,
    .lseek = fat32_file_lseek,
};

/* --------------------------------------------------------------------------
 * VFS inode (directory) operations
 * -------------------------------------------------------------------------- */

static struct vfs_inode_ops fat32_inode_ops;

static struct vfs_inode *fat32_create_inode(fat32_fs_t *fs,
                                              fat32_dirent_t *ent) {
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

static struct vfs_dentry *fat32_lookup(struct vfs_inode *dir, const char *name) {
    fat32_inode_priv_t *priv = (fat32_inode_priv_t *)dir->private_data;
    fat32_fs_t *fs = priv->fs;

    fat32_dirent_t ent;
    if (fat32_find_in_dir(fs, priv->cluster, name, &ent) < 0)
        return NULL;

    struct vfs_inode *inode = fat32_create_inode(fs, &ent);
    if (!inode) return NULL;

    struct vfs_dentry *dentry = vfs_dentry_create(name, inode, dir->sb ? dir->sb->root : NULL);
    return dentry;
}

static int fat32_create(struct vfs_inode *dir, const char *name, uint32_t mode) {
    (void)dir; (void)name; (void)mode;
    return -1;  /* Read-only for now */
}

static int fat32_mkdir(struct vfs_inode *dir, const char *name, uint32_t mode) {
    (void)dir; (void)name; (void)mode;
    return -1;  /* Read-only for now */
}

static int fat32_unlink(struct vfs_inode *dir, const char *name) {
    (void)dir; (void)name;
    return -1;  /* Read-only for now */
}

static int fat32_rmdir(struct vfs_inode *dir, const char *name) {
    (void)dir; (void)name;
    return -1;  /* Read-only for now */
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

    serial_printf(SERIAL_COM1, "[FAT32] readdir: cluster=%d, bytes_per_cluster=%d\n",
        cluster, fs->bytes_per_cluster);
    int current = 0;
    while (cluster >= FAT32_CLUSTER_MIN && cluster <= FAT32_CLUSTER_MAX) {
        uint32_t lba = fat32_cluster_to_lba(fs, cluster);
        serial_printf(SERIAL_COM1, "[FAT32] readdir: reading cluster %d at LBA %d\n", cluster, lba);
        if (fat32_read_cluster(fs, cluster, cluster_buf) < 0) {
            serial_printf(SERIAL_COM1, "[FAT32] readdir: failed to read cluster %d\n", cluster);
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
