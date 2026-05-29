#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>

#define KB_BUF_SIZE 256

void keyboard_init(void);
int keyboard_getc(void);
int keyboard_poll(void);

#endif
