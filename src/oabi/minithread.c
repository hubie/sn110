/*
 * minithread.c — POSIX thread API via Linux clone()
 *
 * Implements pthread_create/join/mutex using clone() and SWP-based
 * spinlocks. Suitable for the single-core ARM7TDMI in the SN110.
 *
 * Key differences from full pthreads:
 *   - Thread stacks are heap-allocated (bump allocator, never freed)
 *   - Mutexes use SWP spinlocks (efficient on single-core)
 *   - No thread-local storage, no cancellation, no detach
 *   - pthread_join uses waitpid() since clone children are processes
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* From syscalls.S */
extern int waitpid(int pid, int *status, int options);

/* From minilib.c */
extern void *malloc(size_t size);

/*
 * _thread_create(stack_top, fn, arg) — defined in syscalls.S
 * Places fn/arg on child stack, calls clone(), handles child trampoline.
 * Returns child PID to parent.
 */
extern int _thread_create(void *stack_top, void *(*fn)(void *), void *arg);

/* Thread stack size — 8KB is adequate for our workload */
#define THREAD_STACK_SIZE 8192

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg)
{
    char *stack;
    int pid;

    (void)attr;

    stack = (char *)malloc(THREAD_STACK_SIZE);
    if (!stack)
        return 12; /* ENOMEM */

    pid = _thread_create(stack + THREAD_STACK_SIZE, start_routine, arg);
    if (pid < 0)
        return -pid; /* return positive errno */

    *thread = (pthread_t)pid;
    return 0;
}

int pthread_join(pthread_t thread, void **retval)
{
    int status = 0;

    (void)retval;

    waitpid((int)thread, &status, 0);
    return 0;
}

/*
 * Mutex via SWP instruction (ARMv4T atomic swap).
 *
 * SWP atomically: old = *addr; *addr = val; return old
 * If old == 0, we acquired the lock. If old == 1, someone else has it.
 */
static inline int arm_swap(volatile int *addr, int val)
{
    int old;
    __asm__ __volatile__(
        "swp %0, %2, [%1]"
        : "=&r" (old)
        : "r" (addr), "r" (val)
        : "memory"
    );
    return old;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const void *attr)
{
    (void)attr;
    mutex->lock = 0;
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    while (arm_swap(&mutex->lock, 1) != 0) {
        /* Spin. On single-core ARM7, this means the lock holder is
         * not currently running. A brief busy-wait is acceptable
         * given our ~44Hz DMX refresh rate. */
    }
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    mutex->lock = 0;
    return 0;
}
