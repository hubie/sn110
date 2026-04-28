/*
 * Art-Net receiver implementation
 *
 * Receives Art-Net ArtDmx (OpOutput/OpDmx) packets containing DMX data.
 * Handles ArtPoll with automatic ArtPollReply.
 *
 * Art-Net is a trademark of Artistic Licence Holdings Ltd.
 * This is a clean-room implementation based on the publicly available
 * Art-Net 4 protocol specification.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include "artnet.h"
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
#define LOG(fmt, ...) fprintf(stderr, "artnet: " fmt "\n", ##__VA_ARGS__)
#else
#define LOG(fmt, ...) dprintf(2, "artnet: " fmt "\n", ##__VA_ARGS__)
#endif

/* Art-Net packet offsets */
#define OFF_MAGIC       0   /* "Art-Net\0" (8 bytes) */
#define OFF_OPCODE      8   /* OpCode (little-endian 16-bit) */
#define OFF_PROTVER_HI  10  /* Protocol version high byte */
#define OFF_PROTVER_LO  11  /* Protocol version low byte */
#define OFF_SEQUENCE    12
#define OFF_PHYSICAL    13
#define OFF_UNIVERSE    14  /* SubUni (low) + Net (high) — little-endian */
#define OFF_LENGTH_HI   16
#define OFF_LENGTH_LO   17
#define OFF_DMX_DATA    18

#define ARTNET_MIN_DMX_LEN 18

static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/* Node identity for ArtPollReply — set during init */
static uint32_t g_my_ip;
static uint8_t  g_my_mac[6];

int artnet_init(uint32_t my_ip, const uint8_t *my_mac)
{
    int sock;
    int reuse = 1;
    struct sockaddr_in addr;

    g_my_ip = my_ip;
    if (my_mac)
        memcpy(g_my_mac, my_mac, 6);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return -1;

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ARTNET_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG("bind failed: %d", errno);
        close(sock);
        return -1;
    }

    /* Enable broadcast reception for Art-Net */
    int bcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &bcast, sizeof(bcast));

    /* Set non-blocking — select() is broken on Linux 2.0/uClinux for UDP */
    {
        int nonblock = 1;
        ioctl(sock, 0x5421, &nonblock); /* FIONBIO */
    }

    LOG("listening on port %d", ARTNET_PORT);
    return sock;
}

/*
 * Send a minimal ArtPollReply to the requesting address.
 * This lets controllers discover us on the network.
 */
static void artnet_send_poll_reply(int sock_fd, struct sockaddr_in *dest,
                                   uint32_t my_ip, const uint8_t *my_mac)
{
    uint8_t reply[239];
    memset(reply, 0, sizeof(reply));

    /* Header */
    memcpy(reply, ARTNET_MAGIC, ARTNET_MAGIC_LEN);
    reply[8] = (ARTNET_OP_POLL_REPLY) & 0xFF;
    reply[9] = (ARTNET_OP_POLL_REPLY >> 8) & 0xFF;

    /* IP address (bytes 10-13, network byte order) */
    {
        uint32_t ip_nbo = htonl(my_ip);
        memcpy(reply + 10, &ip_nbo, 4);
    }

    /* Port (bytes 14-15, little-endian) */
    reply[14] = ARTNET_PORT & 0xFF;
    reply[15] = (ARTNET_PORT >> 8) & 0xFF;

    /* Firmware version (bytes 16-17) */
    reply[16] = 0;
    reply[17] = 1;

    /* Short name (bytes 26-43) */
    memcpy(reply + 26, "SN110-OpenFW", 12);

    /* Long name (bytes 44-107) */
    memcpy(reply + 44, "Strand SN110 Open Firmware DMX Node", 35);

    /* MAC address (bytes 201-206) */
    if (my_mac)
        memcpy(reply + 201, my_mac, 6);

    /* Number of ports (byte 173) */
    reply[173] = DMX_MAX_PORTS;

    /* Port types: DMX512 output */
    reply[174] = 0x80; /* output, DMX512 */
    reply[175] = 0x80;

    dest->sin_port = htons(ARTNET_PORT);
    sendto(sock_fd, reply, sizeof(reply), 0,
           (struct sockaddr *)dest, sizeof(*dest));
}

int artnet_receive(int sock_fd, artnet_dmx_packet_t *pkt)
{
    uint8_t buf[530]; /* 18 header + 512 DMX */
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int n;
    uint16_t opcode, dmx_len;

    n = recvfrom(sock_fd, buf, sizeof(buf), 0,
                 (struct sockaddr *)&from, &fromlen);
    if (n < ARTNET_MIN_DMX_LEN)
        return -1;

    /* Validate magic */
    if (memcmp(buf, ARTNET_MAGIC, ARTNET_MAGIC_LEN) != 0)
        return -1;

    opcode = read_u16_le(buf + OFF_OPCODE);

    /* Handle ArtPoll → send ArtPollReply */
    if (opcode == ARTNET_OP_POLL) {
        artnet_send_poll_reply(sock_fd, &from, g_my_ip, g_my_mac);
        return -1; /* not a DMX packet */
    }

    if (opcode != ARTNET_OP_DMX)
        return -1;

    /* Protocol version check */
    if (buf[OFF_PROTVER_LO] < ARTNET_PROTOCOL_VERSION) {
        /* accept anyway — be lenient */
    }

    pkt->sequence = buf[OFF_SEQUENCE];
    pkt->physical = buf[OFF_PHYSICAL];
    pkt->universe = read_u16_le(buf + OFF_UNIVERSE);

    dmx_len = (buf[OFF_LENGTH_HI] << 8) | buf[OFF_LENGTH_LO];
    if (dmx_len > DMX_UNIVERSE_SIZE)
        dmx_len = DMX_UNIVERSE_SIZE;
    if (OFF_DMX_DATA + dmx_len > (uint16_t)n)
        dmx_len = n - OFF_DMX_DATA;

    memcpy(pkt->dmx_data, buf + OFF_DMX_DATA, dmx_len);
    pkt->dmx_length = dmx_len;

    return 0;
}

void artnet_cleanup(int sock_fd)
{
    if (sock_fd >= 0)
        close(sock_fd);
}
