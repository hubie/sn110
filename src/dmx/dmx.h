/*
 * DMX device abstraction layer
 *
 * Provides a platform-independent interface to DMX ports.
 * On the SN110, this talks to /dev/dmx0 and /dev/dmx1.
 * On the host (for testing), this uses file-based mock devices.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SN110_DMX_H
#define SN110_DMX_H

#include "../common.h"

/* DMX device operations — platform abstraction */
typedef struct dmx_ops {
    /* Open a DMX port. Returns fd >= 0 on success, -1 on error. */
    int (*open)(const char *device);

    /* Set port mode (tx, rx, off, etc). Returns 0 on success. */
    int (*set_mode)(int fd, int mode);

    /* Write a complete DMX frame (up to 512 bytes). Returns bytes written. */
    int (*write_frame)(int fd, const uint8_t *data, int len);

    /* Read a DMX frame (for DMX IN). Returns bytes read. */
    int (*read_frame)(int fd, uint8_t *data, int max_len);

    /* Close the DMX port. */
    void (*close)(int fd);
} dmx_ops_t;

/* Get the appropriate DMX operations for the current platform.
 * driver: DMX_DRIVER_KERNEL (default) or DMX_DRIVER_DIRECT */
const dmx_ops_t *dmx_get_ops(int driver);

/* Device paths */
#ifdef HOST_BUILD
  #define DMX_DEVICE_0  "/tmp/sn110_mock_dmx0"
  #define DMX_DEVICE_1  "/tmp/sn110_mock_dmx1"
#else
  #define DMX_DEVICE_0  "/dev/dmx0"
  #define DMX_DEVICE_1  "/dev/dmx1"
#endif

#endif /* SN110_DMX_H */
