#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>
#include <stddef.h>
#include "device.h"

/* --------------------------------------------------------------------------
 * Block device management
 * -------------------------------------------------------------------------- */

typedef struct block_device {
    device_t    *dev;
    uint64_t     block_size;
    uint64_t     num_blocks;
    const char  *name;
} block_dev_t;

void block_init(void);

int block_register(device_t *dev);
void block_unregister(device_t *dev);

device_t *block_find(const char *name);

device_t *block_get_first(void);

/* Read/write blocks (LBA addressing). These go through a small write-through
 * sector cache in block.c (see block_read/block_write there) so that repeated
 * reads of hot metadata (FAT chains, ext4 inode/extent blocks) don't re-hit the
 * disk, and large sequential reads are issued as few big DMA ops. */
int block_read(device_t *dev, uint64_t lba, void *buf, size_t count);
int block_write(device_t *dev, uint64_t lba, const void *buf, size_t count);

static inline int block_flush(device_t *dev) {
    if (dev && dev->class == DEV_CLASS_BLOCK && dev->ops.block_ops && dev->ops.block_ops->flush)
        return dev->ops.block_ops->flush(dev);
    return -1;
}

#endif /* BLOCK_H */
