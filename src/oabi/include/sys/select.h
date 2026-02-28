/*
 * Minimal sys/select.h for SN110 OABI build
 */
#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#include <sys/time.h>

#define FD_SETSIZE 256

typedef struct {
    unsigned long fds_bits[FD_SETSIZE / (8 * sizeof(unsigned long))];
} fd_set;

#define FD_ZERO(set)      __builtin_memset(set, 0, sizeof(fd_set))
#define FD_SET(fd, set)   ((set)->fds_bits[(fd) / (8*sizeof(unsigned long))] |= \
                           (1UL << ((fd) % (8*sizeof(unsigned long)))))
#define FD_CLR(fd, set)   ((set)->fds_bits[(fd) / (8*sizeof(unsigned long))] &= \
                           ~(1UL << ((fd) % (8*sizeof(unsigned long)))))
#define FD_ISSET(fd, set) ((set)->fds_bits[(fd) / (8*sizeof(unsigned long))] & \
                           (1UL << ((fd) % (8*sizeof(unsigned long)))))

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);

#endif
