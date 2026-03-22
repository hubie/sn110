/*
 * Mock DMX device driver for host-based testing
 *
 * Simulates /dev/dmxN using in-memory buffers so the full protocol stack
 * can be tested on a development machine without hardware.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifdef HOST_BUILD

#include "dmx.h"
#include "../common.h"

#include <string.h>
#include <stdio.h>

#define MOCK_MAX_PORTS 4

typedef struct {
    int      in_use;
    int      mode;
    uint8_t  tx_buf[DMX_UNIVERSE_SIZE];
    uint8_t  rx_buf[DMX_UNIVERSE_SIZE];
    int      tx_len;
    int      rx_len;
} mock_port_t;

static mock_port_t mock_ports[MOCK_MAX_PORTS];
static int mock_next_fd = 100;  /* fake fd values */

/* Expose internals for tests */
const uint8_t *mock_dmx_get_tx_buf(int fd)
{
    int idx = fd - 100;
    if (idx < 0 || idx >= MOCK_MAX_PORTS || !mock_ports[idx].in_use)
        return NULL;
    return mock_ports[idx].tx_buf;
}

int mock_dmx_get_tx_len(int fd)
{
    int idx = fd - 100;
    if (idx < 0 || idx >= MOCK_MAX_PORTS || !mock_ports[idx].in_use)
        return -1;
    return mock_ports[idx].tx_len;
}

void mock_dmx_inject_rx(int fd, const uint8_t *data, int len)
{
    int idx = fd - 100;
    if (idx < 0 || idx >= MOCK_MAX_PORTS || !mock_ports[idx].in_use)
        return;
    if (len > DMX_UNIVERSE_SIZE) len = DMX_UNIVERSE_SIZE;
    memcpy(mock_ports[idx].rx_buf, data, len);
    mock_ports[idx].rx_len = len;
}

void mock_dmx_reset(void)
{
    memset(mock_ports, 0, sizeof(mock_ports));
    mock_next_fd = 100;
}

static int dmx_mock_open(const char *device)
{
    int i;
    (void)device;
    for (i = 0; i < MOCK_MAX_PORTS; i++) {
        if (!mock_ports[i].in_use) {
            mock_ports[i].in_use = 1;
            mock_ports[i].mode = DMX_MODE_OFF;
            mock_ports[i].tx_len = 0;
            mock_ports[i].rx_len = 0;
            memset(mock_ports[i].tx_buf, 0, DMX_UNIVERSE_SIZE);
            memset(mock_ports[i].rx_buf, 0, DMX_UNIVERSE_SIZE);
            return 100 + i;
        }
    }
    return -1;
}

static int dmx_mock_set_mode(int fd, int mode)
{
    int idx = fd - 100;
    if (idx < 0 || idx >= MOCK_MAX_PORTS || !mock_ports[idx].in_use)
        return -1;
    mock_ports[idx].mode = mode;
    return 0;
}

static int dmx_mock_write_frame(int fd, const uint8_t *data, int len)
{
    int idx = fd - 100;
    if (idx < 0 || idx >= MOCK_MAX_PORTS || !mock_ports[idx].in_use)
        return -1;
    if (len > DMX_UNIVERSE_SIZE) len = DMX_UNIVERSE_SIZE;
    memcpy(mock_ports[idx].tx_buf, data, len);
    mock_ports[idx].tx_len = len;
    return len;
}

static int dmx_mock_read_frame(int fd, uint8_t *data, int max_len)
{
    int idx = fd - 100;
    if (idx < 0 || idx >= MOCK_MAX_PORTS || !mock_ports[idx].in_use)
        return -1;
    if (mock_ports[idx].rx_len == 0)
        return 0;
    if (max_len > mock_ports[idx].rx_len)
        max_len = mock_ports[idx].rx_len;
    memcpy(data, mock_ports[idx].rx_buf, max_len);
    return max_len;
}

static void dmx_mock_close(int fd)
{
    int idx = fd - 100;
    if (idx >= 0 && idx < MOCK_MAX_PORTS) {
        mock_ports[idx].in_use = 0;
    }
}

static const dmx_ops_t mock_ops = {
    .open        = dmx_mock_open,
    .set_mode    = dmx_mock_set_mode,
    .write_frame = dmx_mock_write_frame,
    .read_frame  = dmx_mock_read_frame,
    .close       = dmx_mock_close,
};

const dmx_ops_t *dmx_get_ops(int driver)
{
    (void)driver;  /* mock always returns mock ops */
    return &mock_ops;
}

const dmx_ops_t *dmx_get_rx_ops(void)
{
    return &mock_ops;
}

#endif /* HOST_BUILD */
