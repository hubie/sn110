/*
 * Minimal sys/time.h for SN110 OABI build
 */
#ifndef _SYS_TIME_H
#define _SYS_TIME_H

struct timeval {
    long tv_sec;
    long tv_usec;
};

int gettimeofday(struct timeval *tv, void *tz);

/* Pull in select() — system libc does this transitively */
#include <sys/select.h>

#endif
