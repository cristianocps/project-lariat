#include "net.h"
#include "kapi.h"
#include <string.h>

pbuf_t *pbuf_alloc(void) {
    pbuf_t *p = (pbuf_t *)kmalloc(sizeof(pbuf_t));
    if (!p) return NULL;
    p->next = NULL;
    p->nif = NULL;
    p->off = PBUF_HEADROOM;
    p->len = 0;
    return p;
}

void pbuf_free(pbuf_t *p) {
    if (p) kfree(p);
}

uint8_t *pbuf_push(pbuf_t *p, uint16_t n) {
    if (p->off < n) return NULL;   /* not enough headroom */
    p->off -= n;
    p->len += n;
    return p->store + p->off;
}

uint8_t *pbuf_pull(pbuf_t *p, uint16_t n) {
    if (p->len < n) return NULL;
    p->off += n;
    p->len -= n;
    return p->store + p->off;
}
