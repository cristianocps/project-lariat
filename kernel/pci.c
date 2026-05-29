#include "pci.h"
#include "ports.h"
#include "kapi.h"

/* --------------------------------------------------------------------------
 * PCI Configuration Space access (Type 1)
 * -------------------------------------------------------------------------- */
uint32_t pci_read_config(uint16_t bus, uint16_t slot, uint16_t func, uint16_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) |
                                   (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDR, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config(uint16_t bus, uint16_t slot, uint16_t func, uint16_t offset, uint32_t val) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) |
                                   (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDR, address);
    outl(PCI_CONFIG_DATA, val);
}

uint16_t pci_vendor(uint16_t bus, uint16_t slot, uint16_t func) {
    return (uint16_t)(pci_read_config(bus, slot, func, PCI_VENDOR_ID) & 0xFFFF);
}

uint16_t pci_device(uint16_t bus, uint16_t slot, uint16_t func) {
    return (uint16_t)(pci_read_config(bus, slot, func, PCI_DEVICE_ID) >> 16);
}

uint8_t pci_class(uint16_t bus, uint16_t slot, uint16_t func) {
    return (uint8_t)(pci_read_config(bus, slot, func, PCI_CLASS) >> 24);
}

uint8_t pci_subclass(uint16_t bus, uint16_t slot, uint16_t func) {
    return (uint8_t)(pci_read_config(bus, slot, func, PCI_SUBCLASS) >> 16);
}

uint8_t pci_prog_if(uint16_t bus, uint16_t slot, uint16_t func) {
    return (uint8_t)(pci_read_config(bus, slot, func, PCI_PROG_IF) >> 8);
}

uint32_t pci_bar(uint16_t bus, uint16_t slot, uint16_t func, uint8_t bar) {
    if (bar > 5) return 0;
    return pci_read_config(bus, slot, func, PCI_BAR0 + (bar * 4));
}

void pci_set_command(uint16_t bus, uint16_t slot, uint16_t func, uint16_t cmd) {
    uint32_t reg = pci_read_config(bus, slot, func, PCI_COMMAND);
    reg = (reg & ~0xFFFF) | cmd;
    pci_write_config(bus, slot, func, PCI_COMMAND, reg);
}

uint16_t pci_get_command(uint16_t bus, uint16_t slot, uint16_t func) {
    return (uint16_t)(pci_read_config(bus, slot, func, PCI_COMMAND) & 0xFFFF);
}

void pci_enable_bus_mastering(uint16_t bus, uint16_t slot, uint16_t func) {
    uint16_t cmd = pci_get_command(bus, slot, func);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEM_SPACE | PCI_CMD_IO_SPACE;
    pci_set_command(bus, slot, func, cmd);
}

/* --------------------------------------------------------------------------
 * PCI bus scanning
 * -------------------------------------------------------------------------- */

static bus_t pci_bus;

static uint32_t pci_bus_read_config(device_t *dev, uint16_t offset) {
    pci_dev_t *pci = device_pci(dev);
    if (!pci) return 0xFFFFFFFF;
    return pci_read_config(pci->bus, pci->slot, pci->func, offset);
}

static void pci_bus_write_config(device_t *dev, uint16_t offset, uint32_t val) {
    pci_dev_t *pci = device_pci(dev);
    if (!pci) return;
    pci_write_config(pci->bus, pci->slot, pci->func, offset, val);
}

static int pci_bus_scan(bus_t *bus) {
    (void)bus;
    int found = 0;

    for (uint16_t b = 0; b < 256; b++) {
        for (uint16_t s = 0; s < 32; s++) {
            uint16_t vend = pci_vendor(b, s, 0);
            if (vend == 0xFFFF) continue;

            uint8_t header = (uint8_t)(pci_read_config(b, s, 0, PCI_HEADER_TYPE) & 0xFF);
            uint8_t funcs = (header & 0x80) ? 8 : 1;

            for (uint8_t f = 0; f < funcs; f++) {
                vend = pci_vendor(b, s, f);
                if (vend == 0xFFFF) continue;

                pci_dev_t *pci = kzalloc(sizeof(pci_dev_t));
                if (!pci) continue;

                pci->bus      = b;
                pci->slot     = s;
                pci->func     = f;
                pci->vendor_id = vend;
                pci->device_id = pci_device(b, s, f);
                pci->class_code = pci_class(b, s, f);
                pci->subclass   = pci_subclass(b, s, f);
                pci->prog_if    = pci_prog_if(b, s, f);
                pci->revision   = (uint8_t)(pci_read_config(b, s, f, PCI_REVISION) & 0xFF);
                pci->header_type = header & 0x7F;
                pci->int_line   = (uint8_t)(pci_read_config(b, s, f, PCI_INT_LINE) & 0xFF);
                pci->int_pin    = (uint8_t)(pci_read_config(b, s, f, PCI_INT_PIN) & 0xFF);

                for (int i = 0; i < 6; i++) {
                    pci->bar[i] = pci_bar(b, s, f, i);
                }

                /* Create a device node */
                static char devnames[256][32];
                static int devname_idx = 0;
                char *dn = devnames[devname_idx++];

                /* Format: pciBBB:DD.F - using safe digit extraction */
                uint16_t bb = b, ss = s;
                dn[0] = 'p'; dn[1] = 'c'; dn[2] = 'i';
                dn[3] = '0' + (bb / 100); bb %= 100;
                dn[4] = '0' + (bb / 10);  bb %= 10;
                dn[5] = '0' + bb;
                dn[6] = ':';
                dn[7] = '0' + (ss / 10); ss %= 10;
                dn[8] = '0' + ss;
                dn[9] = '.';
                dn[10] = '0' + f;
                dn[11] = '\0';

                device_t *dev = device_create(dn, DEV_CLASS_OTHER, NULL, &pci_bus, NULL);
                if (dev) {
                    dev->bus_data = pci;
                    device_register(dev);
                    found++;

                    KAPI_INFO("PCI %s: %x:%x class=%x.%x.%x\n",
                              dn, pci->vendor_id, pci->device_id,
                              pci->class_code, pci->subclass, pci->prog_if);
                } else {
                    kfree(pci);
                }
            }
        }
    }

    KAPI_INFO("PCI scan complete: %d devices found\n", found);
    return found;
}

/* --------------------------------------------------------------------------
 * PCI driver helpers
 * -------------------------------------------------------------------------- */

static int pci_driver_match(driver_t *drv, device_t *dev) {
    pci_driver_t *pdrv = (pci_driver_t *)drv;
    pci_dev_t *pci = device_pci(dev);
    if (!pci || !pdrv->ids) return 0;

    for (pci_id_t *id = pdrv->ids; id->vendor_id != 0 || id->device_id != 0; id++) {
        int vendor_match = (id->vendor_id == PCI_ID_ANY) || (id->vendor_id == pci->vendor_id);
        int device_match = (id->device_id == PCI_ID_ANY) || (id->device_id == pci->device_id);
        int class_match  = (id->class_code == PCI_ID_ANY) || (id->class_code == pci->class_code);
        int subclass_match = (id->subclass == PCI_ID_ANY) || (id->subclass == pci->subclass);
        int prog_match   = (id->prog_if == PCI_ID_ANY) || (id->prog_if == pci->prog_if);

        if (vendor_match && device_match && class_match && subclass_match && prog_match) {
            return 1;
        }
    }
    return 0;
}

static int pci_driver_probe(driver_t *drv, device_t *dev) {
    pci_driver_t *pdrv = (pci_driver_t *)drv;
    pci_dev_t *pci = device_pci(dev);
    if (!pdrv->probe_pci) return -1;
    return pdrv->probe_pci(pdrv, dev, pci);
}

static void pci_driver_remove(driver_t *drv, device_t *dev) {
    pci_driver_t *pdrv = (pci_driver_t *)drv;
    if (pdrv->remove_pci) {
        pdrv->remove_pci(pdrv, dev);
    }
}

int pci_register_driver(pci_driver_t *pdrv) {
    pdrv->drv.bus    = &pci_bus;
    pdrv->drv.match  = pci_driver_match;
    pdrv->drv.probe  = pci_driver_probe;
    pdrv->drv.remove = pci_driver_remove;
    return driver_register(&pdrv->drv);
}

void pci_unregister_driver(pci_driver_t *pdrv) {
    driver_unregister(&pdrv->drv);
}

/* --------------------------------------------------------------------------
 * PCI bus init
 * -------------------------------------------------------------------------- */
void pci_init(void) {
    pci_bus.name         = "pci";
    pci_bus.scan         = pci_bus_scan;
    pci_bus.read_config  = pci_bus_read_config;
    pci_bus.write_config = pci_bus_write_config;
    pci_bus.request_irq  = NULL;  /* TODO: wire to request_irq */
    pci_bus.free_irq     = NULL;

    bus_register(&pci_bus);
}
