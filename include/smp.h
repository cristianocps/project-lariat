#ifndef SMP_H
#define SMP_H

#include <stdint.h>
#include "acpi.h"

struct thread;

/* Per-CPU data block.  On application processors IA32_GS_BASE points here and
 * `self` (offset 0) lets us recover the pointer with a single gs:0 load.
 *
 * Note: the scheduler does NOT rely on GS to find this block (the SYSCALL/SYSRET
 * path clears GS on every return to ring 3).  Instead it identifies the running
 * CPU by its LAPIC id via smp_cpu_index(), which works in any GS state.  `self`
 * is kept only for the few GS-based helpers. */
struct percpu {
    struct percpu    *self;        /* offset 0: pointer to this struct */
    uint32_t          cpu_index;
    uint32_t          lapic_id;
    volatile uint64_t ticks;       /* incremented by the LAPIC timer */
    volatile int      online;
    int               is_bsp;      /* 1 for the bootstrap processor */
    uint64_t          stack_top;

    /* Scheduler state owned by this CPU. */
    struct thread    *current;     /* thread currently running on this CPU */
    struct thread    *idle;        /* this CPU's idle thread */
};

extern struct percpu g_cpus[MAX_CPUS];
extern volatile uint32_t g_cpus_online;

/* Vectors used by the APIC/IPI layer. */
#define VEC_LAPIC_TIMER  240
#define VEC_TLB_SHOOTDOWN 253
#define VEC_SPURIOUS     255

/* Bring up all application processors reported by ACPI. */
void smp_init(void);

/* Number of CPUs that successfully came online (including the BSP). */
uint32_t smp_cpu_count(void);

/* Index of the CPU executing this call (requires IA32_GS_BASE set up). */
uint32_t smp_this_cpu(void);

/* GS-free CPU identification: derive the executing CPU's index (and per-CPU
 * block) from its LAPIC id.  Safe in any GS state, including the SYSCALL path
 * and exception handlers, so the scheduler uses these everywhere. */
uint32_t smp_cpu_index(void);
struct percpu *smp_this_percpu(void);

/* --- SMP work queue: a tiny, lock-protected task scheduler that runs jobs in
 * parallel across every online core (BSP idle + APs). --- */
typedef void (*smp_work_fn)(void *arg);
int  smp_enqueue_work(smp_work_fn fn, void *arg);
void smp_run_pending_work(void);   /* called by idle loops */

/* Broadcast a TLB shootdown IPI to all other CPUs. */
void smp_tlb_shootdown(void);

#endif /* SMP_H */
