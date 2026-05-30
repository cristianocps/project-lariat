#ifndef LAPIC_H
#define LAPIC_H

#include <stdint.h>

void lapic_init(void);          /* enable LAPIC on the current CPU */
uint32_t lapic_id(void);        /* current CPU's LAPIC id */
void lapic_eoi(void);           /* end-of-interrupt */

/* Inter-processor interrupts */
void lapic_send_init(uint8_t apic_id);
void lapic_send_startup(uint8_t apic_id, uint8_t vector);
void lapic_send_ipi(uint8_t apic_id, uint8_t vector);

/* Per-CPU LAPIC timer (periodic), vector + initial count. */
void lapic_timer_init(uint8_t vector, uint32_t initial_count);

#endif /* LAPIC_H */
