#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include <stddef.h>

/* ATA bus I/O ports */
#define ATA_PRIMARY_BASE    0x1F0
#define ATA_PRIMARY_CTRL    0x3F6
#define ATA_SECONDARY_BASE  0x170
#define ATA_SECONDARY_CTRL  0x376

/* ATA register offsets */
#define ATA_REG_DATA        0x00
#define ATA_REG_ERROR       0x01
#define ATA_REG_FEATURES    0x01
#define ATA_REG_SECCOUNT    0x02
#define ATA_REG_LBA_LO      0x03
#define ATA_REG_LBA_MID     0x04
#define ATA_REG_LBA_HIGH    0x05
#define ATA_REG_DRIVE       0x06
#define ATA_REG_STATUS      0x07
#define ATA_REG_COMMAND     0x07

/* ATA commands */
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_FLUSH_CACHE 0xE7
#define ATA_CMD_READ_DMA    0xC8
#define ATA_CMD_WRITE_DMA   0xCA
#define ATA_CMD_READ_DMA_EXT 0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35

/* ATA status bits */
#define ATA_STAT_ERR        0x01
#define ATA_STAT_DRQ        0x08
#define ATA_STAT_SRV        0x10
#define ATA_STAT_DF         0x20
#define ATA_STAT_RDY        0x40
#define ATA_STAT_BSY        0x80

/* Drive selection */
#define ATA_DRIVE_MASTER    0xA0
#define ATA_DRIVE_SLAVE     0xB0
#define ATA_DRIVE_LBA       0x40

/* BMIDE registers (offsets from BMIDE base) */
#define BMIDE_CMD           0x00
#define BMIDE_STATUS        0x02
#define BMIDE_PRDT          0x04

/* BMIDE command bits */
#define BMIDE_CMD_START     0x01
#define BMIDE_CMD_READ      0x08

/* BMIDE status bits */
#define BMIDE_STAT_DMA_ACT  0x01
#define BMIDE_STAT_DMA_ERR  0x02
#define BMIDE_STAT_INT      0x04

/* PRDT entry */
typedef struct ata_prdt {
    uint32_t phys_addr;
    uint16_t byte_count;
    uint16_t flags;
} __attribute__((packed)) ata_prdt_t;

#define PRDT_FLAG_EOT       0x8000

typedef struct ata_device {
    uint16_t base;
    uint16_t ctrl;
    uint16_t bmide;
    uint8_t  drive;
    uint8_t  present;
    uint8_t  lba48;
    uint64_t sectors;
    char     model[41];
} ata_dev_t;

void ata_init(void);

int ata_read_sectors(ata_dev_t *dev, uint64_t lba, void *buf, size_t count);
int ata_write_sectors(ata_dev_t *dev, uint64_t lba, const void *buf, size_t count);
int ata_read_sectors_dma(ata_dev_t *dev, uint64_t lba, void *buf, size_t count);
int ata_write_sectors_dma(ata_dev_t *dev, uint64_t lba, const void *buf, size_t count);
int ata_flush(ata_dev_t *dev);

ata_dev_t *ata_get_device(int idx);

#endif /* ATA_H */
