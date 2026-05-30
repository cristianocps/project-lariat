/* Unified input event queue exposed as /dev/input.
 *
 * Both the PS/2 keyboard IRQ and the PS/2 mouse IRQ push struct input_event
 * records here; the userspace compositor reads (and poll()s) the device to get
 * a single ordered stream of key presses and mouse motion/buttons. */

#include "gfx.h"
#include "vfs.h"
#include "kapi.h"
#include "sched.h"
#include "uapi.h"
#include "errno.h"
#include <string.h>

#define INPUT_QLEN 256

static struct input_event ev_ring[INPUT_QLEN];
static volatile uint16_t ev_read, ev_write;
static spinlock_t ev_lock = SPINLOCK_INIT;
static wait_queue_t ev_waitq = WAIT_QUEUE_INIT;

void input_push(uint32_t type, uint32_t code, int32_t value) {
    uint64_t f = spin_lock_irqsave(&ev_lock);
    uint16_t next = (uint16_t)((ev_write + 1) % INPUT_QLEN);
    if (next != ev_read) {
        ev_ring[ev_write].type = type;
        ev_ring[ev_write].code = code;
        ev_ring[ev_write].value = value;
        ev_write = next;
    }
    spin_unlock_irqrestore(&ev_lock, f);
    wq_wake_all(&ev_waitq);
}

static int input_empty(void) { return ev_read == ev_write; }

static ssize_t input_read(struct vfs_file *file, void *buf, size_t count) {
    (void)file;
    if (count < sizeof(struct input_event)) return -EINVAL;

    int intr = 0;
    WAIT_EVENT_INTR(ev_waitq, !input_empty(), intr);
    if (intr) return -EINTR;

    struct input_event *out = (struct input_event *)buf;
    size_t max = count / sizeof(struct input_event);
    size_t n = 0;

    uint64_t f = spin_lock_irqsave(&ev_lock);
    while (n < max && ev_read != ev_write) {
        out[n++] = ev_ring[ev_read];
        ev_read = (uint16_t)((ev_read + 1) % INPUT_QLEN);
    }
    spin_unlock_irqrestore(&ev_lock, f);
    return (ssize_t)(n * sizeof(struct input_event));
}

static short input_poll(struct vfs_file *file, short events) {
    (void)file;
    short r = 0;
    if ((events & POLLIN) && !input_empty()) r |= POLLIN;
    return r;
}

static int input_close(struct vfs_file *file) { (void)file; return 0; }

static struct vfs_file_ops input_fops = {
    .read  = input_read,
    .poll  = input_poll,
    .close = input_close,
};

static struct vfs_inode input_inode;

void input_init(void) {
    ev_read = ev_write = 0;
    memset(&input_inode, 0, sizeof(input_inode));
    input_inode.mode = S_IFCHR | 0666;
    input_inode.f_ops = &input_fops;
    vfs_devfs_register("input", &input_inode);
}
