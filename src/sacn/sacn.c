/*
 * sACN (E1.31) receiver implementation
 *
 * Receives streaming ACN (ANSI E1.31) multicast packets containing DMX data.
 * This is the primary modern protocol for DMX-over-IP.
 *
 * Packet format (simplified):
 *   Root Layer: preamble (12 bytes) + flags/length + vector + CID
 *   Framing Layer: flags/length + vector + source name + priority + sequence + universe
 *   DMP Layer: flags/length + vector + address/data type + DMX start code + DMX data
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include "sacn.h"
#include "../common.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#ifdef HOST_BUILD
#include <stdio.h>
#define LOG(fmt, ...) fprintf(stderr, "sacn: " fmt "\n", ##__VA_ARGS__)
#else
#define LOG(fmt, ...) dprintf(2, "sacn: " fmt "\n", ##__VA_ARGS__)
#endif

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
#define OFF_OPTIONS          112
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

/* Network byte order helpers for potentially-unaligned reads */
static uint16_t read_u16_be(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

uint32_t sacn_multicast_addr(uint16_t universe)
{
    /* 239.255.{high byte}.{low byte} in network byte order */
    uint32_t addr = SACN_MULTICAST_BASE | (uint32_t)universe;
    return htonl(addr);
}

int sacn_init(uint16_t universe)
{
    int sock;
    int reuse = 1;
    struct sockaddr_in addr;
    struct ip_mreq mreq;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        LOG("socket() failed: %d", sock);
        return -1;
    }
    LOG("socket ok: fd=%d", sock);

    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SACN_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG("bind port %d failed: %d", SACN_PORT, errno);
        close(sock);
        return -1;
    }
    LOG("bind port %d ok", SACN_PORT);

    /* Join multicast group for this universe */
    memset(&mreq, 0, sizeof(mreq));
    mreq.imr_multiaddr.s_addr = sacn_multicast_addr(universe);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   &mreq, sizeof(mreq)) < 0) {
        LOG("multicast join failed for universe %d: %d", universe, errno);
        /* Don't fail — we can still receive unicast sACN */
        LOG("continuing without multicast (unicast only)");
    }

    return sock;
}

int sacn_receive(int sock_fd, sacn_packet_t *pkt)
{
    uint8_t buf[638]; /* max sACN packet = 638 bytes */
    int n;
    uint16_t preamble, universe, prop_count, dmx_len;
    uint32_t root_vector;

    n = recv(sock_fd, buf, sizeof(buf), 0);
    if (n < SACN_MIN_PACKET_LEN)
        return -1;

    /* Validate preamble */
    preamble = read_u16_be(buf + OFF_PREAMBLE);
    if (preamble != SACN_PREAMBLE_SIZE)
        return -1;

    /* Validate ACN identifier */
    if (memcmp(buf + OFF_ACN_ID, SACN_ACN_IDENTIFIER, SACN_ACN_IDENTIFIER_LEN) != 0)
        return -1;

    /* Check root vector = E1.31 data */
    root_vector = read_u32_be(buf + OFF_ROOT_VECTOR);
    if (root_vector != VECTOR_ROOT_E131_DATA)
        return -1;

    /* Extract CID */
    memcpy(pkt->cid, buf + OFF_CID, 16);

    /* Extract framing layer fields */
    memcpy(pkt->source_name, buf + OFF_SOURCE_NAME, 64);
    pkt->source_name[63] = '\0';
    pkt->priority = buf[OFF_PRIORITY];
    pkt->sequence = buf[OFF_SEQUENCE];
    pkt->options = buf[OFF_OPTIONS];

    universe = read_u16_be(buf + OFF_UNIVERSE);
    pkt->universe = universe;

    /* DMP layer */
    if (buf[OFF_DMP_VECTOR] != VECTOR_DMP_SET_PROPERTY)
        return -1;

    prop_count = read_u16_be(buf + OFF_DMP_PROP_COUNT);
    if (prop_count < 1)
        return -1;

    /* First property value is the DMX start code */
    pkt->start_code = buf[OFF_DMP_START_CODE];
    dmx_len = prop_count - 1; /* subtract start code */
    if (dmx_len > DMX_UNIVERSE_SIZE)
        dmx_len = DMX_UNIVERSE_SIZE;

    if (OFF_DMP_DMX_DATA + dmx_len > (uint16_t)n)
        dmx_len = n - OFF_DMP_DMX_DATA;

    memcpy(pkt->dmx_data, buf + OFF_DMP_DMX_DATA, dmx_len);
    pkt->dmx_length = dmx_len;

    return 0;
}

void sacn_cleanup(int sock_fd)
{
    if (sock_fd >= 0)
        close(sock_fd);
}
