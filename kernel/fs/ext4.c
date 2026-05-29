#include "ext4.h"
#include "vfs.h"
#include "block.h"
#include "serial.h"
#include "kapi.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Private filesystem state
 * -------------------------------------------------------------------------- */
typedef struct ext4_fs {
    device_t                  *dev;
    struct ext4_superblock     sb;
    struct ext4_group_desc    *group_desc;
    uint32_t                   block_size;
    uint32_t                   groups_count;
    uint32_t                   desc_size;
    uint32_t                   inodes_per_block;
} ext4_fs_t;

typedef struct ext4_inode_priv {
    ext4_fs_t              *fs;
    struct ext4_inode       raw;
    uint32_t                inode_no;
} ext4_inode_priv_t;

/* --------------------------------------------------------------------------
 * Low-level block I/O
 * -------------------------------------------------------------------------- */
static int ext4_read_block(ext4_fs_t *fs, uint64_t block, void *buf) {
    uint64_t lba = block * (fs->block_size / 512);
    size_t sectors = fs->block_size / 512;
    return block_read(fs->dev, lba, buf, sectors);
}

static int ext4_read_blocks(ext4_fs_t *fs, uint64_t block, void *buf, size_t count) {
    uint64_t lba = block * (fs->block_size / 512);
    size_t sectors = count * (fs->block_size / 512);
    return block_read(fs->dev, lba, buf, sectors);
}

/* --------------------------------------------------------------------------
 * Superblock and group descriptors
 * -------------------------------------------------------------------------- */
static int ext4_read_superblock(ext4_fs_t *fs) {
    uint8_t buf[1024];
    /* Superblock is at byte 1024 = sector 2 */
    if (block_read(fs->dev, 2, buf, 2) < 0)
        return -1;

    memcpy(&fs->sb, buf, sizeof(fs->sb));

    if (fs->sb.s_magic != EXT4_SUPER_MAGIC) {
        serial_printf(SERIAL_COM1, "[EXT4] Bad magic: 0x%x\n", fs->sb.s_magic);
        return -1;
    }

    fs->block_size = 1024U << fs->sb.s_log_block_size;

    /* Check for unsupported incompat features */
    uint32_t unsupported = fs->sb.s_feature_incompat & ~EXT4_SUPPORTED_INCOMPAT;
    if (unsupported) {
        serial_printf(SERIAL_COM1,
            "[EXT4] Unsupported incompat features: 0x%x\n", unsupported);
        return -1;
    }

    /* Determine group descriptor size */
    if (fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        fs->desc_size = fs->sb.s_desc_size ? fs->sb.s_desc_size : EXT4_MIN_DESC_SIZE_64BIT;
    else
        fs->desc_size = EXT4_MIN_DESC_SIZE;

    fs->inodes_per_block = fs->block_size / fs->sb.s_inode_size;

    /* Calculate number of block groups */
    uint64_t total_blocks = ((uint64_t)fs->sb.s_blocks_count_hi << 32) |
                            fs->sb.s_blocks_count_lo;
    fs->groups_count = (uint32_t)((total_blocks + fs->sb.s_blocks_per_group - 1) /
                                   fs->sb.s_blocks_per_group);

    serial_printf(SERIAL_COM1,
        "[EXT4] block_size=%d groups=%d desc_size=%d inodes_per_group=%d\n",
        fs->block_size, fs->groups_count, fs->desc_size, fs->sb.s_inodes_per_group);
    serial_printf(SERIAL_COM1,
        "[EXT4] incompat=0x%x ro_compat=0x%x\n",
        fs->sb.s_feature_incompat, fs->sb.s_feature_ro_compat);
    return 0;
}

static int ext4_load_group_desc(ext4_fs_t *fs) {
    if (fs->groups_count == 0) return -1;

    size_t desc_table_size = (size_t)fs->groups_count * fs->desc_size;
    size_t blocks_needed = (desc_table_size + fs->block_size - 1) / fs->block_size;

    fs->group_desc = kzalloc(blocks_needed * fs->block_size);
    if (!fs->group_desc) return -1;

    /* Group descriptors start at block 1 (or block 2 for 1KB blocks) */
    uint64_t gd_start = (fs->block_size == 1024) ? 2 : 1;

    for (size_t i = 0; i < blocks_needed; i++) {
        if (ext4_read_block(fs, gd_start + i,
                            (uint8_t *)fs->group_desc + i * fs->block_size) < 0) {
            kfree(fs->group_desc);
            fs->group_desc = NULL;
            return -1;
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Group descriptor accessors
 * -------------------------------------------------------------------------- */
static uint64_t ext4_bg_block_bitmap(ext4_fs_t *fs, uint32_t bg) {
    struct ext4_group_desc *gd = (struct ext4_group_desc *)((uint8_t *)fs->group_desc + bg * fs->desc_size);
    uint64_t block = gd->bg_block_bitmap_lo;
    if (fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        block |= ((uint64_t)gd->bg_block_bitmap_hi << 32);
    return block;
}

static uint64_t ext4_bg_inode_table(ext4_fs_t *fs, uint32_t bg) {
    struct ext4_group_desc *gd = (struct ext4_group_desc *)((uint8_t *)fs->group_desc + bg * fs->desc_size);
    uint64_t block = gd->bg_inode_table_lo;
    if (fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        block |= ((uint64_t)gd->bg_inode_table_hi << 32);
    return block;
}

/* --------------------------------------------------------------------------
 * Inode lookup
 * -------------------------------------------------------------------------- */
static int ext4_read_inode(ext4_fs_t *fs, uint32_t inode_no, struct ext4_inode *inode) {
    if (inode_no == 0) return -1;

    uint32_t group = (inode_no - 1) / fs->sb.s_inodes_per_group;
    uint32_t index = (inode_no - 1) % fs->sb.s_inodes_per_group;

    if (group >= fs->groups_count) return -1;

    uint64_t inode_table = ext4_bg_inode_table(fs, group);
    uint64_t block = inode_table + (index / fs->inodes_per_block);
    uint32_t offset = (index % fs->inodes_per_block) * fs->sb.s_inode_size;

    uint8_t *block_buf = kzalloc(fs->block_size);
    if (!block_buf) return -1;

    if (ext4_read_block(fs, block, block_buf) < 0) {
        kfree(block_buf);
        return -1;
    }

    memcpy(inode, block_buf + offset, sizeof(struct ext4_inode));
    kfree(block_buf);
    return 0;
}

/* --------------------------------------------------------------------------
 * Extent tree traversal
 * -------------------------------------------------------------------------- */
static uint64_t ext4_extent_start(struct ext4_extent *ext) {
    return ((uint64_t)ext->ee_start_hi << 32) | ext->ee_start_lo;
}

static uint64_t ext4_find_extent_block(ext4_fs_t *fs, struct ext4_inode *inode,
                                       uint32_t logical_block, uint8_t **block_buf_out) {
    struct ext4_extent_header *eh = (struct ext4_extent_header *)inode->i_block;

    if (eh->eh_magic != EXT4_EXTENT_MAGIC) {
        serial_printf(SERIAL_COM1, "[EXT4] Bad extent magic: 0x%x\n", eh->eh_magic);
        return 0;
    }

    uint8_t *block_buf = kzalloc(fs->block_size);
    if (!block_buf) return 0;

    while (eh->eh_depth > 0) {
        struct ext4_extent_idx *idx = (struct ext4_extent_idx *)(eh + 1);
        int i;
        for (i = 0; i < eh->eh_entries; i++) {
            if (logical_block < idx[i].ei_block)
                break;
        }
        if (i == 0) {
            kfree(block_buf);
            return 0;
        }
        i--;

        uint64_t leaf_block = ((uint64_t)idx[i].ei_leaf_hi << 32) | idx[i].ei_leaf_lo;
        if (ext4_read_block(fs, leaf_block, block_buf) < 0) {
            kfree(block_buf);
            return 0;
        }
        eh = (struct ext4_extent_header *)block_buf;
        if (eh->eh_magic != EXT4_EXTENT_MAGIC) {
            kfree(block_buf);
            return 0;
        }
    }

    struct ext4_extent *ext = (struct ext4_extent *)(eh + 1);
    for (int i = 0; i < eh->eh_entries; i++) {
        if (logical_block >= ext[i].ee_block &&
            logical_block < ext[i].ee_block + ext[i].ee_len) {
            uint64_t phys = ext4_extent_start(&ext[i]) + (logical_block - ext[i].ee_block);
            *block_buf_out = block_buf;
            return phys;
        }
    }

    kfree(block_buf);
    return 0;
}

/* --------------------------------------------------------------------------
 * File read
 * -------------------------------------------------------------------------- */
static ssize_t ext4_file_read(struct vfs_file *file, void *buf, size_t count) {
    if (!file || !file->inode) return -1;

    ext4_inode_priv_t *priv = (ext4_inode_priv_t *)file->inode->private_data;
    ext4_fs_t *fs = priv->fs;
    struct ext4_inode *inode = &priv->raw;

    uint64_t file_size = ((uint64_t)inode->i_size_hi << 32) | inode->i_size_lo;
    if ((uint64_t)file->pos >= file_size) return 0;
    if ((uint64_t)file->pos + count > file_size)
        count = (size_t)(file_size - (uint64_t)file->pos);

    uint8_t *dest = buf;
    size_t bytes_read = 0;
    uint64_t pos = (uint64_t)file->pos;

    while (bytes_read < count) {
        uint32_t logical_block = (uint32_t)(pos / fs->block_size);
        uint32_t block_offset = (uint32_t)(pos % fs->block_size);
        size_t to_copy = count - bytes_read;
        if (to_copy > fs->block_size - block_offset)
            to_copy = fs->block_size - block_offset;

        uint8_t *block_buf = NULL;
        uint64_t phys_block = ext4_find_extent_block(fs, inode, logical_block, &block_buf);
        if (phys_block == 0) {
            /* Sparse file or hole: return zeros */
            for (size_t i = 0; i < to_copy; i++)
                dest[bytes_read + i] = 0;
        } else {
            uint8_t *data = block_buf + (phys_block % (fs->block_size / 512) == 0 ? 0 : 0);
            /* Actually block_buf contains the extent block, we need to read the data block */
            /* Wait, ext4_find_extent_block returns the PHYSICAL block number, not a buffer offset */
            kfree(block_buf);
            block_buf = kzalloc(fs->block_size);
            if (!block_buf) break;
            if (ext4_read_block(fs, phys_block, block_buf) < 0) {
                kfree(block_buf);
                break;
            }
            memcpy(dest + bytes_read, block_buf + block_offset, to_copy);
        }
        if (block_buf) kfree(block_buf);

        bytes_read += to_copy;
        pos += to_copy;
    }

    file->pos = (off_t)pos;
    return (ssize_t)bytes_read;
}

static ssize_t ext4_file_write(struct vfs_file *file, const void *buf, size_t count) {
    (void)file; (void)buf; (void)count;
    return -1;  /* Read-only for now */
}

static int ext4_file_close(struct vfs_file *file) {
    (void)file;
    return 0;
}

static off_t ext4_file_lseek(struct vfs_file *file, off_t offset, int whence) {
    if (!file || !file->inode) return -1;
    ext4_inode_priv_t *priv = (ext4_inode_priv_t *)file->inode->private_data;
    uint64_t size = ((uint64_t)priv->raw.i_size_hi << 32) | priv->raw.i_size_lo;

    switch (whence) {
        case 0: file->pos = offset; break;
        case 1: file->pos += offset; break;
        case 2: file->pos = (off_t)size + offset; break;
        default: return -1;
    }
    if (file->pos < 0) file->pos = 0;
    return file->pos;
}

static struct vfs_file_ops ext4_file_ops = {
    .read  = ext4_file_read,
    .write = ext4_file_write,
    .close = ext4_file_close,
    .lseek = ext4_file_lseek,
};

/* --------------------------------------------------------------------------
 * Directory operations
 * -------------------------------------------------------------------------- */
static uint32_t ext4_file_type_to_mode(uint8_t ft) {
    switch (ft) {
        case EXT4_FT_REG_FILE: return S_IFREG | 0644;
        case EXT4_FT_DIR:      return S_IFDIR | 0755;
        case EXT4_FT_SYMLINK:  return S_IFLNK | 0777;
        case EXT4_FT_CHRDEV:   return S_IFCHR | 0644;
        case EXT4_FT_BLKDEV:   return S_IFBLK | 0644;
        case EXT4_FT_FIFO:     return S_IFIFO | 0644;
        case EXT4_FT_SOCK:     return S_IFSOCK | 0644;
        default:               return S_IFREG | 0644;
    }
}

static int ext4_readdir_vfs(struct vfs_inode *dir, int index, struct vfs_dir_entry *entry) {
    ext4_inode_priv_t *priv = (ext4_inode_priv_t *)dir->private_data;
    ext4_fs_t *fs = priv->fs;
    struct ext4_inode *inode = &priv->raw;

    uint64_t file_size = ((uint64_t)inode->i_size_hi << 32) | inode->i_size_lo;
    uint8_t *dir_buf = kzalloc(fs->block_size);
    if (!dir_buf) return 0;

    int current = 0;
    uint64_t offset = 0;

    while (offset < file_size) {
        uint32_t logical_block = (uint32_t)(offset / fs->block_size);
        uint32_t block_offset = (uint32_t)(offset % fs->block_size);

        uint8_t *block_buf = NULL;
        uint64_t phys_block = ext4_find_extent_block(fs, inode, logical_block, &block_buf);
        if (phys_block == 0) {
            offset += fs->block_size - block_offset;
            if (block_buf) kfree(block_buf);
            continue;
        }
        kfree(block_buf);

        if (ext4_read_block(fs, phys_block, dir_buf) < 0) {
            kfree(dir_buf);
            return 0;
        }

        uint32_t bo = block_offset;
        while (bo < fs->block_size) {
            struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(dir_buf + bo);
            if (de->inode == 0) {
                bo += de->rec_len;
                continue;
            }
            if (de->name_len == 1 && de->name[0] == '.') {
                bo += de->rec_len;
                continue;
            }
            if (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.') {
                bo += de->rec_len;
                continue;
            }

            if (current == index) {
                size_t len = de->name_len;
                if (len >= sizeof(entry->name)) len = sizeof(entry->name) - 1;
                memcpy(entry->name, de->name, len);
                entry->name[len] = '\0';
                entry->inode_no = de->inode;
                entry->mode = ext4_file_type_to_mode(de->file_type);
                kfree(dir_buf);
                return 1;
            }
            current++;
            bo += de->rec_len;
        }

        offset += fs->block_size - block_offset;
    }

    kfree(dir_buf);
    return 0;
}

static struct vfs_dentry *ext4_lookup(struct vfs_inode *dir, const char *name) {
    ext4_inode_priv_t *priv = (ext4_inode_priv_t *)dir->private_data;
    ext4_fs_t *fs = priv->fs;
    struct ext4_inode *inode = &priv->raw;

    uint64_t file_size = ((uint64_t)inode->i_size_hi << 32) | inode->i_size_lo;
    uint8_t *dir_buf = kzalloc(fs->block_size);
    if (!dir_buf) return NULL;

    uint64_t offset = 0;
    while (offset < file_size) {
        uint32_t logical_block = (uint32_t)(offset / fs->block_size);

        uint8_t *block_buf = NULL;
        uint64_t phys_block = ext4_find_extent_block(fs, inode, logical_block, &block_buf);
        if (phys_block == 0) {
            offset += fs->block_size;
            if (block_buf) kfree(block_buf);
            continue;
        }
        kfree(block_buf);

        if (ext4_read_block(fs, phys_block, dir_buf) < 0) {
            kfree(dir_buf);
            return NULL;
        }

        uint32_t bo = 0;
        while (bo < fs->block_size) {
            struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(dir_buf + bo);
            if (de->inode == 0) {
                bo += de->rec_len;
                continue;
            }

            size_t name_len = de->name_len;
            if (name_len > 255) name_len = 255;
            if (name_len == strlen(name) && memcmp(de->name, name, name_len) == 0) {
                /* Found: load inode and create dentry */
                struct ext4_inode child_inode;
                if (ext4_read_inode(fs, de->inode, &child_inode) < 0) {
                    kfree(dir_buf);
                    return NULL;
                }

                ext4_inode_priv_t *child_priv = kzalloc(sizeof(ext4_inode_priv_t));
                if (!child_priv) {
                    kfree(dir_buf);
                    return NULL;
                }
                child_priv->fs = fs;
                child_priv->raw = child_inode;
                child_priv->inode_no = de->inode;

                struct vfs_inode *vino = kzalloc(sizeof(struct vfs_inode));
                if (!vino) {
                    kfree(child_priv);
                    kfree(dir_buf);
                    return NULL;
                }

                uint64_t size = ((uint64_t)child_inode.i_size_hi << 32) | child_inode.i_size_lo;
                vino->inode_no = de->inode;
                vino->mode = ext4_file_type_to_mode(de->file_type);
                vino->size = (uint32_t)size;  /* truncate for 32-bit vfs */
                vino->nlink = child_inode.i_links_count;
                vino->sb = dir->sb;
                vino->i_ops = dir->i_ops;  /* same inode ops */
                vino->f_ops = (vino->mode & S_IFDIR) ? NULL : &ext4_file_ops;
                vino->private_data = child_priv;

                struct vfs_dentry *dentry = vfs_dentry_create(name, vino, dir->sb->root);
                kfree(dir_buf);
                return dentry;
            }
            bo += de->rec_len;
        }
        offset += fs->block_size;
    }

    kfree(dir_buf);
    return NULL;
}

static int ext4_create(struct vfs_inode *dir, const char *name, uint32_t mode) {
    (void)dir; (void)name; (void)mode;
    return -1;
}

static int ext4_mkdir(struct vfs_inode *dir, const char *name, uint32_t mode) {
    (void)dir; (void)name; (void)mode;
    return -1;
}

static int ext4_unlink(struct vfs_inode *dir, const char *name) {
    (void)dir; (void)name;
    return -1;
}

static int ext4_rmdir(struct vfs_inode *dir, const char *name) {
    (void)dir; (void)name;
    return -1;
}

static struct vfs_inode_ops ext4_inode_ops = {
    .lookup  = ext4_lookup,
    .create  = ext4_create,
    .mkdir   = ext4_mkdir,
    .unlink  = ext4_unlink,
    .rmdir   = ext4_rmdir,
    .readdir = ext4_readdir_vfs,
};

/* --------------------------------------------------------------------------
 * Mount / unmount
 * -------------------------------------------------------------------------- */
static struct vfs_superblock *ext4_mount(const char *dev_name) {
    device_t *dev = block_find(dev_name);
    if (!dev) {
        serial_printf(SERIAL_COM1, "[EXT4] Device not found: %s\n", dev_name);
        return NULL;
    }

    ext4_fs_t *fs = kzalloc(sizeof(ext4_fs_t));
    if (!fs) return NULL;
    fs->dev = dev;

    if (ext4_read_superblock(fs) < 0) {
        kfree(fs);
        return NULL;
    }

    if (ext4_load_group_desc(fs) < 0) {
        kfree(fs);
        return NULL;
    }

    /* Read root inode (inode 2) */
    struct ext4_inode root_raw;
    if (ext4_read_inode(fs, 2, &root_raw) < 0) {
        kfree(fs->group_desc);
        kfree(fs);
        return NULL;
    }

    if (!(root_raw.i_flags & EXT4_EXTENTS_FL)) {
        serial_printf(SERIAL_COM1, "[EXT4] Root inode does not use extents\n");
        kfree(fs->group_desc);
        kfree(fs);
        return NULL;
    }

    ext4_inode_priv_t *root_priv = kzalloc(sizeof(ext4_inode_priv_t));
    if (!root_priv) {
        kfree(fs->group_desc);
        kfree(fs);
        return NULL;
    }
    root_priv->fs = fs;
    root_priv->raw = root_raw;
    root_priv->inode_no = 2;

    struct vfs_inode *root_inode = kzalloc(sizeof(struct vfs_inode));
    if (!root_inode) {
        kfree(root_priv);
        kfree(fs->group_desc);
        kfree(fs);
        return NULL;
    }

    uint64_t root_size = ((uint64_t)root_raw.i_size_hi << 32) | root_raw.i_size_lo;
    root_inode->inode_no = 2;
    root_inode->mode = S_IFDIR | 0755;
    root_inode->size = (uint32_t)root_size;
    root_inode->nlink = root_raw.i_links_count;
    root_inode->i_ops = &ext4_inode_ops;
    root_inode->f_ops = NULL;
    root_inode->private_data = root_priv;

    struct vfs_superblock *sb = kzalloc(sizeof(struct vfs_superblock));
    if (!sb) {
        kfree(root_inode);
        kfree(root_priv);
        kfree(fs->group_desc);
        kfree(fs);
        return NULL;
    }

    sb->root = vfs_dentry_create("/", root_inode, NULL);
    sb->private_data = fs;
    root_inode->sb = sb;

    serial_printf(SERIAL_COM1, "[EXT4] Mounted %s successfully\n", dev_name);
    return sb;
}

static int ext4_unmount(struct vfs_superblock *sb) {
    if (!sb) return -1;
    ext4_fs_t *fs = (ext4_fs_t *)sb->private_data;
    if (fs) {
        if (fs->group_desc) kfree(fs->group_desc);
        kfree(fs);
    }
    kfree(sb);
    return 0;
}

static struct vfs_fs_type ext4_fs_type = {
    .name     = "ext4",
    .mount    = ext4_mount,
    .unmount  = ext4_unmount,
};

/* --------------------------------------------------------------------------
 * Init
 * -------------------------------------------------------------------------- */
void ext4_init(void) {
    vfs_register_fs(&ext4_fs_type);
}
