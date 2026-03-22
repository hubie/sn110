/*
 * Real DMX device driver for SN110 hardware
 *
 * Talks to /dev/dmx0 and /dev/dmx1 via the reverse-engineered ioctl interface.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef HOST_BUILD

#include "dmx.h"
#include "dmx_ioctl.h"
#include "dmx_direct.h"
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
    int nonblock;

    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = (uint8_t)mode;

    if (mode == DMX_MODE_RAW) {
        cfg.buf_size = DMX_UNIVERSE_SIZE;
        cfg.rate = 100;
    }

    /* Set non-blocking I/O for TX and RX modes */
    if (mode == DMX_MODE_TX || mode == DMX_MODE_RX) {
        nonblock = 1;
        ioctl(fd, SN110_FIONBIO, &nonblock);
    }

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
    if (max_len > DMX_UNIVERSE_SIZE)
        max_len = DMX_UNIVERSE_SIZE;
    return read(fd, data, max_len);
}

static void dmx_real_close(int fd)
{
    struct dmx_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = DMX_MODE_OFF;
    ioctl(fd, DMX_IOC_SET_CONFIG, &cfg);
    close(fd);
}

static const dmx_ops_t real_ops = {
    .open        = dmx_real_open,
    .set_mode    = dmx_real_set_mode,
    .write_frame = dmx_real_write_frame,
    .read_frame  = dmx_real_read_frame,
    .close       = dmx_real_close,
};

const dmx_ops_t *dmx_get_ops(int driver)
{
    if (driver == DMX_DRIVER_DIRECT)
        return dmx_direct_get_ops();
    return &real_ops;
}

const dmx_ops_t *dmx_get_rx_ops(void)
{
    return &real_ops;
}

#endif /* !HOST_BUILD */
