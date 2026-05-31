#ifndef _LARIAT_LIPC_H
#define _LARIAT_LIPC_H

/* --------------------------------------------------------------------------
 * Lariat IPC ports - userspace API (Phase M).
 *
 * Named, kernel-resident datagram message ports with a bootstrap name registry.
 * A server registers a well-known name and receives; clients open the name and
 * send.  Small control messages travel inline (up to LIPC_MSG_MAX); bulk data
 * (e.g. window framebuffers) is shared via mmap(MAP_SHARED) negotiated over a
 * port.  See docs/adr/0007-ipc-ports-and-service-manager.md.
 * -------------------------------------------------------------------------- */

#define LIPC_NAME_MAX   32
#define LIPC_MSG_MAX    4096

/* Create/register a port with a well-known name. Returns a port id (>=0) or a
 * negative errno (-EEXIST if the name is taken). */
int port_create(const char *name);

/* Open an existing named port. Returns a port id or negative errno (-ENOENT). */
int port_open(const char *name);

/* Send len bytes (<= LIPC_MSG_MAX) to a port. Returns bytes sent or -errno. */
long port_send(int port, const void *buf, unsigned long len);

/* Receive one datagram from a port into buf (blocks unless nonblock). Returns
 * bytes received or -errno (-EAGAIN if nonblock and empty). */
long port_recv(int port, void *buf, unsigned long max, int nonblock);

#endif
