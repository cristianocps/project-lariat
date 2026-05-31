#ifndef SMPTEST_H
#define SMPTEST_H

/*
 * smptest - SMP bring-up self-tests.
 *
 * Exercise the cross-core work dispatch and the PMM under concurrent load at
 * boot.  Both are single-core safe (the demo is skipped and the stress falls
 * back to BSP-only work when just one CPU is online).
 */

/* Dispatch a batch of jobs across all online cores and log where each ran. */
void smp_demo(void);

/* Hammer the PMM concurrently from every core and verify the free-page count
 * returns to its starting value (catches lost pages / pmm_lock corruption). */
void smp_stress(void);

#endif /* SMPTEST_H */
