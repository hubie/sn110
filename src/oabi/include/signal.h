/*
 * Minimal signal.h for SN110 OABI build
 * Signal numbers match ARM Linux kernel.
 */
#ifndef _SIGNAL_H
#define _SIGNAL_H

#define SIGINT   2
#define SIGTERM  15

typedef void (*sighandler_t)(int);

sighandler_t signal(int signum, sighandler_t handler);

#endif
