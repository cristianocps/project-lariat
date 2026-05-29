#include "block.h"
#include "serial.h"
#include "kapi.h"

static spinlock_t block_lock = SPINLOCK_INIT;

device_t *block_find(const char *name) {
    return device_find(name);
}

device_t *block_get_first(void) {
    return device_find_by_class(DEV_CLASS_BLOCK);
}

int block_register(device_t *dev) {
    if (!dev || dev->class != DEV_CLASS_BLOCK) return -1;
    device_register(dev);
    serial_printf(SERIAL_COM1, "[BLOCK] registered: %s\n", dev->name);
    return 0;
}

void block_unregister(device_t *dev) {
    if (!dev) return;
    device_unregister(dev);
}

void block_init(void) {
    block_lock.locked = 0;
}
