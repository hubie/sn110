/*
 * sACN (E1.31) transmitter
 *
 * Builds and sends sACN multicast packets from DMX input data.
 * Used for the DMX-In → sACN direction.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SN110_SACN_TX_H
#define SN110_SACN_TX_H

#include "../common.h"

typedef struct {
    int       sock_fd;
    uint8_t   sequence;
    uint16_t  universe;
    uint8_t   cid[16];
    char      source_name[64];
    uint8_t   priority;
} sacn_tx_t;

/*
 * Initialize sACN transmitter.
 * Creates a UDP socket for multicast output.
 * Returns 0 on success, -1 on error.
 */
int sacn_tx_init(sacn_tx_t *tx, uint16_t universe, const char *source_name);

/*
 * Build an sACN packet into out_buf.
 * Returns packet length on success, -1 on error.
 */
int sacn_tx_build_packet(sacn_tx_t *tx, const uint8_t *dmx_data, int dmx_len,
                         uint8_t *out_buf, int buf_size);

/*
 * Build and send an sACN packet with the given DMX data.
 * Increments the sequence counter.
 * Returns 0 on success, -1 on error.
 */
int sacn_tx_send(sacn_tx_t *tx, const uint8_t *dmx_data, int dmx_len);

/*
 * Clean up sACN transmitter (close socket).
 */
void sacn_tx_cleanup(sacn_tx_t *tx);

#endif /* SN110_SACN_TX_H */
