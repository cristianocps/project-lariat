#include "net.h"
#include "kapi.h"
#include "timer.h"
#include "serial.h"
#include <string.h>

#define ARP_CACHE_SIZE 16
#define ARP_TTL_TICKS  (120 * TIMER_HZ)   /* entries live ~2 minutes */

struct arp_entry {
    uint32_t ip;             /* network byte order */
    uint8_t  mac[6];
    int      valid;
    uint64_t expire;
    pbuf_t  *pending;        /* one packet waiting on resolution */
    netif_t *nif;
};

static struct arp_entry cache[ARP_CACHE_SIZE];
static spinlock_t arp_lock = SPINLOCK_INIT;

static const uint8_t bcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

void arp_init(void) {
    memset(cache, 0, sizeof(cache));
}

static struct arp_entry *arp_find(uint32_t ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (cache[i].valid && cache[i].ip == ip) return &cache[i];
    return NULL;
}

static struct arp_entry *arp_alloc_slot(uint32_t ip) {
    /* Prefer a free/expired slot; otherwise evict the first. */
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (!cache[i].valid && !cache[i].pending) { return &cache[i]; }
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (cache[i].valid && cache[i].expire < now && !cache[i].pending) return &cache[i];
    for (int i = 0; i < ARP_CACHE_SIZE; i++)
        if (!cache[i].pending) return &cache[i];
    (void)ip;
    return &cache[0];
}

static void arp_send_request(netif_t *nif, uint32_t target_ip) {
    pbuf_t *p = pbuf_alloc();
    if (!p) return;
    struct arp_hdr *a = (struct arp_hdr *)pbuf_data(p);
    a->htype = htons(1);
    a->ptype = htons(ETH_P_IP);
    a->hlen = 6;
    a->plen = 4;
    a->op = htons(ARP_OP_REQUEST);
    memcpy(a->sha, nif->mac, 6);
    memcpy(a->spa, &nif->ip, 4);
    memset(a->tha, 0, 6);
    memcpy(a->tpa, &target_ip, 4);
    p->len = sizeof(struct arp_hdr);
    eth_output(nif, p, bcast_mac, ETH_P_ARP);
}

int arp_resolve(netif_t *nif, uint32_t dst_ip, uint8_t *mac_out, pbuf_t *pending) {
    uint64_t f = spin_lock_irqsave(&arp_lock);
    struct arp_entry *e = arp_find(dst_ip);
    if (e && e->valid) {
        memcpy(mac_out, e->mac, 6);
        spin_unlock_irqrestore(&arp_lock, f);
        return 0;
    }

    /* Not resolved: stash the pending packet and (re)send a request. */
    if (!e) {
        e = arp_alloc_slot(dst_ip);
        if (e->pending) { pbuf_free(e->pending); e->pending = NULL; }
        e->ip = dst_ip;
        e->valid = 0;
        e->nif = nif;
    }
    if (pending) {
        if (e->pending) pbuf_free(e->pending);  /* keep only the latest */
        e->pending = pending;
    }
    spin_unlock_irqrestore(&arp_lock, f);

    arp_send_request(nif, dst_ip);
    return -1;
}

static void arp_cache_insert(netif_t *nif, uint32_t ip, const uint8_t *mac) {
    pbuf_t *flush = NULL;
    uint64_t f = spin_lock_irqsave(&arp_lock);
    struct arp_entry *e = arp_find(ip);
    if (!e) e = arp_alloc_slot(ip);
    e->ip = ip;
    e->nif = nif;
    memcpy(e->mac, mac, 6);
    e->valid = 1;
    e->expire = timer_get_ticks() + ARP_TTL_TICKS;
    if (e->pending) { flush = e->pending; e->pending = NULL; }
    spin_unlock_irqrestore(&arp_lock, f);

    if (flush) {
        eth_output(nif, flush, mac, ETH_P_IP);
    }
}

void arp_input(netif_t *nif, pbuf_t *p) {
    if (p->len < (int)sizeof(struct arp_hdr)) { pbuf_free(p); return; }
    struct arp_hdr *a = (struct arp_hdr *)pbuf_data(p);

    uint32_t spa, tpa;
    memcpy(&spa, a->spa, 4);
    memcpy(&tpa, a->tpa, 4);
    uint16_t op = ntohs(a->op);

    /* Learn the sender. */
    arp_cache_insert(nif, spa, a->sha);

    if (op == ARP_OP_REQUEST && tpa == nif->ip) {
        /* Reply with our MAC. */
        pbuf_t *r = pbuf_alloc();
        if (r) {
            struct arp_hdr *ra = (struct arp_hdr *)pbuf_data(r);
            ra->htype = htons(1);
            ra->ptype = htons(ETH_P_IP);
            ra->hlen = 6;
            ra->plen = 4;
            ra->op = htons(ARP_OP_REPLY);
            memcpy(ra->sha, nif->mac, 6);
            memcpy(ra->spa, &nif->ip, 4);
            memcpy(ra->tha, a->sha, 6);
            memcpy(ra->tpa, a->spa, 4);
            r->len = sizeof(struct arp_hdr);
            eth_output(nif, r, a->sha, ETH_P_ARP);
        }
    }

    pbuf_free(p);
}
