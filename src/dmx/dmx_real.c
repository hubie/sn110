/*
 * Real DMX device driver for SN110 hardware
 *
 * Talks to /dev/dmx0 and /dev/dmx1 via the reverse-engineered ioctl interface.
 *
 * Key discovery: the factory dmxtst "test" mode (which passes the loopback
 * self-test) does NOT use the DMX ioctl at all.  It opens the device, writes
 * 512 bytes, CLOSES the fd, sleeps 50ms, then reopens.  The close() appears
 * to trigger the kernel driver to transmit the buffered data as a DMX frame.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef HOST_BUILD

#include "dmx.h"
#include "dmx_ioctl.h"
#include "../common.h"

#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

static int dmx_real_open(const char *device)
{
    return open(device, O_RDWR);
}

static int dmx_real_set_mode(int fd, int mode)
{
    struct dmx_config cfg;
    int nonblock = 1;

    memset(&cfg, 0, sizeof(cfg));

    /*
     * Configure the DMX port via ioctl.
     * Mode values (corrected from dmxtst binary analysis):
     *   1=TX (buf_size=512, rate=100), 2=RX, 3=RAW
     * TX port: set driver to TX mode so the UART is configured for output.
     * RX port: set driver to RX mode so the UART listens for incoming DMX.
     */
    cfg.mode = mode;
    if (mode == DMX_MODE_TX) {
        cfg.buf_size = DMX_UNIVERSE_SIZE;
        cfg.rate = 100;
    }
    /* RX and OFF modes: buf_size=0, rate=0 (per dmxtst) */

    if (mode == DMX_MODE_RX)
        ioctl(fd, SN110_FIONBIO, &nonblock);

    return ioctl(fd, DMX_IOC_SET_CONFIG, &cfg);
}

static int dmx_real_write_frame(int fd, const uint8_t *data, int len)
{
    if (len > DMX_UNIVERSE_SIZE)
        len = DMX_UNIVERSE_SIZE;

    return write(fd, data, len);
}

static int dmx_real_read_frame(int fd, uint8_t *data, int max_len)
{
    int n;

    if (max_len > DMX_UNIVERSE_SIZE)
        max_len = DMX_UNIVERSE_SIZE;

    n = read(fd, data, max_len);
    return n;
}

static void dmx_real_close(int fd)
{
    close(fd);
}

static const dmx_ops_t real_ops = {
    .open        = dmx_real_open,
    .set_mode    = dmx_real_set_mode,
    .write_frame = dmx_real_write_frame,
    .read_frame  = dmx_real_read_frame,
    .close       = dmx_real_close,
};

const dmx_ops_t *dmx_get_ops(void)
{
    return &real_ops;
}

#endif /* !HOST_BUILD */
