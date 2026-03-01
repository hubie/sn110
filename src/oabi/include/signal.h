/*
 * Minimal signal.h for SN110 OABI build
 * Signal numbers match ARM Linux kernel.
 */
#ifndef _SIGNAL_H
#define _SIGNAL_H

#define SIG_IGN  ((sighandler_t)1)

#define SIGINT    2
#define SIGPIPE  13
#define SIGTERM  15

typedef void (*sighandler_t)(int);

sighandler_t signal(int signum, sighandler_t handler);

#endif
