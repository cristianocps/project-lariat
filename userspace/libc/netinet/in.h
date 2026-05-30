#ifndef NETINET_IN_H
#define NETINET_IN_H

#include <stdint.h>

#define AF_INET     2
#define INADDR_ANY  0x00000000u

typedef uint32_t socklen_t;
typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;
typedef uint16_t sa_family_t;

struct in_addr { in_addr_t s_addr; };   /* network byte order */

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_in {
    sa_family_t    sin_family;
    in_port_t      sin_port;   /* network byte order */
    struct in_addr sin_addr;   /* network byte order */
    uint8_t        sin_zero[8];
};

#endif /* NETINET_IN_H */
