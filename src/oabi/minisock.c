/*
 * minisock.c — Socket wrappers via Linux socketcall() multiplexer
 *
 * On ARM Linux 2.0, individual socket syscalls (socket, bind, recv, etc.)
 * don't exist. They all go through socketcall(2) with a sub-function number
 * and a pointer to the arguments array.
 *
 * socketcall sub-function numbers:
 *   SYS_SOCKET=1, SYS_BIND=2, SYS_CONNECT=3, SYS_LISTEN=4,
 *   SYS_ACCEPT=5, SYS_SEND=9, SYS_RECV=10, SYS_SENDTO=11,
 *   SYS_RECVFROM=12, SYS_SETSOCKOPT=14, SYS_GETSOCKOPT=15
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>

/* Raw socketcall from syscalls.S */
extern int _sys_socketcall(int call, unsigned long *args);

#define SYS_SOCKET      1
#define SYS_BIND        2
#define SYS_CONNECT     3
#define SYS_LISTEN      4
#define SYS_ACCEPT      5
#define SYS_SEND        9
#define SYS_RECV        10
#define SYS_SENDTO      11
#define SYS_RECVFROM    12
#define SYS_SETSOCKOPT  14
#define SYS_GETSOCKOPT  15

int socket(int domain, int type, int protocol)
{
    unsigned long a[3];
    a[0] = (unsigned long)domain;
    a[1] = (unsigned long)type;
    a[2] = (unsigned long)protocol;
    return _sys_socketcall(SYS_SOCKET, a);
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    unsigned long a[3];
    a[0] = (unsigned long)sockfd;
    a[1] = (unsigned long)addr;
    a[2] = (unsigned long)addrlen;
    return _sys_socketcall(SYS_BIND, a);
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    unsigned long a[4];
    a[0] = (unsigned long)sockfd;
    a[1] = (unsigned long)buf;
    a[2] = (unsigned long)len;
    a[3] = (unsigned long)flags;
    return _sys_socketcall(SYS_RECV, a);
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen)
{
    unsigned long a[6];
    a[0] = (unsigned long)sockfd;
    a[1] = (unsigned long)buf;
    a[2] = (unsigned long)len;
    a[3] = (unsigned long)flags;
    a[4] = (unsigned long)src_addr;
    a[5] = (unsigned long)addrlen;
    return _sys_socketcall(SYS_RECVFROM, a);
}

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen)
{
    unsigned long a[6];
    a[0] = (unsigned long)sockfd;
    a[1] = (unsigned long)buf;
    a[2] = (unsigned long)len;
    a[3] = (unsigned long)flags;
    a[4] = (unsigned long)dest_addr;
    a[5] = (unsigned long)addrlen;
    return _sys_socketcall(SYS_SENDTO, a);
}

int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen)
{
    unsigned long a[5];
    a[0] = (unsigned long)sockfd;
    a[1] = (unsigned long)level;
    a[2] = (unsigned long)optname;
    a[3] = (unsigned long)optval;
    a[4] = (unsigned long)optlen;
    return _sys_socketcall(SYS_SETSOCKOPT, a);
}
