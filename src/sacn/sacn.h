/*
 * sACN (E1.31) receiver
 *
 * Listens for sACN multicast packets and extracts DMX data.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SN110_SACN_H
#define SN110_SACN_H

#include "../common.h"

/* sACN constants */
#define SACN_PORT               5568
#define SACN_MULTICAST_BASE     0xEFFF0000  /* 239.255.0.0 */
#define SACN_MAX_PRIORITY       200
#define SACN_DEFAULT_PRIORITY   100
#define SACN_TIMEOUT_MS         2500        /* Source considered lost after 2.5s */

/* sACN packet identification */
#define SACN_ACN_IDENTIFIER     "ASC-E1.17\0\0\0"
#define SACN_ACN_IDENTIFIER_LEN 12
#define SACN_PREAMBLE_SIZE      0x0010

/* Root layer vectors */
#define VECTOR_ROOT_E131_DATA   0x00000004
#define VECTOR_ROOT_E131_EXTENDED 0x00000008

/* Framing layer vectors */
#define VECTOR_E131_DATA_PACKET 0x00000002

/* DMP layer vectors */
#define VECTOR_DMP_SET_PROPERTY 0x02

/* Parsed sACN packet */
typedef struct {
    uint8_t  cid[16];                /* Component Identifier */
    char     source_name[64];        /* Human-readable source name */
    uint8_t  priority;               /* 0-200 */
    uint8_t  sequence;               /* Sequence number */
    uint16_t universe;               /* Universe number */
    uint8_t  options;                /* Bit flags */
    uint8_t  start_code;             /* DMX start code (normally 0) */
    uint8_t  dmx_data[DMX_UNIVERSE_SIZE];
    uint16_t dmx_length;             /* Number of DMX channels */
} sacn_packet_t;

/*
 * Initialize sACN receiver.
 * Creates a UDP socket and joins the multicast group for the given universe.
 * Returns socket fd >= 0 on success, -1 on error.
 */
int sacn_init(uint16_t universe);

/*
 * Parse a raw sACN packet buffer into a sacn_packet_t.
 * Returns 0 on success, -1 on invalid/malformed packet.
 */
int sacn_parse(const uint8_t *buf, int len, sacn_packet_t *pkt);

/*
 * Receive and parse a single sACN packet.
 * Blocks until a packet is received or timeout.
 * Returns 0 on success, -1 on error/timeout.
 */
int sacn_receive(int sock_fd, sacn_packet_t *packet);

/*
 * Calculate the multicast address for a given sACN universe.
 * Returns address in network byte order.
 */
uint32_t sacn_multicast_addr(uint16_t universe);

/*
 * Clean up sACN receiver.
 */
void sacn_cleanup(int sock_fd);

#endif /* SN110_SACN_H */
