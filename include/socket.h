#ifndef KSOCKET_H
#define KSOCKET_H

/* Kernel BSD-socket layer: wraps udp_pcb/tcp_pcb in a vfs_file so sockets live
 * in the normal fd table and respond to read()/write()/close(). */

#include <stdint.h>
#include "vfs.h"
#include "uapi.h"

struct socket;

/* Allocate a socket and an associated open-file handle.  Returns the file on
 * success (with the socket reachable via file->inode->private_data), NULL on
 * error (*err set to a negative errno). */
struct vfs_file *socket_create(int domain, int type, int proto, int *err);

/* If `file` is a socket, return its socket object, else NULL. */
struct socket *socket_from_file(struct vfs_file *file);

/* Socket operations (return 0/positive on success, negative errno on error). */
int socket_bind(struct socket *s, const struct sockaddr *addr, socklen_t len);
int socket_connect(struct socket *s, const struct sockaddr *addr, socklen_t len);
int socket_listen(struct socket *s, int backlog);
/* On success returns a new vfs_file for the accepted connection via *out. */
int socket_accept(struct socket *s, struct sockaddr *addr, socklen_t *len,
                  struct vfs_file **out);
long socket_sendto(struct socket *s, const void *buf, size_t len, int flags,
                   const struct sockaddr *addr, socklen_t alen);
long socket_recvfrom(struct socket *s, void *buf, size_t len, int flags,
                     struct sockaddr *addr, socklen_t *alen);
int socket_getsockname(struct socket *s, struct sockaddr *addr, socklen_t *len);

#endif /* KSOCKET_H */
