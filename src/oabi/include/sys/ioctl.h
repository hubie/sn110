/*
 * Minimal sys/ioctl.h for SN110 OABI build
 */
#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

int ioctl(int fd, unsigned long request, ...);

#endif
