#include "net.h"
#include "serial.h"
#include <string.h>

/* Defined in arp.c / ipv4.c (M3). */
void arp_input(netif_t *nif, pbuf_t *p);
void ipv4_input(netif_t *nif, pbuf_t *p);

int eth_output(netif_t *nif, pbuf_t *p, const uint8_t *dst_mac, uint16_t ethertype) {
    struct eth_hdr *eh = (struct eth_hdr *)pbuf_push(p, ETH_HDRLEN);
    if (!eh) return -1;
    memcpy(eh->dst, dst_mac, 6);
    memcpy(eh->src, nif->mac, 6);
    eh->ethertype = htons(ethertype);
    return nif->transmit(nif, p);
}

void eth_input(netif_t *nif, pbuf_t *p) {
    if (p->len < ETH_HDRLEN) { pbuf_free(p); return; }
    struct eth_hdr *eh = (struct eth_hdr *)pbuf_data(p);
    uint16_t type = ntohs(eh->ethertype);

    pbuf_pull(p, ETH_HDRLEN);

    switch (type) {
        case ETH_P_ARP:
            arp_input(nif, p);
            break;
        case ETH_P_IP:
            ipv4_input(nif, p);
            break;
        default:
            pbuf_free(p);
            break;
    }
}
