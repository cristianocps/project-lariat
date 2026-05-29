#ifndef DRIVER_H
#define DRIVER_H

#include <stdint.h>
#include "device.h"

/* --------------------------------------------------------------------------
 * Driver model
 *
 * Drivers register with the kernel. Buses enumerate devices and call
 * driver_match() → driver_probe() for each compatible device.
 * -------------------------------------------------------------------------- */

typedef struct driver {
    const char    *name;
    const char    *version;

    /* Bus this driver attaches to (NULL = any / platform) */
    struct bus    *bus;

    /* Called during system init or module load */
    int (*init)(void);

    /* Called during system shutdown or module unload */
    void (*exit)(void);

    /*
     * match: return non-zero if this driver can handle the device.
     * probe: attach driver to device; allocate priv, set up ops.
     * remove: detach; free priv, shut down device.
     */
    int  (*match)(struct driver *drv, device_t *dev);
    int  (*probe)(struct driver *drv, device_t *dev);
    void (*remove)(struct driver *drv, device_t *dev);

    struct driver *next;
} driver_t;

/* --------------------------------------------------------------------------
 * Bus abstraction
 * -------------------------------------------------------------------------- */
typedef struct bus {
    const char    *name;

    /* Enumerate devices on this bus and register them */
    int  (*scan)(struct bus *bus);

    /* Read/write bus-specific config (e.g. PCI config space) */
    uint32_t (*read_config)(device_t *dev, uint16_t offset);
    void     (*write_config)(device_t *dev, uint16_t offset, uint32_t val);

    /* IRQ routing */
    int  (*request_irq)(device_t *dev, uint8_t irq, void (*handler)(void*), void *ctx);
    void (*free_irq)(device_t *dev, uint8_t irq);

    struct bus *next;
} bus_t;

/* --------------------------------------------------------------------------
 * Registration API
 * -------------------------------------------------------------------------- */
void driver_init(void);

int  driver_register(driver_t *drv);
void driver_unregister(driver_t *drv);

int  bus_register(bus_t *bus);
void bus_unregister(bus_t *bus);

/* Probe all registered drivers against all registered devices */
void driver_probe_all(void);

/* Find a driver by name */
driver_t *driver_find(const char *name);
bus_t    *bus_find(const char *name);

/* Macro for static driver registration */
#define __driver_section __attribute__((section(".drivers"), used))

#define REGISTER_DRIVER(drv) \
    static driver_t *__driver_##drv __driver_section = &drv

#endif
