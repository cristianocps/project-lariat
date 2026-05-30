#include "ioapic.h"
#include "acpi.h"
#include "kapi.h"
#include "mm.h"
#include "serial.h"

#define IOAPIC_REGSEL 0x00
#define IOAPIC_REGWIN 0x10

#define IOAPIC_REG_VER     0x01
#define IOAPIC_REG_REDTBL  0x10   /* each entry is 2 dwords */

static volatile uint32_t *ioapic = 0;
static uint32_t gsi_base = 0;

static void ioapic_write(uint32_t reg, uint32_t val) {
    ioapic[IOAPIC_REGSEL / 4] = reg;
    ioapic[IOAPIC_REGWIN / 4] = val;
}

static uint32_t ioapic_read(uint32_t reg) {
    ioapic[IOAPIC_REGSEL / 4] = reg;
    return ioapic[IOAPIC_REGWIN / 4];
}

void ioapic_init(void) {
    ioapic = (volatile uint32_t *)ioremap(g_acpi.ioapic_base, 0x1000);
    gsi_base = g_acpi.ioapic_gsi_base;
    uint32_t ver = ioapic_read(IOAPIC_REG_VER);
    uint32_t maxred = (ver >> 16) & 0xFF;
    serial_printf(SERIAL_COM1, "[IOAPIC] base %x mapped %x entries=%d\n",
                  g_acpi.ioapic_base, (uint64_t)ioapic, (int)(maxred + 1));
    /* Mask everything initially. */
    for (uint32_t i = 0; i <= maxred; i++) {
        ioapic_write(IOAPIC_REG_REDTBL + i * 2, 1 << 16);
        ioapic_write(IOAPIC_REG_REDTBL + i * 2 + 1, 0);
    }
}

void ioapic_route(uint8_t gsi, uint8_t vector, uint8_t apic_id) {
    uint32_t idx = (gsi - gsi_base);
    uint32_t lo = vector;                 /* fixed delivery, edge, active-high, unmasked */
    uint32_t hi = (uint32_t)apic_id << 24;
    ioapic_write(IOAPIC_REG_REDTBL + idx * 2 + 1, hi);
    ioapic_write(IOAPIC_REG_REDTBL + idx * 2, lo);
}

void ioapic_route_ex(uint8_t gsi, uint8_t vector, uint8_t apic_id,
                     int level, int active_low) {
    uint32_t idx = (gsi - gsi_base);
    uint32_t lo = vector;
    if (active_low) lo |= (1 << 13);      /* interrupt input polarity = low */
    if (level)      lo |= (1 << 15);      /* trigger mode = level */
    uint32_t hi = (uint32_t)apic_id << 24;
    ioapic_write(IOAPIC_REG_REDTBL + idx * 2 + 1, hi);
    ioapic_write(IOAPIC_REG_REDTBL + idx * 2, lo);
}

void ioapic_mask(uint8_t gsi, int masked) {
    uint32_t idx = (gsi - gsi_base);
    uint32_t lo = ioapic_read(IOAPIC_REG_REDTBL + idx * 2);
    if (masked) lo |= (1 << 16);
    else        lo &= ~(1 << 16);
    ioapic_write(IOAPIC_REG_REDTBL + idx * 2, lo);
}
