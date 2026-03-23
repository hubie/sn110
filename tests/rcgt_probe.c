/*
 * rcgt_probe — On-device validation tool for RCGT disable + parasitic RX
 *
 * Cross-compile as bFLT, FTP to device, run via telnet.
 * See: docs/plans/2026-03-22-001-fix-rcgt-disable-parasitic-rx-plan.md
 *
 * Usage:
 *   rcgt_probe read              Read UART registers for both channels
 *   rcgt_probe disable [dev]     Disable RCGT on CH1, read from dev (default /dev/dmx0)
 *   rcgt_probe tty               Disable RCGT, read from /dev/ttyS1
 *   rcgt_probe brk               Poll RX_BRK for 2 seconds, count transitions
 *   rcgt_probe map               Read both channels to determine connector mapping
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdint.h>

/* ===== NS7520 UART register definitions ===== */

#define SER_MODULE_BASE     0xFFD00000
#define SER_CH1_OFFSET      0x00
#define SER_CH2_OFFSET      0x40
#define MMAP_SIZE           0x80    /* Two channels, 64 bytes each */

/* Register offsets within a channel */
#define REG_CTRL_A          0x00
#define REG_CTRL_B          0x04
#define REG_STATUS_A        0x08
#define REG_BITRATE         0x0C
#define REG_FIFO            0x10
#define REG_RX_BUF_TMR      0x14
#define REG_RX_CHAR_TMR     0x18

/* Control B bits */
#define CTLB_RCGT_EN        0x04000000  /* RX char gap timer enable — the 6ms culprit */
#define CTLB_RBGT_EN        0x08000000  /* RX buffer gap timer enable */

/* Status A bits */
#define STATA_RX_BRK        0x00008000  /* Break detected */
#define STATA_RX_FRMERR     0x00004000  /* Framing error */
#define STATA_RX_OVERRUN    0x00001000  /* FIFO overrun */
#define STATA_RX_RDY        0x00000800  /* Data in RX FIFO */
#define STATA_RXFDB_MASK    0x00300000
#define STATA_RXFDB_SHIFT   20

/* ===== Register access helpers ===== */

static volatile uint32_t *g_regs;    /* mmap'd register base */

static uint32_t reg_read(int ch_offset, int reg)
{
    volatile uint32_t *addr = (volatile uint32_t *)
        ((char *)g_regs + ch_offset + reg);
    return *addr;
}

static void reg_write(int ch_offset, int reg, uint32_t val)
{
    volatile uint32_t *addr = (volatile uint32_t *)
        ((char *)g_regs + ch_offset + reg);
    *addr = val;
}

/* ===== mmap setup ===== */

static int setup_mmap(void)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        printf("ERROR: Cannot open /dev/mem (fd=%d)\n", fd);
        return -1;
    }

    g_regs = (volatile uint32_t *)mmap(
        0, MMAP_SIZE,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        fd, SER_MODULE_BASE);

    close(fd);

    if (g_regs == MAP_FAILED) {
        printf("ERROR: mmap failed for 0x%08X\n", SER_MODULE_BASE);
        g_regs = 0;
        return -1;
    }

    return 0;
}

static void cleanup_mmap(void)
{
    if (g_regs && g_regs != MAP_FAILED) {
        munmap((void *)g_regs, MMAP_SIZE);
        g_regs = 0;
    }
}

/* ===== Print helpers ===== */

static void print_channel_regs(const char *label, int ch_offset)
{
    uint32_t ctrl_a  = reg_read(ch_offset, REG_CTRL_A);
    uint32_t ctrl_b  = reg_read(ch_offset, REG_CTRL_B);
    uint32_t stat_a  = reg_read(ch_offset, REG_STATUS_A);
    uint32_t bitrate = reg_read(ch_offset, REG_BITRATE);
    uint32_t rx_char = reg_read(ch_offset, REG_RX_CHAR_TMR);

    printf("%s:\n", label);
    printf("  ctrl_a     = 0x%08lX\n", (unsigned long)ctrl_a);
    printf("  ctrl_b     = 0x%08lX", (unsigned long)ctrl_b);
    if (ctrl_b & CTLB_RCGT_EN)
        printf("  [RCGT_EN]");
    if (ctrl_b & CTLB_RBGT_EN)
        printf("  [RBGT_EN]");
    printf("\n");
    printf("  status_a   = 0x%08lX", (unsigned long)stat_a);
    if (stat_a & STATA_RX_BRK)     printf("  [RX_BRK]");
    if (stat_a & STATA_RX_OVERRUN) printf("  [OVERRUN]");
    if (stat_a & STATA_RX_RDY)     printf("  [RX_RDY]");
    printf("\n");
    printf("  bitrate    = 0x%08lX\n", (unsigned long)bitrate);
    printf("  rx_char_tm = 0x%08lX\n", (unsigned long)rx_char);
}

static void print_hex_line(const uint8_t *buf, int len)
{
    int i;
    int show = len > 16 ? 16 : len;
    for (i = 0; i < show; i++)
        printf("%02X ", buf[i]);
    if (len > 16)
        printf("...");
}

/* ===== Experiment: read registers ===== */

static int cmd_read(void)
{
    printf("=== UART Register Dump ===\n\n");
    print_channel_regs("CH1 (offset 0x00, likely /dev/dmx0)", SER_CH1_OFFSET);
    printf("\n");
    print_channel_regs("CH2 (offset 0x40, likely /dev/dmx1)", SER_CH2_OFFSET);
    return 0;
}

/* ===== Experiment: disable RCGT and read ===== */

static int cmd_disable(const char *devpath)
{
    uint32_t ctrl_b;
    uint8_t buf[513];
    int fd;
    int total_bytes = 0;
    int read_calls = 0;
    int i;

    printf("=== Disable RCGT on CH1 ===\n\n");

    /* Read current ctrl_b */
    ctrl_b = reg_read(SER_CH1_OFFSET, REG_CTRL_B);
    printf("ctrl_b BEFORE: 0x%08lX", (unsigned long)ctrl_b);
    if (ctrl_b & CTLB_RCGT_EN)
        printf("  [RCGT_EN set]");
    else
        printf("  [RCGT_EN already clear!]");
    printf("\n");

    /* Clear RCGT_EN */
    ctrl_b &= ~CTLB_RCGT_EN;
    reg_write(SER_CH1_OFFSET, REG_CTRL_B, ctrl_b);

    /* Verify */
    ctrl_b = reg_read(SER_CH1_OFFSET, REG_CTRL_B);
    printf("ctrl_b AFTER:  0x%08lX", (unsigned long)ctrl_b);
    if (ctrl_b & CTLB_RCGT_EN)
        printf("  [RCGT_EN still set — FAILED]");
    else
        printf("  [RCGT_EN cleared — OK]");
    printf("\n\n");

    /* Open device for reading */
    printf("Opening %s (non-blocking)...\n", devpath);
    fd = open(devpath, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        printf("ERROR: Cannot open %s (fd=%d)\n", devpath, fd);
        return -1;
    }

    /* Read loop for 10 seconds (2000 iterations * 5ms) */
    printf("Reading for 10 seconds...\n\n");
    for (i = 0; i < 2000; i++) {
        int n = read(fd, buf, sizeof(buf));
        uint32_t stat_a = reg_read(SER_CH1_OFFSET, REG_STATUS_A);

        if (n > 0) {
            read_calls++;
            total_bytes += n;
            if (read_calls <= 20) {
                /* Print first 20 reads in detail */
                printf("[%4d] read=%3d  sta=0x%08lX", i, n, (unsigned long)stat_a);
                if (stat_a & STATA_RX_BRK)     printf(" BRK");
                if (stat_a & STATA_RX_OVERRUN)  printf(" OVR");
                printf("  data: ");
                print_hex_line(buf, n);
                printf("\n");
            }
        } else if (i % 200 == 0) {
            /* Print status every second even if no data */
            printf("[%4d] no data  sta=0x%08lX", i, (unsigned long)stat_a);
            if (stat_a & STATA_RX_BRK)     printf(" BRK");
            if (stat_a & STATA_RX_OVERRUN)  printf(" OVR");
            printf("\n");
        }

        usleep(5000);  /* 5ms */
    }

    close(fd);

    printf("\n=== Summary ===\n");
    printf("Total reads with data: %d\n", read_calls);
    printf("Total bytes received:  %d\n", total_bytes);
    if (read_calls == 0)
        printf("No data received. Try 'rcgt_probe tty' to test /dev/ttyS1.\n");

    return 0;
}

/* ===== Experiment: poll RX_BRK ===== */

static int cmd_brk(void)
{
    int brk_count = 0;
    int prev_brk = 0;
    int overrun_count = 0;
    uint32_t stat_a;
    int i;

    printf("=== RX_BRK Polling (CH1, 2 seconds) ===\n\n");

    /* Also disable RCGT so data flows */
    {
        uint32_t ctrl_b = reg_read(SER_CH1_OFFSET, REG_CTRL_B);
        ctrl_b &= ~CTLB_RCGT_EN;
        reg_write(SER_CH1_OFFSET, REG_CTRL_B, ctrl_b);
        printf("RCGT disabled on CH1\n\n");
    }

    /* Tight poll for 2 seconds (checking every 100us = 20000 iterations) */
    printf("Polling status_a for BRK transitions...\n");
    for (i = 0; i < 20000; i++) {
        stat_a = reg_read(SER_CH1_OFFSET, REG_STATUS_A);

        int cur_brk = (stat_a & STATA_RX_BRK) ? 1 : 0;
        if (cur_brk && !prev_brk) {
            brk_count++;
            if (brk_count <= 10) {
                printf("  BRK #%d at iter %d (sta=0x%08lX)\n",
                       brk_count, i, (unsigned long)stat_a);
            }
        }
        prev_brk = cur_brk;

        if (stat_a & STATA_RX_OVERRUN)
            overrun_count++;

        usleep(100);  /* 100us */
    }

    printf("\n=== BRK Summary ===\n");
    printf("BRK transitions (0->1): %d in 2 seconds\n", brk_count);
    printf("Expected (44 fps DMX):  ~88\n");
    printf("Overrun flags seen:     %d\n", overrun_count);

    /* Test clear semantics */
    printf("\n=== BRK Clear Test ===\n");
    stat_a = reg_read(SER_CH1_OFFSET, REG_STATUS_A);
    printf("status_a before clear: 0x%08lX", (unsigned long)stat_a);
    if (stat_a & STATA_RX_BRK) printf(" [BRK]");
    printf("\n");

    /* Write 1 to BRK bit to try to clear it */
    reg_write(SER_CH1_OFFSET, REG_STATUS_A, STATA_RX_BRK);
    usleep(100);
    stat_a = reg_read(SER_CH1_OFFSET, REG_STATUS_A);
    printf("status_a after w1c:    0x%08lX", (unsigned long)stat_a);
    if (stat_a & STATA_RX_BRK)
        printf(" [BRK still set — NOT write-1-to-clear]");
    else
        printf(" [BRK cleared — write-1-to-clear confirmed]");
    printf("\n");

    return 0;
}

/* ===== Experiment: channel mapping ===== */

static int cmd_map(void)
{
    int ch1_bytes = 0, ch2_bytes = 0;
    int ch1_brk = 0, ch2_brk = 0;
    uint32_t stat_a;
    int i;

    printf("=== Channel Mapping Test (5 seconds) ===\n\n");
    printf("Send DMX on ONE connector only.\n");
    printf("This test reads both channels to see which gets data.\n\n");

    /* Disable RCGT on both channels */
    {
        uint32_t ctrl_b;
        ctrl_b = reg_read(SER_CH1_OFFSET, REG_CTRL_B);
        ctrl_b &= ~CTLB_RCGT_EN;
        reg_write(SER_CH1_OFFSET, REG_CTRL_B, ctrl_b);

        ctrl_b = reg_read(SER_CH2_OFFSET, REG_CTRL_B);
        ctrl_b &= ~CTLB_RCGT_EN;
        reg_write(SER_CH2_OFFSET, REG_CTRL_B, ctrl_b);
        printf("RCGT disabled on both channels\n\n");
    }

    /* Poll both channels for 5 seconds */
    for (i = 0; i < 5000; i++) {
        stat_a = reg_read(SER_CH1_OFFSET, REG_STATUS_A);
        if (stat_a & STATA_RX_RDY)  ch1_bytes++;
        if (stat_a & STATA_RX_BRK)  ch1_brk++;

        stat_a = reg_read(SER_CH2_OFFSET, REG_STATUS_A);
        if (stat_a & STATA_RX_RDY)  ch2_bytes++;
        if (stat_a & STATA_RX_BRK)  ch2_brk++;

        usleep(1000);  /* 1ms */
    }

    printf("=== Results ===\n");
    printf("CH1 (offset 0x00): RX_RDY count=%d, BRK count=%d\n", ch1_bytes, ch1_brk);
    printf("CH2 (offset 0x40): RX_RDY count=%d, BRK count=%d\n", ch2_bytes, ch2_brk);
    printf("\n");

    if (ch1_bytes > 0 && ch2_bytes == 0)
        printf("RESULT: Active connector -> CH1 (offset 0x00)\n");
    else if (ch2_bytes > 0 && ch1_bytes == 0)
        printf("RESULT: Active connector -> CH2 (offset 0x40)\n");
    else if (ch1_bytes > 0 && ch2_bytes > 0)
        printf("RESULT: Both channels have data — check if DMX is on both connectors\n");
    else
        printf("RESULT: No data on either channel — check DMX source\n");

    return 0;
}

/* ===== Usage ===== */

static void usage(void)
{
    printf("rcgt_probe — RCGT disable validation tool for SN110\n\n");
    printf("Usage:\n");
    printf("  rcgt_probe read              Read UART registers\n");
    printf("  rcgt_probe disable [dev]     Disable RCGT, read from dev\n");
    printf("                               (default: /dev/dmx0)\n");
    printf("  rcgt_probe tty               Disable RCGT, read from /dev/ttyS1\n");
    printf("  rcgt_probe brk               Poll RX_BRK, count transitions\n");
    printf("  rcgt_probe map               Channel mapping (send DMX on one port)\n");
}

/* ===== Main ===== */

int main(int argc, char *argv[])
{
    int ret;

    if (argc < 2) {
        usage();
        return 1;
    }

    if (setup_mmap() < 0)
        return 1;

    if (strcmp(argv[1], "read") == 0) {
        ret = cmd_read();
    }
    else if (strcmp(argv[1], "disable") == 0) {
        const char *dev = (argc > 2) ? argv[2] : "/dev/dmx0";
        ret = cmd_disable(dev);
    }
    else if (strcmp(argv[1], "tty") == 0) {
        ret = cmd_disable("/dev/ttyS1");
    }
    else if (strcmp(argv[1], "brk") == 0) {
        ret = cmd_brk();
    }
    else if (strcmp(argv[1], "map") == 0) {
        ret = cmd_map();
    }
    else {
        printf("Unknown command: %s\n\n", argv[1]);
        usage();
        ret = 1;
    }

    cleanup_mmap();
    return ret;
}
