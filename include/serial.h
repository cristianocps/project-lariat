#ifndef SERIAL_H
#define SERIAL_H

#include <stdint.h>

#define SERIAL_COM1 0x3F8
#define SERIAL_COM2 0x2F8

#include <stdarg.h>

void serial_init(uint16_t port);
void serial_enable_rx_interrupt(uint16_t port);
void serial_putc(uint16_t port, char c);
void serial_print(uint16_t port, const char *str);
void serial_vprintf(uint16_t port, const char *fmt, va_list args);

/* Acquire/release the shared serial output lock so a caller can emit a
 * multi-character message atomically (used by the console device). */
uint64_t serial_lock_acquire(void);
void     serial_lock_release(uint64_t flags);
void serial_printf(uint16_t port, const char *fmt, ...);
int serial_received(uint16_t port);
char serial_getc(uint16_t port);

#endif
