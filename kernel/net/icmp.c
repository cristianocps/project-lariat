#include "net.h"
#include "serial.h"
#include <string.h>

void icmp_input(netif_t *nif, pbuf_t *p, uint32_t src_ip) {
    (void)nif;
    if (p->len < (int)sizeof(struct icmp_hdr)) { pbuf_free(p); return; }
    struct icmp_hdr *ic = (struct icmp_hdr *)pbuf_data(p);

    if (ic->type == ICMP_ECHO_REQUEST) {
        /* Turn the request into a reply in place and send it back. */
        ic->type = ICMP_ECHO_REPLY;
        ic->code = 0;
        ic->csum = 0;
        ic->csum = inet_checksum(ic, p->len);
        ipv4_output(src_ip, IP_PROTO_ICMP, p);   /* consumes p */
        return;
    }

    pbuf_free(p);
}
