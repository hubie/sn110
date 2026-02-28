/*
 * Off-device test runner for sn110dmx
 *
 * Compiles and runs on the host (macOS/Linux) using mock DMX devices
 * and real UDP sockets on loopback for protocol testing.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <arpa/inet.h>

/* Include the headers we're testing */
#include "../src/common.h"
#include "../src/dmx/dmx.h"
#include "../src/sacn/sacn.h"
#include "../src/artnet/artnet.h"
#include "../src/shownet/shownet.h"
#include "../src/config/config.h"

/* Mock DMX test helpers (from dmx_mock.c) */
extern const uint8_t *mock_dmx_get_tx_buf(int fd);
extern int mock_dmx_get_tx_len(int fd);
extern void mock_dmx_inject_rx(int fd, const uint8_t *data, int len);
extern void mock_dmx_reset(void);

/* ShowNet RLE decoder (from shownet.c) */
extern int shownet_decode_rle(const uint8_t *encoded, int encoded_len,
                              uint8_t *decoded, int max_decoded_len);

/* ========================================================================= */
/* Test helpers                                                              */
/* ========================================================================= */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) \
    do { \
        tests_run++; \
        printf("  TEST: %-50s ", #name); \
        test_##name(); \
        tests_passed++; \
        printf("PASS\n"); \
    } while(0)

/* ========================================================================= */
/* Common / struct tests                                                     */
/* ========================================================================= */

static void test_config_defaults(void) {
    node_config_t config;
    memset(&config, 0, sizeof(config));
    assert(sizeof(config.ports) == sizeof(port_config_t) * DMX_MAX_PORTS);
    assert(DMX_UNIVERSE_SIZE == 512);
    assert(DMX_MAX_PORTS == 2);
}

static void test_dmx_frame_size(void) {
    dmx_frame_t frame;
    assert(sizeof(frame.data) == 512);
    assert(sizeof(frame) < 600);
}

static void test_dmx_mode_values(void) {
    /* These must match the reverse-engineered kernel driver values */
    assert(DMX_MODE_OFF == 0);
    assert(DMX_MODE_RAW == 1);
    assert(DMX_MODE_TX  == 2);
    assert(DMX_MODE_RX  == 3);
}

static void test_dmx_ioctl_constants(void) {
    assert(DMX_IOCTL_TYPE == 'd');
    assert(DMX_IOCTL_NR == 1);
    assert(DMX_IOCTL_SIZE == 16);
}

/* ========================================================================= */
/* Mock DMX driver tests                                                     */
/* ========================================================================= */

static void test_mock_open_close(void) {
    const dmx_ops_t *ops = dmx_get_ops();
    mock_dmx_reset();

    int fd = ops->open("/dev/dmx0");
    assert(fd >= 0);
    ops->close(fd);
}

static void test_mock_set_mode(void) {
    const dmx_ops_t *ops = dmx_get_ops();
    mock_dmx_reset();

    int fd = ops->open("/dev/dmx0");
    assert(fd >= 0);
    assert(ops->set_mode(fd, DMX_MODE_TX) == 0);
    assert(ops->set_mode(fd, DMX_MODE_RX) == 0);
    assert(ops->set_mode(fd, DMX_MODE_OFF) == 0);
    ops->close(fd);
}

static void test_mock_write_read(void) {
    const dmx_ops_t *ops = dmx_get_ops();
    mock_dmx_reset();

    int fd = ops->open("/dev/dmx0");
    assert(fd >= 0);
    ops->set_mode(fd, DMX_MODE_TX);

    /* Write a test frame */
    uint8_t frame[512];
    memset(frame, 0, sizeof(frame));
    frame[0] = 0xFF;
    frame[1] = 0x80;
    frame[511] = 0x42;

    int written = ops->write_frame(fd, frame, 512);
    assert(written == 512);

    /* Verify via mock inspection */
    const uint8_t *tx_buf = mock_dmx_get_tx_buf(fd);
    assert(tx_buf != NULL);
    assert(tx_buf[0] == 0xFF);
    assert(tx_buf[1] == 0x80);
    assert(tx_buf[511] == 0x42);
    assert(mock_dmx_get_tx_len(fd) == 512);

    ops->close(fd);
}

static void test_mock_rx_inject(void) {
    const dmx_ops_t *ops = dmx_get_ops();
    mock_dmx_reset();

    int fd = ops->open("/dev/dmx0");
    assert(fd >= 0);
    ops->set_mode(fd, DMX_MODE_RX);

    /* Inject data into the mock RX buffer */
    uint8_t inject[512];
    memset(inject, 0xAA, sizeof(inject));
    mock_dmx_inject_rx(fd, inject, 512);

    /* Read it back */
    uint8_t readback[512];
    int n = ops->read_frame(fd, readback, 512);
    assert(n == 512);
    assert(readback[0] == 0xAA);
    assert(readback[255] == 0xAA);

    ops->close(fd);
}

static void test_mock_multiple_ports(void) {
    const dmx_ops_t *ops = dmx_get_ops();
    mock_dmx_reset();

    int fd0 = ops->open("/dev/dmx0");
    int fd1 = ops->open("/dev/dmx1");
    assert(fd0 >= 0);
    assert(fd1 >= 0);
    assert(fd0 != fd1);

    uint8_t frame0[512], frame1[512];
    memset(frame0, 0x11, sizeof(frame0));
    memset(frame1, 0x22, sizeof(frame1));

    ops->write_frame(fd0, frame0, 512);
    ops->write_frame(fd1, frame1, 512);

    /* Each port should have its own data */
    assert(mock_dmx_get_tx_buf(fd0)[0] == 0x11);
    assert(mock_dmx_get_tx_buf(fd1)[0] == 0x22);

    ops->close(fd0);
    ops->close(fd1);
}

/* ========================================================================= */
/* sACN tests                                                                */
/* ========================================================================= */

static void test_sacn_multicast_address(void) {
    /* sacn_multicast_addr returns network byte order */
    uint32_t addr_u1 = sacn_multicast_addr(1);
    /* 239.255.0.1 in network order */
    struct in_addr in;
    in.s_addr = addr_u1;
    assert(strcmp(inet_ntoa(in), "239.255.0.1") == 0);

    uint32_t addr_u256 = sacn_multicast_addr(256);
    in.s_addr = addr_u256;
    assert(strcmp(inet_ntoa(in), "239.255.1.0") == 0);

    uint32_t addr_u63999 = sacn_multicast_addr(63999);
    in.s_addr = addr_u63999;
    assert(strcmp(inet_ntoa(in), "239.255.249.255") == 0);
}

static void test_sacn_constants(void) {
    assert(SACN_PORT == 5568);
    assert(SACN_MAX_PRIORITY == 200);
    assert(SACN_DEFAULT_PRIORITY == 100);
    assert(SACN_TIMEOUT_MS == 2500);
}

/* ========================================================================= */
/* Art-Net tests                                                             */
/* ========================================================================= */

static void test_artnet_constants(void) {
    assert(ARTNET_PORT == 6454);
    assert(ARTNET_PROTOCOL_VERSION == 14);
    assert(strcmp(ARTNET_MAGIC, "Art-Net") == 0);
}

/* ========================================================================= */
/* ShowNet tests                                                             */
/* ========================================================================= */

static void test_shownet_constants(void) {
    assert(SHOWNET_PORT == 2501);
    assert(SHOWNET_MAX_SLOTS == 18432);
}

static void test_shownet_rle_passthrough(void) {
    /* Non-RLE data (no 0x80 bytes) should pass through unchanged */
    uint8_t encoded[] = {0x10, 0x20, 0x30, 0x40, 0x50};
    uint8_t decoded[16];
    int n = shownet_decode_rle(encoded, 5, decoded, sizeof(decoded));
    assert(n == 5);
    assert(decoded[0] == 0x10);
    assert(decoded[4] == 0x50);
}

static void test_shownet_rle_compressed(void) {
    /* RLE: 0x80, count=5, value=0xFF → 5 bytes of 0xFF */
    uint8_t encoded[] = {0x80, 5, 0xFF};
    uint8_t decoded[16];
    int n = shownet_decode_rle(encoded, 3, decoded, sizeof(decoded));
    assert(n == 5);
    int i;
    for (i = 0; i < 5; i++)
        assert(decoded[i] == 0xFF);
}

static void test_shownet_rle_mixed(void) {
    /* Mixed: literal 0x42, RLE 3×0xAA, literal 0x55 */
    uint8_t encoded[] = {0x42, 0x80, 3, 0xAA, 0x55};
    uint8_t decoded[16];
    int n = shownet_decode_rle(encoded, 5, decoded, sizeof(decoded));
    assert(n == 5);
    assert(decoded[0] == 0x42);
    assert(decoded[1] == 0xAA);
    assert(decoded[2] == 0xAA);
    assert(decoded[3] == 0xAA);
    assert(decoded[4] == 0x55);
}

/* ========================================================================= */
/* Config parser tests                                                       */
/* ========================================================================= */

static void test_config_defaults_values(void) {
    node_config_t config;
    config_defaults(&config);
    assert(config.active_protocol == PROTO_SACN);
    assert(config.ports[0].universe == 1);
    assert(config.ports[1].universe == 2);
    assert(config.ports[0].mode == DMX_MODE_TX);
    assert(config.dmx_hold_time == 5);
    assert(strcmp(config.hostname, "SN110") == 0);
}

static void test_config_save_load(void) {
    node_config_t orig, loaded;
    const char *path = "/tmp/sn110_test.cfg";

    config_defaults(&orig);
    orig.active_protocol = PROTO_ARTNET;
    orig.ports[0].universe = 42;
    orig.ports[1].universe = 99;
    orig.dmx_hold_time = 10;
    strncpy(orig.hostname, "TESTNODE", sizeof(orig.hostname));
    orig.ip_addr = (10 << 24) | (0 << 16) | (1 << 8) | 50;

    assert(config_save(path, &orig) == 0);
    assert(config_load(path, &loaded) == 0);

    assert(loaded.active_protocol == PROTO_ARTNET);
    assert(loaded.ports[0].universe == 42);
    assert(loaded.ports[1].universe == 99);
    assert(loaded.dmx_hold_time == 10);
    assert(strcmp(loaded.hostname, "TESTNODE") == 0);
    assert(loaded.ip_addr == orig.ip_addr);

    /* Cleanup */
    remove(path);
}

/* ========================================================================= */
/* Main                                                                      */
/* ========================================================================= */

int main(void) {
    printf("sn110dmx — Off-Device Test Suite\n");
    printf("================================\n\n");

    printf("Common:\n");
    TEST(config_defaults);
    TEST(dmx_frame_size);
    TEST(dmx_mode_values);
    TEST(dmx_ioctl_constants);

    printf("\nMock DMX Driver:\n");
    TEST(mock_open_close);
    TEST(mock_set_mode);
    TEST(mock_write_read);
    TEST(mock_rx_inject);
    TEST(mock_multiple_ports);

    printf("\nsACN:\n");
    TEST(sacn_multicast_address);
    TEST(sacn_constants);

    printf("\nArt-Net:\n");
    TEST(artnet_constants);

    printf("\nShowNet:\n");
    TEST(shownet_constants);
    TEST(shownet_rle_passthrough);
    TEST(shownet_rle_compressed);
    TEST(shownet_rle_mixed);

    printf("\nConfig:\n");
    TEST(config_defaults_values);
    TEST(config_save_load);

    printf("\n================================\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
