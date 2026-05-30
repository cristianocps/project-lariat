#include "net.h"
#include "serial.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Internet checksum (RFC 1071).  Computed in host byte order; the folded,
 * complemented result stored directly into the packet is correct on the wire
 * regardless of endianness (byte-swap of the sum == sum of byte-swapped words).
 * -------------------------------------------------------------------------- */
uint32_t inet_sum(const void *data, uint16_t len, uint32_t start) {
    uint32_t sum = start;
    const uint8_t *p = (const uint8_t *)data;
    while (len > 1) {
        sum += (uint16_t)(p[0] | (p[1] << 8));
        p += 2;
        len -= 2;
    }
    if (len) sum += p[0];
    return sum;
}

uint16_t inet_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t inet_checksum(const void *data, uint16_t len) {
    return inet_fold(inet_sum(data, len, 0));
}

/* --------------------------------------------------------------------------
 * Input
 * -------------------------------------------------------------------------- */
void ipv4_input(netif_t *nif, pbuf_t *p) {
    if (p->len < (int)sizeof(struct ip_hdr)) { pbuf_free(p); return; }
    struct ip_hdr *ih = (struct ip_hdr *)pbuf_data(p);

    uint8_t ihl = (ih->ver_ihl & 0x0F) * 4;
    if ((ih->ver_ihl >> 4) != 4 || ihl < 20) { pbuf_free(p); return; }

    uint16_t tot = ntohs(ih->tot_len);
    if (tot > p->len) { pbuf_free(p); return; }

    uint32_t src = ih->src;
    uint32_t dst = ih->dst;
    uint8_t proto = ih->proto;

    /* Trim any ethernet padding, then strip the IP header. */
    p->len = tot;
    pbuf_pull(p, ihl);

    switch (proto) {
        case IP_PROTO_ICMP: icmp_input(nif, p, src);      break;
        case IP_PROTO_UDP:  udp_input(nif, p, src, dst);  break;
        case IP_PROTO_TCP:  tcp_input(nif, p, src, dst);  break;
        default:            pbuf_free(p);                 break;
    }
}

/* --------------------------------------------------------------------------
 * Output
 * -------------------------------------------------------------------------- */
static uint16_t ip_id_counter = 1;

int ipv4_output(uint32_t dst_ip, uint8_t proto, pbuf_t *p) {
    netif_t *nif = netif_route(dst_ip);
    if (!nif) { pbuf_free(p); return -1; }

    struct ip_hdr *ih = (struct ip_hdr *)pbuf_push(p, sizeof(struct ip_hdr));
    if (!ih) { pbuf_free(p); return -1; }

    ih->ver_ihl = (4 << 4) | 5;
    ih->tos = 0;
    ih->tot_len = htons(p->len);
    ih->id = htons(ip_id_counter++);
    ih->frag_off = 0;
    ih->ttl = 64;
    ih->proto = proto;
    ih->csum = 0;
    ih->src = nif->ip;
    ih->dst = dst_ip;
    ih->csum = inet_checksum(ih, sizeof(struct ip_hdr));

    /* Loopback: no real link layer, just fake an ethernet header so the frame
     * loops back through eth_input. */
    if (nif == netif_loopback()) {
        uint8_t zero[6] = {0};
        return eth_output(nif, p, zero, ETH_P_IP);
    }

    /* Next hop: directly to dst if on-link, otherwise via the gateway. */
    uint32_t nexthop = dst_ip;
    if ((dst_ip & nif->netmask) != (nif->ip & nif->netmask)) {
        nexthop = nif->gateway;
    }

    uint8_t mac[6];
    if (arp_resolve(nif, nexthop, mac, p) == 0) {
        return eth_output(nif, p, mac, ETH_P_IP);
    }
    /* Unresolved: arp_resolve queued p (or dropped it) and sent a request. */
    return 0;
}
