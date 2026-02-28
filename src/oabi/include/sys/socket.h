/*
 * Minimal sys/socket.h for SN110 OABI build
 * Constants match ARM Linux kernel.
 * All socket calls go through socketcall(2) multiplexer.
 */
#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define AF_INET      2
#define SOCK_DGRAM   2
#define SOL_SOCKET   1
#define SO_REUSEADDR 2
#define SO_BROADCAST 6

struct sockaddr {
    sa_family_t sa_family;
    char        sa_data[14];
};

int     socket(int domain, int type, int protocol);
int     bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
int     setsockopt(int sockfd, int level, int optname,
                   const void *optval, socklen_t optlen);

#endif
