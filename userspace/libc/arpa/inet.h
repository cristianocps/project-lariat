#ifndef ARPA_INET_H
#define ARPA_INET_H

#include <stdint.h>
#include "libc/netinet/in.h"

static inline uint16_t htons(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8)  | ((x & 0xFF000000u) >> 24);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

/* Parse dotted-quad "a.b.c.d" into a network-order address.  Returns
 * INADDR_NONE (0xFFFFFFFF) on malformed input. */
#define INADDR_NONE 0xFFFFFFFFu

static inline in_addr_t inet_addr(const char *s) {
    uint32_t parts[4] = {0,0,0,0};
    int pi = 0, digits = 0;
    uint32_t cur = 0;
    for (const char *p = s; ; p++) {
        if (*p >= '0' && *p <= '9') {
            cur = cur * 10 + (uint32_t)(*p - '0');
            if (cur > 255) return INADDR_NONE;
            digits = 1;
        } else if (*p == '.') {
            if (!digits || pi >= 3) return INADDR_NONE;
            parts[pi++] = cur; cur = 0; digits = 0;
        } else if (*p == '\0') {
            if (!digits || pi != 3) return INADDR_NONE;
            parts[pi] = cur;
            break;
        } else {
            return INADDR_NONE;
        }
    }
    /* network byte order: a is most significant */
    return htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
}

static inline int inet_aton(const char *s, struct in_addr *out) {
    in_addr_t a = inet_addr(s);
    if (a == INADDR_NONE) return 0;
    out->s_addr = a;
    return 1;
}

#endif /* ARPA_INET_H */
