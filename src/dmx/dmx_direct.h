/*
 * NS7520 UART register definitions for direct DMX access
 *
 * Extracted from the NetSilicon/Red Hat netarm_ser_module.h reference header.
 * Only the registers and bits needed for DMX 250kbaud 8N2 operation.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SN110_DMX_DIRECT_H
#define SN110_DMX_DIRECT_H

#include <stdint.h>

/* NS7520 Serial module base address */
#define SER_MODULE_BASE     0xFFD00000

/* Channel offsets from base (each channel is a 16-word block = 64 bytes) */
#define SER_CH1_OFFSET      0x00    /* Channel 1 — /dev/dmx0 (minor 1) */
#define SER_CH2_OFFSET      0x40    /* Channel 2 — /dev/dmx1 (minor 0) */

/* Register offsets within a channel (32-bit registers) */
#define REG_CTRL_A          0x00
#define REG_CTRL_B          0x04
#define REG_STATUS_A        0x08
#define REG_BITRATE         0x0C
#define REG_FIFO            0x10
#define REG_RX_BUF_TMR      0x14
#define REG_RX_CHAR_TMR     0x18

/* Control A bits */
#define CTLA_ENABLE         0x80000000
#define CTLA_BRK            0x40000000  /* Force break on TX line */
#define CTLA_P_NONE         0x00000000  /* No parity */
#define CTLA_2STOP          0x00000000  /* 2 stop bits (errata: naming swapped) */
#define CTLA_8BITS          0x03000000  /* 8 data bits */

/* Control A — interrupt enable bits (clear these to prevent kernel ISR) */
#define CTLA_IE_RX_BRK      0x00008000
#define CTLA_IE_RX_FRMERR   0x00004000
#define CTLA_IE_RX_PARERR   0x00002000
#define CTLA_IE_RX_OVERRUN  0x00001000
#define CTLA_IE_RX_RDY      0x00000800
#define CTLA_IE_RX_HALF     0x00000400
#define CTLA_IE_RX_FULL     0x00000200
#define CTLA_IE_RX_DMAEN    0x00000100
#define CTLA_IE_RX_ALL      0x0000FF00
#define CTLA_IE_TX_ALL      0x0000001F

/* Status A bits */
#define STATA_RX_BRK        0x00008000  /* Break detected */
#define STATA_RX_FRMERR     0x00004000  /* Framing error */
#define STATA_RX_OVERRUN    0x00001000  /* FIFO overrun */
#define STATA_RX_RDY        0x00000800  /* Data in RX FIFO */
#define STATA_RX_HALF       0x00000400  /* RX FIFO half full */
#define STATA_RX_FULL       0x00000100  /* RX FIFO full */
#define STATA_TX_RDY        0x00000008  /* TX FIFO has room */
#define STATA_TX_HALF       0x00000004  /* TX FIFO half empty */
#define STATA_TX_EMPTY      0x00000008  /* TX FIFO empty (same as TX_RDY) */

/* RXFDB — bytes valid in last FIFO word */
#define STATA_RXFDB_MASK    0x00300000
#define STATA_RXFDB_SHIFT   20
/* RXFDB values: 1=1byte, 2=2bytes, 3=3bytes, 0=4bytes */

/* Status A — bits to write-1-to-clear */
#define STATA_CLR_ALL       (STATA_RX_BRK | STATA_RX_FRMERR | \
                             STATA_RX_OVERRUN | STATA_RX_HALF | \
                             STATA_RX_FULL | 0x00000200 | \
                             0x00000080 | 0x00000040 | 0x00000020 | \
                             0x00000010)

/* Control B bits */
#define CTLB_UART_MODE      0x00000000
#define CTLB_RBGT_EN        0x08000000  /* RX buffer gap timer enable */
#define CTLB_RCGT_EN        0x04000000  /* RX char gap timer enable — the 6ms culprit */

/* Bitrate register bits */
#define BR_ENABLE           0x80000000
#define BR_TMODE            0x40000000
#define BR_RX_CLK_INT       0x00000000
#define BR_TX_CLK_INT       0x00000000
#define BR_CLK_EXT_5        0x00000000  /* Use XTAL/5 directly */
#define BR_MASK             0x000007FF

/* FIFO depth: 32 bytes, read/written as 4-byte words */
#define FIFO_DEPTH          32

/* DMX timing (in microseconds) */
#define DMX_BREAK_US        92   /* Break: minimum 88µs, we use 92 */
#define DMX_MAB_US          12   /* Mark After Break: minimum 8µs */

/* Crystal frequency (NS7520 standard) */
#define XTAL_FREQ           18432000

/* DMX baud rate */
#define DMX_BAUD            250000

/* Baud divisor: N = (Fxtal / (Fbaud * 10)) - 1
 * With 18.432 MHz and PLL bypass: N = (18432000 / 2500000) - 1 ≈ 6
 * Actual: 18432000 / (10 * 7) = 263314 baud — close enough for DMX tolerance
 * Note: direct driver reads the kernel's configured baud value on startup */

/* Forward declaration — dmx_ops_t defined in dmx.h */
#include "dmx.h"
const dmx_ops_t *dmx_direct_get_ops(void);

#endif /* SN110_DMX_DIRECT_H */
