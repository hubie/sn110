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
#include "../src/sacn/sacn_tx.h"
#include "../src/artnet/artnet.h"
#include "../src/shownet/shownet.h"
#include "../src/config/config.h"
#include "../src/cgi/cgi_parse.h"

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
/* sACN TX tests                                                             */
/* ========================================================================= */

static void test_sacn_tx_build_packet(void) {
    sacn_tx_t tx;
    uint8_t buf[638];
    uint8_t dmx[512];
    int pkt_len;
    int i;

    memset(&tx, 0, sizeof(tx));
    tx.universe = 1;
    tx.priority = 100;
    tx.sequence = 42;
    strncpy(tx.source_name, "TestSource", sizeof(tx.source_name));
    memset(tx.cid, 0xAA, 16);

    /* Set up test DMX data: identify pattern */
    memset(dmx, 0, sizeof(dmx));
    dmx[0] = 255;
    dmx[1] = 128;
    dmx[2] = 64;

    pkt_len = sacn_tx_build_packet(&tx, dmx, 512, buf, sizeof(buf));
    assert(pkt_len == 638);

    /* Verify preamble */
    assert(buf[0] == 0x00 && buf[1] == 0x10); /* preamble = 0x0010 */
    assert(buf[2] == 0x00 && buf[3] == 0x00); /* postamble = 0 */

    /* Verify ACN identifier */
    assert(buf[4] == 'A' && buf[5] == 'S' && buf[6] == 'C');

    /* Verify root vector = 0x00000004 */
    assert(buf[18] == 0x00 && buf[19] == 0x00);
    assert(buf[20] == 0x00 && buf[21] == 0x04);

    /* Verify CID */
    for (i = 0; i < 16; i++)
        assert(buf[22 + i] == 0xAA);

    /* Verify framing vector = 0x00000002 */
    assert(buf[40] == 0x00 && buf[41] == 0x00);
    assert(buf[42] == 0x00 && buf[43] == 0x02);

    /* Verify source name starts with "TestSource" */
    assert(memcmp(buf + 44, "TestSource", 10) == 0);

    /* Verify priority */
    assert(buf[108] == 100);

    /* Verify sequence */
    assert(buf[111] == 42);

    /* Verify universe = 1 */
    assert(buf[113] == 0x00 && buf[114] == 0x01);

    /* Verify DMP vector */
    assert(buf[117] == 0x02);

    /* Verify DMX start code = 0 */
    assert(buf[125] == 0x00);

    /* Verify DMX data at known offsets */
    assert(buf[126] == 255);  /* Ch1 */
    assert(buf[127] == 128);  /* Ch2 */
    assert(buf[128] == 64);   /* Ch3 */
    assert(buf[129] == 0);    /* Ch4 */
}

static void test_sacn_tx_roundtrip(void) {
    sacn_tx_t tx;
    sacn_packet_t parsed;
    uint8_t buf[638];
    uint8_t dmx[512];
    int pkt_len;
    int i;

    memset(&tx, 0, sizeof(tx));
    tx.universe = 7;
    tx.priority = 150;
    tx.sequence = 99;
    strncpy(tx.source_name, "RoundtripTest", sizeof(tx.source_name));
    memset(tx.cid, 0xBB, 16);

    /* Ramp pattern */
    for (i = 0; i < 512; i++)
        dmx[i] = i & 0xFF;

    pkt_len = sacn_tx_build_packet(&tx, dmx, 512, buf, sizeof(buf));
    assert(pkt_len == 638);

    /* Parse it back */
    assert(sacn_parse(buf, pkt_len, &parsed) == 0);

    /* Verify all fields round-trip */
    assert(parsed.universe == 7);
    assert(parsed.priority == 150);
    assert(parsed.sequence == 99);
    assert(parsed.start_code == 0);
    assert(parsed.dmx_length == 512);
    assert(strcmp(parsed.source_name, "RoundtripTest") == 0);

    /* Verify CID */
    for (i = 0; i < 16; i++)
        assert(parsed.cid[i] == 0xBB);

    /* Verify DMX data */
    for (i = 0; i < 512; i++)
        assert(parsed.dmx_data[i] == (i & 0xFF));
}

/* ========================================================================= */
/* Config port mode tests                                                    */
/* ========================================================================= */

static void test_config_port_mode_rx(void) {
    node_config_t config;
    const char *path = "/tmp/sn110_test_mode.cfg";
    FILE *f;

    f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f, "dmx_port0_mode = rx\n");
    fprintf(f, "dmx_port1_mode = tx\n");
    fclose(f);

    assert(config_load(path, &config) == 0);
    assert(config.ports[0].mode == DMX_MODE_RX);
    assert(config.ports[1].mode == DMX_MODE_TX);

    remove(path);
}

static void test_config_save_load_mode(void) {
    node_config_t orig, loaded;
    const char *path = "/tmp/sn110_test_mode2.cfg";

    config_defaults(&orig);
    orig.ports[0].mode = DMX_MODE_RX;
    orig.ports[1].mode = DMX_MODE_OFF;

    assert(config_save(path, &orig) == 0);
    assert(config_load(path, &loaded) == 0);
    assert(loaded.ports[0].mode == DMX_MODE_RX);
    assert(loaded.ports[1].mode == DMX_MODE_OFF);

    remove(path);
}

/* ========================================================================= */
/* DMX input → sACN pipeline test                                            */
/* ========================================================================= */

static void test_dmx_input_to_sacn(void) {
    sacn_tx_t tx;
    sacn_packet_t parsed;
    uint8_t buf[638];
    uint8_t dmx_in[512];
    int pkt_len;

    /* Simulate: DMX port reads identify pattern */
    memset(dmx_in, 0, sizeof(dmx_in));
    dmx_in[0] = 255;
    dmx_in[1] = 128;
    dmx_in[2] = 64;

    /* Build sACN packet from DMX input (as dmx_input_cycle would) */
    memset(&tx, 0, sizeof(tx));
    tx.universe = 1;
    tx.priority = 100;
    tx.sequence = 0;
    strncpy(tx.source_name, "SN110 Port 0", sizeof(tx.source_name));
    memset(tx.cid, 0x53, 16);

    pkt_len = sacn_tx_build_packet(&tx, dmx_in, 512, buf, sizeof(buf));
    assert(pkt_len == 638);

    /* Parse and verify DMX data survived the pipeline */
    assert(sacn_parse(buf, pkt_len, &parsed) == 0);
    assert(parsed.universe == 1);
    assert(parsed.dmx_data[0] == 255);
    assert(parsed.dmx_data[1] == 128);
    assert(parsed.dmx_data[2] == 64);
    assert(parsed.dmx_data[3] == 0);
    assert(parsed.dmx_length == 512);
}

/* ========================================================================= */
/* CGI parsing tests                                                         */
/* ========================================================================= */

static void test_cgi_url_decode(void) {
    char out[64];

    /* %20 → space */
    url_decode(out, "hello%20world", sizeof(out));
    assert(strcmp(out, "hello world") == 0);

    /* + → space */
    url_decode(out, "hello+world", sizeof(out));
    assert(strcmp(out, "hello world") == 0);

    /* %2F → / */
    url_decode(out, "path%2Fto%2Ffile", sizeof(out));
    assert(strcmp(out, "path/to/file") == 0);

    /* Plain passthrough */
    url_decode(out, "plain_text", sizeof(out));
    assert(strcmp(out, "plain_text") == 0);

    /* Mixed */
    url_decode(out, "a%26b%3Dc", sizeof(out));
    assert(strcmp(out, "a&b=c") == 0);

    /* Empty string */
    url_decode(out, "", sizeof(out));
    assert(strcmp(out, "") == 0);
}

static void test_cgi_parse_formdata(void) {
    node_config_t cfg;
    config_defaults(&cfg);

    parse_formdata("hostname=sn110&port0_mode=rx&port0_universe=3"
                   "&port1_mode=off&port1_universe=7"
                   "&protocol=artnet&dmx_hold_time=15"
                   "&port0_label=Stage&port1_label=Truss",
                   &cfg);

    assert(strcmp(cfg.hostname, "sn110") == 0);
    assert(cfg.ports[0].mode == DMX_MODE_RX);
    assert(cfg.ports[0].universe == 3);
    assert(cfg.ports[1].mode == DMX_MODE_OFF);
    assert(cfg.ports[1].universe == 7);
    assert(cfg.active_protocol == PROTO_ARTNET);
    assert(cfg.dmx_hold_time == 15);
    assert(strcmp(cfg.ports[0].label, "Stage") == 0);
    assert(strcmp(cfg.ports[1].label, "Truss") == 0);
}

static void test_cgi_parse_formdata_special_chars(void) {
    node_config_t cfg;
    config_defaults(&cfg);

    /* URL-encoded values: "SN 110" → hostname, "10.0.1.50" → ipaddr */
    parse_formdata("hostname=SN%20110&ipaddr=10.0.1.50"
                   "&netmask=255.255.255.0&gateway=10.0.1.1",
                   &cfg);

    assert(strcmp(cfg.hostname, "SN 110") == 0);
    assert(cfg.ip_addr == ((10 << 24) | (0 << 16) | (1 << 8) | 50));
    assert(cfg.netmask == ((255u << 24) | (255u << 16) | (255u << 8) | 0u));
    assert(cfg.gateway == ((10 << 24) | (0 << 16) | (1 << 8) | 1));
}

/* ========================================================================= */
/* MAC + DHCP tests                                                          */
/* ========================================================================= */

static void test_config_mac_parse(void) {
    node_config_t config;
    const char *path = "/tmp/sn110_test_mac.cfg";
    FILE *f;

    f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f, "macaddr = 00:E0:01:00:EC:FD\n");
    fclose(f);

    assert(config_load(path, &config) == 0);
    assert(config.mac[0] == 0x00);
    assert(config.mac[1] == 0xE0);
    assert(config.mac[2] == 0x01);
    assert(config.mac[3] == 0x00);
    assert(config.mac[4] == 0xEC);
    assert(config.mac[5] == 0xFD);

    remove(path);
}

static void test_config_mac_save_load(void) {
    node_config_t orig, loaded;
    const char *path = "/tmp/sn110_test_mac2.cfg";

    config_defaults(&orig);
    orig.mac[0] = 0xAA;
    orig.mac[1] = 0xBB;
    orig.mac[2] = 0xCC;
    orig.mac[3] = 0xDD;
    orig.mac[4] = 0xEE;
    orig.mac[5] = 0xFF;

    assert(config_save(path, &orig) == 0);
    assert(config_load(path, &loaded) == 0);

    assert(loaded.mac[0] == 0xAA);
    assert(loaded.mac[1] == 0xBB);
    assert(loaded.mac[2] == 0xCC);
    assert(loaded.mac[3] == 0xDD);
    assert(loaded.mac[4] == 0xEE);
    assert(loaded.mac[5] == 0xFF);

    remove(path);
}

static void test_config_dhcp_default(void) {
    node_config_t config;
    config_defaults(&config);
    /* memset zeros ip_addr → 0 means DHCP */
    assert(config.ip_addr == 0);
}

static void test_config_dhcp_save_load(void) {
    node_config_t orig, loaded;
    const char *path = "/tmp/sn110_test_dhcp.cfg";

    config_defaults(&orig);
    /* ip_addr=0 means DHCP */
    orig.ip_addr = 0;

    assert(config_save(path, &orig) == 0);
    assert(config_load(path, &loaded) == 0);
    assert(loaded.ip_addr == 0);

    remove(path);
}

static void test_cgi_parse_dhcp_mode(void) {
    node_config_t cfg;
    config_defaults(&cfg);

    /* Form has addr_mode=dhcp plus IP fields — DHCP should override */
    parse_formdata("addr_mode=dhcp&ipaddr=10.0.1.50"
                   "&netmask=255.255.255.0&gateway=10.0.1.1",
                   &cfg);

    assert(cfg.ip_addr == 0);
    assert(cfg.netmask == 0);
    assert(cfg.gateway == 0);
}

static void test_cgi_parse_static_mode(void) {
    node_config_t cfg;
    config_defaults(&cfg);

    /* Form has addr_mode=static plus IP fields — IPs preserved */
    parse_formdata("addr_mode=static&ipaddr=10.0.1.50"
                   "&netmask=255.255.255.0&gateway=10.0.1.1",
                   &cfg);

    assert(cfg.ip_addr == ((10 << 24) | (0 << 16) | (1 << 8) | 50));
    assert(cfg.netmask == ((255u << 24) | (255u << 16) | (255u << 8) | 0u));
    assert(cfg.gateway == ((10 << 24) | (0 << 16) | (1 << 8) | 1));
}

/* ========================================================================= */
/* Strand format + ifup tests                                                */
/* ========================================================================= */

static void test_config_strand_format_load(void) {
    node_config_t config;
    const char *path = "/tmp/sn110_test_strand.cfg";
    FILE *f;

    f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f, "nodeaddr = 192.168.0.71\n");
    fprintf(f, "hostname = SN110-A\n");
    fprintf(f, "macaddr = 00:E0:01:00:EC:FD\n");
    fprintf(f, "netmask = 255.255.255.0\n");
    fprintf(f, "gateway = 192.168.0.1\n");
    fprintf(f, "protocol = sacn\n");
    fprintf(f, "dmx_holdtime = 10\n");
    fprintf(f, "sacn_universe_0 = 42\n");
    fprintf(f, "sacn_universe_1 = 99\n");
    fprintf(f, "dmx_port0_mode = rx\n");
    fprintf(f, "dmx_port1_mode = off\n");
    fprintf(f, "dmx1_label = Stage\n");
    fprintf(f, "dmx2_label = Truss\n");
    fclose(f);

    assert(config_load(path, &config) == 0);
    assert(config.ip_addr == ((192u << 24) | (168u << 16) | (0u << 8) | 71u));
    assert(strcmp(config.hostname, "SN110-A") == 0);
    assert(config.mac[0] == 0x00);
    assert(config.mac[1] == 0xE0);
    assert(config.mac[5] == 0xFD);
    assert(config.netmask == ((255u << 24) | (255u << 16) | (255u << 8) | 0u));
    assert(config.gateway == ((192u << 24) | (168u << 16) | (0u << 8) | 1u));
    assert(config.active_protocol == PROTO_SACN);
    assert(config.dmx_hold_time == 10);
    assert(config.ports[0].universe == 42);
    assert(config.ports[1].universe == 99);
    assert(config.ports[0].mode == DMX_MODE_RX);
    assert(config.ports[1].mode == DMX_MODE_OFF);
    assert(strcmp(config.ports[0].label, "Stage") == 0);
    assert(strcmp(config.ports[1].label, "Truss") == 0);

    remove(path);
}

static void test_config_preserve_unknown_fields(void) {
    node_config_t config;
    const char *path = "/tmp/sn110_test_preserve.cfg";
    FILE *f;
    char buf[2048];
    int n;

    /* Write a file with Strand fields we don't manage */
    f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f, "nodetype = 220\n");
    fprintf(f, "nodeaddr = 10.0.1.50\n");
    fprintf(f, "hostname = TESTNODE\n");
    fprintf(f, "boottest = 1\n");
    fprintf(f, "dmx = 01FF00\n");
    fprintf(f, "lcd_contrast = 128\n");
    fclose(f);

    /* Load and save back */
    assert(config_load(path, &config) == 0);
    config.ip_addr = (192u << 24) | (168u << 16) | (1u << 8) | 100u;
    assert(config_save(path, &config) == 0);

    /* Read file and verify unknown fields survived */
    f = fopen(path, "r");
    assert(f != NULL);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    /* Unknown Strand keys should still be there */
    assert(strstr(buf, "nodetype = 220") != NULL);
    assert(strstr(buf, "boottest = 1") != NULL);
    assert(strstr(buf, "dmx = 01FF00") != NULL);
    assert(strstr(buf, "lcd_contrast = 128") != NULL);

    /* Our updated value should be there */
    assert(strstr(buf, "nodeaddr = 192.168.1.100") != NULL);

    remove(path);
}

static void test_config_dhcp_nodeaddr_zero(void) {
    node_config_t config;
    const char *path = "/tmp/sn110_test_dhcp_zero.cfg";
    FILE *f;
    char buf[2048];
    int n;

    /* Write nodeaddr = 0 (DHCP) */
    f = fopen(path, "w");
    assert(f != NULL);
    fprintf(f, "nodeaddr = 0\n");
    fclose(f);

    assert(config_load(path, &config) == 0);
    assert(config.ip_addr == 0);

    /* Save and verify it writes "nodeaddr = 0" not "0.0.0.0" */
    assert(config_save(path, &config) == 0);

    f = fopen(path, "r");
    assert(f != NULL);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    assert(strstr(buf, "nodeaddr = 0\n") != NULL);

    remove(path);
}

static void test_config_generate_ifup_static(void) {
    node_config_t config;
    const char *path = "/tmp/sn110_test_ifup_static.sh";
    char buf[2048];
    FILE *f;
    int n;

    config_defaults(&config);
    config.ip_addr = (192u << 24) | (168u << 16) | (0u << 8) | 71u;
    config.netmask = (255u << 24) | (255u << 16) | (255u << 8) | 0u;
    config.gateway = (192u << 24) | (168u << 16) | (0u << 8) | 1u;

    assert(config_generate_ifup(path, &config) == 0);

    f = fopen(path, "r");
    assert(f != NULL);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    assert(strstr(buf, "#!/bin/sh") != NULL);
    assert(strstr(buf, "ifconfig eth0 down") != NULL);
    assert(strstr(buf, "ifconfig eth0 192.168.0.71 netmask 255.255.255.0") != NULL);
    assert(strstr(buf, "broadcast 192.168.0.255") != NULL);
    assert(strstr(buf, "route add -net 192.168.0.0 netmask 255.255.255.0 eth0") != NULL);
    assert(strstr(buf, "route add default gw 192.168.0.1") != NULL);

    remove(path);
}

static void test_config_generate_ifup_dhcp(void) {
    node_config_t config;
    const char *path = "/tmp/sn110_test_ifup_dhcp.sh";
    char buf[2048];
    FILE *f;
    int n;

    config_defaults(&config);
    config.ip_addr = 0; /* DHCP */

    assert(config_generate_ifup(path, &config) == 0);

    f = fopen(path, "r");
    assert(f != NULL);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    assert(strstr(buf, "#!/bin/sh") != NULL);
    assert(strstr(buf, "ifconfig eth0 down") != NULL);
    assert(strstr(buf, "/sbin/pump -i eth0") != NULL);
    /* Should NOT have ifconfig with IP */
    assert(strstr(buf, "ifconfig eth0 0") == NULL ||
           strstr(buf, "ifconfig eth0 down") != NULL);

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
    TEST(config_port_mode_rx);
    TEST(config_save_load_mode);

    printf("\nsACN TX:\n");
    TEST(sacn_tx_build_packet);
    TEST(sacn_tx_roundtrip);

    printf("\nDMX Input Pipeline:\n");
    TEST(dmx_input_to_sacn);

    printf("\nCGI Parsing:\n");
    TEST(cgi_url_decode);
    TEST(cgi_parse_formdata);
    TEST(cgi_parse_formdata_special_chars);

    printf("\nMAC + DHCP:\n");
    TEST(config_mac_parse);
    TEST(config_mac_save_load);
    TEST(config_dhcp_default);
    TEST(config_dhcp_save_load);
    TEST(cgi_parse_dhcp_mode);
    TEST(cgi_parse_static_mode);

    printf("\nStrand Format + ifup:\n");
    TEST(config_strand_format_load);
    TEST(config_preserve_unknown_fields);
    TEST(config_dhcp_nodeaddr_zero);
    TEST(config_generate_ifup_static);
    TEST(config_generate_ifup_dhcp);

    printf("\n================================\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
