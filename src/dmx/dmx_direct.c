/*
 * Userspace UART driver for DMX — direct register access via /dev/mem
 *
 * Bypasses the kernel serial driver's RCGT (RX Character Gap Timer) which
 * requires >6ms idle between received DMX frames. Many modern consoles
 * send frames back-to-back with only the minimum 176µs break, causing
 * the kernel driver to miss frames.
 *
 * This driver mmaps the NS7520 serial module registers and uses hardware
 * break detection (instant, bit-level) instead of the kernel's gap timer.
 *
 * Opt-in via config: dmx_driver = direct
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef HOST_BUILD

#include "dmx.h"
#include "dmx_direct.h"
#include "../common.h"

#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

extern int nanosleep(const void *req, void *rem);

/* ================================================================== */
/* Internal state                                                      */
/* ================================================================== */

struct _timespec_direct {
    long tv_sec;
    long tv_nsec;
};

/* Per-port state */
static struct {
    volatile uint32_t *regs;    /* mmap'd register base for this channel */
    int    active;              /* 1 if port is open */
    int    port_mode;           /* current DMX_MODE_* */
    int    rx_pos;              /* write position in rx_buf */
    int    rx_have_break;       /* saw a break — frame in progress */
    uint8_t rx_buf[DMX_UNIVERSE_SIZE];
} dport[2];

static volatile uint32_t *mapped_base;  /* mmap'd base of serial module */
static int mem_fd = -1;                 /* /dev/mem file descriptor */

/* Channel register offsets indexed by port number.
 * /dev/dmx0 → serial channel at offset 0x00 (CH1 in datasheet)
 * /dev/dmx1 → serial channel at offset 0x40 (CH2 in datasheet)
 * Note: verify mapping matches hardware during on-device testing. */
static const unsigned int ch_offset[2] = { 0x00, 0x40 };

/* ================================================================== */
/* Register access helpers                                             */
/* ================================================================== */

static inline uint32_t reg_read(int port, int reg)
{
    return dport[port].regs[reg >> 2];
}

static inline void reg_write(int port, int reg, uint32_t val)
{
    dport[port].regs[reg >> 2] = val;
}

static void usleep_direct(unsigned int us)
{
    struct _timespec_direct ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = (long)us * 1000;
    nanosleep(&ts, NULL);
}

/* ================================================================== */
/* Map serial module registers via /dev/mem                            */
/* ================================================================== */

static int ensure_mapped(void)
{
    if (mapped_base)
        return 0;

    mem_fd = open("/dev/mem", O_RDWR | O_SYNC, 0);
    if (mem_fd < 0)
        return -1;

    mapped_base = (volatile uint32_t *)mmap(
        NULL, 4096,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        mem_fd, SER_MODULE_BASE);

    if (mapped_base == (volatile uint32_t *)MAP_FAILED) {
        mapped_base = NULL;
        close(mem_fd);
        mem_fd = -1;
        return -1;
    }

    /* Set up per-port register pointers */
    dport[0].regs = (volatile uint32_t *)((char *)mapped_base + ch_offset[0]);
    dport[1].regs = (volatile uint32_t *)((char *)mapped_base + ch_offset[1]);

    return 0;
}

/* ================================================================== */
/* Open — parse device path, map registers                             */
/* ================================================================== */

static int direct_open(const char *device)
{
    int port;
    uint32_t ctrl_a;

    /* Parse port number from device path: /dev/dmx0 → 0, /dev/dmx1 → 1 */
    if (device && strlen(device) > 0) {
        char last = device[strlen(device) - 1];
        port = (last == '1') ? 1 : 0;
    } else {
        port = 0;
    }

    if (port < 0 || port > 1)
        return -1;

    if (ensure_mapped() < 0)
        return -1;

    /* Clear interrupt enable bits to prevent kernel ISR interference.
     * Without IE bits set, the kernel's serial ISR is a no-op for this channel. */
    ctrl_a = reg_read(port, REG_CTRL_A);
    ctrl_a &= ~(CTLA_IE_RX_ALL | CTLA_IE_TX_ALL);
    /* Configure for DMX: 8 data bits, no parity, 2 stop bits, enabled */
    ctrl_a |= CTLA_ENABLE | CTLA_8BITS | CTLA_P_NONE | CTLA_2STOP;
    reg_write(port, REG_CTRL_A, ctrl_a);

    /* Disable RCGT (the 6ms gap timer) and RBGT in control B */
    {
        uint32_t ctrl_b = reg_read(port, REG_CTRL_B);
        ctrl_b &= ~(CTLB_RCGT_EN | CTLB_RBGT_EN);
        ctrl_b |= CTLB_UART_MODE;
        reg_write(port, REG_CTRL_B, ctrl_b);
    }

    dport[port].active = 1;
    dport[port].port_mode = DMX_MODE_OFF;
    dport[port].rx_pos = 0;
    dport[port].rx_have_break = 0;

    /* Return port index as pseudo-fd */
    return port;
}

/* ================================================================== */
/* Set mode — configure channel for TX, RX, or OFF                     */
/* ================================================================== */

static int direct_set_mode(int fd, int mode)
{
    int port = fd;
    uint32_t ctrl_a;

    if (port < 0 || port > 1 || !dport[port].active)
        return -1;

    ctrl_a = reg_read(port, REG_CTRL_A);

    switch (mode) {
    case DMX_MODE_TX:
        /* Clear any pending break, enable TX */
        ctrl_a &= ~CTLA_BRK;
        ctrl_a &= ~(CTLA_IE_RX_ALL | CTLA_IE_TX_ALL);
        ctrl_a |= CTLA_ENABLE | CTLA_8BITS | CTLA_P_NONE | CTLA_2STOP;
        break;

    case DMX_MODE_RX:
        /* Enable RX, clear break flag, flush FIFO by reading it */
        ctrl_a &= ~(CTLA_BRK | CTLA_IE_RX_ALL | CTLA_IE_TX_ALL);
        ctrl_a |= CTLA_ENABLE | CTLA_8BITS | CTLA_P_NONE | CTLA_2STOP;
        dport[port].rx_pos = 0;
        dport[port].rx_have_break = 0;
        /* Flush RX FIFO */
        {
            uint32_t status;
            int flush_count = 0;
            do {
                status = reg_read(port, REG_STATUS_A);
                if (status & STATA_RX_RDY) {
                    (void)reg_read(port, REG_FIFO);
                    flush_count++;
                }
            } while ((status & STATA_RX_RDY) && flush_count < 64);
            /* Clear error/break flags */
            reg_write(port, REG_STATUS_A, STATA_CLR_ALL);
        }
        break;

    case DMX_MODE_OFF:
    default:
        ctrl_a &= ~CTLA_ENABLE;
        break;
    }

    reg_write(port, REG_CTRL_A, ctrl_a);
    dport[port].port_mode = mode;
    return 0;
}

/* ================================================================== */
/* TX — write a complete DMX frame with break/MAB timing               */
/* ================================================================== */

static int direct_write_frame(int fd, const uint8_t *data, int len)
{
    int port = fd;
    uint32_t ctrl_a;
    int sent = 0;
    int total;

    if (port < 0 || port > 1 || !dport[port].active)
        return -1;
    if (len > DMX_UNIVERSE_SIZE)
        len = DMX_UNIVERSE_SIZE;

    /* Total bytes: start code (0x00) + data */
    total = 1 + len;

    /* --- Break --- */
    ctrl_a = reg_read(port, REG_CTRL_A);
    ctrl_a |= CTLA_BRK;
    reg_write(port, REG_CTRL_A, ctrl_a);
    usleep_direct(DMX_BREAK_US);

    /* --- Mark After Break --- */
    ctrl_a &= ~CTLA_BRK;
    reg_write(port, REG_CTRL_A, ctrl_a);
    usleep_direct(DMX_MAB_US);

    /* --- Start code (0x00) + channel data --- */
    /* Write to FIFO in 4-byte words, polling TX_RDY between batches.
     * The FIFO is 32 bytes deep. First byte in MSB of word. */
    {
        uint8_t frame[1 + DMX_UNIVERSE_SIZE];

        frame[0] = 0x00;  /* DMX start code */
        memcpy(frame + 1, data, len);

        while (sent < total) {
            uint32_t word = 0;
            int bytes_in_word = 0;
            int j;

            /* Pack up to 4 bytes into one FIFO word, MSB first */
            for (j = 0; j < 4 && sent + j < total; j++) {
                word |= (uint32_t)frame[sent + j] << (24 - j * 8);
                bytes_in_word++;
            }

            /* Poll until TX FIFO has room */
            {
                int timeout = 10000;
                while (!(reg_read(port, REG_STATUS_A) & STATA_TX_RDY)) {
                    if (--timeout <= 0)
                        return sent;
                }
            }

            reg_write(port, REG_FIFO, word);
            sent += bytes_in_word;
        }
    }

    /* Wait for TX to complete (all bytes shifted out) */
    {
        int timeout = 50000;
        while (!(reg_read(port, REG_STATUS_A) & STATA_TX_RDY)) {
            if (--timeout <= 0)
                break;
        }
    }

    return len;
}

/* ================================================================== */
/* RX — poll for break + FIFO data, non-blocking                       */
/* ================================================================== */

static int direct_read_frame(int fd, uint8_t *data, int max_len)
{
    int port = fd;
    uint32_t status;
    int completed = 0;

    if (port < 0 || port > 1 || !dport[port].active)
        return -1;
    if (max_len > DMX_UNIVERSE_SIZE)
        max_len = DMX_UNIVERSE_SIZE;

    status = reg_read(port, REG_STATUS_A);

    /* Check for break — this is a frame boundary */
    if (status & STATA_RX_BRK) {
        /* If we had data accumulated, that's a complete frame */
        if (dport[port].rx_have_break && dport[port].rx_pos > 0) {
            int frame_len = dport[port].rx_pos;
            if (frame_len > max_len)
                frame_len = max_len;

            /* Skip start code (first byte), copy channel data */
            if (frame_len > 1) {
                memcpy(data, dport[port].rx_buf + 1, frame_len - 1);
                completed = frame_len - 1;
            }
        }

        /* Clear break flag and start new frame */
        reg_write(port, REG_STATUS_A, STATA_RX_BRK | STATA_RX_FRMERR);
        dport[port].rx_pos = 0;
        dport[port].rx_have_break = 1;
    }

    /* Read available FIFO data into the accumulation buffer */
    {
        int read_count = 0;
        status = reg_read(port, REG_STATUS_A);

        while ((status & STATA_RX_RDY) && read_count < 256) {
            uint32_t rxfdb;
            uint32_t word;
            int nbytes, i;

            /* Determine valid bytes in this FIFO word */
            rxfdb = (status & STATA_RXFDB_MASK) >> STATA_RXFDB_SHIFT;
            nbytes = (rxfdb == 0) ? 4 : (int)rxfdb;

            word = reg_read(port, REG_FIFO);

            /* Extract bytes (MSB first) into accumulation buffer */
            for (i = 0; i < nbytes && dport[port].rx_pos < DMX_UNIVERSE_SIZE; i++) {
                dport[port].rx_buf[dport[port].rx_pos++] =
                    (uint8_t)(word >> (24 - i * 8));
            }

            read_count++;
            status = reg_read(port, REG_STATUS_A);
        }

        /* Clear any error flags */
        if (status & (STATA_RX_FRMERR | STATA_RX_OVERRUN))
            reg_write(port, REG_STATUS_A,
                      status & (STATA_RX_FRMERR | STATA_RX_OVERRUN));
    }

    return completed;
}

/* ================================================================== */
/* Close — disable channel, release resources                          */
/* ================================================================== */

static void direct_close(int fd)
{
    int port = fd;
    uint32_t ctrl_a;

    if (port < 0 || port > 1 || !dport[port].active)
        return;

    /* Disable the channel */
    ctrl_a = reg_read(port, REG_CTRL_A);
    ctrl_a &= ~CTLA_ENABLE;
    reg_write(port, REG_CTRL_A, ctrl_a);

    dport[port].active = 0;
    dport[port].port_mode = DMX_MODE_OFF;

    /* Unmap if both ports are closed */
    if (!dport[0].active && !dport[1].active && mapped_base) {
        munmap((void *)mapped_base, 4096);
        mapped_base = NULL;
        if (mem_fd >= 0) {
            close(mem_fd);
            mem_fd = -1;
        }
    }
}

/* ================================================================== */
/* Operations table                                                    */
/* ================================================================== */

static const dmx_ops_t direct_ops = {
    .open        = direct_open,
    .set_mode    = direct_set_mode,
    .write_frame = direct_write_frame,
    .read_frame  = direct_read_frame,
    .close       = direct_close,
};

const dmx_ops_t *dmx_direct_get_ops(void)
{
    return &direct_ops;
}

#endif /* !HOST_BUILD */
