/*
 * SN110 DMX kernel driver ioctl interface
 *
 * Reverse-engineered from the dmxtst binary (bFLT, ARM7TDMI).
 * The NS7520 kernel driver uses a custom character device for each DMX port
 * (/dev/dmx0, /dev/dmx1) with a single ioctl for mode configuration.
 *
 * ioctl number: _IOW('d', 1, struct dmx_config) = 0x40106401
 *
 * Data I/O is via standard read()/write() of 512-byte DMX frames.
 * Blocking mode is controlled via FIONBIO (0x5421).
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SN110_DMX_IOCTL_H
#define SN110_DMX_IOCTL_H

#include <stdint.h>

/*
 * 16-byte config struct passed to the DMX ioctl.
 *
 * Struct layout determined by tracing register/memory writes in dmxtst
 * for each mode (off, raw, tx, rx):
 *
 *   offset 0  (uint8_t):  mode       — 0=OFF, 1=RAW, 2=TX, 3=RX
 *   offset 1  (uint8_t):  flags      — always 0 in dmxtst
 *   offset 2  (uint16_t): buf_size   — 512 for RAW, 0 for TX/RX/OFF
 *   offset 4  (uint16_t): rate       — 100 for RAW, 0 for TX/RX/OFF
 *   offset 6  (char[8]):  label      — up to 8-char label from CLI
 *   offset 14 (uint8_t):  reserved   — always 0
 *   offset 15 (uint8_t):  pad        — struct padding
 */
struct dmx_config {
    uint8_t  mode;          /* DMX_MODE_OFF/RAW/TX/RX */
    uint8_t  flags;
    uint16_t buf_size;      /* 512 for RAW mode */
    uint16_t rate;          /* 100 for RAW mode (units unknown, possibly Hz) */
    char     label[8];
    uint8_t  reserved;
    uint8_t  pad;
};

/*
 * Construct the ioctl number at compile time.
 * On ARM Linux 2.0: _IOC(dir, type, nr, size)
 *   dir  = bits 30-31
 *   size = bits 16-29
 *   type = bits 8-15
 *   nr   = bits 0-7
 */
#define _IOC_WRITE  1
#define DMX_IOC_SET_CONFIG \
    ((_IOC_WRITE << 30) | (sizeof(struct dmx_config) << 16) | ('d' << 8) | 1)
/* = 0x40106401 */

/* FIONBIO for blocking control */
#define SN110_FIONBIO  0x5421

#endif /* SN110_DMX_IOCTL_H */
