#include "acpi.h"
#include "mm.h"
#include "serial.h"
#include <string.h>

struct acpi_info g_acpi = {
    .cpu_count = 0,
    .lapic_base = 0xFEE00000ULL,   /* architectural default */
    .ioapic_base = 0xFEC00000ULL,
    .ioapic_gsi_base = 0,
    .bsp_index = 0,
};

struct rsdp_desc {
    char     signature[8];
    uint8_t  checksum;
    char     oemid[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oemid[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct madt {
    struct sdt_header header;
    uint32_t lapic_address;
    uint32_t flags;
    uint8_t  entries[];
} __attribute__((packed));

struct madt_entry_hdr {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

static int checksum_ok(const void *p, uint32_t len) {
    const uint8_t *b = p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += b[i];
    return sum == 0;
}

static struct rsdp_desc *find_rsdp(void) {
    /* 1) Search the first KB of the EBDA. */
    uint16_t ebda_seg = *(uint16_t *)phys_to_virt(0x40E);
    uint64_t ebda = (uint64_t)ebda_seg << 4;
    if (ebda) {
        for (uint64_t a = ebda; a < ebda + 1024; a += 16) {
            char *s = (char *)phys_to_virt(a);
            if (memcmp(s, "RSD PTR ", 8) == 0) return (struct rsdp_desc *)s;
        }
    }
    /* 2) Search the BIOS area 0xE0000-0xFFFFF. */
    for (uint64_t a = 0xE0000; a < 0x100000; a += 16) {
        char *s = (char *)phys_to_virt(a);
        if (memcmp(s, "RSD PTR ", 8) == 0) return (struct rsdp_desc *)s;
    }
    return NULL;
}

static void parse_madt(struct madt *madt) {
    g_acpi.lapic_base = madt->lapic_address;
    uint8_t *p = madt->entries;
    uint8_t *end = (uint8_t *)madt + madt->header.length;

    while (p < end) {
        struct madt_entry_hdr *h = (struct madt_entry_hdr *)p;
        if (h->length == 0) break;
        switch (h->type) {
        case 0: { /* Processor Local APIC */
            uint8_t apic_id = p[3];
            uint32_t flags = *(uint32_t *)(p + 4);
            if ((flags & 1) && g_acpi.cpu_count < MAX_CPUS) {  /* enabled */
                g_acpi.lapic_ids[g_acpi.cpu_count++] = apic_id;
            }
            break;
        }
        case 1: { /* I/O APIC */
            g_acpi.ioapic_base = *(uint32_t *)(p + 4);
            g_acpi.ioapic_gsi_base = *(uint32_t *)(p + 8);
            break;
        }
        case 5: { /* Local APIC Address Override (64-bit) */
            g_acpi.lapic_base = *(uint64_t *)(p + 4);
            break;
        }
        default: break;
        }
        p += h->length;
    }
}

int acpi_init(void) {
    struct rsdp_desc *rsdp = find_rsdp();
    if (!rsdp) {
        serial_print(SERIAL_COM1, "[ACPI] RSDP not found\n");
        return -1;
    }
    serial_printf(SERIAL_COM1, "[ACPI] RSDP at %x rev=%d\n",
                  virt_to_phys(rsdp), rsdp->revision);

    /* Walk the RSDT (ACPI 1.0) or XSDT (ACPI 2.0+) and locate the MADT. */
    int found = 0;
    if (rsdp->revision >= 2 && rsdp->xsdt_address) {
        struct sdt_header *xsdt = (struct sdt_header *)phys_to_virt(rsdp->xsdt_address);
        uint32_t n = (xsdt->length - sizeof(struct sdt_header)) / 8;
        uint64_t *ents = (uint64_t *)((uint8_t *)xsdt + sizeof(struct sdt_header));
        for (uint32_t i = 0; i < n; i++) {
            struct sdt_header *t = (struct sdt_header *)phys_to_virt(ents[i]);
            if (memcmp(t->signature, "APIC", 4) == 0) { parse_madt((struct madt *)t); found = 1; }
        }
    } else if (rsdp->rsdt_address) {
        struct sdt_header *rsdt = (struct sdt_header *)phys_to_virt(rsdp->rsdt_address);
        uint32_t n = (rsdt->length - sizeof(struct sdt_header)) / 4;
        uint32_t *ents = (uint32_t *)((uint8_t *)rsdt + sizeof(struct sdt_header));
        for (uint32_t i = 0; i < n; i++) {
            struct sdt_header *t = (struct sdt_header *)phys_to_virt(ents[i]);
            if (memcmp(t->signature, "APIC", 4) == 0) { parse_madt((struct madt *)t); found = 1; }
        }
    }

    if (!found || g_acpi.cpu_count == 0) {
        serial_print(SERIAL_COM1, "[ACPI] No MADT/CPUs found; assuming 1 CPU\n");
        g_acpi.cpu_count = 1;
        g_acpi.lapic_ids[0] = 0;
    }

    (void)checksum_ok;
    serial_printf(SERIAL_COM1,
        "[ACPI] CPUs=%d lapic_base=%x ioapic_base=%x\n",
        (int)g_acpi.cpu_count, g_acpi.lapic_base, g_acpi.ioapic_base);
    return 0;
}
