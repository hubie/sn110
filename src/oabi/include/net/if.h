/*
 * Minimal net/if.h for SN110 OABI build
 * Constants and struct ifreq match ARM Linux kernel.
 */
#ifndef _NET_IF_H
#define _NET_IF_H

#include <sys/socket.h>

#define IFNAMSIZ 16

/* ioctl request codes for network interfaces */
#define SIOCGIFADDR   0x8915  /* Get interface address */
#define SIOCGIFFLAGS  0x8913  /* Get interface flags */

/* Interface flags */
#define IFF_UP        0x1     /* Interface is up */
#define IFF_RUNNING   0x40    /* Interface is running */

struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifr_addr;
        short           ifr_flags;
        char            ifr_pad[24];  /* ensure struct is large enough */
    };
};

#endif
