/*
 * ShowNet receiver implementation (legacy Strand compatibility)
 *
 * Decodes Strand ShowNet protocol packets. ShowNet uses a proprietary
 * format with RLE-compressed DMX data. This implementation is based on
 * reverse engineering by the PyShowNet project and nickvsnetworking.com.
 *
 * ShowNet packet format (simplified):
 *   Bytes 0-1:   Node type (0x8002 = SN110)
 *   Bytes 2-4:   IP address (last 3 octets)
 *   Byte  5:     Slot length (2 = 16-bit slots)
 *   Bytes 6-7:   Net slot (starting DMX address, 1-based)
 *   Bytes 8-9:   Slot count (number of DMX channels)
 *   Byte  10:    Index block (packet index, typically 0)
 *   Byte  11:    Number of blocks in this transfer
 *   Bytes 12-13: Block counter
 *   Byte  14:    Flags (bit 0: RLE compressed)
 *   Bytes 15+:   DMX data (possibly RLE-compressed)
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include "shownet.h"
#include "../common.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#ifdef HOST_BUILD
#include <stdio.h>
#define LOG(fmt, ...) fprintf(stderr, "shownet: " fmt "\n", ##__VA_ARGS__)
#else
#define LOG(fmt, ...) dprintf(2, "shownet: " fmt "\n", ##__VA_ARGS__)
#endif

/* ShowNet packet offsets */
#define OFF_NODE_TYPE       0
#define OFF_IP_OCTETS       2
#define OFF_SLOT_LEN        5
#define OFF_NET_SLOT        6
#define OFF_SLOT_COUNT      8
#define OFF_INDEX_BLOCK     10
#define OFF_NUM_BLOCKS      11
#define OFF_BLOCK_COUNTER   12
#define OFF_FLAGS           14
#define OFF_DATA            15

/* Flags */
#define SHOWNET_FLAG_RLE    0x01

#define SHOWNET_MIN_PACKET  16

static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

int shownet_decode_rle(const uint8_t *encoded, int encoded_len,
                       uint8_t *decoded, int max_decoded_len)
{
    int in_pos = 0;
    int out_pos = 0;

    while (in_pos < encoded_len && out_pos < max_decoded_len) {
        uint8_t b = encoded[in_pos++];

        if (b == 0x80 && in_pos + 1 < encoded_len) {
            /* RLE: 0x80, count, value */
            int count = encoded[in_pos++];
            uint8_t value = encoded[in_pos++];
            while (count > 0 && out_pos < max_decoded_len) {
                decoded[out_pos++] = value;
                count--;
            }
        } else {
            decoded[out_pos++] = b;
        }
    }

    return out_pos;
}

int shownet_init(void)
{
    int sock;
    int reuse = 1;
    int bcast = 1;
    struct sockaddr_in addr;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return -1;

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SHOWNET_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG("bind failed: %d", errno);
        close(sock);
        return -1;
    }

    /* Set non-blocking — select() is broken on Linux 2.0/uClinux for UDP */
    {
        int nonblock = 1;
        ioctl(sock, 0x5421, &nonblock); /* FIONBIO */
    }

    LOG("listening on port %d", SHOWNET_PORT);
    return sock;
}

int shownet_receive(int sock_fd, shownet_packet_t *pkt)
{
    uint8_t buf[1500];
    int n;
    uint16_t net_slot, slot_count;
    uint8_t flags;
    int data_len;

    n = recv(sock_fd, buf, sizeof(buf), 0);
    if (n < SHOWNET_MIN_PACKET)
        return -1;

    net_slot = read_u16_le(buf + OFF_NET_SLOT);
    slot_count = read_u16_le(buf + OFF_SLOT_COUNT);
    flags = buf[OFF_FLAGS];
    data_len = n - OFF_DATA;

    pkt->net_slot = net_slot;

    if (flags & SHOWNET_FLAG_RLE) {
        pkt->dmx_length = shownet_decode_rle(buf + OFF_DATA, data_len,
                                              pkt->dmx_data, DMX_UNIVERSE_SIZE);
    } else {
        if (data_len > DMX_UNIVERSE_SIZE)
            data_len = DMX_UNIVERSE_SIZE;
        if ((uint16_t)data_len > slot_count)
            data_len = slot_count;
        memcpy(pkt->dmx_data, buf + OFF_DATA, data_len);
        pkt->dmx_length = data_len;
    }

    return 0;
}

void shownet_cleanup(int sock_fd)
{
    if (sock_fd >= 0)
        close(sock_fd);
}
