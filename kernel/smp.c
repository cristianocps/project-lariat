#include "smp.h"
#include "acpi.h"
#include "lapic.h"
#include "idt.h"
#include "gdt.h"
#include "kapi.h"
#include "mm.h"
#include "msr.h"
#include "serial.h"
#include "timer.h"
#include "vmm.h"
#include "sched.h"
#include "syscall.h"
#include <string.h>

#define AP_TRAMPOLINE_PHYS 0x8000
#define AP_PARAMS_PHYS     0x9000
#define IA32_GS_BASE       0xC0000100

/* Embedded AP trampoline blob (see boot/ap_trampoline.asm). */
extern uint8_t _ap_trampoline_start[];
extern uint8_t _ap_trampoline_end[];

struct percpu g_cpus[MAX_CPUS];
volatile uint32_t g_cpus_online = 1;   /* BSP counts immediately */

/* AP kernel stacks live in BSS (identity-mapped low memory), so they are
 * reachable by the AP while it still runs on the boot PML4 and before it has
 * installed an IDT. */
#define AP_STACK_SIZE 8192
static uint8_t ap_stacks[MAX_CPUS][AP_STACK_SIZE] __attribute__((aligned(16)));

/* Parameter block shared with the trampoline (one AP brought up at a time). */
struct ap_params {
    uint32_t pml4;       /* 0x00 */
    uint32_t _pad;       /* 0x04 */
    uint64_t stack;      /* 0x08 */
    uint64_t entry;      /* 0x10 */
    uint32_t cpu_arg;    /* 0x18 */
    volatile uint32_t online; /* 0x1C */
} __attribute__((packed));

static inline struct percpu *this_cpu(void) {
    struct percpu *p;
    __asm__ __volatile__("movq %%gs:0, %0" : "=r"(p));
    return p;
}

uint32_t smp_this_cpu(void) {
    return this_cpu()->cpu_index;
}

/* GS-free: match the executing CPU by its LAPIC id.  Used by the scheduler and
 * syscall path, where GS may have been cleared by SYSRET. */
uint32_t smp_cpu_index(void) {
    uint32_t id = lapic_id();
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (g_cpus[i].online && g_cpus[i].lapic_id == id)
            return i;
    }
    return (uint32_t)g_acpi.bsp_index;
}

struct percpu *smp_this_percpu(void) {
    return &g_cpus[smp_cpu_index()];
}

/* --------------------------------------------------------------------------
 * SMP work queue
 * -------------------------------------------------------------------------- */
struct work_item {
    smp_work_fn fn;
    void       *arg;
};

#define WORK_QUEUE_CAP 256
static struct work_item work_queue[WORK_QUEUE_CAP];
static volatile uint32_t work_head = 0;   /* next to run */
static volatile uint32_t work_tail = 0;   /* next free slot */
static spinlock_t work_lock = SPINLOCK_INIT;

int smp_enqueue_work(smp_work_fn fn, void *arg) {
    uint64_t flags = spin_lock_irqsave(&work_lock);
    if ((work_tail + 1) % WORK_QUEUE_CAP == work_head % WORK_QUEUE_CAP) {
        spin_unlock_irqrestore(&work_lock, flags);
        return -1;   /* full */
    }
    work_queue[work_tail % WORK_QUEUE_CAP].fn = fn;
    work_queue[work_tail % WORK_QUEUE_CAP].arg = arg;
    work_tail++;
    spin_unlock_irqrestore(&work_lock, flags);
    return 0;
}

void smp_run_pending_work(void) {
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&work_lock);
        if (work_head == work_tail) {
            spin_unlock_irqrestore(&work_lock, flags);
            return;
        }
        struct work_item item = work_queue[work_head % WORK_QUEUE_CAP];
        work_head++;
        spin_unlock_irqrestore(&work_lock, flags);
        item.fn(item.arg);
    }
}

/* --------------------------------------------------------------------------
 * Interrupt handlers (per-CPU LAPIC timer, TLB shootdown, spurious)
 * -------------------------------------------------------------------------- */
static void lapic_timer_handler(registers_t *r) {
    /* Identify the CPU by its LAPIC id rather than GS, so this handler is safe
     * regardless of GS state at interrupt time. */
    uint32_t id = lapic_id();
    struct percpu *pc = NULL;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (g_cpus[i].online && g_cpus[i].lapic_id == id) {
            g_cpus[i].ticks++;
            pc = &g_cpus[i];
            break;
        }
    }

    /* EOI before any context switch (scheduler_tick may not return here). */
    lapic_eoi();

    /* The BSP owns the global wall-clock tick (drives timer_sleep + sleeping
     * thread wakeups).  Every CPU then runs the preemptive scheduler against
     * the shared ready queue, so application processors schedule ring-3 threads
     * just like the BSP. */
    if (pc && pc->is_bsp) {
        timer_lapic_tick(r);
    }
    scheduler_tick(r);
}

static void tlb_shootdown_handler(registers_t *r) {
    (void)r;
    /* Reload CR3 to flush the entire TLB (simple, correct). */
    vmm_switch_pagetable(vmm_get_cr3());
    lapic_eoi();
}

static void spurious_handler(registers_t *r) {
    (void)r;
    lapic_eoi();
}

/* --------------------------------------------------------------------------
 * Application processor entry (called from the trampoline in long mode)
 * -------------------------------------------------------------------------- */
void ap_main(uint32_t cpu_index) {
    /* All shared trampoline parameters (stack, entry, cpu_arg) have now been
     * consumed, so it is safe to let the BSP reuse the parameter block for the
     * next AP.  Signal online immediately. */
    *(volatile uint32_t *)(uintptr_t)(AP_PARAMS_PHYS + 0x1C) = 1;

    /* Switch off the trampoline's temporary GDT/IDT onto the shared ones, and
     * load this CPU's own TSS (its descriptor lives in the shared GDT). */
    gdt_load_ap();
    idt_load_ap();
    tss_init_cpu(cpu_index);

    /* Per-CPU data via GS base.  Safe here because APs never execute the
     * SYSCALL/SYSRET path that zeroes GS on the BSP. */
    struct percpu *pc = &g_cpus[cpu_index];
    pc->self = pc;
    wrmsr(IA32_GS_BASE, (uint64_t)pc);

    lapic_init();
    pc->lapic_id = lapic_id();
    pc->is_bsp = 0;
    pc->online = 1;

    serial_printf(SERIAL_COM1, "[SMP] CPU %d online (lapic id %d)\n",
                  cpu_index, pc->lapic_id);

    __sync_fetch_and_add(&g_cpus_online, 1);

    /* Enable SYSCALL/SYSRET on this core (the MSRs are per-CPU), so ring-3
     * threads scheduled here can trap into the kernel. */
    syscall_init_cpu();

    /* Claim this CPU's idle thread; from now on the scheduler treats the AP as
     * a first-class CPU that runs ring-3 threads pulled off the shared ready
     * queue. */
    sched_init_cpu();

    /* Arm this CPU's local APIC timer and unmask interrupts.  The timer handler
     * is GS-independent (identifies the CPU by LAPIC id) and runs the
     * preemptive scheduler on every core. */
    lapic_timer_init(VEC_LAPIC_TIMER, 10000000);
    __asm__ __volatile__("sti");

    /* Idle loop: drain any kernel work items, then halt until the next timer
     * tick preempts us into a ready thread. */
    for (;;) {
        smp_run_pending_work();
        __asm__ __volatile__("sti; hlt");
    }
}

/* --------------------------------------------------------------------------
 * Bringup
 * -------------------------------------------------------------------------- */
static void install_trampoline(void) {
    size_t len = (size_t)(_ap_trampoline_end - _ap_trampoline_start);
    /* 0x8000 lives in the bootloader's low identity map (virtual == physical),
     * which is exactly where the AP must find it in real mode. */
    memcpy((void *)(uintptr_t)AP_TRAMPOLINE_PHYS, _ap_trampoline_start, len);
}

void smp_init(void) {
    /* Register APIC/IPI interrupt handlers (shared IDT). */
    register_interrupt_handler(VEC_LAPIC_TIMER, lapic_timer_handler);
    register_interrupt_handler(VEC_TLB_SHOOTDOWN, tlb_shootdown_handler);
    register_interrupt_handler(VEC_SPURIOUS, spurious_handler);

    /* The BSP also gets a per-CPU block (index 0).  We intentionally do NOT load
     * a GS base for the BSP: its SYSCALL return path clears GS (mov gs,ax).  The
     * scheduler therefore never relies on GS - it identifies the running CPU via
     * smp_cpu_index() (LAPIC id), which works in every GS state - so the BSP and
     * APs share the exact same scheduling path. */
    memset(g_cpus, 0, sizeof(g_cpus));
    g_cpus[0].cpu_index = 0;
    g_cpus[0].lapic_id = lapic_id();
    g_cpus[0].online = 1;
    g_cpus[0].is_bsp = 1;
    g_cpus[0].self = &g_cpus[0];

    if (g_acpi.cpu_count <= 1) {
        serial_print(SERIAL_COM1, "[SMP] Single CPU; no APs to start\n");
        return;
    }

    install_trampoline();

    struct ap_params *params = (struct ap_params *)(uintptr_t)AP_PARAMS_PHYS;
    uint8_t bsp_id = (uint8_t)lapic_id();

    for (uint32_t i = 0; i < g_acpi.cpu_count; i++) {
        uint8_t apic_id = g_acpi.lapic_ids[i];
        if (apic_id == bsp_id) continue;   /* skip the BSP */
        if (i >= MAX_CPUS) break;

        g_cpus[i].cpu_index = i;
        g_cpus[i].stack_top = (uint64_t)&ap_stacks[i][AP_STACK_SIZE];

        params->pml4    = (uint32_t)vmm_get_cr3();
        params->stack   = g_cpus[i].stack_top;
        params->entry   = (uint64_t)ap_main;
        params->cpu_arg = i;
        params->online  = 0;

        serial_printf(SERIAL_COM1, "[SMP] cr3=%x stack=%x entry=%x\n",
                      vmm_get_cr3(), params->stack, params->entry);

        serial_printf(SERIAL_COM1, "[SMP] starting AP apic_id=%d (idx %d)...\n",
                      apic_id, i);

        lapic_send_init(apic_id);
        lapic_send_startup(apic_id, AP_TRAMPOLINE_PHYS >> 12);
        lapic_send_startup(apic_id, AP_TRAMPOLINE_PHYS >> 12);

        /* Wait for the AP to consume the shared params and signal online before
         * reusing the parameter block for the next AP. */
        long timeout = 200000000;
        while (!params->online && timeout-- > 0) {
            __asm__ __volatile__("pause");
        }
        if (!params->online) {
            serial_printf(SERIAL_COM1, "[SMP] AP idx %d failed to start\n", i);
        }
    }

    serial_printf(SERIAL_COM1, "[SMP] %d CPU(s) online\n", g_cpus_online);
}

uint32_t smp_cpu_count(void) {
    return g_cpus_online;
}

void smp_tlb_shootdown(void) {
    /* Nothing to do until other CPUs are online (and the LAPIC is up). */
    if (g_cpus_online <= 1) return;

    uint8_t bsp_id = (uint8_t)lapic_id();
    for (uint32_t i = 0; i < g_acpi.cpu_count; i++) {
        uint8_t apic_id = g_acpi.lapic_ids[i];
        if (apic_id == bsp_id) continue;
        if (g_cpus[i].online)
            lapic_send_ipi(apic_id, VEC_TLB_SHOOTDOWN);
    }
}
