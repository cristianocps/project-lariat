#include "kapi.h"
#include "smp.h"
#include "lapic.h"
#include "pmm.h"
#include "mm.h"
#include "serial.h"
#include "smptest.h"

/* --------------------------------------------------------------------------
 * SMP demo: dispatch a batch of jobs across all online cores and log where
 * each one ran.  Single-core safe (skipped when only the BSP is up).
 * -------------------------------------------------------------------------- */
static spinlock_t smp_demo_lock = SPINLOCK_INIT;

static void smp_demo_job(void *arg) {
    int id = (int)(long)arg;
    uint64_t flags = spin_lock_irqsave(&smp_demo_lock);
    serial_printf(SERIAL_COM1, "[SMP] job %d ran on lapic id %d\n",
                  id, (int)lapic_id());
    spin_unlock_irqrestore(&smp_demo_lock, flags);
}

void smp_demo(void) {
    if (smp_cpu_count() <= 1) {
        serial_print(SERIAL_COM1, "[SMP] demo skipped (1 CPU)\n");
        return;
    }
    serial_printf(SERIAL_COM1, "[SMP] dispatching 24 jobs across %d CPUs\n",
                  smp_cpu_count());
    for (int i = 0; i < 24; i++)
        smp_enqueue_work(smp_demo_job, (void *)(long)i);
    /* Give the APs a chance to grab their share before the BSP drains the rest,
     * so the work is visibly distributed across cores. */
    for (volatile int spin = 0; spin < 2000000; spin++)
        __asm__ __volatile__("pause");
    smp_run_pending_work();
}

/* --------------------------------------------------------------------------
 * SMP stress: hammer the PMM concurrently from every core and verify that the
 * free-page count returns to its starting value (catches lost pages and
 * pmm_lock corruption).  Each allocation is touched through the direct map,
 * which also validates physmap access from the application processors.
 * -------------------------------------------------------------------------- */
#define PMM_STRESS_ITERS 4000
static volatile int pmm_stress_remaining;

static void pmm_stress_job(void *arg) {
    (void)arg;
    for (int i = 0; i < PMM_STRESS_ITERS; i++) {
        uint64_t p = pmm_alloc_page();
        if (p) {
            *(volatile uint64_t *)phys_to_virt(p) = p ^ 0xA5A5A5A5ULL;
            pmm_free_page(p);
        }
    }
    __sync_fetch_and_sub(&pmm_stress_remaining, 1);
}

void smp_stress(void) {
    uint64_t before = pmm_get_free_count();
    int jobs = (int)smp_cpu_count() * 4;
    if (jobs < 4) jobs = 4;

    pmm_stress_remaining = jobs;
    for (int i = 0; i < jobs; i++)
        smp_enqueue_work(pmm_stress_job, 0);
    /* BSP joins in, then waits for every job to finish. */
    while (pmm_stress_remaining > 0) {
        smp_run_pending_work();
        __asm__ __volatile__("pause");
    }

    uint64_t after = pmm_get_free_count();
    serial_printf(SERIAL_COM1,
        "[STRESS] pmm %d jobs x %d alloc/free across %d core(s): "
        "free before=%d after=%d %s\n",
        jobs, PMM_STRESS_ITERS, (int)smp_cpu_count(),
        (int)before, (int)after,
        before == after ? "OK" : "*** LEAK/CORRUPTION ***");
}
