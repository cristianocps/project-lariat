#ifndef LIBC_INPUT_H
#define LIBC_INPUT_H

#include <stdint.h>

/* Event record read from /dev/input (must match include/uapi.h). */
struct input_event {
    uint32_t type;
    uint32_t code;
    int32_t  value;
};

#define EV_KEY  1
#define EV_REL  2

#define REL_X 0
#define REL_Y 1

#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

#endif /* LIBC_INPUT_H */
