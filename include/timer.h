#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include "idt.h"

/* The system tick runs at 100 Hz (10 ms per tick). */
#define TIMER_HZ 100

void timer_init(uint32_t frequency);
void timer_lapic_tick(registers_t *r);
uint64_t timer_get_ticks(void);
void timer_sleep(uint64_t ms);

/* --------------------------------------------------------------------------
 * Wall-clock + monotonic time
 * -------------------------------------------------------------------------- */
/* Seconds since the Unix epoch, captured from the RTC at boot + ticks since. */
uint64_t clock_realtime_ns(void);
uint64_t clock_monotonic_ns(void);
/* Read the RTC (CMOS) once at boot to seed wall-clock time. */
void clock_init(void);

/* --------------------------------------------------------------------------
 * Kernel timers: lightweight one-shot callbacks fired from the BSP tick.
 * Callbacks run in interrupt context (keep them short - e.g. wake a waitq).
 * -------------------------------------------------------------------------- */
struct ktimer {
    uint64_t        expires;     /* absolute tick when it fires */
    void          (*fn)(void *); /* callback */
    void           *arg;
    struct ktimer  *next;
    int             active;
};

void ktimer_add(struct ktimer *t, uint64_t delay_ticks, void (*fn)(void *), void *arg);
void ktimer_cancel(struct ktimer *t);
/* Run any expired timers; called from the BSP tick. */
void ktimer_run(void);

#endif
