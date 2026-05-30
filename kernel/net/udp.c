#include "netpcb.h"
#include "kapi.h"
#include "serial.h"
#include <string.h>

static udp_pcb_t *udp_list = NULL;
static spinlock_t udp_lock = SPINLOCK_INIT;
static uint16_t   udp_ephemeral = 49152;

void udp_init(void) { udp_list = NULL; }

udp_pcb_t *udp_new(void) {
    udp_pcb_t *p = kzalloc(sizeof(udp_pcb_t));
    if (!p) return NULL;
    uint64_t f = spin_lock_irqsave(&udp_lock);
    p->next = udp_list;
    udp_list = p;
    spin_unlock_irqrestore(&udp_lock, f);
    return p;
}

int udp_bind(udp_pcb_t *pcb, uint32_t ip, uint16_t port) {
    if (port == 0) port = udp_ephemeral++;
    pcb->local_ip = ip;
    pcb->local_port = port;
    pcb->bound = 1;
    return port;
}

int udp_connect(udp_pcb_t *pcb, uint32_t ip, uint16_t port) {
    pcb->remote_ip = ip;
    pcb->remote_port = port;
    if (!pcb->bound) udp_bind(pcb, 0, 0);
    return 0;
}

static uint16_t udp_csum(uint32_t src, uint32_t dst, const struct udp_hdr *uh,
                         const void *data, uint16_t dlen) {
    /* Pseudo-header: src(4) dst(4) zero(1) proto(1) udplen(2). */
    uint8_t pseudo[12];
    memcpy(pseudo, &src, 4);
    memcpy(pseudo + 4, &dst, 4);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_UDP;
    uint16_t ulen = ntohs(uh->len);
    pseudo[10] = ulen >> 8;
    pseudo[11] = ulen & 0xFF;
    uint32_t sum = inet_sum(pseudo, 12, 0);
    sum = inet_sum(uh, sizeof(*uh), sum);
    sum = inet_sum(data, dlen, sum);
    uint16_t c = inet_fold(sum);
    return c ? c : 0xFFFF;   /* 0 means "no checksum"; avoid it */
}

int udp_sendto(udp_pcb_t *pcb, uint32_t ip, uint16_t port,
               const void *data, uint16_t len) {
    if (!pcb->bound) udp_bind(pcb, 0, 0);
    if (len > PBUF_SIZE - 64) return -1;

    pbuf_t *p = pbuf_alloc();
    if (!p) return -1;
    memcpy(pbuf_data(p), data, len);
    p->len = len;

    struct udp_hdr *uh = (struct udp_hdr *)pbuf_push(p, sizeof(struct udp_hdr));
    uh->src = htons(pcb->local_port);
    uh->dst = htons(port);
    uh->len = htons(sizeof(struct udp_hdr) + len);
    uh->csum = 0;

    netif_t *nif = netif_route(ip);
    uint32_t src_ip = nif ? nif->ip : 0;
    uh->csum = udp_csum(src_ip, ip, uh, data, len);

    ipv4_output(ip, IP_PROTO_UDP, p);
    return len;
}

int udp_recv(udp_pcb_t *pcb, void *buf, uint16_t len,
             uint32_t *src_ip, uint16_t *src_port) {
    uint64_t f = spin_lock_irqsave(&udp_lock);
    pbuf_t *p = pcb->rx_head;
    if (!p) { spin_unlock_irqrestore(&udp_lock, f); return -1; }
    pcb->rx_head = p->next;
    if (!pcb->rx_head) pcb->rx_tail = NULL;
    spin_unlock_irqrestore(&udp_lock, f);

    uint16_t n = p->len < len ? p->len : len;
    memcpy(buf, pbuf_data(p), n);
    if (src_ip) *src_ip = p->saddr;
    if (src_port) *src_port = p->sport;
    pbuf_free(p);
    return n;
}

void udp_close(udp_pcb_t *pcb) {
    uint64_t f = spin_lock_irqsave(&udp_lock);
    udp_pcb_t **pp = &udp_list;
    while (*pp) {
        if (*pp == pcb) { *pp = pcb->next; break; }
        pp = &(*pp)->next;
    }
    pbuf_t *p = pcb->rx_head;
    while (p) { pbuf_t *n = p->next; pbuf_free(p); p = n; }
    spin_unlock_irqrestore(&udp_lock, f);
    kfree(pcb);
}

void udp_input(netif_t *nif, pbuf_t *p, uint32_t src_ip, uint32_t dst_ip) {
    (void)nif;
    if (p->len < (int)sizeof(struct udp_hdr)) { pbuf_free(p); return; }
    struct udp_hdr *uh = (struct udp_hdr *)pbuf_data(p);
    uint16_t dport = ntohs(uh->dst);
    uint16_t sport = ntohs(uh->src);
    uint16_t ulen = ntohs(uh->len);
    if (ulen < sizeof(struct udp_hdr) || ulen > p->len) { pbuf_free(p); return; }

    pbuf_pull(p, sizeof(struct udp_hdr));
    p->len = ulen - sizeof(struct udp_hdr);
    p->saddr = src_ip;
    p->sport = sport;

    uint64_t f = spin_lock_irqsave(&udp_lock);
    udp_pcb_t *pcb = udp_list, *match = NULL;
    while (pcb) {
        if (pcb->bound && pcb->local_port == dport &&
            (pcb->local_ip == 0 || pcb->local_ip == dst_ip)) {
            match = pcb;
            break;
        }
        pcb = pcb->next;
    }
    if (match) {
        p->next = NULL;
        if (match->rx_tail) match->rx_tail->next = p; else match->rx_head = p;
        match->rx_tail = p;
    }
    spin_unlock_irqrestore(&udp_lock, f);

    if (!match) { pbuf_free(p); return; }
    wq_wake_all(&match->recvq);
}
