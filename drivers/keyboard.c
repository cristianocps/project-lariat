#include "keyboard.h"
#include "ports.h"
#include "idt.h"
#include "pic.h"

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_COMMAND_PORT 0x64

static volatile uint8_t keyboard_buffer[KB_BUF_SIZE];
static volatile uint16_t kb_read_idx = 0;
static volatile uint16_t kb_write_idx = 0;

static const char scancode_to_ascii[] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8',
    '9', '0', '-', '=', '\b',
    '\t',
    'q', 'w', 'e', 'r',
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`', 0,
    '\\', 'z', 'x', 'c', 'v', 'b', 'n',
    'm', ',', '.', '/', 0,
    '*',
    0,
    ' ',
};

static void keyboard_callback(registers_t *r) {
    (void)r;
    uint8_t scancode = inb(PS2_DATA_PORT);

    if (scancode & 0x80) {
        return;
    }

    uint16_t next = (kb_write_idx + 1) % KB_BUF_SIZE;
    if (next != kb_read_idx) {
        keyboard_buffer[kb_write_idx] = scancode;
        kb_write_idx = next;
    }
}

void keyboard_init(void) {
    register_interrupt_handler(33, keyboard_callback);
    pic_clear_mask(1);
}

int keyboard_poll(void) {
    return kb_read_idx != kb_write_idx;
}

int keyboard_getc(void) {
    __asm__ __volatile__("sti");
    while (kb_read_idx == kb_write_idx) {
        /* Also poll serial port (COM1) for input */
        if (inb(0x3FD) & 0x01) {
            return inb(0x3F8);
        }
        __asm__ __volatile__("pause");
    }
    uint8_t scancode = keyboard_buffer[kb_read_idx];
    kb_read_idx = (kb_read_idx + 1) % KB_BUF_SIZE;

    if (scancode < sizeof(scancode_to_ascii)) {
        return scancode_to_ascii[scancode];
    }
    return 0;
}
