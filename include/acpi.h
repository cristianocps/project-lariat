#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

#define MAX_CPUS 32

struct acpi_info {
    uint32_t cpu_count;
    uint8_t  lapic_ids[MAX_CPUS];   /* APIC id of each detected CPU */
    uint64_t lapic_base;            /* Local APIC MMIO physical base */
    uint64_t ioapic_base;           /* I/O APIC MMIO physical base */
    uint32_t ioapic_gsi_base;       /* global system interrupt base */
    int      bsp_index;             /* index in lapic_ids of the BSP */
};

extern struct acpi_info g_acpi;

/* Parse ACPI tables (RSDP -> RSDT/XSDT -> MADT).  Returns 0 on success. */
int acpi_init(void);

#endif /* ACPI_H */
