/*
 * crt0.S — C runtime startup for OABI ARM Linux (uClinux/bFLT)
 *
 * Entry point for flat binaries. On this uClinux (Linux 2.0), the
 * kernel passes arguments in registers:
 *   r0 = argc
 *   r1 = argv
 *   r2 = envp
 *
 * The stack is set up but does NOT contain argc/argv in standard
 * ELF layout. This was verified by comparing with the entry point
 * of the device's original dmxtst binary.
 *
 * Sets sl (r10) = GOT base for -fpic -msingle-pic-base code.
 * All C code compiled with these flags uses sl to access globals
 * through the GOT, keeping all relocations in the data section
 * (required by bFLT v2 format).
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

    .text
    .global _start
    .type _start, %function

_start:
    /* Clear the frame pointer for clean backtraces */
    mov     fp, #0

    /* Set sl (r10) = GOT base for PIC code */
    ldr     sl, .Lgot_off
.Lgot_pc:
    add     sl, pc, sl

    /* argc is already in r0, argv in r1 from the kernel */

    /* Align stack to 8 bytes (AAPCS requirement) */
    bic     sp, sp, #7

    /* Call main(argc, argv) */
    bl      main

    /* Exit with main's return value (already in r0) */
    swi     #0x900001           /* __NR_exit = 1 */

    /* Should never reach here */
1:  b       1b

    .align 2
.Lgot_off:
    .word   _GLOBAL_OFFSET_TABLE_ - (.Lgot_pc + 8)

    .size _start, . - _start

/* Inform linker we don't need executable stack */
    .section .note.GNU-stack, "", %progbits
