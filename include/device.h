#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>
#include <stddef.h>
#include "kapi.h"
/* ssize_t is not in freestanding stdint.h */
typedef intptr_t ssize_t;

/* Device classes */
typedef enum {
    DEV_CLASS_NONE,
    DEV_CLASS_CHAR,       /* Serial, keyboard */
    DEV_CLASS_BLOCK,      /* Disks */
    DEV_CLASS_NET,        /* NICs */
    DEV_CLASS_FRAMEBUFFER,/* VGA, GPU */
    DEV_CLASS_BUS,        /* PCI, USB controllers */
    DEV_CLASS_OTHER,
} device_class_t;

/* Forward declarations */
struct driver;
struct device;
struct bus;

/* --------------------------------------------------------------------------
 * Device operations vtable (per class)
 * -------------------------------------------------------------------------- */
typedef struct {
    int     (*open)(struct device *dev, int flags);
    int     (*close)(struct device *dev);
    ssize_t (*read)(struct device *dev, void *buf, size_t count);
    ssize_t (*write)(struct device *dev, const void *buf, size_t count);
    int     (*ioctl)(struct device *dev, unsigned long req, void *arg);
    int     (*poll)(struct device *dev, short events);
} char_ops_t;

typedef struct {
    int     (*open)(struct device *dev, int flags);
    int     (*close)(struct device *dev);
    ssize_t (*read)(struct device *dev, uint64_t lba, void *buf, size_t count);
    ssize_t (*write)(struct device *dev, uint64_t lba, const void *buf, size_t count);
    int     (*flush)(struct device *dev);
} block_ops_t;

/* --------------------------------------------------------------------------
 * Device instance
 * -------------------------------------------------------------------------- */
typedef struct device {
    const char         *name;
    device_class_t      class;
    struct driver      *driver;
    struct bus         *bus;
    struct device      *parent;
    struct device      *children;
    struct device      *sibling;

    union {
        const char_ops_t  *char_ops;
        const block_ops_t *block_ops;
        void              *raw_ops;
    } ops;

    void               *priv;       /* Driver private data */
    void               *bus_data;   /* Bus-specific data (pci_dev_t*, etc) */

    uint32_t            ref_count;
    int                 flags;
} device_t;

/* --------------------------------------------------------------------------
 * Device tree / registry
 * -------------------------------------------------------------------------- */
extern device_t *device_list;
extern spinlock_t device_lock;

void device_init(void);

device_t *device_create(const char *name, device_class_t class,
                        struct driver *drv, struct bus *bus,
                        device_t *parent);
void device_destroy(device_t *dev);
void device_register(device_t *dev);
void device_unregister(device_t *dev);

device_t *device_find(const char *name);
device_t *device_find_by_class(device_class_t class);
void device_list_class(device_class_t class,
                       void (*cb)(device_t *dev, void *ctx), void *ctx);

/* Reference counting */
device_t *device_get(device_t *dev);
void device_put(device_t *dev);

/* Class helpers */
static inline ssize_t device_read(device_t *dev, void *buf, size_t count) {
    if (dev->class == DEV_CLASS_CHAR && dev->ops.char_ops && dev->ops.char_ops->read)
        return dev->ops.char_ops->read(dev, buf, count);
    return -1;
}

static inline ssize_t device_write(device_t *dev, const void *buf, size_t count) {
    if (dev->class == DEV_CLASS_CHAR && dev->ops.char_ops && dev->ops.char_ops->write)
        return dev->ops.char_ops->write(dev, buf, count);
    return -1;
}

#endif
