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

/* Channel offsets from base (each channel is a 16-word block = 64 bytes).
 * Mapping: /dev/dmx0 → CH1 (offset 0x00), /dev/dmx1 → CH2 (offset 0x40).
 * TODO: verify this mapping on hardware — the kernel minor numbers
 * (dmx0=minor 1, dmx1=minor 0) hint it could be reversed. */
#define SER_CH1_OFFSET      0x00
#define SER_CH2_OFFSET      0x40

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
#define CTLA_IE_RX_ALL      0x0000FF00
#define CTLA_IE_TX_ALL      0x0000001F

/* Status A bits */
#define STATA_RX_BRK        0x00008000  /* Break detected */
#define STATA_RX_FRMERR     0x00004000  /* Framing error */
#define STATA_RX_OVERRUN    0x00001000  /* FIFO overrun */
#define STATA_RX_RDY        0x00000800  /* Data in RX FIFO */
#define STATA_TX_RDY        0x00000008  /* TX FIFO has room (NOT shift reg empty) */

/* RXFDB — bytes valid in last FIFO word */
#define STATA_RXFDB_MASK    0x00300000
#define STATA_RXFDB_SHIFT   20

/* Status A — bits to write-1-to-clear */
#define STATA_CLR_ALL       (STATA_RX_BRK | STATA_RX_FRMERR | \
                             STATA_RX_OVERRUN | 0x00000400 | \
                             0x00000200 | 0x00000100 | \
                             0x00000080 | 0x00000040 | 0x00000020 | \
                             0x00000010)

/* Control B bits */
#define CTLB_UART_MODE      0x00000000
#define CTLB_RBGT_EN        0x08000000  /* RX buffer gap timer enable */
#define CTLB_RCGT_EN        0x04000000  /* RX char gap timer — the 6ms culprit */

/* Bitrate register bits */
#define BR_ENABLE           0x80000000
#define BR_TMODE            0x40000000  /* Test mode (used in ref driver) */
#define BR_CLK_EXT_5        0x00000000  /* Use XTAL/5 directly (PLL bypass) */
#define BR_MASK             0x000007FF

/* FIFO depth: 32 bytes, read/written as 4-byte words */
#define FIFO_DEPTH          32

/* DMX timing (in microseconds) */
#define DMX_BREAK_US        92   /* Break: minimum 88us, we use 92 */
#define DMX_MAB_US          12   /* Mark After Break: minimum 8us */

/* Crystal frequency (NS7520 standard) */
#define XTAL_FREQ           18432000

/* DMX baud rate and divisor.
 * Formula: N = (Fxtal / (Fbaud * 10)) - 1  (from HW Ref Guide 7.5.4)
 * With PLL bypass: N = (18432000 / (250000 * 10)) - 1 = 6.3728 → 6
 * Actual baud: 18432000 / (10 * 7) = 263314 — within DMX tolerance.
 * The BR_X16 macro from the kernel divides by an additional 16, giving:
 *   ((18432000 / (250000 * 10)) - 1) / 16 = 0
 * This means the raw divisor field is 0 and baud is set by the base formula. */
#define DMX_BAUD            250000
#define DMX_BAUD_REG        (BR_ENABLE | BR_CLK_EXT_5 | 0)

/* Bit time at 250kbaud: 4us per bit, 44us per byte (1 start + 8 data + 2 stop) */
#define DMX_BYTE_US         44

/* Forward declaration — dmx_ops_t defined in dmx.h */
#include "dmx.h"
const dmx_ops_t *dmx_direct_get_ops(void);

#endif /* SN110_DMX_DIRECT_H */
