#ifndef IPC_H
#define IPC_H

#include <stdint.h>
#include <stddef.h>

/* --------------------------------------------------------------------------
 * Phase M: first-class IPC "ports" (Mach-port-like message ports).
 *
 * A port is a named, kernel-resident message queue with a bootstrap name
 * registry: a server registers a well-known name, clients look it up and send
 * datagram messages.  Receiving blocks until a message arrives.  This is the
 * capability/IPC substrate the hybrid-kernel service layer is built on (see
 * docs/adr/0007).  Bulk transfer (e.g. window pixel buffers) uses MAP_SHARED
 * memory negotiated over a port; ports themselves carry small control messages.
 * -------------------------------------------------------------------------- */

#define IPC_NAME_MAX   32
#define IPC_MSG_MAX    4096   /* max single message payload */
#define IPC_MAX_PORTS  128

void ipc_init(void);

/* Register (create) a port with a well-known name; returns its id or <0.
 * Fails with -EEXIST if the name is taken. */
int ipc_port_register(const char *name, uint32_t owner_tid);

/* Look up an existing port by name; returns its id or <0. */
int ipc_port_lookup(const char *name);

/* Send `len` bytes to port `id` (non-blocking, datagram). */
long ipc_port_send(int id, const void *buf, size_t len, uint32_t sender_tid);

/* Receive one message from port `id` into buf (blocks if empty unless
 * non-blocking). Returns bytes copied or <0. */
long ipc_port_recv(int id, void *buf, size_t max, int nonblock);

/* Release a reference / close a port handle for a thread. */
void ipc_port_close(int id);

#endif
