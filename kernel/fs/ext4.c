#include "ext4.h"
#include "vfs.h"
#include "block.h"
#include "serial.h"
#include "kapi.h"
#include "timer.h"
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
 * Write support: metadata writeback + block/inode allocation
 *
 * Scope: the on-disk images Lariat uses are created with
 *   mkfs.ext4 -O ^64bit,^metadata_csum,^huge_file
 * i.e. 32-byte group descriptors, no 64-bit fields, and no metadata/bitmap/
 * inode checksums - so writeback never has to compute CRCs.  There is no
 * journal (RECOVER is rejected at mount), so updates are written straight
 * through; the write-through block cache keeps things coherent.  Files use
 * inline (depth-0) extents with up to 4 records, which is ample for the
 * configuration files and packages we store; growing into a full extent tree
 * is intentionally not supported and returns an error.
 * -------------------------------------------------------------------------- */
static int ext4_write_block(ext4_fs_t *fs, uint64_t block, const void *buf) {
    uint64_t lba = block * (fs->block_size / 512);
    size_t sectors = fs->block_size / 512;
    return block_write(fs->dev, lba, buf, sectors);
}

static struct ext4_group_desc *ext4_gd(ext4_fs_t *fs, uint32_t bg) {
    return (struct ext4_group_desc *)((uint8_t *)fs->group_desc + bg * fs->desc_size);
}

static uint64_t ext4_bg_inode_bitmap(ext4_fs_t *fs, uint32_t bg) {
    struct ext4_group_desc *gd = ext4_gd(fs, bg);
    uint64_t block = gd->bg_inode_bitmap_lo;
    if (fs->sb.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        block |= ((uint64_t)gd->bg_inode_bitmap_hi << 32);
    return block;
}

/* Persist the superblock (1024 bytes at byte offset 1024) and the in-RAM group
 * descriptor table back to disk. */
static int ext4_sync_meta(ext4_fs_t *fs) {
    if (block_write(fs->dev, 2, &fs->sb, 2) < 0)
        return -1;
    size_t desc_table_size = (size_t)fs->groups_count * fs->desc_size;
    size_t blocks_needed = (desc_table_size + fs->block_size - 1) / fs->block_size;
    uint64_t gd_start = (fs->block_size == 1024) ? 2 : 1;
    for (size_t i = 0; i < blocks_needed; i++)
        if (ext4_write_block(fs, gd_start + i,
                             (uint8_t *)fs->group_desc + i * fs->block_size) < 0)
            return -1;
    return 0;
}

/* Write one inode back into its slot in the inode table (read-modify-write the
 * containing block so the inode's extra/unmanaged tail bytes are preserved). */
static int ext4_write_inode(ext4_fs_t *fs, uint32_t inode_no,
                            const struct ext4_inode *inode) {
    if (inode_no == 0) return -1;
    uint32_t group = (inode_no - 1) / fs->sb.s_inodes_per_group;
    uint32_t index = (inode_no - 1) % fs->sb.s_inodes_per_group;
    if (group >= fs->groups_count) return -1;

    uint64_t block = ext4_bg_inode_table(fs, group) + (index / fs->inodes_per_block);
    uint32_t offset = (index % fs->inodes_per_block) * fs->sb.s_inode_size;

    uint8_t *bb = kzalloc(fs->block_size);
    if (!bb) return -1;
    if (ext4_read_block(fs, block, bb) < 0) { kfree(bb); return -1; }
    memcpy(bb + offset, inode, sizeof(struct ext4_inode));
    int rc = ext4_write_block(fs, block, bb);
    kfree(bb);
    return rc;
}

/* Allocate one data block from the first group with a free bit.  Returns the
 * absolute block number (and zero-fills it) or 0 on failure. */
static uint64_t ext4_alloc_block(ext4_fs_t *fs) {
    uint8_t *bm = kzalloc(fs->block_size);
    if (!bm) return 0;
    for (uint32_t bg = 0; bg < fs->groups_count; bg++) {
        struct ext4_group_desc *gd = ext4_gd(fs, bg);
        if (gd->bg_free_blocks_count_lo == 0) continue;
        uint64_t bm_block = ext4_bg_block_bitmap(fs, bg);
        if (ext4_read_block(fs, bm_block, bm) < 0) break;
        for (uint32_t bit = 0; bit < fs->sb.s_blocks_per_group; bit++) {
            if (!(bm[bit / 8] & (1u << (bit % 8)))) {
                bm[bit / 8] |= (1u << (bit % 8));
                if (ext4_write_block(fs, bm_block, bm) < 0) { kfree(bm); return 0; }
                gd->bg_free_blocks_count_lo--;
                if (fs->sb.s_free_blocks_count_lo) fs->sb.s_free_blocks_count_lo--;
                ext4_sync_meta(fs);
                uint64_t abs = (uint64_t)bg * fs->sb.s_blocks_per_group +
                               fs->sb.s_first_data_block + bit;
                /* Zero the freshly allocated block. */
                memset(bm, 0, fs->block_size);
                ext4_write_block(fs, abs, bm);
                kfree(bm);
                return abs;
            }
        }
    }
    kfree(bm);
    return 0;
}

static void ext4_free_block(ext4_fs_t *fs, uint64_t abs) {
    if (abs < fs->sb.s_first_data_block) return;
    uint64_t rel = abs - fs->sb.s_first_data_block;
    uint32_t bg = (uint32_t)(rel / fs->sb.s_blocks_per_group);
    uint32_t bit = (uint32_t)(rel % fs->sb.s_blocks_per_group);
    if (bg >= fs->groups_count) return;
    uint8_t *bm = kzalloc(fs->block_size);
    if (!bm) return;
    uint64_t bm_block = ext4_bg_block_bitmap(fs, bg);
    if (ext4_read_block(fs, bm_block, bm) == 0) {
        if (bm[bit / 8] & (1u << (bit % 8))) {
            bm[bit / 8] &= ~(1u << (bit % 8));
            ext4_write_block(fs, bm_block, bm);
            ext4_gd(fs, bg)->bg_free_blocks_count_lo++;
            fs->sb.s_free_blocks_count_lo++;
            ext4_sync_meta(fs);
        }
    }
    kfree(bm);
}

/* Allocate an inode number; marks it used in the bitmap and updates counts.
 * `is_dir` bumps the group's used-dirs count.  Returns 0 on failure. */
static uint32_t ext4_alloc_inode(ext4_fs_t *fs, int is_dir) {
    uint8_t *bm = kzalloc(fs->block_size);
    if (!bm) return 0;
    for (uint32_t bg = 0; bg < fs->groups_count; bg++) {
        struct ext4_group_desc *gd = ext4_gd(fs, bg);
        if (gd->bg_free_inodes_count_lo == 0) continue;
        uint64_t bm_block = ext4_bg_inode_bitmap(fs, bg);
        if (ext4_read_block(fs, bm_block, bm) < 0) break;
        for (uint32_t bit = 0; bit < fs->sb.s_inodes_per_group; bit++) {
            if (!(bm[bit / 8] & (1u << (bit % 8)))) {
                bm[bit / 8] |= (1u << (bit % 8));
                if (ext4_write_block(fs, bm_block, bm) < 0) { kfree(bm); return 0; }
                gd->bg_free_inodes_count_lo--;
                if (fs->sb.s_free_inodes_count) fs->sb.s_free_inodes_count--;
                if (is_dir) gd->bg_used_dirs_count_lo++;
                ext4_sync_meta(fs);
                kfree(bm);
                return bg * fs->sb.s_inodes_per_group + bit + 1;
            }
        }
    }
    kfree(bm);
    return 0;
}

static void ext4_free_inode(ext4_fs_t *fs, uint32_t inode_no, int is_dir) {
    if (inode_no == 0) return;
    uint32_t bg = (inode_no - 1) / fs->sb.s_inodes_per_group;
    uint32_t bit = (inode_no - 1) % fs->sb.s_inodes_per_group;
    if (bg >= fs->groups_count) return;
    uint8_t *bm = kzalloc(fs->block_size);
    if (!bm) return;
    uint64_t bm_block = ext4_bg_inode_bitmap(fs, bg);
    if (ext4_read_block(fs, bm_block, bm) == 0) {
        bm[bit / 8] &= ~(1u << (bit % 8));
        ext4_write_block(fs, bm_block, bm);
        struct ext4_group_desc *gd = ext4_gd(fs, bg);
        gd->bg_free_inodes_count_lo++;
        if (is_dir && gd->bg_used_dirs_count_lo) gd->bg_used_dirs_count_lo--;
        fs->sb.s_free_inodes_count++;
        ext4_sync_meta(fs);
    }
    kfree(bm);
}

/* Append one freshly-allocated physical block as logical block `lblk` to the
 * inode's inline (depth-0) extent list, coalescing with the last extent when
 * contiguous.  Returns 0 on success, -1 if the extent list is full (would need
 * a real tree, unsupported) or the inode is not extent-mapped. */
static int ext4_extent_append(struct ext4_inode *inode, uint32_t lblk, uint64_t phys) {
    struct ext4_extent_header *eh = (struct ext4_extent_header *)inode->i_block;
    if (eh->eh_magic != EXT4_EXTENT_MAGIC || eh->eh_depth != 0)
        return -1;
    struct ext4_extent *ext = (struct ext4_extent *)(eh + 1);
    if (eh->eh_entries > 0) {
        struct ext4_extent *last = &ext[eh->eh_entries - 1];
        uint64_t lstart = ((uint64_t)last->ee_start_hi << 32) | last->ee_start_lo;
        if (last->ee_block + last->ee_len == lblk &&
            lstart + last->ee_len == phys &&
            last->ee_len < 32768) {
            last->ee_len++;
            return 0;
        }
    }
    if (eh->eh_entries >= eh->eh_max)
        return -1;   /* inline list full; extent tree not supported */
    struct ext4_extent *ne = &ext[eh->eh_entries];
    ne->ee_block = lblk;
    ne->ee_len = 1;
    ne->ee_start_hi = (uint16_t)(phys >> 32);
    ne->ee_start_lo = (uint32_t)phys;
    eh->eh_entries++;
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

        uint8_t *xbuf = NULL;
        uint64_t phys_block = ext4_find_extent_block(fs, inode, logical_block, &xbuf);
        if (xbuf) kfree(xbuf);   /* only the physical block number is needed */
        if (phys_block == 0) {
            /* Sparse file or hole: return zeros */
            for (size_t i = 0; i < to_copy; i++)
                dest[bytes_read + i] = 0;
        } else {
            uint8_t *block_buf = kzalloc(fs->block_size);
            if (!block_buf) break;
            if (ext4_read_block(fs, phys_block, block_buf) < 0) {
                kfree(block_buf);
                break;
            }
            memcpy(dest + bytes_read, block_buf + block_offset, to_copy);
            kfree(block_buf);
        }

        bytes_read += to_copy;
        pos += to_copy;
    }

    file->pos = (off_t)pos;
    return (ssize_t)bytes_read;
}

static ssize_t ext4_file_write(struct vfs_file *file, const void *buf, size_t count) {
    if (!file || !file->inode) return -1;
    ext4_inode_priv_t *priv = (ext4_inode_priv_t *)file->inode->private_data;
    ext4_fs_t *fs = priv->fs;
    struct ext4_inode *inode = &priv->raw;
    const uint8_t *src = (const uint8_t *)buf;

    uint64_t size = ((uint64_t)inode->i_size_hi << 32) | inode->i_size_lo;
    uint64_t pos = (uint64_t)file->pos;
    size_t written = 0;
    uint8_t *block_buf = kzalloc(fs->block_size);
    if (!block_buf) return -1;

    while (written < count) {
        uint32_t lblk = (uint32_t)(pos / fs->block_size);
        uint32_t boff = (uint32_t)(pos % fs->block_size);
        size_t chunk = count - written;
        if (chunk > fs->block_size - boff)
            chunk = fs->block_size - boff;

        uint8_t *xbuf = NULL;
        uint64_t phys = ext4_find_extent_block(fs, inode, lblk, &xbuf);
        if (xbuf) kfree(xbuf);

        if (phys == 0) {
            /* Need to back this logical block with a fresh physical block. */
            uint64_t nb = ext4_alloc_block(fs);
            if (nb == 0) break;
            if (ext4_extent_append(inode, lblk, nb) < 0) {
                ext4_free_block(fs, nb);
                break;   /* extent list full (file too fragmented/large) */
            }
            inode->i_blocks_lo += fs->block_size / 512;
            phys = nb;
            memset(block_buf, 0, fs->block_size);
        } else if (boff != 0 || chunk != fs->block_size) {
            /* Partial overwrite: preserve the rest of the existing block. */
            if (ext4_read_block(fs, phys, block_buf) < 0) break;
        } else {
            memset(block_buf, 0, fs->block_size);
        }

        memcpy(block_buf + boff, src + written, chunk);
        if (ext4_write_block(fs, phys, block_buf) < 0) break;

        written += chunk;
        pos += chunk;
        if (pos > size) size = pos;
    }
    kfree(block_buf);

    if (written == 0) return -1;

    inode->i_size_lo = (uint32_t)size;
    inode->i_size_hi = (uint32_t)(size >> 32);
    ext4_write_inode(fs, priv->inode_no, inode);
    file->inode->size = (uint32_t)size;
    file->pos = (off_t)pos;
    return (ssize_t)written;
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

static int ext4_file_truncate(struct vfs_file *file, off_t length);

static struct vfs_file_ops ext4_file_ops = {
    .read     = ext4_file_read,
    .write    = ext4_file_write,
    .close    = ext4_file_close,
    .lseek    = ext4_file_lseek,
    .truncate = ext4_file_truncate,
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
                /* Use the on-disk inode's real mode/owner (type + permission
                 * bits + uid/gid), not just the coarse directory-entry type, so
                 * chmod/chown and creator ownership round-trip through disk. */
                vino->mode = child_inode.i_mode ? child_inode.i_mode
                                                : ext4_file_type_to_mode(de->file_type);
                vino->uid = (uint32_t)child_inode.i_uid | ((uint32_t)child_inode.i_uid_hi << 16);
                vino->gid = (uint32_t)child_inode.i_gid | ((uint32_t)child_inode.i_gid_hi << 16);
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

/* Round a directory-entry length up to the 4-byte alignment ext4 requires. */
static inline uint32_t ext4_de_need(uint32_t name_len) {
    return (8 + name_len + 3) & ~3u;
}

/* Initialise an extent-mapped, zero-length inode in `out` (caller fills mode /
 * link count). */
static void ext4_init_extent_inode(struct ext4_inode *out, uint16_t mode) {
    memset(out, 0, sizeof(*out));
    out->i_mode = mode;
    out->i_flags = EXT4_EXTENTS_FL;
    struct ext4_extent_header *eh = (struct ext4_extent_header *)out->i_block;
    eh->eh_magic = EXT4_EXTENT_MAGIC;
    eh->eh_entries = 0;
    eh->eh_max = (60 - sizeof(struct ext4_extent_header)) / sizeof(struct ext4_extent);
    eh->eh_depth = 0;
}

/* Scan directory `dpriv` for an entry named `name`; returns its inode number
 * or 0 if absent.  Used to keep create/mkdir idempotent (reject duplicates). */
static uint32_t ext4_dir_find(ext4_fs_t *fs, ext4_inode_priv_t *dpriv,
                              const char *name) {
    struct ext4_inode *dir = &dpriv->raw;
    uint32_t nlen = (uint32_t)strlen(name);
    uint64_t dsize = ((uint64_t)dir->i_size_hi << 32) | dir->i_size_lo;
    uint8_t *blk = kzalloc(fs->block_size);
    if (!blk) return 0;
    uint32_t found = 0;
    for (uint64_t off = 0; off < dsize && !found; off += fs->block_size) {
        uint32_t lblk = (uint32_t)(off / fs->block_size);
        uint8_t *xb = NULL;
        uint64_t phys = ext4_find_extent_block(fs, dir, lblk, &xb);
        if (xb) kfree(xb);
        if (phys == 0 || ext4_read_block(fs, phys, blk) < 0) continue;
        uint32_t bo = 0;
        while (bo < fs->block_size) {
            struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(blk + bo);
            if (de->rec_len < 8 || bo + de->rec_len > fs->block_size) break;
            if (de->inode != 0 && de->name_len == nlen &&
                memcmp(de->name, name, nlen) == 0) { found = de->inode; break; }
            bo += de->rec_len;
        }
    }
    kfree(blk);
    return found;
}

/* Insert a directory entry (name -> child_ino) into directory `dpriv`.  Splits
 * slack out of an existing entry, reuses an empty slot, or grows the directory
 * by one block.  Returns 0 on success. */
static int ext4_dir_insert(ext4_fs_t *fs, ext4_inode_priv_t *dpriv,
                           const char *name, uint32_t child_ino, uint8_t ftype) {
    struct ext4_inode *dir = &dpriv->raw;
    uint32_t nlen = (uint32_t)strlen(name);
    uint32_t need = ext4_de_need(nlen);
    uint64_t dsize = ((uint64_t)dir->i_size_hi << 32) | dir->i_size_lo;

    uint8_t *blk = kzalloc(fs->block_size);
    if (!blk) return -1;

    for (uint64_t off = 0; off < dsize; off += fs->block_size) {
        uint32_t lblk = (uint32_t)(off / fs->block_size);
        uint8_t *xb = NULL;
        uint64_t phys = ext4_find_extent_block(fs, dir, lblk, &xb);
        if (xb) kfree(xb);
        if (phys == 0) continue;
        if (ext4_read_block(fs, phys, blk) < 0) continue;

        uint32_t bo = 0;
        while (bo < fs->block_size) {
            struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(blk + bo);
            if (de->rec_len < 8 || bo + de->rec_len > fs->block_size) break;

            if (de->inode == 0 && de->rec_len >= need) {
                de->inode = child_ino;
                de->name_len = (uint8_t)nlen;
                de->file_type = ftype;
                memcpy(de->name, name, nlen);
                if (ext4_write_block(fs, phys, blk) < 0) { kfree(blk); return -1; }
                kfree(blk);
                return 0;
            }
            if (de->inode != 0) {
                uint32_t used = ext4_de_need(de->name_len);
                if (de->rec_len - used >= need) {
                    uint16_t old = de->rec_len;
                    de->rec_len = (uint16_t)used;
                    struct ext4_dir_entry_2 *nd =
                        (struct ext4_dir_entry_2 *)(blk + bo + used);
                    nd->inode = child_ino;
                    nd->rec_len = (uint16_t)(old - used);
                    nd->name_len = (uint8_t)nlen;
                    nd->file_type = ftype;
                    memcpy(nd->name, name, nlen);
                    if (ext4_write_block(fs, phys, blk) < 0) { kfree(blk); return -1; }
                    kfree(blk);
                    return 0;
                }
            }
            bo += de->rec_len;
        }
    }

    /* No room in existing blocks: append a fresh directory block. */
    uint64_t nb = ext4_alloc_block(fs);
    if (nb == 0) { kfree(blk); return -1; }
    if (ext4_extent_append(dir, (uint32_t)(dsize / fs->block_size), nb) < 0) {
        ext4_free_block(fs, nb);
        kfree(blk);
        return -1;
    }
    dir->i_blocks_lo += fs->block_size / 512;
    dsize += fs->block_size;
    dir->i_size_lo = (uint32_t)dsize;
    dir->i_size_hi = (uint32_t)(dsize >> 32);

    memset(blk, 0, fs->block_size);
    struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)blk;
    de->inode = child_ino;
    de->rec_len = (uint16_t)fs->block_size;
    de->name_len = (uint8_t)nlen;
    de->file_type = ftype;
    memcpy(de->name, name, nlen);
    int rc = ext4_write_block(fs, nb, blk);
    kfree(blk);
    if (rc < 0) return -1;
    return ext4_write_inode(fs, dpriv->inode_no, dir);
}

/* Remove the entry `name` from directory `dpriv`; returns the removed entry's
 * inode number in *out_ino (0 if not found). */
static int ext4_dir_remove(ext4_fs_t *fs, ext4_inode_priv_t *dpriv,
                           const char *name, uint32_t *out_ino) {
    struct ext4_inode *dir = &dpriv->raw;
    uint32_t nlen = (uint32_t)strlen(name);
    uint64_t dsize = ((uint64_t)dir->i_size_hi << 32) | dir->i_size_lo;
    *out_ino = 0;

    uint8_t *blk = kzalloc(fs->block_size);
    if (!blk) return -1;

    for (uint64_t off = 0; off < dsize; off += fs->block_size) {
        uint32_t lblk = (uint32_t)(off / fs->block_size);
        uint8_t *xb = NULL;
        uint64_t phys = ext4_find_extent_block(fs, dir, lblk, &xb);
        if (xb) kfree(xb);
        if (phys == 0) continue;
        if (ext4_read_block(fs, phys, blk) < 0) continue;

        uint32_t bo = 0, prev = 0xFFFFFFFF;
        while (bo < fs->block_size) {
            struct ext4_dir_entry_2 *de = (struct ext4_dir_entry_2 *)(blk + bo);
            if (de->rec_len < 8 || bo + de->rec_len > fs->block_size) break;
            if (de->inode != 0 && de->name_len == nlen &&
                memcmp(de->name, name, nlen) == 0) {
                *out_ino = de->inode;
                if (prev != 0xFFFFFFFF) {
                    struct ext4_dir_entry_2 *pd =
                        (struct ext4_dir_entry_2 *)(blk + prev);
                    pd->rec_len = (uint16_t)(pd->rec_len + de->rec_len);
                } else {
                    de->inode = 0;   /* first entry in block: just clear it */
                }
                if (ext4_write_block(fs, phys, blk) < 0) { kfree(blk); return -1; }
                kfree(blk);
                return 0;
            }
            prev = bo;
            bo += de->rec_len;
        }
    }
    kfree(blk);
    return 0;   /* not found (out_ino stays 0) */
}

/* Free every data block of an extent-mapped inode at or beyond logical block
 * `from_lblk`, trimming/removing the affected (depth-0) extents. */
static void ext4_free_blocks_from(ext4_fs_t *fs, struct ext4_inode *inode,
                                  uint32_t from_lblk) {
    struct ext4_extent_header *eh = (struct ext4_extent_header *)inode->i_block;
    if (eh->eh_magic != EXT4_EXTENT_MAGIC || eh->eh_depth != 0) return;
    struct ext4_extent *ext = (struct ext4_extent *)(eh + 1);

    int n = eh->eh_entries;
    for (int i = n - 1; i >= 0; i--) {
        uint32_t e_start = ext[i].ee_block;
        uint32_t e_end = e_start + ext[i].ee_len;      /* exclusive */
        uint64_t phys = ((uint64_t)ext[i].ee_start_hi << 32) | ext[i].ee_start_lo;
        if (e_end <= from_lblk) continue;              /* fully kept */
        uint32_t keep = (from_lblk > e_start) ? (from_lblk - e_start) : 0;
        for (uint32_t b = keep; b < ext[i].ee_len; b++) {
            ext4_free_block(fs, phys + b);
            inode->i_blocks_lo -= fs->block_size / 512;
        }
        if (keep == 0) {
            eh->eh_entries--;          /* drop the whole (now-last) extent */
        } else {
            ext[i].ee_len = (uint16_t)keep;
        }
    }
}

/* Turn an inode (whose data blocks have already been released) into a freed
 * inode on disk: zero link count, clear the size/extent map and stamp i_dtime
 * so fsck treats the slot as deleted rather than a live orphan. */
static void ext4_mark_inode_deleted(struct ext4_inode *inode) {
    inode->i_links_count = 0;
    /* Stamp a real (large) deletion time. fsck overloads i_dtime as an
     * orphan-chain "next inode" pointer, so a small value would look like a
     * corrupted orphan list; a wall-clock timestamp is far above any inode #. */
    inode->i_dtime = (uint32_t)(clock_realtime_ns() / 1000000000ULL);
    if (inode->i_dtime == 0) inode->i_dtime = 0x4D000000u;  /* fallback if clock unset */
    inode->i_size_lo = 0;
    inode->i_size_hi = 0;
    inode->i_blocks_lo = 0;
    /* Reset the inline extent header so no data blocks appear referenced. */
    struct ext4_extent_header *eh = (struct ext4_extent_header *)inode->i_block;
    if (eh->eh_magic == EXT4_EXTENT_MAGIC) {
        eh->eh_entries = 0;
    } else {
        memset(inode->i_block, 0, sizeof(inode->i_block));
    }
}

static int ext4_file_truncate(struct vfs_file *file, off_t length) {
    if (!file || !file->inode || length < 0) return -1;
    ext4_inode_priv_t *priv = (ext4_inode_priv_t *)file->inode->private_data;
    ext4_fs_t *fs = priv->fs;
    struct ext4_inode *inode = &priv->raw;

    uint32_t newblocks = (uint32_t)(((uint64_t)length + fs->block_size - 1) / fs->block_size);
    ext4_free_blocks_from(fs, inode, newblocks);
    inode->i_size_lo = (uint32_t)length;
    inode->i_size_hi = (uint32_t)((uint64_t)length >> 32);
    file->inode->size = (uint32_t)length;
    if (file->pos > length) file->pos = length;
    return ext4_write_inode(fs, priv->inode_no, inode);
}

static int ext4_create(struct vfs_inode *dir, const char *name, uint32_t mode) {
    ext4_inode_priv_t *dpriv = (ext4_inode_priv_t *)dir->private_data;
    ext4_fs_t *fs = dpriv->fs;

    if (ext4_dir_find(fs, dpriv, name) != 0) return -1;   /* already exists */

    uint32_t ino = ext4_alloc_inode(fs, 0);
    if (ino == 0) return -1;

    struct ext4_inode ni;
    ext4_init_extent_inode(&ni, (uint16_t)(S_IFREG | (mode & 0777)));
    ni.i_links_count = 1;
    if (ext4_write_inode(fs, ino, &ni) < 0) { ext4_free_inode(fs, ino, 0); return -1; }

    if (ext4_dir_insert(fs, dpriv, name, ino, EXT4_FT_REG_FILE) < 0) {
        ext4_free_inode(fs, ino, 0);
        return -1;
    }
    return 0;
}

static int ext4_mkdir(struct vfs_inode *dir, const char *name, uint32_t mode) {
    ext4_inode_priv_t *dpriv = (ext4_inode_priv_t *)dir->private_data;
    ext4_fs_t *fs = dpriv->fs;

    if (ext4_dir_find(fs, dpriv, name) != 0) return -1;   /* already exists */

    uint32_t ino = ext4_alloc_inode(fs, 1);
    if (ino == 0) return -1;
    uint64_t dblk = ext4_alloc_block(fs);
    if (dblk == 0) { ext4_free_inode(fs, ino, 1); return -1; }

    struct ext4_inode ni;
    ext4_init_extent_inode(&ni, (uint16_t)(S_IFDIR | (mode & 0777)));
    ni.i_links_count = 2;     /* self + "." */
    ext4_extent_append(&ni, 0, dblk);
    ni.i_blocks_lo = fs->block_size / 512;
    ni.i_size_lo = fs->block_size;

    /* Lay out "." and ".." filling the directory's first block. */
    uint8_t *blk = kzalloc(fs->block_size);
    if (!blk) { ext4_free_block(fs, dblk); ext4_free_inode(fs, ino, 1); return -1; }
    struct ext4_dir_entry_2 *d1 = (struct ext4_dir_entry_2 *)blk;
    d1->inode = ino; d1->rec_len = 12; d1->name_len = 1; d1->file_type = EXT4_FT_DIR;
    d1->name[0] = '.';
    struct ext4_dir_entry_2 *d2 = (struct ext4_dir_entry_2 *)(blk + 12);
    d2->inode = dpriv->inode_no;
    d2->rec_len = (uint16_t)(fs->block_size - 12);
    d2->name_len = 2; d2->file_type = EXT4_FT_DIR;
    d2->name[0] = '.'; d2->name[1] = '.';
    int rc = ext4_write_block(fs, dblk, blk);
    kfree(blk);
    if (rc < 0) { ext4_free_block(fs, dblk); ext4_free_inode(fs, ino, 1); return -1; }

    if (ext4_write_inode(fs, ino, &ni) < 0 ||
        ext4_dir_insert(fs, dpriv, name, ino, EXT4_FT_DIR) < 0) {
        ext4_free_block(fs, dblk);
        ext4_free_inode(fs, ino, 1);
        return -1;
    }

    /* The new ".." adds a link to the parent. */
    dpriv->raw.i_links_count++;
    ext4_write_inode(fs, dpriv->inode_no, &dpriv->raw);
    dir->nlink = dpriv->raw.i_links_count;
    return 0;
}

static int ext4_unlink(struct vfs_inode *dir, const char *name) {
    ext4_inode_priv_t *dpriv = (ext4_inode_priv_t *)dir->private_data;
    ext4_fs_t *fs = dpriv->fs;

    uint32_t ino = 0;
    if (ext4_dir_remove(fs, dpriv, name, &ino) < 0) return -1;
    if (ino == 0) return -1;

    struct ext4_inode ci;
    if (ext4_read_inode(fs, ino, &ci) == 0) {
        if (ci.i_links_count > 0) ci.i_links_count--;
        if (ci.i_links_count == 0) {
            ext4_free_blocks_from(fs, &ci, 0);   /* release all data blocks */
            ext4_mark_inode_deleted(&ci);        /* persist as a freed inode */
            ext4_write_inode(fs, ino, &ci);
            ext4_free_inode(fs, ino, 0);
        } else {
            ext4_write_inode(fs, ino, &ci);
        }
    }
    return 0;
}

static int ext4_rmdir(struct vfs_inode *dir, const char *name) {
    ext4_inode_priv_t *dpriv = (ext4_inode_priv_t *)dir->private_data;
    ext4_fs_t *fs = dpriv->fs;

    /* Resolve the target directory and ensure it is empty (only "." / ".."). */
    struct vfs_dentry *d = ext4_lookup(dir, name);
    if (!d || !d->inode) return -1;
    ext4_inode_priv_t *tpriv = (ext4_inode_priv_t *)d->inode->private_data;
    uint32_t tino = tpriv->inode_no;

    struct vfs_dir_entry tmp;
    int empty = (ext4_readdir_vfs(d->inode, 0, &tmp) == 0);  /* readdir skips . / .. */

    if (!empty) return -1;

    uint32_t removed = 0;
    if (ext4_dir_remove(fs, dpriv, name, &removed) < 0 || removed == 0)
        return -1;

    struct ext4_inode ti;
    if (ext4_read_inode(fs, tino, &ti) == 0) {
        ext4_free_blocks_from(fs, &ti, 0);     /* release "." / ".." block(s) */
        ext4_mark_inode_deleted(&ti);          /* persist as a freed inode */
        ext4_write_inode(fs, tino, &ti);
    }
    ext4_free_inode(fs, tino, 1);

    /* Removing the child's ".." drops a link from the parent. */
    if (dpriv->raw.i_links_count > 0) dpriv->raw.i_links_count--;
    ext4_write_inode(fs, dpriv->inode_no, &dpriv->raw);
    dir->nlink = dpriv->raw.i_links_count;
    return 0;
}

/* Persist mode/uid/gid changes (chmod/chown/creator ownership) to the on-disk
 * inode so they survive the next lookup (which re-reads from disk) and remount. */
static int ext4_setattr(struct vfs_inode *inode) {
    if (!inode || !inode->private_data) return -1;
    ext4_inode_priv_t *priv = (ext4_inode_priv_t *)inode->private_data;
    priv->raw.i_mode   = (uint16_t)(inode->mode & 0xFFFF);
    priv->raw.i_uid    = (uint16_t)(inode->uid & 0xFFFF);
    priv->raw.i_uid_hi = (uint16_t)((inode->uid >> 16) & 0xFFFF);
    priv->raw.i_gid    = (uint16_t)(inode->gid & 0xFFFF);
    priv->raw.i_gid_hi = (uint16_t)((inode->gid >> 16) & 0xFFFF);
    return ext4_write_inode(priv->fs, priv->inode_no, &priv->raw);
}

static struct vfs_inode_ops ext4_inode_ops = {
    .lookup  = ext4_lookup,
    .create  = ext4_create,
    .mkdir   = ext4_mkdir,
    .unlink  = ext4_unlink,
    .rmdir   = ext4_rmdir,
    .readdir = ext4_readdir_vfs,
    .setattr = ext4_setattr,
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
