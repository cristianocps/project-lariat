#ifndef GFX_H
#define GFX_H

#include <stdint.h>

/* M11 GUI subsystems. */

/* Bochs/QEMU stdvga linear framebuffer; registers /dev/fb0.  Returns 0 on
 * success, negative if no compatible display adapter was found. */
int  bochs_vbe_init(void);

/* Unified input event queue backing /dev/input.  Called once at boot. */
void input_init(void);
/* Push one event onto the queue from an IRQ handler (keyboard/mouse). */
void input_push(uint32_t type, uint32_t code, int32_t value);

/* PS/2 mouse driver (IRQ12).  Requires input_init() first. */
void mouse_init(void);

#endif /* GFX_H */
