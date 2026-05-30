#include "gdt.h"
#include "pmm.h"
#include "mm.h"
#include "acpi.h"
#include "smp.h"
#include "serial.h"
#include <string.h>

/* GDT entry (8 bytes) */
struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

/* TSS descriptor in GDT needs 16 bytes (2 entries) in long mode */
struct gdt_tss_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* Fixed descriptors occupy slots 0-5 (null, kcode, kdata, ucode32, udata,
 * ucode64).  Each CPU's 64-bit TSS descriptor is 16 bytes (2 slots) and starts
 * at slot TSS_SLOT_BASE, so CPU i uses slots TSS_SLOT_BASE + 2*i. */
#define TSS_SLOT_BASE 6
#define GDT_ENTRIES   (TSS_SLOT_BASE + 2 * MAX_CPUS)

#define TSS_SELECTOR(cpu) (((TSS_SLOT_BASE) + 2 * (cpu)) * 8)

static struct gdt_entry gdt[GDT_ENTRIES];
static struct tss cpu_tss[MAX_CPUS] __attribute__((aligned(16)));
static struct gdt_ptr gp;

extern void gdt_flush(uint64_t gdt_ptr);
extern void tss_flush(uint16_t sel);

static void gdt_set_gate(int num, uint64_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low    = (base & 0xFFFF);
    gdt[num].base_mid    = (base >> 16) & 0xFF;
    gdt[num].base_high   = (base >> 24) & 0xFF;
    gdt[num].limit_low   = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[num].access      = access;
}

static void gdt_set_tss(int num, uint64_t base, uint32_t limit) {
    struct gdt_tss_entry *e = (struct gdt_tss_entry *)&gdt[num];
    e->limit_low    = limit & 0xFFFF;
    e->base_low     = base & 0xFFFF;
    e->base_mid     = (base >> 16) & 0xFF;
    e->access       = 0x89; /* Available 64-bit TSS, present, DPL=0 */
    e->granularity  = 0x00;
    e->base_high    = (base >> 24) & 0xFF;
    e->base_upper   = (base >> 32) & 0xFFFFFFFF;
    e->reserved     = 0;
}

void gdt_init(void) {
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (uint64_t)&gdt;

    /* Null descriptor */
    gdt_set_gate(0, 0, 0, 0, 0);

    /* Kernel code (64-bit, execute/read, DPL=0) */
    gdt_set_gate(1, 0, 0xFFFFF, 0x9A, 0xA0);

    /* Kernel data (read/write, DPL=0) */
    gdt_set_gate(2, 0, 0xFFFFF, 0x92, 0xA0);

    /* User code 32-bit compat (execute/read, DPL=3) */
    gdt_set_gate(3, 0, 0xFFFFF, 0xFA, 0xA0);

    /* User data (read/write, DPL=3) */
    gdt_set_gate(4, 0, 0xFFFFF, 0xF2, 0xA0);

    /* User code 64-bit (execute/read, DPL=3) */
    gdt_set_gate(5, 0, 0xFFFFF, 0xFA, 0xA0);

    /* Per-CPU TSS descriptors (filled in by tss_init_cpu). */
    for (int i = TSS_SLOT_BASE; i < GDT_ENTRIES; i++) {
        memset(&gdt[i], 0, sizeof(gdt[i]));
    }

    gdt_flush((uint64_t)&gp);

    serial_print(SERIAL_COM1, "[GDT] GDT loaded with ring-0/3 segments\n");
}

/* Install and load this CPU's own TSS (descriptor lives in the shared GDT). */
void tss_init_cpu(uint32_t cpu) {
    if (cpu >= MAX_CPUS) return;

    struct tss *t = &cpu_tss[cpu];
    memset(t, 0, sizeof(*t));
    t->iopb_offset = sizeof(struct tss);

    gdt_set_tss(TSS_SLOT_BASE + 2 * cpu, (uint64_t)t, sizeof(struct tss) - 1);

    /* The GDT limit already covers every CPU's descriptor, so the descriptor is
     * visible as soon as it is written; just load this CPU's TR. */
    tss_flush(TSS_SELECTOR(cpu));
}

/* BSP convenience wrapper. */
void tss_init(void) {
    tss_init_cpu(0);
    serial_print(SERIAL_COM1, "[TSS] per-CPU TSS table ready (BSP loaded)\n");
}

/* Load the shared GDT on an application processor (segments reload to the
 * kernel selectors).  Note: this resets GS base to 0, so set IA32_GS_BASE
 * afterwards if the AP uses GS-relative per-CPU data. */
void gdt_load_ap(void) {
    gdt_flush((uint64_t)&gp);
}

/* Set the ring-0 stack in the executing CPU's TSS.  The scheduler calls this on
 * every context switch; each core has its own TSS (loaded by tss_init_cpu), so
 * a ring-3 thread that traps (syscall/IRQ/exception) lands on the right kernel
 * stack no matter which CPU it runs on. */
void tss_set_rsp0(uint64_t rsp) {
    uint32_t cpu = smp_cpu_index();
    if (cpu >= MAX_CPUS) cpu = 0;
    cpu_tss[cpu].rsp0 = rsp;
}

struct tss *tss_get_current(void) {
    uint32_t cpu = smp_cpu_index();
    if (cpu >= MAX_CPUS) cpu = 0;
    return &cpu_tss[cpu];
}
