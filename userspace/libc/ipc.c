#include "lipc.h"
#include "syscall.h"
#include "unistd.h"

/* Thin userspace wrappers over the Lariat IPC-port syscalls (Phase M). */

int port_create(const char *name) {
    return (int)syscall1(SYS_LARIAT_PORT_CREATE, (long)name);
}

int port_open(const char *name) {
    return (int)syscall1(SYS_LARIAT_PORT_OPEN, (long)name);
}

long port_send(int port, const void *buf, unsigned long len) {
    return syscall3(SYS_LARIAT_PORT_SEND, (long)port, (long)buf, (long)len);
}

long port_recv(int port, void *buf, unsigned long max, int nonblock) {
    return syscall4(SYS_LARIAT_PORT_RECV, (long)port, (long)buf,
                    (long)max, (long)nonblock);
}
