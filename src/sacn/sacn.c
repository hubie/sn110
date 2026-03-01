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
#include "sacn_internal.h"
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

int sacn_parse(const uint8_t *buf, int len, sacn_packet_t *pkt)
{
    uint16_t preamble, universe, prop_count, dmx_len;
    uint32_t root_vector;

    if (len < SACN_MIN_PACKET_LEN)
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

    if (OFF_DMP_DMX_DATA + dmx_len > (uint16_t)len)
        dmx_len = len - OFF_DMP_DMX_DATA;

    memcpy(pkt->dmx_data, buf + OFF_DMP_DMX_DATA, dmx_len);
    pkt->dmx_length = dmx_len;

    return 0;
}

int sacn_receive(int sock_fd, sacn_packet_t *pkt)
{
    uint8_t buf[SACN_MAX_PACKET_LEN];
    int n;

    n = recv(sock_fd, buf, sizeof(buf), 0);
    if (n < SACN_MIN_PACKET_LEN)
        return -1;

    return sacn_parse(buf, n, pkt);
}

void sacn_cleanup(int sock_fd)
{
    if (sock_fd >= 0)
        close(sock_fd);
}
