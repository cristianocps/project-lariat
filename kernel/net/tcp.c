#include "netpcb.h"
#include "kapi.h"
#include "timer.h"
#include "serial.h"
#include "errno.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * A pragmatic TCP: connection setup/teardown, in-order data transfer with a
 * single retransmit timer, peer-window flow control.  No congestion control,
 * SACK, or out-of-order reassembly (out-of-order segments are dropped and
 * recovered by retransmission).  Good enough for echo servers and httpget.
 * -------------------------------------------------------------------------- */

#define TCP_RTO_TICKS  (TIMER_HZ / 2)   /* 500 ms retransmit timeout */
#define TCP_MAX_RETRIES 6

static tcp_pcb_t *tcp_list = NULL;
static spinlock_t tcp_lock = SPINLOCK_INIT;
static uint16_t   tcp_ephemeral = 49152;
static uint32_t   tcp_iss = 0x10000;

void tcp_init(void) { tcp_list = NULL; }

static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static int seq_gt(uint32_t a, uint32_t b) { return (int32_t)(a - b) > 0; }

/* Invariant once ESTABLISHED: snd_buf[0] corresponds to sequence snd_una,
 * because acked data is memmove'd out of snd_buf in lockstep with snd_una. */
static void tcp_send_seg(tcp_pcb_t *pcb, uint8_t flags, uint32_t seq,
                         const void *data, uint32_t datalen) {
    pbuf_t *p = pbuf_alloc();
    if (!p) return;
    if (data && datalen) memcpy(pbuf_data(p), data, datalen);
    p->len = (uint16_t)datalen;

    struct tcp_hdr *th = (struct tcp_hdr *)pbuf_push(p, sizeof(struct tcp_hdr));
    th->src = htons(pcb->local_port);
    th->dst = htons(pcb->remote_port);
    th->seq = htonl(seq);
    th->ack = htonl(pcb->rcv_nxt);
    th->off = (5 << 4);
    th->flags = flags;
    uint32_t rcv_space = TCP_RCVBUF - pcb->rcv_len;
    th->win = htons(rcv_space > 0xFFFF ? 0xFFFF : rcv_space);
    th->urg = 0;
    th->csum = 0;

    netif_t *nif = netif_route(pcb->remote_ip);
    uint32_t src_ip = nif ? nif->ip : 0;
    uint8_t pseudo[12];
    memcpy(pseudo, &src_ip, 4);
    memcpy(pseudo + 4, &pcb->remote_ip, 4);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_TCP;
    uint16_t seglen = sizeof(struct tcp_hdr) + datalen;
    pseudo[10] = seglen >> 8;
    pseudo[11] = seglen & 0xFF;
    uint32_t sum = inet_sum(pseudo, 12, 0);
    sum = inet_sum(th, seglen, sum);
    th->csum = inet_fold(sum);

    ipv4_output(pcb->remote_ip, IP_PROTO_TCP, p);
}

/* base sequence of snd_buf[0]: stored in pcb->snd_una at the moment data
 * transfer begins.  We add a field through the rto.arg trick? No - add a real
 * field. */

/* Transmit any unsent data within the peer's window. */
static void tcp_output_data(tcp_pcb_t *pcb, uint32_t base) {
    uint32_t outstanding = pcb->snd_nxt - pcb->snd_una;
    uint32_t offset = pcb->snd_nxt - base;     /* index of next unsent byte */
    while (offset < pcb->snd_len) {
        uint32_t wnd = pcb->snd_wnd ? pcb->snd_wnd : 1;
        if (outstanding >= wnd) break;
        uint32_t can = wnd - outstanding;
        uint32_t chunk = pcb->snd_len - offset;
        if (chunk > TCP_MSS) chunk = TCP_MSS;
        if (chunk > can) chunk = can;
        if (chunk == 0) break;
        tcp_send_seg(pcb, TCP_ACK | TCP_PSH, pcb->snd_nxt,
                     pcb->snd_buf + offset, chunk);
        pcb->snd_nxt += chunk;
        outstanding += chunk;
        offset += chunk;
    }
}

static void tcp_rto_cb(void *arg);

static void tcp_arm_rto(tcp_pcb_t *pcb) {
    ktimer_add(&pcb->rto, TCP_RTO_TICKS, tcp_rto_cb, pcb);
}

static void tcp_free(tcp_pcb_t *pcb) {
    ktimer_cancel(&pcb->rto);
    tcp_pcb_t **pp = &tcp_list;
    while (*pp) {
        if (*pp == pcb) { *pp = pcb->next; break; }
        pp = &(*pp)->next;
    }
    kfree(pcb);
}

static void tcp_rto_cb(void *arg) {
    tcp_pcb_t *pcb = (tcp_pcb_t *)arg;
    uint64_t f = spin_lock_irqsave(&tcp_lock);

    if (pcb->state == TCP_CLOSED) { spin_unlock_irqrestore(&tcp_lock, f); return; }

    if (++pcb->retries > TCP_MAX_RETRIES) {
        pcb->state = TCP_CLOSED;
        pcb->reset = 1;
        spin_unlock_irqrestore(&tcp_lock, f);
        wq_wake_all(&pcb->wq);
        return;
    }

    /* Retransmit the oldest unacked control/data. */
    if (pcb->state == TCP_SYN_SENT) {
        tcp_send_seg(pcb, TCP_SYN, pcb->snd_una, NULL, 0);
    } else if (pcb->state == TCP_SYN_RCVD) {
        tcp_send_seg(pcb, TCP_SYN | TCP_ACK, pcb->snd_una, NULL, 0);
    } else if (pcb->snd_len > 0 && seq_lt(pcb->snd_una, pcb->snd_nxt)) {
        uint32_t base = pcb->snd_una;   /* snd_buf[0] aligns to snd_una here */
        uint32_t chunk = pcb->snd_len;
        if (chunk > TCP_MSS) chunk = TCP_MSS;
        tcp_send_seg(pcb, TCP_ACK | TCP_PSH, base, pcb->snd_buf, chunk);
    } else if (pcb->fin_sent) {
        tcp_send_seg(pcb, TCP_ACK | TCP_FIN, pcb->snd_nxt - 1, NULL, 0);
    }
    tcp_arm_rto(pcb);
    spin_unlock_irqrestore(&tcp_lock, f);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */
tcp_pcb_t *tcp_new(void) {
    tcp_pcb_t *p = kzalloc(sizeof(tcp_pcb_t));
    if (!p) return NULL;
    p->state = TCP_CLOSED;
    p->snd_wnd = TCP_MSS;
    uint64_t f = spin_lock_irqsave(&tcp_lock);
    p->next = tcp_list;
    tcp_list = p;
    spin_unlock_irqrestore(&tcp_lock, f);
    return p;
}

int tcp_bind(tcp_pcb_t *pcb, uint32_t ip, uint16_t port) {
    if (port == 0) port = tcp_ephemeral++;
    pcb->local_ip = ip;
    pcb->local_port = port;
    return port;
}

int tcp_listen(tcp_pcb_t *pcb) {
    if (pcb->local_port == 0) tcp_bind(pcb, 0, 0);
    pcb->state = TCP_LISTEN;
    return 0;
}

int tcp_connect(tcp_pcb_t *pcb, uint32_t ip, uint16_t port) {
    uint64_t f = spin_lock_irqsave(&tcp_lock);
    if (pcb->local_port == 0) pcb->local_port = tcp_ephemeral++;
    netif_t *nif = netif_route(ip);
    pcb->local_ip = nif ? nif->ip : 0;
    pcb->remote_ip = ip;
    pcb->remote_port = port;
    uint32_t iss = tcp_iss; tcp_iss += 64000;
    pcb->snd_una = iss;
    pcb->snd_nxt = iss + 1;     /* SYN consumes one */
    pcb->state = TCP_SYN_SENT;
    pcb->retries = 0;
    tcp_send_seg(pcb, TCP_SYN, iss, NULL, 0);
    tcp_arm_rto(pcb);
    spin_unlock_irqrestore(&tcp_lock, f);

    int intr;
    WAIT_EVENT_INTR(pcb->wq, pcb->state == TCP_ESTABLISHED || pcb->reset ||
                        pcb->state == TCP_CLOSED, intr);
    if (intr && pcb->state != TCP_ESTABLISHED) return -EINTR;
    return (pcb->state == TCP_ESTABLISHED) ? 0 : -1;
}

tcp_pcb_t *tcp_accept(tcp_pcb_t *pcb) {
    for (;;) {
        int intr;
        WAIT_EVENT_INTR(pcb->wq, pcb->accept_head != NULL, intr);
        if (intr) return NULL;   /* interrupted by a signal */
        uint64_t f = spin_lock_irqsave(&tcp_lock);
        tcp_pcb_t *c = pcb->accept_head;
        if (c) {
            pcb->accept_head = c->accept_link;
            if (!pcb->accept_head) pcb->accept_tail = NULL;
            c->accept_link = NULL;
        }
        spin_unlock_irqrestore(&tcp_lock, f);
        if (c) return c;
    }
}

int tcp_send(tcp_pcb_t *pcb, const void *data, uint32_t len) {
    const uint8_t *src = (const uint8_t *)data;
    uint32_t sent = 0;
    while (sent < len) {
        int intr;
        WAIT_EVENT_INTR(pcb->wq, pcb->snd_len < TCP_SNDBUF || pcb->reset ||
                            pcb->state == TCP_CLOSED, intr);
        if (intr) return sent ? (int)sent : -EINTR;
        if (pcb->reset || pcb->state == TCP_CLOSED) return sent ? (int)sent : -1;

        uint64_t f = spin_lock_irqsave(&tcp_lock);
        uint32_t space = TCP_SNDBUF - pcb->snd_len;
        uint32_t chunk = len - sent;
        if (chunk > space) chunk = space;
        if (chunk) {
            memcpy(pcb->snd_buf + pcb->snd_len, src + sent, chunk);
            pcb->snd_len += chunk;
            sent += chunk;
            tcp_output_data(pcb, pcb->snd_una);
            if (seq_lt(pcb->snd_una, pcb->snd_nxt)) tcp_arm_rto(pcb);
        }
        spin_unlock_irqrestore(&tcp_lock, f);
    }
    return (int)sent;
}

int tcp_recv(tcp_pcb_t *pcb, void *buf, uint32_t len) {
    int intr;
    WAIT_EVENT_INTR(pcb->wq, pcb->rcv_len > 0 || pcb->state == TCP_CLOSE_WAIT ||
                        pcb->state == TCP_CLOSED || pcb->reset, intr);
    if (intr && pcb->rcv_len == 0) return -EINTR;
    uint64_t f = spin_lock_irqsave(&tcp_lock);
    uint32_t n = pcb->rcv_len < len ? pcb->rcv_len : len;
    if (n) {
        memcpy(buf, pcb->rcv_buf, n);
        memmove(pcb->rcv_buf, pcb->rcv_buf + n, pcb->rcv_len - n);
        pcb->rcv_len -= n;
    }
    int eof = (n == 0 && (pcb->state == TCP_CLOSE_WAIT ||
                          pcb->state == TCP_CLOSED));
    spin_unlock_irqrestore(&tcp_lock, f);
    if (n) return (int)n;
    return eof ? 0 : -1;
}

void tcp_close(tcp_pcb_t *pcb) {
    uint64_t f = spin_lock_irqsave(&tcp_lock);
    switch (pcb->state) {
        case TCP_ESTABLISHED:
            tcp_send_seg(pcb, TCP_ACK | TCP_FIN, pcb->snd_nxt, NULL, 0);
            pcb->snd_nxt++;
            pcb->fin_sent = 1;
            pcb->state = TCP_FIN_WAIT_1;
            tcp_arm_rto(pcb);
            break;
        case TCP_CLOSE_WAIT:
            tcp_send_seg(pcb, TCP_ACK | TCP_FIN, pcb->snd_nxt, NULL, 0);
            pcb->snd_nxt++;
            pcb->fin_sent = 1;
            pcb->state = TCP_LAST_ACK;
            tcp_arm_rto(pcb);
            break;
        case TCP_LISTEN:
        case TCP_SYN_SENT:
            pcb->state = TCP_CLOSED;
            tcp_free(pcb);
            spin_unlock_irqrestore(&tcp_lock, f);
            return;
        default:
            break;
    }
    spin_unlock_irqrestore(&tcp_lock, f);
}

/* --------------------------------------------------------------------------
 * Input state machine
 * -------------------------------------------------------------------------- */
static tcp_pcb_t *tcp_find(uint32_t lip, uint16_t lport, uint32_t rip, uint16_t rport) {
    for (tcp_pcb_t *p = tcp_list; p; p = p->next) {
        if (p->state != TCP_LISTEN && p->local_port == lport &&
            p->remote_port == rport && p->remote_ip == rip &&
            (p->local_ip == 0 || p->local_ip == lip))
            return p;
    }
    return NULL;
}

static tcp_pcb_t *tcp_find_listener(uint16_t lport) {
    for (tcp_pcb_t *p = tcp_list; p; p = p->next)
        if (p->state == TCP_LISTEN && p->local_port == lport) return p;
    return NULL;
}

static void tcp_deliver_data(tcp_pcb_t *pcb, const uint8_t *data, uint32_t len) {
    uint32_t space = TCP_RCVBUF - pcb->rcv_len;
    if (len > space) len = space;
    if (len) {
        memcpy(pcb->rcv_buf + pcb->rcv_len, data, len);
        pcb->rcv_len += len;
        pcb->rcv_nxt += len;
    }
}

void tcp_input(netif_t *nif, pbuf_t *p, uint32_t src_ip, uint32_t dst_ip) {
    (void)nif;
    if (p->len < (int)sizeof(struct tcp_hdr)) { pbuf_free(p); return; }
    struct tcp_hdr *th = (struct tcp_hdr *)pbuf_data(p);
    uint16_t sport = ntohs(th->src);
    uint16_t dport = ntohs(th->dst);
    uint32_t seg_seq = ntohl(th->seq);
    uint32_t seg_ack = ntohl(th->ack);
    uint8_t  flags = th->flags;
    uint16_t win = ntohs(th->win);
    uint32_t hdrlen = ((th->off >> 4) & 0xF) * 4;
    if (hdrlen < sizeof(struct tcp_hdr) || hdrlen > p->len) { pbuf_free(p); return; }
    uint8_t *data = pbuf_data(p) + hdrlen;
    uint32_t datalen = p->len - hdrlen;

    uint64_t f = spin_lock_irqsave(&tcp_lock);
    tcp_pcb_t *pcb = tcp_find(dst_ip, dport, src_ip, sport);

    if (!pcb) {
        /* Maybe a SYN to a listening socket. */
        if (flags & TCP_SYN) {
            tcp_pcb_t *lis = tcp_find_listener(dport);
            if (lis) {
                tcp_pcb_t *c = kzalloc(sizeof(tcp_pcb_t));
                if (c) {
                    c->state = TCP_SYN_RCVD;
                    c->local_ip = dst_ip;
                    c->local_port = dport;
                    c->remote_ip = src_ip;
                    c->remote_port = sport;
                    c->rcv_nxt = seg_seq + 1;
                    c->snd_wnd = win ? win : TCP_MSS;
                    uint32_t iss = tcp_iss; tcp_iss += 64000;
                    c->snd_una = iss;
                    c->snd_nxt = iss + 1;
                    c->parent = lis;
                    c->next = tcp_list;
                    tcp_list = c;
                    tcp_send_seg(c, TCP_SYN | TCP_ACK, iss, NULL, 0);
                    tcp_arm_rto(c);
                }
            }
        }
        spin_unlock_irqrestore(&tcp_lock, f);
        pbuf_free(p);
        return;
    }

    if (flags & TCP_RST) {
        pcb->reset = 1;
        pcb->state = TCP_CLOSED;
        ktimer_cancel(&pcb->rto);
        spin_unlock_irqrestore(&tcp_lock, f);
        wq_wake_all(&pcb->wq);
        pbuf_free(p);
        return;
    }

    int wake = 0;

    switch (pcb->state) {
    case TCP_SYN_SENT:
        if ((flags & TCP_SYN) && (flags & TCP_ACK)) {
            if (seg_ack == pcb->snd_nxt) {
                pcb->rcv_nxt = seg_seq + 1;
                pcb->snd_una = seg_ack;
                pcb->snd_wnd = win ? win : TCP_MSS;
                pcb->state = TCP_ESTABLISHED;
                pcb->retries = 0;
                ktimer_cancel(&pcb->rto);
                tcp_send_seg(pcb, TCP_ACK, pcb->snd_nxt, NULL, 0);
                wake = 1;
            }
        }
        break;

    case TCP_SYN_RCVD:
        if (flags & TCP_ACK && seg_ack == pcb->snd_nxt) {
            pcb->snd_una = seg_ack;
            pcb->snd_wnd = win ? win : TCP_MSS;
            pcb->state = TCP_ESTABLISHED;
            pcb->retries = 0;
            ktimer_cancel(&pcb->rto);
            /* Attach to the listener's accept queue. */
            tcp_pcb_t *lis = pcb->parent;
            if (lis) {
                if (lis->accept_tail) lis->accept_tail->accept_link = pcb;
                else lis->accept_head = pcb;
                lis->accept_tail = pcb;
                wq_wake_all(&lis->wq);
            }
        }
        break;

    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_CLOSE_WAIT:
    case TCP_LAST_ACK:
    case TCP_CLOSING:
        /* Process ACK. */
        if (flags & TCP_ACK) {
            if (seq_gt(seg_ack, pcb->snd_una) && seq_le(seg_ack, pcb->snd_nxt)) {
                uint32_t base = pcb->snd_una;
                uint32_t acked = seg_ack - pcb->snd_una;
                /* Remove acked data bytes from snd_buf (clamp control bytes). */
                uint32_t data_acked = acked;
                if (data_acked > pcb->snd_len) data_acked = pcb->snd_len;
                if (data_acked) {
                    memmove(pcb->snd_buf, pcb->snd_buf + data_acked,
                            pcb->snd_len - data_acked);
                    pcb->snd_len -= data_acked;
                }
                pcb->snd_una = seg_ack;
                pcb->retries = 0;
                (void)base;
                if (pcb->snd_una == pcb->snd_nxt) ktimer_cancel(&pcb->rto);
                wake = 1;
            }
            pcb->snd_wnd = win ? win : pcb->snd_wnd;

            /* FIN acked -> advance close states. */
            if (pcb->state == TCP_FIN_WAIT_1 && pcb->snd_una == pcb->snd_nxt)
                pcb->state = TCP_FIN_WAIT_2;
            else if (pcb->state == TCP_LAST_ACK && pcb->snd_una == pcb->snd_nxt) {
                pcb->state = TCP_CLOSED;
                ktimer_cancel(&pcb->rto);
                tcp_free(pcb);
                spin_unlock_irqrestore(&tcp_lock, f);
                pbuf_free(p);
                return;
            } else if (pcb->state == TCP_CLOSING && pcb->snd_una == pcb->snd_nxt) {
                pcb->state = TCP_CLOSED;
                ktimer_cancel(&pcb->rto);
                tcp_free(pcb);
                spin_unlock_irqrestore(&tcp_lock, f);
                pbuf_free(p);
                return;
            }
        }

        /* Process in-order data. */
        if (datalen > 0 && seg_seq == pcb->rcv_nxt &&
            (pcb->state == TCP_ESTABLISHED || pcb->state == TCP_FIN_WAIT_1 ||
             pcb->state == TCP_FIN_WAIT_2)) {
            tcp_deliver_data(pcb, data, datalen);
            tcp_send_seg(pcb, TCP_ACK, pcb->snd_nxt, NULL, 0);
            wake = 1;
        } else if (datalen > 0 && seq_lt(seg_seq, pcb->rcv_nxt + 1)) {
            /* Out of order or already received: ack our current position. */
            tcp_send_seg(pcb, TCP_ACK, pcb->snd_nxt, NULL, 0);
        }

        /* Process FIN. */
        if ((flags & TCP_FIN) && seg_seq + datalen == pcb->rcv_nxt) {
            pcb->rcv_nxt++;
            tcp_send_seg(pcb, TCP_ACK, pcb->snd_nxt, NULL, 0);
            if (pcb->state == TCP_ESTABLISHED)
                pcb->state = TCP_CLOSE_WAIT;
            else if (pcb->state == TCP_FIN_WAIT_2) {
                pcb->state = TCP_CLOSED;
                ktimer_cancel(&pcb->rto);
                wake = 1;
            } else if (pcb->state == TCP_FIN_WAIT_1)
                pcb->state = TCP_CLOSING;
            wake = 1;
        }

        /* Try to push any newly-window-opened data. */
        if (pcb->state == TCP_ESTABLISHED && pcb->snd_len > 0)
            tcp_output_data(pcb, pcb->snd_una);
        break;

    default:
        break;
    }

    spin_unlock_irqrestore(&tcp_lock, f);
    if (wake) wq_wake_all(&pcb->wq);
    pbuf_free(p);
}
