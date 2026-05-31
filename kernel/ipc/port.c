#include "ipc.h"
#include "sched.h"
#include "kapi.h"
#include "errno.h"
#include "serial.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * IPC message ports (Phase M).  See include/ipc.h and docs/adr/0007.
 *
 * A fixed table of named ports, each a singly-linked datagram queue with a
 * receive wait queue.  The global scheduler lock serializes the table and the
 * queues so that ipc_port_recv() can block via sched_wait_locked() (the same
 * pattern futex uses).  Messages are heap-allocated; the receiver takes
 * ownership and frees them.
 * -------------------------------------------------------------------------- */

struct ipc_msg {
    struct ipc_msg *next;
    size_t          len;
    uint32_t        sender;
    uint8_t         data[];
};

struct ipc_port {
    int             used;
    char            name[IPC_NAME_MAX];
    uint32_t        owner;
    struct ipc_msg *head, *tail;
    int             count;
    wait_queue_t    recvq;
};

static struct ipc_port ports[IPC_MAX_PORTS];

void ipc_init(void) {
    for (int i = 0; i < IPC_MAX_PORTS; i++) {
        ports[i].used = 0;
        ports[i].head = ports[i].tail = NULL;
        ports[i].count = 0;
        ports[i].recvq = (wait_queue_t)WAIT_QUEUE_INIT;
    }
    serial_print(SERIAL_COM1, "[IPC] message ports initialized\n");
}

int ipc_port_register(const char *name, uint32_t owner_tid) {
    if (!name || !name[0]) return -EINVAL;
    uint64_t f = sched_lock_acquire();
    for (int i = 0; i < IPC_MAX_PORTS; i++)
        if (ports[i].used && strcmp(ports[i].name, name) == 0) {
            sched_lock_release(f);
            return -EEXIST;
        }
    for (int i = 0; i < IPC_MAX_PORTS; i++) {
        if (!ports[i].used) {
            ports[i].used = 1;
            ports[i].owner = owner_tid;
            ports[i].head = ports[i].tail = NULL;
            ports[i].count = 0;
            ports[i].recvq = (wait_queue_t)WAIT_QUEUE_INIT;
            size_t j = 0;
            for (; name[j] && j < IPC_NAME_MAX - 1; j++) ports[i].name[j] = name[j];
            ports[i].name[j] = '\0';
            sched_lock_release(f);
            return i;
        }
    }
    sched_lock_release(f);
    return -ENOSPC;
}

int ipc_port_lookup(const char *name) {
    if (!name) return -EINVAL;
    uint64_t f = sched_lock_acquire();
    for (int i = 0; i < IPC_MAX_PORTS; i++)
        if (ports[i].used && strcmp(ports[i].name, name) == 0) {
            sched_lock_release(f);
            return i;
        }
    sched_lock_release(f);
    return -ENOENT;
}

long ipc_port_send(int id, const void *buf, size_t len, uint32_t sender_tid) {
    if (id < 0 || id >= IPC_MAX_PORTS) return -EINVAL;
    if (len > IPC_MSG_MAX) return -E2BIG;

    /* Allocate the message outside the scheduler lock. */
    struct ipc_msg *m = kmalloc(sizeof(struct ipc_msg) + len);
    if (!m) return -ENOMEM;
    m->next = NULL;
    m->len = len;
    m->sender = sender_tid;
    if (len) memcpy(m->data, buf, len);

    uint64_t f = sched_lock_acquire();
    if (!ports[id].used) {
        sched_lock_release(f);
        kfree(m);
        return -EINVAL;
    }
    if (ports[id].tail) ports[id].tail->next = m;
    else ports[id].head = m;
    ports[id].tail = m;
    ports[id].count++;
    /* Release before waking: sched_wq_wake_n() takes sched_lock itself, and the
     * message is already queued so a not-yet-blocked receiver still sees it. */
    sched_lock_release(f);
    sched_wq_wake_n(&ports[id].recvq, 1);
    return (long)len;
}

long ipc_port_recv(int id, void *buf, size_t max, int nonblock) {
    if (id < 0 || id >= IPC_MAX_PORTS) return -EINVAL;

    uint64_t f = sched_lock_acquire();
    if (!ports[id].used) { sched_lock_release(f); return -EINVAL; }

    while (!ports[id].head) {
        if (nonblock) { sched_lock_release(f); return -EAGAIN; }
        if (sched_signal_pending()) { sched_lock_release(f); return -EINTR; }
        /* Releases the lock, blocks, returns unlocked; re-acquire to re-check. */
        sched_wait_locked(&ports[id].recvq, f);
        f = sched_lock_acquire();
        if (!ports[id].used) { sched_lock_release(f); return -EINVAL; }
    }

    struct ipc_msg *m = ports[id].head;
    ports[id].head = m->next;
    if (!ports[id].head) ports[id].tail = NULL;
    ports[id].count--;
    sched_lock_release(f);

    size_t n = m->len < max ? m->len : max;
    if (n && buf) memcpy(buf, m->data, n);
    kfree(m);
    return (long)n;
}

void ipc_port_close(int id) {
    if (id < 0 || id >= IPC_MAX_PORTS) return;
    uint64_t f = sched_lock_acquire();
    if (ports[id].used) {
        struct ipc_msg *m = ports[id].head;
        while (m) { struct ipc_msg *n = m->next; kfree(m); m = n; }
        ports[id].head = ports[id].tail = NULL;
        ports[id].count = 0;
        ports[id].used = 0;
    }
    sched_lock_release(f);
}
