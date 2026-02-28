/*
 * Art-Net receiver
 *
 * Listens for Art-Net ArtDmx packets and extracts DMX data.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SN110_ARTNET_H
#define SN110_ARTNET_H

#include "../common.h"

/* Art-Net constants */
#define ARTNET_PORT             6454
#define ARTNET_MAGIC            "Art-Net"
#define ARTNET_MAGIC_LEN        8       /* Including null terminator */
#define ARTNET_PROTOCOL_VERSION 14

/* Art-Net OpCodes */
#define ARTNET_OP_POLL          0x2000
#define ARTNET_OP_POLL_REPLY    0x2100
#define ARTNET_OP_DMX           0x5000

/* Parsed Art-Net DMX packet */
typedef struct {
    uint8_t  sequence;
    uint8_t  physical;
    uint16_t universe;           /* 15-bit: Net(7) | SubNet(4) | Universe(4) */
    uint8_t  dmx_data[DMX_UNIVERSE_SIZE];
    uint16_t dmx_length;
} artnet_dmx_packet_t;

/*
 * Initialize Art-Net receiver.
 * Creates a UDP socket bound to port 6454.
 * Returns socket fd >= 0 on success, -1 on error.
 */
int artnet_init(void);

/*
 * Receive and parse a single Art-Net ArtDmx packet.
 * Non-ArtDmx packets (e.g., ArtPoll) are handled internally.
 * Returns 0 on success, -1 on error/timeout.
 */
int artnet_receive(int sock_fd, artnet_dmx_packet_t *packet);

/*
 * Clean up Art-Net receiver.
 */
void artnet_cleanup(int sock_fd);

#endif /* SN110_ARTNET_H */
