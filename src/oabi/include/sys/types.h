/*
 * Minimal sys/types.h for SN110 OABI build
 */
#ifndef _SYS_TYPES_H
#define _SYS_TYPES_H

#include <stddef.h>

typedef int           ssize_t;
typedef int           pid_t;
typedef unsigned int  socklen_t;
typedef unsigned short sa_family_t;
typedef unsigned int  mode_t;

#endif
