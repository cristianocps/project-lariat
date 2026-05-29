#include "driver.h"
#include "device.h"
#include "kapi.h"

static driver_t *driver_list = NULL;
static bus_t    *bus_list    = NULL;
static spinlock_t driver_lock = SPINLOCK_INIT;

void driver_init(void) {
    driver_list = NULL;
    bus_list    = NULL;
    driver_lock.locked = 0;
}

int driver_register(driver_t *drv) {
    if (!drv || !drv->name) return -1;

    uint64_t flags = spin_lock_irqsave(&driver_lock);
    drv->next = driver_list;
    driver_list = drv;
    spin_unlock_irqrestore(&driver_lock, flags);

    KAPI_INFO("registered driver: %s v%s\n", drv->name,
              drv->version ? drv->version : "?");

    /* If the driver has an init hook, call it */
    if (drv->init) {
        int rc = drv->init();
        if (rc < 0) {
            KAPI_ERR("driver '%s' init failed: %d\n", drv->name, rc);
            driver_unregister(drv);
            return rc;
        }
    }

    return 0;
}

void driver_unregister(driver_t *drv) {
    if (!drv) return;

    if (drv->exit) {
        drv->exit();
    }

    uint64_t flags = spin_lock_irqsave(&driver_lock);
    driver_t **pp = &driver_list;
    while (*pp && *pp != drv) {
        pp = &(*pp)->next;
    }
    if (*pp == drv) {
        *pp = drv->next;
    }
    spin_unlock_irqrestore(&driver_lock, flags);
}

int bus_register(bus_t *bus) {
    if (!bus || !bus->name) return -1;

    uint64_t flags = spin_lock_irqsave(&driver_lock);
    bus->next = bus_list;
    bus_list = bus;
    spin_unlock_irqrestore(&driver_lock, flags);

    KAPI_INFO("registered bus: %s\n", bus->name);

    /* Scan the bus immediately */
    if (bus->scan) {
        bus->scan(bus);
    }

    return 0;
}

void bus_unregister(bus_t *bus) {
    if (!bus) return;

    uint64_t flags = spin_lock_irqsave(&driver_lock);
    bus_t **pp = &bus_list;
    while (*pp && *pp != bus) {
        pp = &(*pp)->next;
    }
    if (*pp == bus) {
        *pp = bus->next;
    }
    spin_unlock_irqrestore(&driver_lock, flags);
}

/* Probe all unclaimed devices against all registered drivers */
void driver_probe_all(void) {
    /* Iterate all devices and try matching drivers */
    extern device_t *device_list;
    extern spinlock_t device_lock;

    uint64_t flags = spin_lock_irqsave(&device_lock);

    for (device_t *dev = device_list; dev; dev = dev->sibling) {
        if (dev->driver) continue;  /* Already claimed */

        for (driver_t *drv = driver_list; drv; drv = drv->next) {
            if (drv->bus && drv->bus != dev->bus) continue;
            if (!drv->match || !drv->probe) continue;

            if (drv->match(drv, dev)) {
                int rc = drv->probe(drv, dev);
                if (rc == 0) {
                    dev->driver = drv;
                    KAPI_INFO("driver '%s' bound to device '%s'\n",
                              drv->name, dev->name);
                    break;
                }
            }
        }
    }

    spin_unlock_irqrestore(&device_lock, flags);
}

driver_t *driver_find(const char *name) {
    uint64_t flags = spin_lock_irqsave(&driver_lock);
    for (driver_t *drv = driver_list; drv; drv = drv->next) {
        const char *a = drv->name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) {
            spin_unlock_irqrestore(&driver_lock, flags);
            return drv;
        }
    }
    spin_unlock_irqrestore(&driver_lock, flags);
    return NULL;
}

bus_t *bus_find(const char *name) {
    uint64_t flags = spin_lock_irqsave(&driver_lock);
    for (bus_t *bus = bus_list; bus; bus = bus->next) {
        const char *a = bus->name;
        const char *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == *b) {
            spin_unlock_irqrestore(&driver_lock, flags);
            return bus;
        }
    }
    spin_unlock_irqrestore(&driver_lock, flags);
    return NULL;
}
