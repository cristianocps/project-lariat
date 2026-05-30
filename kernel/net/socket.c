/* BSD socket layer: presents udp_pcb/tcp_pcb as file descriptors. */

#include "socket.h"
#include "net.h"
#include "netpcb.h"
#include "kapi.h"
#include "errno.h"
#include <string.h>

struct socket {
    int          domain;
    int          type;        /* SOCK_STREAM / SOCK_DGRAM */
    int          proto;
    int          listening;
    udp_pcb_t   *udp;
    tcp_pcb_t   *tcp;
    struct vfs_inode inode;   /* embedded; f_ops -> socket_fops */
};

/* ---- vfs_file <-> socket plumbing ---------------------------------------- */
static struct vfs_file_ops socket_fops;

struct socket *socket_from_file(struct vfs_file *file) {
    if (!file || !file->inode) return NULL;
    if (file->inode->f_ops != &socket_fops) return NULL;
    return (struct socket *)file->inode->private_data;
}

static ssize_t socket_file_read(struct vfs_file *file, void *buf, size_t count) {
    struct socket *s = socket_from_file(file);
    if (!s) return -1;
    long r = socket_recvfrom(s, buf, count, 0, NULL, NULL);
    return (ssize_t)r;
}

static ssize_t socket_file_write(struct vfs_file *file, const void *buf, size_t count) {
    struct socket *s = socket_from_file(file);
    if (!s) return -1;
    long r = socket_sendto(s, buf, count, 0, NULL, 0);
    return (ssize_t)r;
}

static int socket_file_close(struct vfs_file *file) {
    struct socket *s = socket_from_file(file);
    if (!s) return 0;
    if (s->tcp) tcp_close(s->tcp);
    if (s->udp) udp_close(s->udp);
    kfree(s);
    return 0;
}

static short socket_file_poll(struct vfs_file *file, short events) {
    struct socket *s = socket_from_file(file);
    if (!s) return POLLNVAL;
    short r = 0;
    if (s->type == SOCK_DGRAM) {
        if ((events & POLLIN) && s->udp->rx_head) r |= POLLIN;
        if (events & POLLOUT) r |= POLLOUT;
    } else {
        tcp_pcb_t *t = s->tcp;
        if (s->listening) {
            if ((events & POLLIN) && t->accept_head) r |= POLLIN;
            return r;
        }
        if (t->reset) r |= POLLERR;
        if (t->rcv_len > 0) r |= POLLIN;
        if (t->state == TCP_CLOSE_WAIT || t->state == TCP_CLOSED) r |= POLLHUP;
        if ((events & POLLOUT) && t->state == TCP_ESTABLISHED &&
            t->snd_len < TCP_SNDBUF)
            r |= POLLOUT;
        r &= (events | POLLERR | POLLHUP);
    }
    return r;
}

static struct vfs_file_ops socket_fops = {
    .read  = socket_file_read,
    .write = socket_file_write,
    .close = socket_file_close,
    .lseek = NULL,
    .poll  = socket_file_poll,
};

/* Build a vfs_file that wraps an existing socket object. */
static struct vfs_file *socket_wrap_file(struct socket *s) {
    struct vfs_file *f = kzalloc(sizeof(struct vfs_file));
    if (!f) return NULL;
    s->inode.mode = S_IFSOCK | 0666;
    s->inode.f_ops = &socket_fops;
    s->inode.private_data = s;
    f->inode = &s->inode;
    f->pos = 0;
    f->flags = O_RDWR;
    f->ref_count = 1;
    return f;
}

struct vfs_file *socket_create(int domain, int type, int proto, int *err) {
    if (domain != AF_INET) { if (err) *err = -EAFNOSUPPORT; return NULL; }
    /* type may carry SOCK_NONBLOCK/SOCK_CLOEXEC in high bits (Linux); mask. */
    int t = type & 0xff;
    if (t != SOCK_STREAM && t != SOCK_DGRAM) {
        if (err) *err = -EINVAL;
        return NULL;
    }
    struct socket *s = kzalloc(sizeof(struct socket));
    if (!s) { if (err) *err = -ENOMEM; return NULL; }
    s->domain = domain; s->type = t; s->proto = proto;
    if (t == SOCK_STREAM) {
        s->tcp = tcp_new();
        if (!s->tcp) { kfree(s); if (err) *err = -ENOMEM; return NULL; }
    } else {
        s->udp = udp_new();
        if (!s->udp) { kfree(s); if (err) *err = -ENOMEM; return NULL; }
    }
    struct vfs_file *f = socket_wrap_file(s);
    if (!f) {
        if (s->tcp) tcp_close(s->tcp);
        if (s->udp) udp_close(s->udp);
        kfree(s);
        if (err) *err = -ENOMEM;
        return NULL;
    }
    if (err) *err = 0;
    return f;
}

/* ---- sockaddr helpers ---------------------------------------------------- */
static int sa_get(const struct sockaddr *addr, socklen_t len,
                  uint32_t *ip, uint16_t *port) {
    if (!addr || len < sizeof(struct sockaddr_in)) return -EINVAL;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    if (sin->sin_family != AF_INET) return -EAFNOSUPPORT;
    *ip = sin->sin_addr.s_addr;        /* network order */
    *port = ntohs(sin->sin_port);      /* host order */
    return 0;
}

static void sa_put(struct sockaddr *addr, socklen_t *len,
                   uint32_t ip, uint16_t port) {
    if (!addr || !len) return;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = ip;
    socklen_t n = *len < sizeof(sin) ? *len : sizeof(sin);
    memcpy(addr, &sin, n);
    *len = sizeof(sin);
}

/* ---- operations ---------------------------------------------------------- */
int socket_bind(struct socket *s, const struct sockaddr *addr, socklen_t len) {
    uint32_t ip; uint16_t port;
    int r = sa_get(addr, len, &ip, &port);
    if (r < 0) return r;
    if (s->type == SOCK_DGRAM) return udp_bind(s->udp, ip, port);
    return tcp_bind(s->tcp, ip, port);
}

int socket_connect(struct socket *s, const struct sockaddr *addr, socklen_t len) {
    uint32_t ip; uint16_t port;
    int r = sa_get(addr, len, &ip, &port);
    if (r < 0) return r;
    if (s->type == SOCK_DGRAM) return udp_connect(s->udp, ip, port);
    r = tcp_connect(s->tcp, ip, port);
    if (r == -EINTR) return -EINTR;
    return r < 0 ? -ECONNREFUSED : 0;
}

int socket_listen(struct socket *s, int backlog) {
    (void)backlog;
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    s->listening = 1;
    return tcp_listen(s->tcp);
}

int socket_accept(struct socket *s, struct sockaddr *addr, socklen_t *len,
                  struct vfs_file **out) {
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    tcp_pcb_t *child = tcp_accept(s->tcp);
    if (!child) return -EINTR;   /* interrupted by a signal */
    struct socket *cs = kzalloc(sizeof(struct socket));
    if (!cs) { tcp_close(child); return -ENOMEM; }
    cs->domain = s->domain; cs->type = SOCK_STREAM; cs->tcp = child;
    struct vfs_file *f = socket_wrap_file(cs);
    if (!f) { tcp_close(child); kfree(cs); return -ENOMEM; }
    if (addr && len) sa_put(addr, len, child->remote_ip, child->remote_port);
    *out = f;
    return 0;
}

long socket_sendto(struct socket *s, const void *buf, size_t len, int flags,
                   const struct sockaddr *addr, socklen_t alen) {
    (void)flags;
    if (s->type == SOCK_DGRAM) {
        uint32_t ip; uint16_t port;
        if (addr) {
            int r = sa_get(addr, alen, &ip, &port);
            if (r < 0) return r;
        } else {
            if (!s->udp->remote_port) return -EDESTADDRREQ;
            ip = s->udp->remote_ip; port = s->udp->remote_port;
        }
        int r = udp_sendto(s->udp, ip, port, buf, (uint16_t)len);
        return r < 0 ? -EIO : (long)len;
    }
    int r = tcp_send(s->tcp, buf, (uint32_t)len);
    return r < 0 ? -EPIPE : (long)r;
}

long socket_recvfrom(struct socket *s, void *buf, size_t len, int flags,
                     struct sockaddr *addr, socklen_t *alen) {
    (void)flags;
    if (s->type == SOCK_DGRAM) {
        int intr;
        WAIT_EVENT_INTR(s->udp->recvq, s->udp->rx_head != NULL, intr);
        if (intr && !s->udp->rx_head) return -EINTR;
        uint32_t sip; uint16_t sport;
        int n = udp_recv(s->udp, buf, (uint16_t)len, &sip, &sport);
        if (n < 0) return -EAGAIN;
        if (addr && alen) sa_put(addr, alen, sip, sport);
        return n;
    }
    int n = tcp_recv(s->tcp, buf, (uint32_t)len);
    if (n == -EINTR) return -EINTR;
    if (n < 0) return -ECONNRESET;
    if (addr && alen) sa_put(addr, alen, s->tcp->remote_ip, s->tcp->remote_port);
    return n;
}

int socket_getsockname(struct socket *s, struct sockaddr *addr, socklen_t *len) {
    uint32_t ip; uint16_t port;
    if (s->type == SOCK_DGRAM) { ip = s->udp->local_ip; port = s->udp->local_port; }
    else { ip = s->tcp->local_ip; port = s->tcp->local_port; }
    sa_put(addr, len, ip, port);
    return 0;
}
