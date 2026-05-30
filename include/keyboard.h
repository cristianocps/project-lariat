#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stddef.h>
#include "uapi.h"

#define KB_BUF_SIZE 256
#define TTY_BUF_SIZE 256

void keyboard_init(void);
void keyboard_register_serial(void);
int keyboard_getc(void);
int keyboard_poll(void);

/* TTY buffer: ASCII chars produced by keyboard IRQ */
int tty_getc(void);
/* Like tty_getc but returns -1 if the calling thread gets a pending,
 * unblocked signal while waiting (so a blocking read can report -EINTR). */
int tty_getc_intr(void);
int tty_poll(void);
int tty_read(char *buf, size_t count);

/* Line discipline / job control state (the kernel TTY). */
void tty_termios_get(struct termios *t);
void tty_termios_set(const struct termios *t);
int  tty_get_fg_pgrp(void);
void tty_set_fg_pgrp(int pgrp);

#endif
