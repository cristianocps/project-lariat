/* PS/2 mouse driver (IRQ12).  Decodes the standard 3-byte movement packet and
 * forwards motion / button changes to the unified /dev/input queue. */

#include "gfx.h"
#include "ports.h"
#include "idt.h"
#include "pic.h"
#include "uapi.h"
#include "serial.h"
#include <stdint.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

static void ps2_wait_input(void) {   /* wait until controller can accept a byte */
    for (int i = 0; i < 100000; i++)
        if ((inb(PS2_STATUS) & 0x02) == 0) return;
}
static void ps2_wait_output(void) {  /* wait until a byte is available to read */
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & 0x01) return;
}

static void mouse_write(uint8_t val) {
    ps2_wait_input();
    outb(PS2_CMD, 0xD4);          /* next byte goes to the auxiliary device */
    ps2_wait_input();
    outb(PS2_DATA, val);
}

static uint8_t mouse_read(void) {
    ps2_wait_output();
    return inb(PS2_DATA);
}

static uint8_t pkt[3];
static int     pkt_idx;
static uint8_t btn_state;

static void mouse_callback(registers_t *r) {
    (void)r;
    uint8_t status = inb(PS2_STATUS);
    /* Only consume bytes that came from the auxiliary (mouse) device. */
    if (!(status & 0x01) || !(status & 0x20)) return;

    uint8_t byte = inb(PS2_DATA);
    switch (pkt_idx) {
    case 0:
        /* Byte 0 must have bit3 set; resync otherwise. */
        if (!(byte & 0x08)) return;
        pkt[0] = byte;
        pkt_idx = 1;
        break;
    case 1:
        pkt[1] = byte;
        pkt_idx = 2;
        break;
    case 2: {
        pkt[2] = byte;
        pkt_idx = 0;

        uint8_t flags = pkt[0];
        int dx = (int)pkt[1] - ((flags & 0x10) ? 256 : 0);
        int dy = (int)pkt[2] - ((flags & 0x20) ? 256 : 0);

        if (dx) input_push(EV_REL, REL_X, dx);
        if (dy) input_push(EV_REL, REL_Y, -dy);  /* screen Y grows downward */

        uint8_t now = flags & 0x07;
        uint8_t changed = now ^ btn_state;
        if (changed & 0x01) input_push(EV_KEY, BTN_LEFT,   (now & 0x01) ? 1 : 0);
        if (changed & 0x02) input_push(EV_KEY, BTN_RIGHT,  (now & 0x02) ? 1 : 0);
        if (changed & 0x04) input_push(EV_KEY, BTN_MIDDLE, (now & 0x04) ? 1 : 0);
        btn_state = now;
        break;
    }
    }
}

void mouse_init(void) {
    /* Enable the auxiliary (mouse) device. */
    ps2_wait_input();
    outb(PS2_CMD, 0xA8);

    /* Enable IRQ12 in the controller config byte (bit 1). */
    ps2_wait_input();
    outb(PS2_CMD, 0x20);
    uint8_t cfg = mouse_read();
    cfg |= 0x02;          /* enable mouse interrupt */
    cfg &= ~0x20;         /* enable mouse clock */
    ps2_wait_input();
    outb(PS2_CMD, 0x60);
    ps2_wait_input();
    outb(PS2_DATA, cfg);

    /* Default settings + enable data reporting. */
    mouse_write(0xF6); (void)mouse_read();   /* ACK */
    mouse_write(0xF4); (void)mouse_read();   /* ACK */

    pkt_idx = 0;
    btn_state = 0;
    register_interrupt_handler(44, mouse_callback);

    serial_print(SERIAL_COM1, "[MOUSE] PS/2 mouse enabled (IRQ12)\n");
}
