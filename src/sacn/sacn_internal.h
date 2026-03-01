/*
 * sACN (E1.31) shared internal definitions
 *
 * Packet offsets and byte-order helpers used by both the receiver
 * (sacn.c) and transmitter (sacn_tx.c).
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SN110_SACN_INTERNAL_H
#define SN110_SACN_INTERNAL_H

#include <stdint.h>

/* sACN packet offsets (E1.31-2016 Table 4-1 through 4-3) */
#define OFF_PREAMBLE        0
#define OFF_POSTAMBLE       2
#define OFF_ACN_ID          4
#define OFF_ROOT_FLAGS_LEN  16
#define OFF_ROOT_VECTOR     18
#define OFF_CID             22

#define OFF_FRAME_FLAGS_LEN 38
#define OFF_FRAME_VECTOR    40
#define OFF_SOURCE_NAME     44
#define OFF_PRIORITY        108
#define OFF_SYNC_ADDR       109
#define OFF_SEQUENCE        111
#define OFF_OPTIONS         112
#define OFF_UNIVERSE        113

#define OFF_DMP_FLAGS_LEN   115
#define OFF_DMP_VECTOR      117
#define OFF_DMP_ADDR_DATA   118
#define OFF_DMP_FIRST_ADDR  119
#define OFF_DMP_ADDR_INC    121
#define OFF_DMP_PROP_COUNT  123
#define OFF_DMP_START_CODE  125
#define OFF_DMP_DMX_DATA    126

#define SACN_MIN_PACKET_LEN 126
#define SACN_MAX_PACKET_LEN 638

/* Network byte order helpers for potentially-unaligned access */
static inline uint16_t read_u16_be(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static inline void write_u16_be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static inline void write_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFF);
}

#endif /* SN110_SACN_INTERNAL_H */
