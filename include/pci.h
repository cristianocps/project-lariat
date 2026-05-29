#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include "device.h"
#include "driver.h"

/* --------------------------------------------------------------------------
 * PCI Configuration Space
 * -------------------------------------------------------------------------- */
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

#define PCI_VENDOR_ID    0x00
#define PCI_DEVICE_ID    0x02
#define PCI_COMMAND      0x04
#define PCI_STATUS       0x06
#define PCI_REVISION     0x08
#define PCI_PROG_IF      0x09
#define PCI_SUBCLASS     0x0A
#define PCI_CLASS        0x0B
#define PCI_CACHE_LINE   0x0C
#define PCI_LATENCY      0x0D
#define PCI_HEADER_TYPE  0x0E
#define PCI_BIST         0x0F
#define PCI_BAR0         0x10
#define PCI_BAR1         0x14
#define PCI_BAR2         0x18
#define PCI_BAR3         0x1C
#define PCI_BAR4         0x20
#define PCI_BAR5         0x24
#define PCI_CAP_PTR      0x34
#define PCI_INT_LINE     0x3C
#define PCI_INT_PIN      0x3D

#define PCI_CLASS_UNCLASSIFIED   0x00
#define PCI_CLASS_STORAGE        0x01
#define PCI_CLASS_NETWORK        0x02
#define PCI_CLASS_DISPLAY        0x03
#define PCI_CLASS_MULTIMEDIA     0x04
#define PCI_CLASS_MEMORY         0x05
#define PCI_CLASS_BRIDGE         0x06
#define PCI_CLASS_SERIAL         0x0C

#define PCI_CMD_IO_SPACE         (1 << 0)
#define PCI_CMD_MEM_SPACE        (1 << 1)
#define PCI_CMD_BUS_MASTER       (1 << 2)
#define PCI_CMD_INT_DISABLE      (1 << 10)

/* --------------------------------------------------------------------------
 * PCI device descriptor (bus_data for device_t)
 * -------------------------------------------------------------------------- */
typedef struct pci_dev {
    uint16_t bus;
    uint16_t slot;
    uint16_t func;

    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;
    uint8_t  int_line;
    uint8_t  int_pin;

    uint32_t bar[6];
} pci_dev_t;

/* --------------------------------------------------------------------------
 * PCI driver matching
 * -------------------------------------------------------------------------- */
typedef struct pci_id {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t class_code;
    uint16_t subclass;
    uint16_t prog_if;
} pci_id_t;

#define PCI_ID_ANY  0xFFFF

/* Helper: match any vendor/device, specific class/subclass/prog_if */
#define PCI_DEVICE_CLASS(cls, sub, prog) \
    { .vendor_id = PCI_ID_ANY, .device_id = PCI_ID_ANY, \
      .class_code = (cls), .subclass = (sub), .prog_if = (prog) }

#define PCI_DEVICE(vend, dev) \
    { .vendor_id = (vend), .device_id = (dev), \
      .class_code = PCI_ID_ANY, .subclass = PCI_ID_ANY, .prog_if = PCI_ID_ANY }

/* --------------------------------------------------------------------------
 * PCI bus API
 * -------------------------------------------------------------------------- */
void pci_init(void);

uint32_t pci_read_config(uint16_t bus, uint16_t slot, uint16_t func, uint16_t offset);
void     pci_write_config(uint16_t bus, uint16_t slot, uint16_t func, uint16_t offset, uint32_t val);

uint16_t pci_vendor(uint16_t bus, uint16_t slot, uint16_t func);
uint16_t pci_device(uint16_t bus, uint16_t slot, uint16_t func);
uint8_t  pci_class(uint16_t bus, uint16_t slot, uint16_t func);
uint8_t  pci_subclass(uint16_t bus, uint16_t slot, uint16_t func);
uint8_t  pci_prog_if(uint16_t bus, uint16_t slot, uint16_t func);
uint32_t pci_bar(uint16_t bus, uint16_t slot, uint16_t func, uint8_t bar);

void pci_set_command(uint16_t bus, uint16_t slot, uint16_t func, uint16_t cmd);
uint16_t pci_get_command(uint16_t bus, uint16_t slot, uint16_t func);

/* Enable bus mastering, I/O, and memory space */
void pci_enable_bus_mastering(uint16_t bus, uint16_t slot, uint16_t func);

/* Check if a BAR is I/O or memory mapped */
static inline int pci_bar_is_io(uint32_t bar)   { return (bar & 1) != 0; }
static inline int pci_bar_is_mem(uint32_t bar)  { return (bar & 1) == 0; }
static inline uint32_t pci_bar_io_addr(uint32_t bar)  { return bar & ~0x3; }
static inline uint64_t pci_bar_mem_addr(uint32_t bar) { return bar & ~0xF; }

/* Get bus_data as pci_dev_t */
static inline pci_dev_t *device_pci(device_t *dev) {
    return (pci_dev_t *)dev->bus_data;
}

/* --------------------------------------------------------------------------
 * PCI driver helper
 * -------------------------------------------------------------------------- */
typedef struct pci_driver {
    driver_t    drv;
    pci_id_t   *ids;       /* NULL-terminated array of supported IDs */
    int       (*probe_pci)(struct pci_driver *pdrv, device_t *dev, pci_dev_t *pci);
    void      (*remove_pci)(struct pci_driver *pdrv, device_t *dev);
} pci_driver_t;

int pci_register_driver(pci_driver_t *pdrv);
void pci_unregister_driver(pci_driver_t *pdrv);

#endif
