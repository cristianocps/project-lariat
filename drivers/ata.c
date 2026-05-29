#include "ata.h"
#include "ports.h"
#include "device.h"
#include "block.h"
#include "serial.h"
#include "kapi.h"
#include "pci.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * PRDT allocation (single shared PRDT, no multitasking)
 * -------------------------------------------------------------------------- */
static ata_prdt_t ata_prdt __attribute__((aligned(4)));
static volatile uint8_t ata_dma_done;

/* --------------------------------------------------------------------------
 * ATA register access
 * -------------------------------------------------------------------------- */
static inline uint8_t ata_read_reg(ata_dev_t *dev, uint8_t reg) {
    return inb(dev->base + reg);
}

static inline void ata_write_reg(ata_dev_t *dev, uint8_t reg, uint8_t val) {
    outb(dev->base + reg, val);
}

static inline void ata_read_data(ata_dev_t *dev, uint16_t *buf, size_t words) {
    for (size_t i = 0; i < words; i++) {
        buf[i] = inw(dev->base + ATA_REG_DATA);
    }
}

static inline void ata_write_data(ata_dev_t *dev, const uint16_t *buf, size_t words) {
    for (size_t i = 0; i < words; i++) {
        outw(dev->base + ATA_REG_DATA, buf[i]);
    }
}

/* BMIDE register access */
static inline uint8_t bmide_read(ata_dev_t *dev, uint8_t reg) {
    return inb(dev->bmide + reg);
}

static inline void bmide_write(ata_dev_t *dev, uint8_t reg, uint8_t val) {
    outb(dev->bmide + reg, val);
}

/* --------------------------------------------------------------------------
 * Wait for BSY to clear and check for errors
 * -------------------------------------------------------------------------- */
static int ata_wait_bsy(ata_dev_t *dev) {
    uint8_t status;
    int timeout = 100000;
    while (timeout-- > 0) {
        status = ata_read_reg(dev, ATA_REG_STATUS);
        if (!(status & ATA_STAT_BSY)) {
            return (status & ATA_STAT_ERR) ? -1 : 0;
        }
    }
    return -1;
}

static int ata_wait_drq(ata_dev_t *dev) {
    uint8_t status;
    int timeout = 100000;
    while (timeout-- > 0) {
        status = ata_read_reg(dev, ATA_REG_STATUS);
        if (status & ATA_STAT_ERR) return -1;
        if (status & ATA_STAT_DRQ) return 0;
        if (!(status & ATA_STAT_BSY)) return -1;
    }
    return -1;
}

/* --------------------------------------------------------------------------
 * Soft reset the bus
 * -------------------------------------------------------------------------- */
static void ata_soft_reset(uint16_t ctrl) {
    outb(ctrl, 0x04);  /* SRST */
    io_wait();
    io_wait();
    outb(ctrl, 0x00);
    io_wait();
    io_wait();
}

/* --------------------------------------------------------------------------
 * Select drive on the bus
 * -------------------------------------------------------------------------- */
static void ata_select_drive(ata_dev_t *dev) {
    ata_write_reg(dev, ATA_REG_DRIVE, dev->drive | ATA_DRIVE_LBA);
    /* Delay after drive select */
    for (volatile int i = 0; i < 15; i++) {
        inb(dev->ctrl);
    }
}

/* --------------------------------------------------------------------------
 * Issue a command (LBA28)
 * -------------------------------------------------------------------------- */
static int ata_issue_cmd(ata_dev_t *dev, uint8_t cmd,
                         uint64_t lba, uint8_t count) {
    ata_select_drive(dev);

    ata_write_reg(dev, ATA_REG_FEATURES, 0);
    ata_write_reg(dev, ATA_REG_SECCOUNT, count);
    ata_write_reg(dev, ATA_REG_LBA_LO,  (uint8_t)(lba & 0xFF));
    ata_write_reg(dev, ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    ata_write_reg(dev, ATA_REG_LBA_HIGH,(uint8_t)((lba >> 16) & 0xFF));
    ata_write_reg(dev, ATA_REG_DRIVE,
                  dev->drive | ATA_DRIVE_LBA | ((uint8_t)((lba >> 24) & 0x0F)));

    ata_write_reg(dev, ATA_REG_COMMAND, cmd);
    return 0;
}

/* --------------------------------------------------------------------------
 * Issue a command (LBA48)
 * -------------------------------------------------------------------------- */
static int ata_issue_cmd_lba48(ata_dev_t *dev, uint8_t cmd,
                               uint64_t lba, uint16_t count) {
    ata_select_drive(dev);

    ata_write_reg(dev, ATA_REG_FEATURES, 0);
    ata_write_reg(dev, ATA_REG_SECCOUNT, (uint8_t)(count >> 8));
    ata_write_reg(dev, ATA_REG_LBA_LO,  (uint8_t)((lba >> 24) & 0xFF));
    ata_write_reg(dev, ATA_REG_LBA_MID, (uint8_t)((lba >> 32) & 0xFF));
    ata_write_reg(dev, ATA_REG_LBA_HIGH,(uint8_t)((lba >> 40) & 0xFF));

    ata_write_reg(dev, ATA_REG_FEATURES, 0);
    ata_write_reg(dev, ATA_REG_SECCOUNT, (uint8_t)(count & 0xFF));
    ata_write_reg(dev, ATA_REG_LBA_LO,  (uint8_t)(lba & 0xFF));
    ata_write_reg(dev, ATA_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    ata_write_reg(dev, ATA_REG_LBA_HIGH,(uint8_t)((lba >> 16) & 0xFF));
    ata_write_reg(dev, ATA_REG_DRIVE, dev->drive | ATA_DRIVE_LBA);

    ata_write_reg(dev, ATA_REG_COMMAND, cmd);
    return 0;
}

/* --------------------------------------------------------------------------
 * Identify device
 * -------------------------------------------------------------------------- */
static int ata_identify(ata_dev_t *dev) {
    /* Select drive and issue identify */
    ata_write_reg(dev, ATA_REG_DRIVE, dev->drive | ATA_DRIVE_LBA);
    for (volatile int i = 0; i < 15; i++) {
        inb(dev->ctrl);
    }

    ata_write_reg(dev, ATA_REG_SECCOUNT, 0);
    ata_write_reg(dev, ATA_REG_LBA_LO, 0);
    ata_write_reg(dev, ATA_REG_LBA_MID, 0);
    ata_write_reg(dev, ATA_REG_LBA_HIGH, 0);
    ata_write_reg(dev, ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    /* Wait for BSY to clear */
    uint8_t status;
    int timeout = 10000;
    while (timeout-- > 0) {
        status = ata_read_reg(dev, ATA_REG_STATUS);
        if (status != 0) break;
    }
    if (status == 0) return -1;  /* No device */

    /* Check for ATAPI / SATA */
    uint8_t mid = ata_read_reg(dev, ATA_REG_LBA_MID);
    uint8_t high = ata_read_reg(dev, ATA_REG_LBA_HIGH);
    if (mid != 0 || high != 0) return -1;  /* Not ATA */

    /* Wait for command to complete */
    if (ata_wait_bsy(dev) < 0) return -1;
    if (!(ata_read_reg(dev, ATA_REG_STATUS) & ATA_STAT_DRQ)) return -1;

    /* Read identify data */
    uint16_t id_buf[256];
    ata_read_data(dev, id_buf, 256);

    /* Extract info */
    dev->sectors = ((uint64_t)id_buf[61] << 16) | id_buf[60];

    /* Check LBA48 support (word 83, bit 10) */
    dev->lba48 = 0;
    if ((id_buf[83] & 0x0400) && (id_buf[86] & 0x0400)) {
        dev->lba48 = 1;
        dev->sectors = ((uint64_t)id_buf[103] << 48) |
                       ((uint64_t)id_buf[102] << 32) |
                       ((uint64_t)id_buf[101] << 16) |
                       (uint64_t)id_buf[100];
    }

    /* Model string: words 27-46, big-endian within each word */
    for (int i = 0; i < 20; i++) {
        dev->model[i * 2]     = (char)(id_buf[27 + i] >> 8);
        dev->model[i * 2 + 1] = (char)(id_buf[27 + i] & 0xFF);
    }
    dev->model[40] = '\0';

    /* Trim trailing spaces */
    for (int i = 39; i >= 0 && dev->model[i] == ' '; i--) {
        dev->model[i] = '\0';
    }

    dev->present = 1;
    return 0;
}

/* --------------------------------------------------------------------------
 * Read / Write sectors (PIO LBA28)
 * -------------------------------------------------------------------------- */
int ata_read_sectors(ata_dev_t *dev, uint64_t lba, void *buf, size_t count) {
    if (!dev || !dev->present) return -1;
    if (lba + count > dev->sectors) return -1;
    if (count == 0 || count > 255) return -1;

    uint8_t *dest = buf;
    for (size_t i = 0; i < count; i++) {
        if (ata_issue_cmd(dev, ATA_CMD_READ_PIO, lba + i, 1) < 0)
            return -1;
        if (ata_wait_drq(dev) < 0)
            return -1;
        ata_read_data(dev, (uint16_t *)dest, 256);
        dest += 512;
    }
    return 0;
}

int ata_write_sectors(ata_dev_t *dev, uint64_t lba, const void *buf, size_t count) {
    if (!dev || !dev->present) return -1;
    if (lba + count > dev->sectors) return -1;
    if (count == 0 || count > 255) return -1;

    const uint8_t *src = buf;
    for (size_t i = 0; i < count; i++) {
        if (ata_issue_cmd(dev, ATA_CMD_WRITE_PIO, lba + i, 1) < 0)
            return -1;
        if (ata_wait_drq(dev) < 0)
            return -1;
        ata_write_data(dev, (const uint16_t *)src, 256);
        if (ata_wait_bsy(dev) < 0)
            return -1;
        src += 512;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * DMA read / write
 * -------------------------------------------------------------------------- */
static int ata_dma_transfer(ata_dev_t *dev, uint64_t lba, void *buf,
                            size_t count, int is_write) {
    if (!dev || !dev->present) return -1;
    if (count == 0 || count > 256) return -1;

    /* For LBA28, max 28-bit address */
    if (!dev->lba48 && (lba + count > dev->sectors || lba > 0x0FFFFFFF))
        return -1;

    /* Build PRDT: single entry for the whole transfer */
    uint32_t phys = (uint32_t)(uint64_t)buf;  /* identity mapped */
    uint16_t bytes = (uint16_t)(count * 512);
    if (bytes == 0) bytes = 0;  /* 0 means 64KB, but we limit to 256 sectors = 128KB */

    ata_prdt.phys_addr = phys;
    ata_prdt.byte_count = bytes;
    ata_prdt.flags = PRDT_FLAG_EOT;

    /* Program BMIDE */
    uint32_t prdt_phys = (uint32_t)(uint64_t)&ata_prdt;
    bmide_write(dev, BMIDE_CMD, 0);
    bmide_write(dev, BMIDE_STATUS, bmide_read(dev, BMIDE_STATUS) | 0x06); /* clear INT+ERR */
    outl(dev->bmide + BMIDE_PRDT, prdt_phys);

    uint8_t cmd;
    if (dev->lba48) {
        cmd = is_write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
        if (ata_issue_cmd_lba48(dev, cmd, lba, (uint16_t)count) < 0)
            return -1;
    } else {
        cmd = is_write ? ATA_CMD_WRITE_DMA : ATA_CMD_READ_DMA;
        if (ata_issue_cmd(dev, cmd, lba, (uint8_t)count) < 0)
            return -1;
    }

    /* Start DMA */
    uint8_t bcmd = BMIDE_CMD_START;
    if (!is_write) bcmd |= BMIDE_CMD_READ;
    bmide_write(dev, BMIDE_CMD, bcmd);

    /* Poll for completion */
    int timeout = 1000000;
    while (timeout-- > 0) {
        uint8_t bmstat = bmide_read(dev, BMIDE_STATUS);
        if (!(bmstat & BMIDE_STAT_DMA_ACT))
            break;
    }

    /* Stop DMA */
    bmide_write(dev, BMIDE_CMD, 0);

    uint8_t bmstat = bmide_read(dev, BMIDE_STATUS);
    uint8_t status = ata_read_reg(dev, ATA_REG_STATUS);

    if (timeout <= 0) {
        serial_printf(SERIAL_COM1, "[ATA] DMA timeout\n");
        return -1;
    }
    if (bmstat & BMIDE_STAT_DMA_ERR) {
        serial_printf(SERIAL_COM1, "[ATA] DMA error (bmstat=0x%x status=0x%x)\n", bmstat, status);
        return -1;
    }
    if (status & ATA_STAT_ERR) {
        serial_printf(SERIAL_COM1, "[ATA] ATA error after DMA (status=0x%x)\n", status);
        return -1;
    }
    return 0;
}

int ata_read_sectors_dma(ata_dev_t *dev, uint64_t lba, void *buf, size_t count) {
    return ata_dma_transfer(dev, lba, buf, count, 0);
}

int ata_write_sectors_dma(ata_dev_t *dev, uint64_t lba, const void *buf, size_t count) {
    return ata_dma_transfer(dev, lba, (void *)buf, count, 1);
}

int ata_flush(ata_dev_t *dev) {
    if (!dev || !dev->present) return -1;
    ata_select_drive(dev);
    ata_write_reg(dev, ATA_REG_COMMAND, ATA_CMD_FLUSH_CACHE);
    return ata_wait_bsy(dev);
}

/* --------------------------------------------------------------------------
 * Block device ops wrapper (uses DMA)
 * -------------------------------------------------------------------------- */
static ssize_t ata_block_read(device_t *dev, uint64_t lba, void *buf, size_t count) {
    ata_dev_t *ata = (ata_dev_t *)dev->priv;
    return ata_read_sectors_dma(ata, lba, buf, count) == 0 ? (ssize_t)(count * 512) : -1;
}

static ssize_t ata_block_write(device_t *dev, uint64_t lba, const void *buf, size_t count) {
    ata_dev_t *ata = (ata_dev_t *)dev->priv;
    return ata_write_sectors_dma(ata, lba, buf, count) == 0 ? (ssize_t)(count * 512) : -1;
}

static int ata_block_flush(device_t *dev) {
    ata_dev_t *ata = (ata_dev_t *)dev->priv;
    return ata_flush(ata);
}

static block_ops_t ata_block_ops = {
    .read  = ata_block_read,
    .write = ata_block_write,
    .flush = ata_block_flush,
};

/* --------------------------------------------------------------------------
 * Detect BMIDE base via PCI
 * -------------------------------------------------------------------------- */
static void ata_detect_bmide(uint16_t *primary, uint16_t *secondary) {
    *primary = 0;
    *secondary = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint16_t slot = 0; slot < 32; slot++) {
            for (uint16_t func = 0; func < 8; func++) {
                uint16_t vendor = pci_vendor(bus, slot, func);
                if (vendor == 0xFFFF) continue;

                uint8_t cls = pci_class(bus, slot, func);
                uint8_t sub = pci_subclass(bus, slot, func);
                if (cls != PCI_CLASS_STORAGE || sub != 0x01)
                    continue;

                uint32_t bar4 = pci_bar(bus, slot, func, 4);
                if (pci_bar_is_io(bar4)) {
                    uint16_t base = (uint16_t)pci_bar_io_addr(bar4);
                    *primary = base;
                    *secondary = base + 8;
                    pci_enable_bus_mastering(bus, slot, func);
                    serial_printf(SERIAL_COM1,
                        "[ATA] Found PCI IDE controller at %04x:%02x.%x, BMIDE=0x%x\n",
                        bus, slot, func, base);
                    return;
                }
            }
        }
    }

    /* Fallback to legacy ports */
    *primary = 0xC000;
    *secondary = 0xC008;
    serial_printf(SERIAL_COM1,
        "[ATA] No PCI IDE controller found, using legacy BMIDE ports\n");
}

/* --------------------------------------------------------------------------
 * Detected devices
 * -------------------------------------------------------------------------- */
static ata_dev_t ata_devices[4];  /* 2 buses * 2 drives */

ata_dev_t *ata_get_device(int idx) {
    if (idx < 0 || idx >= 4) return NULL;
    return ata_devices[idx].present ? &ata_devices[idx] : NULL;
}

/* --------------------------------------------------------------------------
 * Initialize ATA subsystem
 * -------------------------------------------------------------------------- */
void ata_init(void) {
    static const uint16_t bases[2] = { ATA_PRIMARY_BASE, ATA_SECONDARY_BASE };
    static const uint16_t ctrls[2] = { ATA_PRIMARY_CTRL, ATA_SECONDARY_CTRL };
    static const uint8_t  drives[2] = { ATA_DRIVE_MASTER, ATA_DRIVE_SLAVE };
    static const char    *drive_names[4] = { "hda", "hdb", "hdc", "hdd" };

    uint16_t bmide_primary, bmide_secondary;
    ata_detect_bmide(&bmide_primary, &bmide_secondary);
    uint16_t bmide_bases[2] = { bmide_primary, bmide_secondary };

    int found = 0;
    for (int bus = 0; bus < 2; bus++) {
        ata_soft_reset(ctrls[bus]);
        for (int drv = 0; drv < 2; drv++) {
            int idx = bus * 2 + drv;
            ata_dev_t *adev = &ata_devices[idx];
            adev->base   = bases[bus];
            adev->ctrl   = ctrls[bus];
            adev->bmide  = bmide_bases[bus];
            adev->drive  = drives[drv];
            adev->present= 0;
            adev->sectors= 0;
            adev->lba48  = 0;

            serial_printf(SERIAL_COM1, "[ATA] Probing %s...\n", drive_names[idx]);
            if (ata_identify(adev) < 0) {
                serial_printf(SERIAL_COM1, "[ATA] %s not found\n", drive_names[idx]);
                continue;
            }

            found++;
            serial_printf(SERIAL_COM1,
                "[ATA] %s: %s, %d sectors (%d MB), %s\n",
                drive_names[idx], adev->model,
                (int)adev->sectors,
                (int)(adev->sectors / 2048),
                adev->lba48 ? "LBA48" : "LBA28");

            /* Register as block device */
            device_t *bdev = device_create(drive_names[idx],
                                           DEV_CLASS_BLOCK, NULL, NULL, NULL);
            if (bdev) {
                bdev->ops.block_ops = &ata_block_ops;
                bdev->priv = adev;
                block_register(bdev);
            }
        }
    }

    if (found == 0) {
        serial_printf(SERIAL_COM1, "[ATA] No drives detected\n");
    }
}
