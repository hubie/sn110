/*
 * Minimal stdlib.h for SN110 OABI build
 */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

int   atoi(const char *s);
void *malloc(size_t size);
void  free(void *ptr);
void  exit(int status) __attribute__((noreturn));

#endif
