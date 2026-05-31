#include "block.h"
#include "serial.h"
#include "kapi.h"
#include <string.h>

static spinlock_t block_lock = SPINLOCK_INIT;

device_t *block_find(const char *name) {
    return device_find(name);
}

device_t *block_get_first(void) {
    return device_find_by_class(DEV_CLASS_BLOCK);
}

int block_register(device_t *dev) {
    if (!dev || dev->class != DEV_CLASS_BLOCK) return -1;
    device_register(dev);
    serial_printf(SERIAL_COM1, "[BLOCK] registered: %s\n", dev->name);
    return 0;
}

void block_unregister(device_t *dev) {
    if (!dev) return;
    device_unregister(dev);
}

/* --------------------------------------------------------------------------
 * Write-through sector cache
 *
 * A small, fully-associative cache of 512-byte sectors keyed by (dev, lba).
 * Writes update the disk AND the cache (write-through), so the cache never
 * holds dirty data and stays coherent with the device. Reads serve hits from
 * RAM and batch runs of consecutive misses into a single device transfer, so a
 * large cold read still issues only a few big DMA ops instead of one per
 * sector. This is what makes large-file reads (e.g. installing a packaged
 * shared library) fast regardless of the on-disk filesystem.
 * -------------------------------------------------------------------------- */
#define BCACHE_ENTRIES 512                 /* 512 * 512 B = 256 KiB of cache */
#define SECTOR_SIZE    512

typedef struct {
    device_t *dev;
    uint64_t  lba;
    uint32_t  age;     /* LRU stamp; 0 == invalid */
    uint8_t   data[SECTOR_SIZE];
} bcache_ent_t;

static bcache_ent_t bcache[BCACHE_ENTRIES];
static uint32_t     bcache_clock = 1;      /* monotonically increasing; 0 reserved */

static inline int raw_read(device_t *dev, uint64_t lba, void *buf, size_t count) {
    if (dev && dev->class == DEV_CLASS_BLOCK && dev->ops.block_ops && dev->ops.block_ops->read)
        return (int)dev->ops.block_ops->read(dev, lba, buf, count);
    return -1;
}

static inline int raw_write(device_t *dev, uint64_t lba, const void *buf, size_t count) {
    if (dev && dev->class == DEV_CLASS_BLOCK && dev->ops.block_ops && dev->ops.block_ops->write)
        return (int)dev->ops.block_ops->write(dev, lba, buf, count);
    return -1;
}

/* Find a cached sector; returns its slot or -1. Caller holds block_lock. */
static int bcache_find(device_t *dev, uint64_t lba) {
    for (int i = 0; i < BCACHE_ENTRIES; i++)
        if (bcache[i].age && bcache[i].dev == dev && bcache[i].lba == lba)
            return i;
    return -1;
}

/* Pick a slot to (re)use: first invalid, else least-recently-used. */
static int bcache_victim(void) {
    int best = 0;
    uint32_t best_age = 0xFFFFFFFFu;
    for (int i = 0; i < BCACHE_ENTRIES; i++) {
        if (!bcache[i].age) return i;
        if (bcache[i].age < best_age) { best_age = bcache[i].age; best = i; }
    }
    return best;
}

/* Insert/refresh one sector in the cache. Caller holds block_lock. */
static void bcache_put(device_t *dev, uint64_t lba, const void *data) {
    int slot = bcache_find(dev, lba);
    if (slot < 0) slot = bcache_victim();
    bcache[slot].dev = dev;
    bcache[slot].lba = lba;
    bcache[slot].age = ++bcache_clock;
    memcpy(bcache[slot].data, data, SECTOR_SIZE);
}

int block_read(device_t *dev, uint64_t lba, void *buf, size_t count) {
    if (!dev || dev->class != DEV_CLASS_BLOCK) return -1;
    uint8_t *out = (uint8_t *)buf;

    spin_lock(&block_lock);
    size_t i = 0;
    while (i < count) {
        int slot = bcache_find(dev, lba + i);
        if (slot >= 0) {
            memcpy(out + i * SECTOR_SIZE, bcache[slot].data, SECTOR_SIZE);
            bcache[slot].age = ++bcache_clock;
            i++;
            continue;
        }
        /* Coalesce a run of consecutive misses and fetch them in one go. */
        size_t run = 1;
        while (i + run < count && bcache_find(dev, lba + i + run) < 0)
            run++;
        if (raw_read(dev, lba + i, out + i * SECTOR_SIZE, run) < 0) {
            spin_unlock(&block_lock);
            return -1;
        }
        for (size_t k = 0; k < run; k++)
            bcache_put(dev, lba + i + k, out + (i + k) * SECTOR_SIZE);
        i += run;
    }
    spin_unlock(&block_lock);
    return 0;
}

int block_write(device_t *dev, uint64_t lba, const void *buf, size_t count) {
    if (!dev || dev->class != DEV_CLASS_BLOCK) return -1;
    const uint8_t *in = (const uint8_t *)buf;

    /* Write-through: hit the device first, then refresh the cache so it stays
     * coherent (no dirty entries to flush later). */
    if (raw_write(dev, lba, buf, count) < 0)
        return -1;

    spin_lock(&block_lock);
    for (size_t k = 0; k < count; k++)
        bcache_put(dev, lba + k, in + k * SECTOR_SIZE);
    spin_unlock(&block_lock);
    return 0;
}

void block_init(void) {
    block_lock.locked = 0;
    for (int i = 0; i < BCACHE_ENTRIES; i++) bcache[i].age = 0;
    bcache_clock = 1;
}
