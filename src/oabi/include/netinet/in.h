/*
 * Minimal netinet/in.h for SN110 OABI build
 * Constants match ARM Linux kernel.
 */
#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <stdint.h>
#include <sys/socket.h>

#define IPPROTO_IP   0
#define IPPROTO_UDP  17

#define INADDR_ANY   ((uint32_t)0)

#define IP_MULTICAST_TTL  33
#define IP_ADD_MEMBERSHIP 35

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr_in {
    sa_family_t    sin_family;
    uint16_t       sin_port;
    struct in_addr sin_addr;
    unsigned char  sin_zero[8];
};

struct ip_mreq {
    struct in_addr imr_multiaddr;
    struct in_addr imr_interface;
};

/* Byte order conversion — ARM7TDMI is little-endian */
static inline uint16_t htons(uint16_t x)
{
    return (uint16_t)((x >> 8) | (x << 8));
}

static inline uint32_t htonl(uint32_t x)
{
    return ((x >> 24) & 0x000000FF) |
           ((x >>  8) & 0x0000FF00) |
           ((x <<  8) & 0x00FF0000) |
           ((x << 24) & 0xFF000000);
}

#define ntohs(x) htons(x)
#define ntohl(x) htonl(x)

#endif
