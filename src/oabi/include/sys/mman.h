/*
 * Minimal sys/mman.h for SN110 OABI build
 * Provides mmap/munmap prototypes and constants.
 */
#ifndef _SYS_MMAN_H
#define _SYS_MMAN_H

#include <stddef.h>

#define PROT_READ       0x1
#define PROT_WRITE      0x2

#define MAP_SHARED      0x01

#define MAP_FAILED      ((void *)-1)

void *mmap(void *addr, unsigned long len, int prot, int flags,
           int fd, unsigned long offset);
int   munmap(void *addr, size_t length);

#endif
