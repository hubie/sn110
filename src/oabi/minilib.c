/*
 * minilib.c — Minimal C library for SN110 OABI build
 *
 * Provides string functions, printf/snprintf, sscanf, FILE I/O, and
 * memory allocation. All built on top of the raw OABI syscalls.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* Forward declarations for raw syscalls (from syscalls.S) */
extern int  read(int fd, void *buf, size_t count);
extern int  write(int fd, const void *buf, size_t count);
extern int  open(const char *path, int flags, ...);
extern int  close(int fd);
extern int  unlink(const char *path);
extern int  nanosleep(const void *req, void *rem);
extern unsigned long _sys_brk(unsigned long addr);

/* errno storage */
int errno = 0;

/* ================================================================== */
/* String functions                                                    */
/* ================================================================== */

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *a = s1, *b = s2;
    while (n--) {
        if (*a != *b)
            return *a - *b;
        a++;
        b++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
    return dst;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0)
        return 0;
    return (unsigned char)*s1 - (unsigned char)*s2;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

/* ================================================================== */
/* Environment variables                                               */
/* ================================================================== */

/* Set by crt0.S from kernel envp register */
char **environ;

char *getenv(const char *name)
{
    char **env;
    size_t len;

    if (!environ || !name)
        return NULL;

    len = strlen(name);
    for (env = environ; *env; env++) {
        if (strncmp(*env, name, len) == 0 && (*env)[len] == '=')
            return &(*env)[len + 1];
    }
    return NULL;
}

/* ================================================================== */
/* Number conversion                                                   */
/* ================================================================== */

int atoi(const char *s)
{
    int sign = 1, val = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9')
        val = val * 10 + (*s++ - '0');
    return sign * val;
}

/* Unsigned integer to string. Returns number of chars written. */
static int _utoa(unsigned int val, char *buf, int base, int uppercase)
{
    char tmp[12];
    int i = 0, n;
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (val == 0) {
        buf[0] = '0';
        return 1;
    }

    if (base == 10) {
        /* Subtraction-based decimal conversion — avoids __aeabi_uidiv
         * which may not work correctly on all ARM7TDMI targets */
        static const unsigned int pow10[] = {
            1000000000, 100000000, 10000000, 1000000, 100000,
            10000, 1000, 100, 10, 1
        };
        int started = 0;
        for (int p = 0; p < 10; p++) {
            unsigned int d = 0;
            while (val >= pow10[p]) {
                val -= pow10[p];
                d++;
            }
            if (d || started) {
                buf[i++] = '0' + d;
                started = 1;
            }
        }
        return i;
    }

    /* For hex, use shift/mask (avoids __aeabi_uidiv on ARM7TDMI) */
    if (base == 16) {
        while (val) {
            tmp[i++] = digits[val & 0xf];
            val >>= 4;
        }
        n = i;
        i = 0;
        while (n > 0)
            buf[i++] = tmp[--n];
        return i;
    }

    /* For other bases, use modulo (works fine for powers of 2) */
    while (val) {
        tmp[i++] = digits[val % base];
        val /= base;
    }
    n = i;
    i = 0;
    while (n > 0)
        buf[i++] = tmp[--n];
    return i;
}

/* ================================================================== */
/* vsnprintf — supports %s, %d, %u, %x, %X, %c, %p, %%              */
/*             with optional field width and zero-padding             */
/* ================================================================== */

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    size_t pos = 0;
    char tmp[24];
    int len, width, zero_pad;

#define PUT(c) do { if (pos < size - 1) buf[pos] = (c); pos++; } while(0)

    if (size == 0)
        return 0;

    while (*fmt) {
        if (*fmt != '%') {
            PUT(*fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Parse width and zero-padding */
        zero_pad = 0;
        width = 0;
        if (*fmt == '0') { zero_pad = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        switch (*fmt) {
        case 'd': {
            int val = va_arg(ap, int);
            if (val < 0) {
                PUT('-');
                val = -val;
            }
            len = _utoa((unsigned int)val, tmp, 10, 0);
            while (width > len) { PUT(zero_pad ? '0' : ' '); width--; }
            for (int i = 0; i < len; i++) PUT(tmp[i]);
            break;
        }
        case 'u': {
            unsigned int val = va_arg(ap, unsigned int);
            len = _utoa(val, tmp, 10, 0);
            while (width > len) { PUT(zero_pad ? '0' : ' '); width--; }
            for (int i = 0; i < len; i++) PUT(tmp[i]);
            break;
        }
        case 'x': {
            unsigned int val = va_arg(ap, unsigned int);
            len = _utoa(val, tmp, 16, 0);
            while (width > len) { PUT(zero_pad ? '0' : ' '); width--; }
            for (int i = 0; i < len; i++) PUT(tmp[i]);
            break;
        }
        case 'X': {
            unsigned int val = va_arg(ap, unsigned int);
            len = _utoa(val, tmp, 16, 1);
            while (width > len) { PUT(zero_pad ? '0' : ' '); width--; }
            for (int i = 0; i < len; i++) PUT(tmp[i]);
            break;
        }
        case 'p': {
            unsigned int val = (unsigned int)(uintptr_t)va_arg(ap, void *);
            PUT('0'); PUT('x');
            len = _utoa(val, tmp, 16, 0);
            for (int i = 0; i < len; i++) PUT(tmp[i]);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s) PUT(*s++);
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            PUT(c);
            break;
        }
        case '%':
            PUT('%');
            break;
        default:
            PUT('%');
            PUT(*fmt);
            break;
        }
        fmt++;
    }

    buf[(pos < size) ? pos : size - 1] = '\0';
    return (int)pos;
#undef PUT
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* ================================================================== */
/* sscanf — minimal: %u, %d, %[^...] with optional field width       */
/* ================================================================== */

int sscanf(const char *str, const char *fmt, ...)
{
    va_list ap;
    int matched = 0;

    va_start(ap, fmt);

    while (*fmt && *str) {
        if (*fmt == '%') {
            fmt++;
            int width = 0;
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');

            if (*fmt == 'u' || *fmt == 'd') {
                /* Unsigned/signed decimal */
                unsigned int *uptr = NULL;
                int *iptr = NULL;
                unsigned int val = 0;
                int sign = 1;

                if (*fmt == 'u') uptr = va_arg(ap, unsigned int *);
                else iptr = va_arg(ap, int *);

                if (*fmt == 'd' && *str == '-') { sign = -1; str++; }

                if (*str < '0' || *str > '9') break;
                while (*str >= '0' && *str <= '9')
                    val = val * 10 + (*str++ - '0');

                if (uptr) *uptr = val;
                if (iptr) *iptr = (int)val * sign;
                matched++;
                fmt++;
            }
            else if (*fmt == '[') {
                /* Character class: %[^...] only */
                fmt++;
                int negate = 0;
                if (*fmt == '^') { negate = 1; fmt++; }

                /* Collect the character set until ']' */
                char set[64];
                int slen = 0;
                while (*fmt && *fmt != ']' && slen < 63)
                    set[slen++] = *fmt++;
                set[slen] = '\0';
                if (*fmt == ']') fmt++;

                char *dst = va_arg(ap, char *);
                int count = 0;
                int maxw = width ? width : 0x7FFFFFFF;

                while (*str && count < maxw) {
                    /* Check if *str is in the set */
                    int in_set = 0;
                    for (int i = 0; i < slen; i++) {
                        if (*str == set[i]) { in_set = 1; break; }
                    }
                    if (negate ? in_set : !in_set) break;
                    dst[count++] = *str++;
                }
                dst[count] = '\0';
                if (count > 0) matched++;
            }
            else {
                break; /* unsupported format */
            }
        } else {
            /* Literal character match */
            if (*fmt == *str) {
                fmt++;
                str++;
            } else {
                break;
            }
        }
    }

    va_end(ap);
    return matched;
}

/* ================================================================== */
/* FILE I/O — minimal buffered I/O on top of raw syscalls             */
/* ================================================================== */

#define MINI_FILE_BUFSZ 256
#define MINI_MAX_FILES  8

/* O_* flags from fcntl.h */
#define _O_RDONLY  0
#define _O_WRONLY  1
#define _O_CREAT   0100
#define _O_TRUNC   01000

struct _mini_file {
    int  fd;
    int  mode;       /* 'r' or 'w' */
    char buf[MINI_FILE_BUFSZ];
    int  buf_pos;
    int  buf_len;
    int  in_use;
};

typedef struct _mini_file FILE;

static struct _mini_file _files[MINI_MAX_FILES];

/* Pre-initialized stdin/stdout/stderr */
static struct _mini_file _stdin_file  = { .fd = 0, .mode = 'r', .in_use = 1 };
static struct _mini_file _stdout_file = { .fd = 1, .mode = 'w', .in_use = 1 };
static struct _mini_file _stderr_file = { .fd = 2, .mode = 'w', .in_use = 1 };

FILE *stdin  = &_stdin_file;
FILE *stdout = &_stdout_file;
FILE *stderr = &_stderr_file;

FILE *fopen(const char *path, const char *mode)
{
    int fd, i;
    int flags;

    if (mode[0] == 'r')
        flags = _O_RDONLY;
    else if (mode[0] == 'w')
        flags = _O_WRONLY | _O_CREAT | _O_TRUNC;
    else
        return NULL;

    fd = open(path, flags, 0644);
    if (fd < 0)
        return NULL;

    for (i = 0; i < MINI_MAX_FILES; i++) {
        if (!_files[i].in_use) {
            _files[i].fd = fd;
            _files[i].mode = mode[0];
            _files[i].buf_pos = 0;
            _files[i].buf_len = 0;
            _files[i].in_use = 1;
            return &_files[i];
        }
    }

    close(fd);
    return NULL;
}

int fclose(FILE *f)
{
    if (!f || !f->in_use)
        return -1;
    /* Flush write buffer */
    if (f->mode == 'w' && f->buf_pos > 0)
        write(f->fd, f->buf, f->buf_pos);
    close(f->fd);
    f->in_use = 0;
    f->fd = -1;
    return 0;
}

char *fgets(char *s, int size, FILE *f)
{
    int i = 0;
    if (!f || size <= 0)
        return NULL;

    while (i < size - 1) {
        /* Refill buffer if empty */
        if (f->buf_pos >= f->buf_len) {
            f->buf_len = read(f->fd, f->buf, MINI_FILE_BUFSZ);
            f->buf_pos = 0;
            if (f->buf_len <= 0)
                break;
        }
        char c = f->buf[f->buf_pos++];
        s[i++] = c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return NULL;
    s[i] = '\0';
    return s;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
    size_t total = size * nmemb;
    size_t done = 0;
    char *dst = (char *)ptr;

    if (!f || !f->in_use || total == 0)
        return 0;

    /* Drain any buffered data first */
    while (done < total && f->buf_pos < f->buf_len) {
        dst[done++] = f->buf[f->buf_pos++];
    }

    /* Read the rest directly */
    while (done < total) {
        int n = read(f->fd, dst + done, total - done);
        if (n <= 0)
            break;
        done += n;
    }
    return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f)
{
    size_t total = size * nmemb;
    if (!f || !f->in_use || total == 0)
        return 0;
    /* Flush any buffered data, then write directly */
    if (f->mode == 'w' && f->buf_pos > 0) {
        write(f->fd, f->buf, f->buf_pos);
        f->buf_pos = 0;
    }
    int n = write(f->fd, ptr, total);
    return (n > 0) ? (size_t)n / size : 0;
}

int fputs(const char *s, FILE *f)
{
    if (!f || !f->in_use || !s)
        return -1;
    size_t len = strlen(s);
    if (len == 0)
        return 0;
    if (f->mode == 'w' && f->buf_pos > 0) {
        write(f->fd, f->buf, f->buf_pos);
        f->buf_pos = 0;
    }
    return write(f->fd, s, len);
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n > 0 && f && f->in_use)
        write(f->fd, buf, n < 512 ? n : 511);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n > 0)
        write(1, buf, n < 512 ? n : 511);
    return n;
}

int dprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n > 0)
        write(fd, buf, n < 512 ? n : 511);
    return n;
}

int remove(const char *path)
{
    return unlink(path);
}

/* ================================================================== */
/* mmap — old_mmap wrapper (6-arg struct packing for NR 90)           */
/* ================================================================== */

struct mmap_arg_struct {
    unsigned long addr;
    unsigned long len;
    unsigned long prot;
    unsigned long flags;
    unsigned long fd;
    unsigned long offset;
};

extern long _sys_mmap(struct mmap_arg_struct *args);

void *mmap(void *addr, unsigned long len, int prot, int flags,
           int fd, unsigned long offset)
{
    struct mmap_arg_struct args;
    long ret;

    args.addr   = (unsigned long)addr;
    args.len    = len;
    args.prot   = (unsigned long)prot;
    args.flags  = (unsigned long)flags;
    args.fd     = (unsigned long)fd;
    args.offset = offset;

    ret = _sys_mmap(&args);
    if (ret < 0 && ret > -4096)
        return (void *)-1;  /* MAP_FAILED */
    return (void *)ret;
}

/* ================================================================== */
/* usleep — implemented via nanosleep                                 */
/* ================================================================== */

struct _timespec {
    long tv_sec;
    long tv_nsec;
};

int usleep(unsigned int usec)
{
    struct _timespec ts;
    ts.tv_sec  = usec / 1000000;
    ts.tv_nsec = (usec % 1000000) * 1000;
    return nanosleep(&ts, NULL);
}

/* ================================================================== */
/* Memory allocation — simple sbrk-based bump allocator               */
/* ================================================================== */

/* Static heap — brk() may not work on no-MMU uClinux.
 * 32KB is enough for thread stacks and small allocations. */
static char _heap_buf[32768] __attribute__((aligned(8)));
static size_t _heap_used = 0;

void *malloc(size_t size)
{
    /* Align to 8 bytes */
    size = (size + 7) & ~(size_t)7;

    if (_heap_used + size > sizeof(_heap_buf)) {
        errno = 12; /* ENOMEM */
        return NULL;
    }

    void *ptr = &_heap_buf[_heap_used];
    _heap_used += size;
    return ptr;
}

void free(void *ptr)
{
    /* Bump allocator — no-op. Thread stacks are the only allocations
     * and they live for the entire process lifetime. */
    (void)ptr;
}

/* ================================================================== */
/* AEABI helper aliases (compiler may generate calls to these)        */
/* ================================================================== */

void __aeabi_memcpy(void *dst, const void *src, size_t n)
{
    memcpy(dst, src, n);
}

void __aeabi_memcpy4(void *dst, const void *src, size_t n)
{
    memcpy(dst, src, n);
}

void __aeabi_memcpy8(void *dst, const void *src, size_t n)
{
    memcpy(dst, src, n);
}

/* Note: __aeabi_memset has REVERSED args vs standard memset */
void __aeabi_memset(void *dst, size_t n, int c)
{
    memset(dst, c, n);
}

void __aeabi_memset4(void *dst, size_t n, int c)
{
    memset(dst, c, n);
}

void __aeabi_memset8(void *dst, size_t n, int c)
{
    memset(dst, c, n);
}

void __aeabi_memclr(void *dst, size_t n)
{
    memset(dst, 0, n);
}

void __aeabi_memclr4(void *dst, size_t n)
{
    memset(dst, 0, n);
}

void __aeabi_memclr8(void *dst, size_t n)
{
    memset(dst, 0, n);
}

/*
 * raise() — required by libgcc's __aeabi_ldiv0 (divide-by-zero).
 * On the device we just exit; division by zero is a fatal bug.
 */
extern void _exit(int status);

int raise(int sig)
{
    (void)sig;
    _exit(99);
    return 0; /* unreachable */
}
