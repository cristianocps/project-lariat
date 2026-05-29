#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include <stddef.h>
#include "vfs.h"
#include "device.h"

/* FAT32 BPB (offset 0 in boot sector) */
typedef struct __attribute__((packed)) fat32_bpb {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;      /* 0 for FAT32 */
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t fat_size_16;       /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extended BPB */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_num;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
} fat32_bpb_t;

/* Directory entry (32 bytes) */
typedef struct __attribute__((packed)) fat32_dirent {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  nt_res;
    uint8_t  crt_time_tenth;
    uint16_t crt_time;
    uint16_t crt_date;
    uint16_t lst_acc_date;
    uint16_t fst_clus_hi;
    uint16_t wrt_time;
    uint16_t wrt_date;
    uint16_t fst_clus_lo;
    uint32_t file_size;
} fat32_dirent_t;

/* Attributes */
#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       0x0F

/* FAT32 special cluster values */
#define FAT32_CLUSTER_FREE      0x00000000
#define FAT32_CLUSTER_RESERVED  0x00000001
#define FAT32_CLUSTER_MIN       0x00000002
#define FAT32_CLUSTER_MAX       0x0FFFFFF6
#define FAT32_CLUSTER_BAD       0x0FFFFFF7
#define FAT32_CLUSTER_EOF       0x0FFFFFFF

/* FAT32 filesystem instance */
typedef struct fat32_fs {
    device_t     *dev;
    fat32_bpb_t   bpb;
    uint32_t      fat_start;
    uint32_t      data_start;
    uint32_t      root_dir_sectors;
    uint32_t      total_clusters;
    uint32_t      bytes_per_cluster;
} fat32_fs_t;

/* Registration */
void fat32_init(void);

#endif /* FAT32_H */
