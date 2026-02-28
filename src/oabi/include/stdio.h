/*
 * Minimal stdio.h for SN110 OABI build
 * Provides FILE-based I/O using raw syscalls underneath.
 */
#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

typedef struct _mini_file FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE  *fopen(const char *path, const char *mode);
int    fclose(FILE *f);
char  *fgets(char *s, int size, FILE *f);
int    fprintf(FILE *f, const char *fmt, ...);
int    printf(const char *fmt, ...);
int    dprintf(int fd, const char *fmt, ...);
int    snprintf(char *buf, size_t size, const char *fmt, ...);
int    vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int    sscanf(const char *str, const char *fmt, ...);
int    remove(const char *path);

#define EOF (-1)

#endif
