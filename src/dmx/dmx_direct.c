/*
 * Userspace UART driver for DMX — direct register access via /dev/mem
 *
 * Bypasses the kernel serial driver to get precise break/MAB timing
 * for DMX TX. The kernel's RCGT (RX Character Gap Timer) imposes a
 * ~6ms inter-frame sensitivity that limits compatibility with some
 * consoles, but its ISR-based FIFO draining is essential for RX.
 *
 * Therefore this driver is TX-ONLY. RX ports must use the kernel
 * driver (dmx_real.c). The main.c port loop handles this split.
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
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

/* ================================================================== */
/* Internal state                                                      */
/* ================================================================== */

/* Per-port state */
static struct {
    volatile uint32_t *regs;    /* mmap'd register base for this channel */
    int    active;              /* 1 if port is open */
    int    port_mode;           /* current DMX_MODE_* */
} dport[2];

static volatile uint32_t *mapped_base;  /* mmap'd base of serial module */
static int mem_fd = -1;                 /* /dev/mem file descriptor */

/* Channel register offsets indexed by port number.
 * /dev/dmx0 → serial channel at offset 0x00 (CH1 in datasheet)
 * /dev/dmx1 → serial channel at offset 0x40 (CH2 in datasheet)
 * TODO: verify mapping on hardware (see dmx_direct.h comment) */
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

/*
 * Microsecond spin-wait using gettimeofday.
 *
 * nanosleep has 10ms minimum granularity on uClinux 2.0 (HZ=100),
 * which is unusable for DMX break (92us) and MAB (12us) timing.
 * gettimeofday reads the hardware timer and has microsecond resolution.
 *
 * If the process is preempted by the scheduler, gettimeofday detects
 * the elapsed time immediately on return — no over-wait accumulation.
 */
static void spin_wait_us(unsigned int us)
{
    struct timeval start, now;
    long target = (long)us;

    gettimeofday(&start, NULL);
    for (;;) {
        gettimeofday(&now, NULL);
        long elapsed = (now.tv_sec - start.tv_sec) * 1000000L
                     + (now.tv_usec - start.tv_usec);
        if (elapsed >= target)
            break;
    }
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
/* Open — parse device path, map registers, configure UART             */
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

    /* Set baud rate explicitly — don't rely on kernel having set it.
     * DMX requires 250kbaud. With 18.432MHz crystal and PLL bypass,
     * the divisor is 0 (see dmx_direct.h for calculation). */
    reg_write(port, REG_BITRATE, DMX_BAUD_REG);

    /* Clear interrupt enable bits to prevent kernel ISR interference.
     * Without IE bits set, the kernel's serial ISR is a no-op for
     * this channel — it checks status and finds nothing to do. */
    ctrl_a = reg_read(port, REG_CTRL_A);
    ctrl_a &= ~(CTLA_IE_RX_ALL | CTLA_IE_TX_ALL);
    /* Configure for DMX: 8 data bits, no parity, 2 stop bits, enabled */
    ctrl_a |= CTLA_ENABLE | CTLA_8BITS | CTLA_P_NONE | CTLA_2STOP;
    reg_write(port, REG_CTRL_A, ctrl_a);

    /* Disable RCGT and RBGT gap timers in control B */
    {
        uint32_t ctrl_b = reg_read(port, REG_CTRL_B);
        ctrl_b &= ~(CTLB_RCGT_EN | CTLB_RBGT_EN);
        ctrl_b |= CTLB_UART_MODE;
        reg_write(port, REG_CTRL_B, ctrl_b);
    }

    dport[port].active = 1;
    dport[port].port_mode = DMX_MODE_OFF;

    /* Return port index as pseudo-fd */
    return port;
}

/* ================================================================== */
/* Set mode — configure channel for TX or OFF                          */
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
    spin_wait_us(DMX_BREAK_US);

    /* --- Mark After Break --- */
    ctrl_a &= ~CTLA_BRK;
    reg_write(port, REG_CTRL_A, ctrl_a);
    spin_wait_us(DMX_MAB_US);

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

    /* Wait for TX to complete.
     * STATA_TX_RDY means "FIFO has room" not "shift register empty."
     * After the last FIFO write, the UART is still clocking out data.
     * Worst case: 32 bytes in FIFO * 44us/byte = 1.408ms.
     * We spin-wait a conservative 1.5ms to ensure the last byte is
     * fully shifted out before the next frame's break. */
    spin_wait_us(1500);

    return len;
}

/* ================================================================== */
/* RX — not supported in direct mode                                   */
/* ================================================================== */

/*
 * Direct-mode RX is not implemented. The 32-byte FIFO fills in 1.4ms
 * at 250kbaud, but the main loop polls only every 23ms — causing ~16
 * overflows per frame. The kernel ISR drains the FIFO on interrupt,
 * which is essential for RX. Main.c routes RX ports to the kernel
 * driver even when dmx_driver=direct.
 */
static int direct_read_frame(int fd, uint8_t *data, int max_len)
{
    (void)fd;
    (void)data;
    (void)max_len;
    return 0;  /* No data — RX should use kernel driver */
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
