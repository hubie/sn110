/*
 * sACN (E1.31) transmitter implementation
 *
 * Builds and sends sACN multicast packets for DMX-In → network direction.
 * Packet format matches E1.31-2016 and is compatible with sacn_sender.py.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include "sacn_tx.h"
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
#define LOG(fmt, ...) fprintf(stderr, "sacn_tx: " fmt "\n", ##__VA_ARGS__)
#else
#define LOG(fmt, ...) dprintf(2, "sacn_tx: " fmt "\n", ##__VA_ARGS__)
#endif

/* Generate a deterministic CID from source name (simple hash) */
static void generate_cid(const char *name, uint8_t cid[16])
{
    int i;
    uint32_t hash = 0x534E3131; /* "SN11" */

    for (i = 0; name[i] && i < 64; i++)
        hash = hash * 31 + (uint8_t)name[i];

    memset(cid, 0, 16);
    /* Fill CID with hash-derived bytes */
    cid[0] = 0x53; /* 'S' */
    cid[1] = 0x4E; /* 'N' */
    cid[2] = 0x31; /* '1' */
    cid[3] = 0x31; /* '1' */
    cid[4] = 0x30; /* '0' */
    cid[5] = (uint8_t)(hash >> 24);
    cid[6] = (uint8_t)(hash >> 16);
    cid[7] = (uint8_t)(hash >> 8);
    cid[8] = (uint8_t)(hash);
    cid[9] = 0x00;
    cid[10] = 0x00;
    cid[11] = 0x00;
    cid[12] = (uint8_t)(hash >> 12);
    cid[13] = (uint8_t)(hash >> 4);
    cid[14] = 0x00;
    cid[15] = 0x01;
}

int sacn_tx_init(sacn_tx_t *tx, uint16_t universe, const char *source_name)
{
    int ttl = 20;

    memset(tx, 0, sizeof(*tx));
    tx->universe = universe;
    tx->priority = SACN_DEFAULT_PRIORITY;
    tx->sequence = 0;

    strncpy(tx->source_name, source_name, sizeof(tx->source_name) - 1);
    tx->source_name[63] = '\0';
    generate_cid(source_name, tx->cid);

    tx->sock_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (tx->sock_fd < 0) {
        LOG("socket() failed: %d", errno);
        return -1;
    }

    /* Set multicast TTL (ignore failure — still works on loopback) */
    setsockopt(tx->sock_fd, IPPROTO_IP, IP_MULTICAST_TTL,
               &ttl, sizeof(ttl));

    LOG("init ok: universe=%d fd=%d", universe, tx->sock_fd);
    return 0;
}

int sacn_tx_build_packet(sacn_tx_t *tx, const uint8_t *dmx_data, int dmx_len,
                         uint8_t *buf, int buf_size)
{
    int dmx_with_start_len; /* start code + DMX data */
    int dmp_pdu_len, framing_pdu_len, root_pdu_len;
    int total_len;

    if (dmx_len > DMX_UNIVERSE_SIZE)
        dmx_len = DMX_UNIVERSE_SIZE;

    dmx_with_start_len = 1 + dmx_len; /* start code byte + data */

    /* PDU lengths (flags+length field included in PDU, not in length value) */
    /* DMP PDU: flags_len(2) + vector(1) + addr_data(1) + first_addr(2) + addr_inc(2) + prop_count(2) + data */
    dmp_pdu_len = 10 + dmx_with_start_len;
    /* Framing PDU: flags_len(2) + vector(4) + source_name(64) + priority(1) + sync(2) + seq(1) + options(1) + universe(2) + DMP */
    framing_pdu_len = 77 + dmp_pdu_len;
    /* Root PDU: flags_len(2) + vector(4) + CID(16) + Framing */
    root_pdu_len = 22 + framing_pdu_len;

    total_len = 16 + root_pdu_len; /* preamble(2) + postamble(2) + ACN_ID(12) + root PDU */

    if (buf_size < total_len)
        return -1;

    memset(buf, 0, total_len);

    /* === Root Layer === */
    /* Preamble size */
    write_u16_be(buf + OFF_PREAMBLE, SACN_PREAMBLE_SIZE);
    /* Post-amble size */
    write_u16_be(buf + OFF_POSTAMBLE, 0x0000);
    /* ACN Packet Identifier */
    memcpy(buf + OFF_ACN_ID, SACN_ACN_IDENTIFIER, SACN_ACN_IDENTIFIER_LEN);
    /* Root flags + length: 0x7000 | length */
    write_u16_be(buf + OFF_ROOT_FLAGS_LEN, 0x7000 | root_pdu_len);
    /* Root vector */
    write_u32_be(buf + OFF_ROOT_VECTOR, VECTOR_ROOT_E131_DATA);
    /* CID */
    memcpy(buf + OFF_CID, tx->cid, 16);

    /* === Framing Layer === */
    write_u16_be(buf + OFF_FRAME_FLAGS_LEN, 0x7000 | framing_pdu_len);
    write_u32_be(buf + OFF_FRAME_VECTOR, VECTOR_E131_DATA_PACKET);
    memcpy(buf + OFF_SOURCE_NAME, tx->source_name, 64);
    buf[OFF_PRIORITY] = tx->priority;
    write_u16_be(buf + OFF_SYNC_ADDR, 0); /* no sync */
    buf[OFF_SEQUENCE] = tx->sequence;
    buf[OFF_OPTIONS] = 0;
    write_u16_be(buf + OFF_UNIVERSE, tx->universe);

    /* === DMP Layer === */
    write_u16_be(buf + OFF_DMP_FLAGS_LEN, 0x7000 | dmp_pdu_len);
    buf[OFF_DMP_VECTOR] = VECTOR_DMP_SET_PROPERTY;
    buf[OFF_DMP_ADDR_DATA] = 0xA1; /* address & data type */
    write_u16_be(buf + OFF_DMP_FIRST_ADDR, 0x0000);
    write_u16_be(buf + OFF_DMP_ADDR_INC, 0x0001);
    write_u16_be(buf + OFF_DMP_PROP_COUNT, dmx_with_start_len);
    buf[OFF_DMP_START_CODE] = 0x00; /* DMX start code */
    memcpy(buf + OFF_DMP_DMX_DATA, dmx_data, dmx_len);

    return total_len;
}

int sacn_tx_send(sacn_tx_t *tx, const uint8_t *dmx_data, int dmx_len)
{
    uint8_t buf[SACN_MAX_PACKET_LEN];
    struct sockaddr_in dest;
    int pkt_len;
    int sent;

    pkt_len = sacn_tx_build_packet(tx, dmx_data, dmx_len, buf, sizeof(buf));
    if (pkt_len < 0)
        return -1;

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(SACN_PORT);
    dest.sin_addr.s_addr = sacn_multicast_addr(tx->universe);

    sent = sendto(tx->sock_fd, buf, pkt_len, 0,
                  (struct sockaddr *)&dest, sizeof(dest));

    if (sent < 0) {
        LOG("sendto failed: %d", errno);
        return -1;
    }

    tx->sequence++;
    return 0;
}

void sacn_tx_cleanup(sacn_tx_t *tx)
{
    if (tx->sock_fd >= 0) {
        close(tx->sock_fd);
        tx->sock_fd = -1;
    }
}
