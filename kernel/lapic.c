#include "lapic.h"
#include "acpi.h"
#include "kapi.h"
#include "mm.h"
#include "vmm.h"
#include "ports.h"
#include "serial.h"

#define LAPIC_ID       0x020
#define LAPIC_EOI      0x0B0
#define LAPIC_SVR      0x0F0
#define LAPIC_ESR      0x280
#define LAPIC_ICRLO    0x300
#define LAPIC_ICRHI    0x310
#define LAPIC_LVT_TMR  0x320
#define LAPIC_TMR_ICR  0x380
#define LAPIC_TMR_CCR  0x390
#define LAPIC_TMR_DIV  0x3E0

#define ICR_INIT       0x00000500
#define ICR_STARTUP    0x00000600
#define ICR_LEVEL      0x00008000
#define ICR_ASSERT     0x00004000
#define ICR_DELIV_BUSY 0x00001000

#define LVT_TMR_PERIODIC 0x20000
#define LVT_MASKED       0x10000

static volatile uint32_t *lapic = 0;

static inline void lapic_write(uint32_t reg, uint32_t val) {
    lapic[reg / 4] = val;
    (void)lapic[LAPIC_ID / 4];   /* serialise */
}

static inline uint32_t lapic_read(uint32_t reg) {
    return lapic[reg / 4];
}

/* PIT channel-2 based busy delay; independent of IRQs. */
static void pit_udelay(uint32_t us) {
    uint32_t count = (uint32_t)(((uint64_t)us * 1193182ULL) / 1000000ULL);
    if (count == 0) count = 1;
    if (count > 0xFFFF) count = 0xFFFF;
    uint8_t p = (inb(0x61) & 0xFC) | 0x01;   /* gate on, speaker off */
    outb(0x61, p);
    outb(0x43, 0xB0);                        /* ch2, lo/hi, mode 0 */
    outb(0x42, count & 0xFF);
    outb(0x42, (count >> 8) & 0xFF);
    while (!(inb(0x61) & 0x20))
        ;
}

void lapic_init(void) {
    if (!lapic) {
        /* Map the LAPIC MMIO page into the kernel's direct-map (physmap) window,
         * i.e. the upper half (PML4[256]).  This is shared by every address
         * space (kernel + all user processes share upper-half PML4 entries) and
         * is therefore reachable from every CPU, and it is NOT deep-cloned on
         * fork (unlike a low/PML4[0] mapping, which fork would try to copy). */
        lapic = (volatile uint32_t *)ioremap(g_acpi.lapic_base, 0x1000);
        serial_printf(SERIAL_COM1, "[LAPIC] mapped base %x -> %x\n",
                      g_acpi.lapic_base, (uint64_t)lapic);
    }
    /* Enable LAPIC via the spurious interrupt vector register (bit 8). */
    lapic_write(LAPIC_SVR, 0x1FF);
    lapic_write(LAPIC_TMR_DIV, 0x3);   /* divide by 16 */
    lapic_write(LAPIC_LVT_TMR, LVT_MASKED);
}

uint32_t lapic_id(void) {
    if (!lapic) return 0;
    return lapic_read(LAPIC_ID) >> 24;
}

void lapic_eoi(void) {
    if (lapic) lapic_write(LAPIC_EOI, 0);
}

static void icr_wait(void) {
    while (lapic_read(LAPIC_ICRLO) & ICR_DELIV_BUSY)
        ;
}

void lapic_send_init(uint8_t apic_id) {
    lapic_write(LAPIC_ICRHI, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICRLO, ICR_INIT | ICR_LEVEL | ICR_ASSERT);
    icr_wait();
    pit_udelay(200);
    /* INIT deassert */
    lapic_write(LAPIC_ICRHI, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICRLO, ICR_INIT | ICR_LEVEL);
    icr_wait();
    pit_udelay(10000);
}

void lapic_send_startup(uint8_t apic_id, uint8_t vector) {
    lapic_write(LAPIC_ICRHI, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICRLO, ICR_STARTUP | (uint32_t)vector);
    icr_wait();
    pit_udelay(200);
}

void lapic_send_ipi(uint8_t apic_id, uint8_t vector) {
    lapic_write(LAPIC_ICRHI, (uint32_t)apic_id << 24);
    lapic_write(LAPIC_ICRLO, (uint32_t)vector | ICR_ASSERT);
    icr_wait();
}

void lapic_timer_init(uint8_t vector, uint32_t initial_count) {
    lapic_write(LAPIC_TMR_DIV, 0x3);   /* divide by 16 */
    lapic_write(LAPIC_LVT_TMR, (uint32_t)vector | LVT_TMR_PERIODIC);
    lapic_write(LAPIC_TMR_ICR, initial_count);
}
