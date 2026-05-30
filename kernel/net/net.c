#include "net.h"
#include "netpcb.h"
#include "kapi.h"
#include "sched.h"
#include "serial.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Interface registry
 * -------------------------------------------------------------------------- */
static netif_t *netif_list = NULL;
static netif_t *default_netif = NULL;
static netif_t loopback_if;

void netif_register(netif_t *nif) {
    nif->next = netif_list;
    netif_list = nif;
    /* The first non-loopback interface becomes the default route. */
    if (!default_netif && nif != &loopback_if) {
        default_netif = nif;
    }
}

netif_t *netif_default(void) { return default_netif; }
netif_t *netif_loopback(void) { return &loopback_if; }

netif_t *netif_route(uint32_t dst_ip) {
    /* 127.0.0.0/8 -> loopback. */
    if ((ntohl(dst_ip) & 0xFF000000u) == 0x7F000000u) return &loopback_if;
    return default_netif;
}

/* Loopback transmit: feed the frame straight back into the stack. */
static int loopback_tx(netif_t *nif, pbuf_t *p) {
    p->nif = nif;
    net_rx_enqueue(nif, p);
    return 0;
}

/* --------------------------------------------------------------------------
 * RX dispatch: driver IRQs enqueue frames; the netd thread processes them out
 * of interrupt context.
 * -------------------------------------------------------------------------- */
static pbuf_t *rx_head = NULL, *rx_tail = NULL;
static spinlock_t rx_lock = SPINLOCK_INIT;
static wait_queue_t rx_waitq = WAIT_QUEUE_INIT;

void net_rx_enqueue(netif_t *nif, pbuf_t *p) {
    p->nif = nif;
    p->next = NULL;
    uint64_t f = spin_lock_irqsave(&rx_lock);
    if (rx_tail) rx_tail->next = p; else rx_head = p;
    rx_tail = p;
    spin_unlock_irqrestore(&rx_lock, f);
    wq_wake_all(&rx_waitq);
}

static pbuf_t *net_rx_dequeue(void) {
    uint64_t f = spin_lock_irqsave(&rx_lock);
    pbuf_t *p = rx_head;
    if (p) {
        rx_head = p->next;
        if (!rx_head) rx_tail = NULL;
        p->next = NULL;
    }
    spin_unlock_irqrestore(&rx_lock, f);
    return p;
}

static void netd_thread(void *arg) {
    (void)arg;
    serial_printf(SERIAL_COM1, "[NET] netd RX dispatch thread up\n");
    for (;;) {
        WAIT_EVENT(rx_waitq, rx_head != NULL);
        pbuf_t *p;
        while ((p = net_rx_dequeue())) {
            eth_input(p->nif, p);
        }
    }
}

void net_init(void) {
    /* Loopback interface (127.0.0.1). */
    memset(&loopback_if, 0, sizeof(loopback_if));
    strcpy(loopback_if.name, "lo");
    loopback_if.ip = IPV4(127, 0, 0, 1);
    loopback_if.netmask = IPV4(255, 0, 0, 0);
    loopback_if.mtu = 1500;
    loopback_if.transmit = loopback_tx;
    netif_register(&loopback_if);

    arp_init();
    udp_init();
    tcp_init();

    thread_create(netd_thread, NULL);
    serial_printf(SERIAL_COM1, "[NET] core initialized\n");
}
