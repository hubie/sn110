/*
 * syscalls.S — OABI syscall stubs for ARM Linux 2.0
 *
 * On ARM OABI, syscalls are invoked via SWI 0x900000 + NR.
 * Arguments go in r0-r5, return value in r0.
 * On error, r0 contains -errno (range -1 to -4095).
 *
 * Returns raw kernel value: negative = -errno on error.
 * C code checks: if (ret < 0) handle error.
 * No errno global is set by these stubs — this avoids a text-section
 * relocation (literal pool for &errno) that bFLT v2 cannot handle.
 *
 * The EABI equivalent (SWI 0, NR in r7) is NOT supported by Linux 2.0.
 *
 * Syscall numbers from arch/arm/include/asm/unistd.h (Linux 2.0):
 *   exit=1, read=3, write=4, open=5, close=6, waitpid=7,
 *   unlink=10, signal=48, ioctl=54, gettimeofday=78,
 *   socketcall=102, clone=120, _newselect=142, nanosleep=162
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

    .text

/* ================================================================== */
/* Macro: simple syscall (up to 4 args in r0-r3)                      */
/* Returns r0 directly (negative on error).                            */
/* ================================================================== */

.macro SYSCALL name, nr
    .global \name
    .type \name, %function
\name:
    swi     #(0x900000 + \nr)
    bx      lr
    .size \name, . - \name
.endm

/* ================================================================== */
/* Macro: 5-arg syscall (r0-r3 + 5th arg from caller's stack → r4)   */
/* ================================================================== */

.macro SYSCALL5 name, nr
    .global \name
    .type \name, %function
\name:
    stmfd   sp!, {r4, lr}
    ldr     r4, [sp, #8]       /* 5th arg: sp was decremented by 8 */
    swi     #(0x900000 + \nr)
    ldmfd   sp!, {r4, lr}
    bx      lr
    .size \name, . - \name
.endm

/* ================================================================== */
/* File I/O                                                           */
/* ================================================================== */

SYSCALL read, 3             /* ssize_t read(int fd, void *buf, size_t n) */
SYSCALL write, 4            /* ssize_t write(int fd, const void *buf, size_t n) */
SYSCALL open, 5             /* int open(const char *path, int flags, mode_t mode) */
SYSCALL close, 6            /* int close(int fd) */
SYSCALL unlink, 10          /* int unlink(const char *path) */
SYSCALL ioctl, 54           /* int ioctl(int fd, unsigned long req, void *arg) */

/* ================================================================== */
/* Process management                                                 */
/* ================================================================== */

    .global _exit
    .type _exit, %function
_exit:
    swi     #0x900001           /* __NR_exit = 1 — never returns */
1:  b       1b
    .size _exit, . - _exit

    /* Alias: exit() calls _exit() */
    .global exit
    .type exit, %function
exit:
    swi     #0x900001
1:  b       1b
    .size exit, . - exit

SYSCALL waitpid, 7          /* pid_t waitpid(pid_t pid, int *status, int opts) */
SYSCALL getpid, 20          /* pid_t getpid(void) */

/* ================================================================== */
/* Signals                                                            */
/* ================================================================== */

SYSCALL signal, 48          /* sighandler_t signal(int sig, sighandler_t handler) */

/* ================================================================== */
/* Time                                                               */
/* ================================================================== */

SYSCALL gettimeofday, 78    /* int gettimeofday(struct timeval *tv, void *tz) */
SYSCALL nanosleep, 162      /* int nanosleep(struct timespec *req, *rem) */

/* ================================================================== */
/* select — 5 args, uses _newselect (NR 142)                         */
/* ================================================================== */

SYSCALL5 select, 142

/* ================================================================== */
/* Filesystem                                                         */
/* ================================================================== */

SYSCALL rename, 38          /* int rename(const char *old, const char *new) */

/* ================================================================== */
/* Memory mapping                                                     */
/* ================================================================== */

/*
 * old_mmap (NR 90): takes pointer to 6-arg struct, not register args.
 * C wrapper in minilib.c packs args into struct and calls this.
 */
SYSCALL _sys_mmap, 90       /* void *old_mmap(struct mmap_arg_struct *) */
SYSCALL munmap, 91           /* int munmap(void *addr, size_t length) */

/* ================================================================== */
/* Memory — brk is special (doesn't return -errno)                    */
/* ================================================================== */

    .global _sys_brk
    .type _sys_brk, %function
_sys_brk:
    swi     #0x90002D           /* __NR_brk = 45 */
    bx      lr                  /* always returns new break address */
    .size _sys_brk, . - _sys_brk

/* ================================================================== */
/* Networking — socketcall multiplexer (NR 102)                       */
/* ================================================================== */

    .global _sys_socketcall
    .type _sys_socketcall, %function
_sys_socketcall:
    swi     #0x900066           /* __NR_socketcall = 102 (0x66) */
    bx      lr
    .size _sys_socketcall, . - _sys_socketcall

/* ================================================================== */
/* Threading — clone (NR 120)                                         */
/*                                                                    */
/* _thread_create(stack_top, fn, arg)                                 */
/*   r0 = top of pre-allocated child stack                            */
/*   r1 = void *(*fn)(void *)                                        */
/*   r2 = void *arg                                                   */
/*   Returns: child PID to parent; child never returns here           */
/* ================================================================== */

    .global _thread_create
    .type _thread_create, %function
_thread_create:
    /* Save callee-saved registers */
    stmfd   sp!, {r4-r6, lr}

    /* r0=stack_top, r1=fn, r2=arg */
    /* Place fn and arg on the child stack (stack grows down) */
    bic     r0, r0, #7         /* align stack to 8 bytes */
    str     r2, [r0, #-4]!     /* push arg  → child sp[4] */
    str     r1, [r0, #-4]!     /* push fn   → child sp[0] */

    /* clone(flags, child_stack) */
    mov     r1, r0              /* r1 = child_stack */
    ldr     r0, .Lclone_flags   /* r0 = CLONE_VM|FS|FILES|SIGHAND */
    swi     #0x900078           /* __NR_clone = 120 (0x78) */

    /* r0 = 0 → child, r0 > 0 → parent, r0 < 0 → error */
    cmp     r0, #0
    blt     .Lclone_err

    /* r0 = 0 → child, r0 > 0 → parent */
    cmp     r0, #0
    bne     .Lparent

    /* ---- Child path ---- */
    /* sp = child_stack, sp[0]=fn, sp[4]=arg */
    ldr     r4, [sp, #0]       /* fn */
    ldr     r0, [sp, #4]       /* arg (first parameter to fn) */
    add     sp, sp, #8         /* pop fn/arg off stack */
    mov     lr, pc
    bx      r4                  /* call fn(arg) */

    /* fn returned — exit child */
    mov     r0, #0
    swi     #0x900001           /* _exit(0) */
1:  b       1b

.Lparent:
    ldmfd   sp!, {r4-r6, lr}
    bx      lr

.Lclone_err:
    ldmfd   sp!, {r4-r6, lr}
    bx      lr                  /* return negative errno in r0 */

.Lclone_flags:
    /* CLONE_VM=0x100 | CLONE_FS=0x200 | CLONE_FILES=0x400 | CLONE_SIGHAND=0x800 */
    .word   0x00000F00

    .size _thread_create, . - _thread_create

/* Inform linker we don't need executable stack */
    .section .note.GNU-stack, "", %progbits
