#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>

/* --------------------------------------------------------------------------
 * Byte order (the kernel runs little-endian x86; network order is big-endian)
 * -------------------------------------------------------------------------- */
static inline uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8) | ((x & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

/* Build an IPv4 address (host args) into network byte order. */
#define IPV4(a, b, c, d) htonl(((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
                               ((uint32_t)(c) << 8) | (uint32_t)(d))

/* --------------------------------------------------------------------------
 * Packet buffers (pbuf): fixed-size with headroom so protocol layers can
 * prepend their headers cheaply.
 * -------------------------------------------------------------------------- */
#define PBUF_SIZE      2048
#define PBUF_HEADROOM  128

typedef struct pbuf {
    struct pbuf *next;
    uint16_t     off;   /* start of valid data within store[] */
    uint16_t     len;   /* length of valid data */
    /* metadata filled in as the packet moves up the stack */
    struct netif *nif;
    uint32_t     saddr; /* source IP (network order), set by transport demux */
    uint16_t     sport; /* source port (host order) */
    uint8_t      store[PBUF_SIZE];
} pbuf_t;

pbuf_t  *pbuf_alloc(void);
void     pbuf_free(pbuf_t *p);
static inline uint8_t *pbuf_data(pbuf_t *p) { return p->store + p->off; }
/* Reserve/prepend n bytes of header space; returns pointer to new front. */
uint8_t *pbuf_push(pbuf_t *p, uint16_t n);
/* Drop n bytes from the front (consume a header). */
uint8_t *pbuf_pull(pbuf_t *p, uint16_t n);

/* --------------------------------------------------------------------------
 * Network interface
 * -------------------------------------------------------------------------- */
typedef struct netif {
    char          name[8];
    uint8_t       mac[6];
    uint32_t      ip;        /* network byte order */
    uint32_t      netmask;   /* network byte order */
    uint32_t      gateway;   /* network byte order */
    uint16_t      mtu;
    int         (*transmit)(struct netif *nif, pbuf_t *p);  /* send full eth frame */
    void         *priv;
    struct netif *next;
} netif_t;

void     netif_register(netif_t *nif);
netif_t *netif_default(void);
netif_t *netif_loopback(void);
/* Pick the interface that should source/route a packet to dst ip. */
netif_t *netif_route(uint32_t dst_ip);

/* --------------------------------------------------------------------------
 * Net core: RX dispatch thread fed by driver IRQs.
 * -------------------------------------------------------------------------- */
void net_init(void);
/* Called from a driver IRQ: hand a received ethernet frame to the stack. */
void net_rx_enqueue(netif_t *nif, pbuf_t *p);

/* Ethernet input/output (defined in eth.c). */
void eth_input(netif_t *nif, pbuf_t *p);
#define ETH_ALEN   6
#define ETH_HDRLEN 14
#define ETH_P_IP   0x0800
#define ETH_P_ARP  0x0806

struct eth_hdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;   /* network byte order */
} __attribute__((packed));

/* Send a payload pbuf out `nif` to `dst_mac` with the given ethertype.
 * `p` must have ETH_HDRLEN of headroom available via pbuf_push. */
int eth_output(netif_t *nif, pbuf_t *p, const uint8_t *dst_mac, uint16_t ethertype);

/* Driver entry points. */
void rtl8139_init(void);

/* --------------------------------------------------------------------------
 * ARP
 * -------------------------------------------------------------------------- */
struct arp_hdr {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t op;
    uint8_t  sha[6];
    uint8_t  spa[4];   /* kept as bytes: spa/tpa are not 4-aligned */
    uint8_t  tha[6];
    uint8_t  tpa[4];
} __attribute__((packed));

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

void arp_init(void);
/* Resolve dst_ip -> mac.  Returns 0 and fills mac if cached; otherwise sends a
 * request, optionally queues `pending` to send on resolution, and returns -1. */
int  arp_resolve(netif_t *nif, uint32_t dst_ip, uint8_t *mac_out, pbuf_t *pending);

/* --------------------------------------------------------------------------
 * IPv4
 * -------------------------------------------------------------------------- */
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

struct ip_hdr {
    uint8_t  ver_ihl;    /* version (4 bits) + IHL (4 bits) */
    uint8_t  tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t csum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

uint16_t inet_checksum(const void *data, uint16_t len);
/* One's-complement sum that can be folded across multiple calls. */
uint32_t inet_sum(const void *data, uint16_t len, uint32_t start);
uint16_t inet_fold(uint32_t sum);

/* Send `p` (transport header + payload at pbuf_data) to dst_ip via proto.
 * Prepends the IP header and routes/transmits.  Consumes `p`. */
int ipv4_output(uint32_t dst_ip, uint8_t proto, pbuf_t *p);

/* --------------------------------------------------------------------------
 * ICMP / UDP / TCP entry points
 * -------------------------------------------------------------------------- */
struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t csum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));
#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

void icmp_input(netif_t *nif, pbuf_t *p, uint32_t src_ip);

struct udp_hdr {
    uint16_t src;
    uint16_t dst;
    uint16_t len;
    uint16_t csum;
} __attribute__((packed));
void udp_input(netif_t *nif, pbuf_t *p, uint32_t src_ip, uint32_t dst_ip);

struct tcp_hdr {
    uint16_t src;
    uint16_t dst;
    uint32_t seq;
    uint32_t ack;
    uint8_t  off;     /* data offset (high 4 bits) */
    uint8_t  flags;
    uint16_t win;
    uint16_t csum;
    uint16_t urg;
} __attribute__((packed));
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
void tcp_input(netif_t *nif, pbuf_t *p, uint32_t src_ip, uint32_t dst_ip);
void tcp_init(void);
void udp_init(void);

#endif /* NET_H */
