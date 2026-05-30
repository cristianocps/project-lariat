#include "net.h"
#include "pci.h"
#include "kapi.h"
#include "mm.h"
#include "ports.h"
#include "idt.h"
#include "ioapic.h"
#include "lapic.h"
#include "serial.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * RealTek RTL8139 (vendor 0x10EC, device 0x8139) - the classic QEMU NIC.
 * PIO register access via BAR0 (an I/O BAR).
 * -------------------------------------------------------------------------- */
#define RTL_IDR0     0x00   /* MAC, 6 bytes */
#define RTL_TSD0     0x10   /* transmit status, 4 descriptors (stride 4) */
#define RTL_TSAD0    0x20   /* transmit start address, 4 descriptors */
#define RTL_RBSTART  0x30   /* RX buffer physical address */
#define RTL_CR       0x37   /* command register (8-bit) */
#define RTL_CAPR     0x38   /* current address of packet read (16-bit) */
#define RTL_CBR      0x3A   /* current buffer address (16-bit) */
#define RTL_IMR      0x3C   /* interrupt mask (16-bit) */
#define RTL_ISR      0x3E   /* interrupt status (16-bit) */
#define RTL_TCR      0x40   /* transmit config (32-bit) */
#define RTL_RCR      0x44   /* receive config (32-bit) */
#define RTL_CONFIG1  0x52   /* config register 1 (8-bit) */

#define CR_RST       0x10
#define CR_RE        0x08
#define CR_TE        0x04
#define CR_BUFE      0x01   /* RX buffer empty */

#define ISR_ROK      0x0001
#define ISR_RER      0x0002
#define ISR_TOK      0x0004
#define ISR_TER      0x0008
#define ISR_RXOVW    0x0010

#define RCR_AAP      0x01   /* accept all (promiscuous) */
#define RCR_APM      0x02   /* accept physical match */
#define RCR_AM       0x04   /* accept multicast */
#define RCR_AB       0x08   /* accept broadcast */
#define RCR_WRAP     0x80

#define RX_BUF_LEN   8192
#define RX_BUF_PAD   16
#define RX_BUF_WRAP  2048   /* extra room because WRAP lets hw overrun the end */
#define RX_ALLOC     (RX_BUF_LEN + RX_BUF_PAD + RX_BUF_WRAP)
#define TX_BUF_SIZE  2048

typedef struct {
    uint16_t io;
    uint8_t  vector;

    uint8_t  *rx_buf;
    uint64_t  rx_phys;
    uint32_t  rx_off;       /* current offset into the RX ring */

    uint8_t  *tx_buf[4];
    uint64_t  tx_phys[4];
    int       tx_cur;       /* next TX descriptor to use */

    netif_t   nif;
} rtl8139_t;

static rtl8139_t g_rtl;

static inline void rtl_out8(rtl8139_t *r, uint16_t reg, uint8_t v)  { outb(r->io + reg, v); }
static inline void rtl_out16(rtl8139_t *r, uint16_t reg, uint16_t v){ outw(r->io + reg, v); }
static inline void rtl_out32(rtl8139_t *r, uint16_t reg, uint32_t v){ outl(r->io + reg, v); }
static inline uint8_t  rtl_in8(rtl8139_t *r, uint16_t reg)  { return inb(r->io + reg); }
static inline uint16_t rtl_in16(rtl8139_t *r, uint16_t reg) { return inw(r->io + reg); }

/* --------------------------------------------------------------------------
 * Transmit: copy the (already complete) ethernet frame into a TX buffer and
 * kick the next descriptor.
 * -------------------------------------------------------------------------- */
static int rtl8139_transmit(netif_t *nif, pbuf_t *p) {
    rtl8139_t *r = (rtl8139_t *)nif->priv;
    uint16_t len = p->len;
    if (len > TX_BUF_SIZE) { pbuf_free(p); return -1; }
    if (len < 60) len = 60;   /* pad to the ethernet minimum */

    int d = r->tx_cur;
    memset(r->tx_buf[d], 0, 60);
    memcpy(r->tx_buf[d], pbuf_data(p), p->len);

    rtl_out32(r, RTL_TSAD0 + d * 4, (uint32_t)r->tx_phys[d]);
    rtl_out32(r, RTL_TSD0 + d * 4, len);   /* clears OWN -> starts DMA */

    r->tx_cur = (d + 1) & 3;
    pbuf_free(p);
    return 0;
}

/* --------------------------------------------------------------------------
 * Receive: drain the RX ring into pbufs and hand them to the stack.
 * -------------------------------------------------------------------------- */
static void rtl8139_rx(rtl8139_t *r) {
    while (!(rtl_in8(r, RTL_CR) & CR_BUFE)) {
        uint8_t *base = r->rx_buf + (r->rx_off % RX_BUF_LEN);
        uint16_t status = base[0] | (base[1] << 8);
        uint16_t length = base[2] | (base[3] << 8);
        (void)status;

        if (length < 4 || length > PBUF_SIZE) {
            /* Bad packet: resync to empty by resetting CAPR. */
            r->rx_off = 0;
            rtl_out16(r, RTL_CAPR, (uint16_t)(r->rx_off - RX_BUF_PAD));
            break;
        }

        uint16_t payload = length - 4;   /* strip the 4-byte ethernet CRC */
        pbuf_t *p = pbuf_alloc();
        if (p) {
            uint8_t *pkt = base + 4;
            memcpy(pbuf_data(p), pkt, payload);
            p->len = payload;
            net_rx_enqueue(&r->nif, p);
        }

        /* Advance past header(4) + packet, dword-aligned. */
        r->rx_off = (r->rx_off + length + 4 + 3) & ~3u;
        rtl_out16(r, RTL_CAPR, (uint16_t)(r->rx_off - RX_BUF_PAD));
    }
}

static void rtl8139_irq(registers_t *regs) {
    (void)regs;
    rtl8139_t *r = &g_rtl;
    uint16_t isr = rtl_in16(r, RTL_ISR);
    if (!isr) return;
    /* Ack first (write 1 to clear). */
    rtl_out16(r, RTL_ISR, isr);

    if (isr & (ISR_ROK | ISR_RXOVW | ISR_RER)) {
        rtl8139_rx(r);
    }
    /* TX completion (ISR_TOK) needs no action: rtl8139_transmit reuses
     * descriptors round-robin and QEMU completes them immediately. */
}

/* --------------------------------------------------------------------------
 * Probe / bring-up
 * -------------------------------------------------------------------------- */
static int rtl8139_setup(uint16_t bus, uint16_t slot, uint16_t func) {
    rtl8139_t *r = &g_rtl;
    memset(r, 0, sizeof(*r));

    pci_enable_bus_mastering(bus, slot, func);

    uint32_t bar0 = pci_bar(bus, slot, func, 0);
    r->io = (uint16_t)pci_bar_io_addr(bar0);
    uint8_t int_line = (uint8_t)(pci_read_config(bus, slot, func, PCI_INT_LINE) & 0xFF);

    /* Power on, then software reset. */
    rtl_out8(r, RTL_CONFIG1, 0x00);
    rtl_out8(r, RTL_CR, CR_RST);
    int timeout = 100000;
    while ((rtl_in8(r, RTL_CR) & CR_RST) && timeout-- > 0) { }

    /* DMA buffers. */
    r->rx_buf = (uint8_t *)dma_alloc(RX_ALLOC, &r->rx_phys);
    memset(r->rx_buf, 0, RX_ALLOC);
    for (int i = 0; i < 4; i++) {
        r->tx_buf[i] = (uint8_t *)dma_alloc(TX_BUF_SIZE, &r->tx_phys[i]);
    }
    r->rx_off = 0;
    r->tx_cur = 0;

    rtl_out32(r, RTL_RBSTART, (uint32_t)r->rx_phys);

    /* Accept broadcast/multicast/physical/all + WRAP; 8K ring. */
    rtl_out32(r, RTL_RCR, RCR_AAP | RCR_APM | RCR_AM | RCR_AB | RCR_WRAP);
    rtl_out32(r, RTL_TCR, 0x03000700);   /* default IFG + max DMA burst */

    /* Enable RX + TX. */
    rtl_out8(r, RTL_CR, CR_RE | CR_TE);

    /* Unmask RX-OK / TX-OK / errors. */
    rtl_out16(r, RTL_IMR, ISR_ROK | ISR_TOK | ISR_RER | ISR_TER | ISR_RXOVW);

    /* Read the MAC. */
    netif_t *nif = &r->nif;
    memset(nif, 0, sizeof(*nif));
    for (int i = 0; i < 6; i++) nif->mac[i] = rtl_in8(r, RTL_IDR0 + i);

    strcpy(nif->name, "eth0");
    nif->mtu = 1500;
    nif->transmit = rtl8139_transmit;
    nif->priv = r;
    /* Static config matching QEMU user-mode (slirp) networking. */
    nif->ip      = IPV4(10, 0, 2, 15);
    nif->netmask = IPV4(255, 255, 255, 0);
    nif->gateway = IPV4(10, 0, 2, 2);
    netif_register(nif);

    /* Route the NIC's PCI INTx (level, active-low) to a free vector. */
    r->vector = 32 + int_line;
    register_interrupt_handler(r->vector, rtl8139_irq);
    ioapic_route_ex(int_line, r->vector, (uint8_t)lapic_id(), 1, 1);

    serial_printf(SERIAL_COM1,
        "[RTL8139] io=%x irq=%d vec=%d mac=%x:%x:%x:%x:%x:%x ip=10.0.2.15\n",
        r->io, int_line, r->vector,
        nif->mac[0], nif->mac[1], nif->mac[2],
        nif->mac[3], nif->mac[4], nif->mac[5]);
    return 0;
}

/* Scan PCI bus 0 for the RTL8139 and bring it up.  Called late in kmain, after
 * the APIC + interrupts are live, so IO-APIC routing takes effect. */
void rtl8139_init(void) {
    for (uint16_t slot = 0; slot < 32; slot++) {
        if (pci_vendor(0, slot, 0) == 0x10EC &&
            pci_device(0, slot, 0) == 0x8139) {
            rtl8139_setup(0, slot, 0);
            return;
        }
    }
    serial_printf(SERIAL_COM1, "[RTL8139] no NIC found\n");
}
