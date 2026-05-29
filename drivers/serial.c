#include "serial.h"
#include "ports.h"
#include <stdarg.h>

void serial_init(uint16_t port) {
    outb(port + 1, 0x00);    // Disable all interrupts
    outb(port + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(port + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
    outb(port + 1, 0x00);    //                  (hi byte)
    outb(port + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(port + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(port + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

int serial_received(uint16_t port) {
    return inb(port + 5) & 1;
}

char serial_getc(uint16_t port) {
    while (serial_received(port) == 0);
    return inb(port);
}

int serial_transmit_empty(uint16_t port) {
    return inb(port + 5) & 0x20;
}

void serial_putc(uint16_t port, char c) {
    while (serial_transmit_empty(port) == 0);
    outb(port, c);
}

void serial_print(uint16_t port, const char *str) {
    while (*str) {
        serial_putc(port, *str++);
    }
}

static void serial_print_dec(uint16_t port, int64_t value) {
    if (value < 0) {
        serial_putc(port, '-');
        value = -value;
    }
    if (value == 0) {
        serial_putc(port, '0');
        return;
    }
    char buf[32];
    int i = 0;
    while (value > 0) {
        buf[i++] = '0' + (value % 10);
        value /= 10;
    }
    while (i > 0) {
        serial_putc(port, buf[--i]);
    }
}

static void serial_print_hex(uint16_t port, uint64_t value, int digits) {
    const char *hex = "0123456789ABCDEF";
    int started = 0;
    for (int i = digits - 1; i >= 0; i--) {
        char c = hex[(value >> (i * 4)) & 0xF];
        if (c != '0') started = 1;
        if (started || i == 0) {
            serial_putc(port, c);
        }
    }
}

void serial_vprintf(uint16_t port, const char *fmt, va_list args) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            serial_putc(port, *p);
            continue;
        }
        p++;

        /* Skip flags and width/precision modifiers */
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '0' || *p == '#') p++;
        while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') {
            p++;
            while (*p >= '0' && *p <= '9') p++;
        }

        switch (*p) {
            case 'd':
                serial_print_dec(port, va_arg(args, int));
                break;
            case 'l':
                p++;
                if (*p == 'd') {
                    serial_print_dec(port, va_arg(args, long));
                } else if (*p == 'x') {
                    serial_print_hex(port, va_arg(args, unsigned long), 16);
                } else if (*p == 'l' && *(p+1) == 'x') {
                    p++;
                    serial_print_hex(port, va_arg(args, unsigned long long), 16);
                }
                break;
            case 'x':
                serial_print_hex(port, va_arg(args, unsigned int), 8);
                break;
            case 'p':
                serial_print(port, "0x");
                serial_print_hex(port, va_arg(args, uintptr_t), 16);
                break;
            case 's':
                serial_print(port, va_arg(args, char *));
                break;
            case 'c':
                serial_putc(port, va_arg(args, int));
                break;
            case '%':
                serial_putc(port, '%');
                break;
            default:
                serial_putc(port, '%');
                serial_putc(port, *p);
                break;
        }
    }
}

void serial_printf(uint16_t port, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    serial_vprintf(port, fmt, args);
    va_end(args);
}
