#include "timer.h"
#include "ports.h"
#include "idt.h"
#include "pic.h"
#include "sched.h"
#include "serial.h"
#include "kapi.h"

static volatile uint64_t tick_count = 0;

/* Wall-clock seconds at boot (from the RTC); monotonic time is tick-derived. */
static uint64_t boot_unix_seconds = 0;

static void timer_callback(registers_t *r) {
    tick_count++;
    scheduler_tick(r);
}

void timer_init(uint32_t frequency) {
    register_interrupt_handler(32, timer_callback);

    uint32_t divisor = 1193182 / frequency;

    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

/* BSP wall-clock tick, driven by the BSP's local APIC timer once the PIC/PIT is
 * disabled.  Only the global tick_count is advanced here (it backs timer_sleep
 * and sleeping-thread wakeups); the per-CPU preemptive context switch is run by
 * the LAPIC timer handler on every core via scheduler_tick(). */
void timer_lapic_tick(registers_t *r) {
    (void)r;
    tick_count++;
    ktimer_run();
}

uint64_t timer_get_ticks(void) {
    return tick_count;
}

void timer_sleep(uint64_t ms) {
    uint64_t end = tick_count + ms;
    while (tick_count < end) {
        __asm__ __volatile__("hlt");
    }
}

/* --------------------------------------------------------------------------
 * RTC (CMOS) wall clock
 * -------------------------------------------------------------------------- */
static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static int cmos_update_in_progress(void) {
    outb(0x70, 0x0A);
    return inb(0x71) & 0x80;
}

static uint32_t bcd_to_bin(uint8_t v) {
    return (v & 0x0F) + ((v >> 4) * 10);
}

/* Days from the civil calendar to a day number (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void clock_init(void) {
    while (cmos_update_in_progress()) {}
    uint8_t sec = cmos_read(0x00);
    uint8_t min = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t day = cmos_read(0x07);
    uint8_t mon = cmos_read(0x08);
    uint8_t year = cmos_read(0x09);
    uint8_t status_b = cmos_read(0x0B);

    if (!(status_b & 0x04)) {   /* values are BCD: convert */
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        hour = bcd_to_bin(hour);
        day = bcd_to_bin(day);
        mon = bcd_to_bin(mon);
        year = bcd_to_bin(year);
    }

    int64_t full_year = 2000 + year;   /* assume 21st century */
    int64_t days = days_from_civil(full_year, mon, day);
    boot_unix_seconds = (uint64_t)(days * 86400) +
                        hour * 3600 + min * 60 + sec;
    serial_printf(SERIAL_COM1, "[CLOCK] RTC boot time = %ld unix seconds\n",
                  (long)boot_unix_seconds);
}

uint64_t clock_monotonic_ns(void) {
    return tick_count * (1000000000ULL / TIMER_HZ);
}

uint64_t clock_realtime_ns(void) {
    return boot_unix_seconds * 1000000000ULL + clock_monotonic_ns();
}

/* --------------------------------------------------------------------------
 * Kernel timer wheel (simple unsorted list, scanned each BSP tick)
 * -------------------------------------------------------------------------- */
static struct ktimer *ktimer_list = NULL;
static spinlock_t ktimer_lock = SPINLOCK_INIT;

void ktimer_add(struct ktimer *t, uint64_t delay_ticks, void (*fn)(void *), void *arg) {
    uint64_t flags = spin_lock_irqsave(&ktimer_lock);
    if (!t->active) {
        t->fn = fn;
        t->arg = arg;
        t->expires = tick_count + (delay_ticks ? delay_ticks : 1);
        t->active = 1;
        t->next = ktimer_list;
        ktimer_list = t;
    } else {
        /* Already armed: just reschedule. */
        t->fn = fn;
        t->arg = arg;
        t->expires = tick_count + (delay_ticks ? delay_ticks : 1);
    }
    spin_unlock_irqrestore(&ktimer_lock, flags);
}

void ktimer_cancel(struct ktimer *t) {
    uint64_t flags = spin_lock_irqsave(&ktimer_lock);
    if (t->active) {
        struct ktimer **pp = &ktimer_list;
        while (*pp) {
            if (*pp == t) { *pp = t->next; break; }
            pp = &(*pp)->next;
        }
        t->active = 0;
        t->next = NULL;
    }
    spin_unlock_irqrestore(&ktimer_lock, flags);
}

void ktimer_run(void) {
    /* Collect expired timers under the lock, then fire callbacks after
     * releasing it (callbacks may re-arm timers or take other locks). */
    struct ktimer *fired[16];
    int n = 0;
    uint64_t flags = spin_lock_irqsave(&ktimer_lock);
    struct ktimer **pp = &ktimer_list;
    while (*pp && n < 16) {
        struct ktimer *t = *pp;
        if (t->expires <= tick_count) {
            *pp = t->next;
            t->active = 0;
            t->next = NULL;
            fired[n++] = t;
        } else {
            pp = &(*pp)->next;
        }
    }
    spin_unlock_irqrestore(&ktimer_lock, flags);

    for (int i = 0; i < n; i++) {
        if (fired[i]->fn) fired[i]->fn(fired[i]->arg);
    }
}
