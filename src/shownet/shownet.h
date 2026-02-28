/*
 * ShowNet receiver (legacy compatibility)
 *
 * Decodes Strand ShowNet protocol packets with RLE-compressed DMX data.
 * Based on reverse engineering by nickvsnetworking / PyShowNet.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SN110_SHOWNET_H
#define SN110_SHOWNET_H

#include "../common.h"

/* ShowNet constants */
#define SHOWNET_PORT            2501
#define SHOWNET_MAX_SLOTS       18432   /* 36 universes × 512 */

/* Parsed ShowNet packet */
typedef struct {
    uint16_t net_slot;          /* Starting network slot (1-18432) */
    uint8_t  dmx_data[DMX_UNIVERSE_SIZE];
    uint16_t dmx_length;        /* Number of decoded DMX channels */
} shownet_packet_t;

/*
 * Initialize ShowNet receiver.
 * Creates a UDP socket listening on port 2501.
 * Returns socket fd >= 0 on success, -1 on error.
 */
int shownet_init(void);

/*
 * Receive and parse a single ShowNet packet.
 * Handles RLE decompression of DMX data.
 * Returns 0 on success, -1 on error/timeout.
 */
int shownet_receive(int sock_fd, shownet_packet_t *packet);

/*
 * Decode RLE-compressed ShowNet DMX data.
 * Returns number of decoded bytes, or -1 on error.
 */
int shownet_decode_rle(const uint8_t *encoded, int encoded_len,
                       uint8_t *decoded, int max_decoded_len);

/*
 * Clean up ShowNet receiver.
 */
void shownet_cleanup(int sock_fd);

#endif /* SN110_SHOWNET_H */
