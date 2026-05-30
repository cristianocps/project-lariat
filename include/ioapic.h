#ifndef IOAPIC_H
#define IOAPIC_H

#include <stdint.h>

void ioapic_init(void);

/* Route a global system interrupt (gsi) to (apic_id, vector).  Edge-triggered,
 * active-high (suitable for ISA IRQs). */
void ioapic_route(uint8_t gsi, uint8_t vector, uint8_t apic_id);

/* Route with explicit trigger mode / polarity.  PCI INTx lines are
 * level-triggered, active-low (level=1, active_low=1). */
void ioapic_route_ex(uint8_t gsi, uint8_t vector, uint8_t apic_id,
                     int level, int active_low);

/* Mask/unmask a gsi. */
void ioapic_mask(uint8_t gsi, int masked);

#endif /* IOAPIC_H */
