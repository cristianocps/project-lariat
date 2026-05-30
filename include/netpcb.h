#ifndef NETPCB_H
#define NETPCB_H

/* Kernel-internal protocol control blocks for UDP and TCP.  The socket layer
 * (socket.c) drives these; they block callers via scheduler wait queues. */

#include <stdint.h>
#include "net.h"
#include "sched.h"
#include "timer.h"

/* --------------------------------------------------------------------------
 * UDP
 * -------------------------------------------------------------------------- */
typedef struct udp_pcb {
    uint32_t        local_ip;     /* network order, 0 = any */
    uint16_t        local_port;   /* host order */
    uint32_t        remote_ip;
    uint16_t        remote_port;
    int             bound;

    pbuf_t         *rx_head, *rx_tail;   /* received datagrams */
    wait_queue_t    recvq;

    struct udp_pcb *next;
} udp_pcb_t;

udp_pcb_t *udp_new(void);
int   udp_bind(udp_pcb_t *pcb, uint32_t ip, uint16_t port);   /* port host order */
int   udp_connect(udp_pcb_t *pcb, uint32_t ip, uint16_t port);
int   udp_sendto(udp_pcb_t *pcb, uint32_t ip, uint16_t port,
                 const void *data, uint16_t len);
/* Non-blocking dequeue; returns bytes copied or -1 if empty.  Fills *src_ip /
 * *src_port (network/host order). */
int   udp_recv(udp_pcb_t *pcb, void *buf, uint16_t len,
               uint32_t *src_ip, uint16_t *src_port);
void  udp_close(udp_pcb_t *pcb);

/* --------------------------------------------------------------------------
 * TCP
 * -------------------------------------------------------------------------- */
enum tcp_state {
    TCP_CLOSED = 0, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RCVD,
    TCP_ESTABLISHED, TCP_FIN_WAIT_1, TCP_FIN_WAIT_2, TCP_CLOSE_WAIT,
    TCP_CLOSING, TCP_LAST_ACK, TCP_TIME_WAIT
};

#define TCP_SNDBUF 16384
#define TCP_RCVBUF 16384
#define TCP_MSS    1400

typedef struct tcp_pcb {
    enum tcp_state  state;
    uint32_t        local_ip, remote_ip;
    uint16_t        local_port, remote_port;

    uint32_t        snd_una;     /* oldest unacked seq */
    uint32_t        snd_nxt;     /* next seq to send */
    uint32_t        rcv_nxt;     /* next expected seq */
    uint16_t        snd_wnd;     /* peer's advertised window */

    /* Send buffer: bytes queued by the app, not yet acked. */
    uint8_t         snd_buf[TCP_SNDBUF];
    uint32_t        snd_len;     /* bytes in snd_buf */

    /* Receive buffer: in-order bytes for the app. */
    uint8_t         rcv_buf[TCP_RCVBUF];
    uint32_t        rcv_len;

    struct ktimer   rto;         /* retransmit timer */
    int             retries;
    int             fin_sent;
    int             reset;       /* connection was reset/errored */

    /* Listen / accept. */
    struct tcp_pcb *accept_head, *accept_tail;  /* completed conns (on listener) */
    struct tcp_pcb *accept_link;                /* link in parent's accept queue */
    struct tcp_pcb *parent;

    wait_queue_t    wq;          /* connect / accept / recv / send-space */
    struct tcp_pcb *next;        /* global pcb list */
} tcp_pcb_t;

tcp_pcb_t *tcp_new(void);
int   tcp_bind(tcp_pcb_t *pcb, uint32_t ip, uint16_t port);
int   tcp_listen(tcp_pcb_t *pcb);
int   tcp_connect(tcp_pcb_t *pcb, uint32_t ip, uint16_t port);   /* blocks */
tcp_pcb_t *tcp_accept(tcp_pcb_t *pcb);                           /* blocks */
int   tcp_send(tcp_pcb_t *pcb, const void *data, uint32_t len);  /* blocks for space */
int   tcp_recv(tcp_pcb_t *pcb, void *buf, uint32_t len);         /* blocks for data */
void  tcp_close(tcp_pcb_t *pcb);

#endif /* NETPCB_H */
