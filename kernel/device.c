#include "device.h"
#include "driver.h"
#include "serial.h"
#include "vga.h"
#include "kapi.h"

device_t *device_list = NULL;
spinlock_t device_lock = SPINLOCK_INIT;

void device_init(void) {
    device_list = NULL;
    device_lock.locked = 0;
}

device_t *device_create(const char *name, device_class_t class,
                        driver_t *drv, bus_t *bus, device_t *parent)
{
    device_t *dev = kzalloc(sizeof(device_t));
    if (!dev) {
        KAPI_ERR("device_create: out of memory for '%s'\n", name);
        return NULL;
    }

    dev->name    = name;
    dev->class   = class;
    dev->driver  = drv;
    dev->bus     = bus;
    dev->parent  = parent;
    dev->children = NULL;
    dev->sibling  = NULL;
    dev->priv     = NULL;
    dev->bus_data = NULL;
    dev->ref_count = 1;
    dev->flags    = 0;
    dev->ops.raw_ops = NULL;

    if (parent) {
        dev->sibling = parent->children;
        parent->children = dev;
    }

    return dev;
}

void device_destroy(device_t *dev) {
    if (!dev) return;

    /* Detach from parent */
    if (dev->parent) {
        device_t **pp = &dev->parent->children;
        while (*pp && *pp != dev) {
            pp = &(*pp)->sibling;
        }
        if (*pp == dev) {
            *pp = dev->sibling;
        }
    }

    kfree(dev);
}

void device_register(device_t *dev) {
    if (!dev) return;

    uint64_t flags = spin_lock_irqsave(&device_lock);
    dev->sibling = device_list;
    device_list = dev;
    spin_unlock_irqrestore(&device_lock, flags);

    KAPI_INFO("registered device: %s (class=%d)\n", dev->name, dev->class);
}

void device_unregister(device_t *dev) {
    if (!dev) return;

    uint64_t flags = spin_lock_irqsave(&device_lock);
    device_t **pp = &device_list;
    while (*pp && *pp != dev) {
        pp = &(*pp)->sibling;
    }
    if (*pp == dev) {
        *pp = dev->sibling;
    }
    spin_unlock_irqrestore(&device_lock, flags);
}

device_t *device_find(const char *name) {
    uint64_t flags = spin_lock_irqsave(&device_lock);
    for (device_t *dev = device_list; dev; dev = dev->sibling) {
        if (dev->name) {
            const char *a = dev->name;
            const char *b = name;
            while (*a && *a == *b) { a++; b++; }
            if (*a == *b) {
                spin_unlock_irqrestore(&device_lock, flags);
                return device_get(dev);
            }
        }
    }
    spin_unlock_irqrestore(&device_lock, flags);
    return NULL;
}

device_t *device_find_by_class(device_class_t class) {
    uint64_t flags = spin_lock_irqsave(&device_lock);
    for (device_t *dev = device_list; dev; dev = dev->sibling) {
        if (dev->class == class) {
            spin_unlock_irqrestore(&device_lock, flags);
            return device_get(dev);
        }
    }
    spin_unlock_irqrestore(&device_lock, flags);
    return NULL;
}

void device_list_class(device_class_t class,
                       void (*cb)(device_t *dev, void *ctx), void *ctx)
{
    uint64_t flags = spin_lock_irqsave(&device_lock);
    for (device_t *dev = device_list; dev; dev = dev->sibling) {
        if (dev->class == class) {
            cb(dev, ctx);
        }
    }
    spin_unlock_irqrestore(&device_lock, flags);
}

device_t *device_get(device_t *dev) {
    if (!dev) return NULL;
    uint64_t flags = spin_lock_irqsave(&device_lock);
    dev->ref_count++;
    spin_unlock_irqrestore(&device_lock, flags);
    return dev;
}

void device_put(device_t *dev) {
    if (!dev) return;
    uint64_t flags = spin_lock_irqsave(&device_lock);
    dev->ref_count--;
    if (dev->ref_count == 0) {
        spin_unlock_irqrestore(&device_lock, flags);
        device_destroy(dev);
        return;
    }
    spin_unlock_irqrestore(&device_lock, flags);
}
