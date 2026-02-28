/*
 * Minimal pthread.h for SN110 OABI build
 *
 * Implemented via clone() + SWP-based spinlock mutexes.
 * Suitable for single-core ARM7TDMI with Linux 2.0.
 */
#ifndef _PTHREAD_H
#define _PTHREAD_H

typedef unsigned int pthread_t;
typedef void *pthread_attr_t;

typedef struct {
    volatile int lock;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

#endif
