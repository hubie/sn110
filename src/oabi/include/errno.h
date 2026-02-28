/*
 * Minimal errno.h for SN110 OABI build
 */
#ifndef _ERRNO_H
#define _ERRNO_H

extern int errno;

#define EPERM    1
#define ENOENT   2
#define EINTR    4
#define EIO      5
#define EAGAIN   11
#define ENOMEM   12
#define EACCES   13
#define EEXIST   17
#define EINVAL   22

#endif
