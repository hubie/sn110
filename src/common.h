/*
 * sn110dmx — Open-source multi-protocol DMX daemon for Strand SN110 nodes
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SN110_COMMON_H
#define SN110_COMMON_H

#include <stdint.h>

/* DMX constants */
#define DMX_UNIVERSE_SIZE   512
#define DMX_MAX_PORTS       2

/* Protocol identifiers */
#define PROTO_NONE          0
#define PROTO_SACN          1
#define PROTO_ARTNET        2
#define PROTO_SHOWNET       3

/*
 * DMX port modes (reverse-engineered from dmxtst binary disassembly)
 *
 * The kernel driver uses ioctl _IOW('d', 1, struct dmx_config) = 0x40106401
 * where the first byte of the 16-byte config struct is the mode:
 */
#define DMX_MODE_OFF        0   /* Disable DMX port */
#define DMX_MODE_RAW        1   /* Raw serial (buf_size=512, rate=100) */
#define DMX_MODE_TX         2   /* DMX transmit */
#define DMX_MODE_RX         3   /* DMX receive */

/* LCD backlight modes */
#define LCD_BACKLIGHT_OFF   0
#define LCD_BACKLIGHT_ON    1
#define LCD_BACKLIGHT_FLASH 2

/* Address mode — separates DHCP intent from current IP value */
#define ADDR_MODE_STATIC       0
#define ADDR_MODE_DHCP         1
#define ADDR_MODE_DHCP_STATIC  2

/* DMX driver selection — kernel (default) vs userspace direct UART access */
#define DMX_DRIVER_KERNEL  0   /* Default: use /dev/dmx kernel driver */
#define DMX_DRIVER_DIRECT  1   /* Userspace: mmap UART registers directly */

/* SN110 DMX ioctl: _IOW('d', 1, struct dmx_config) */
#define DMX_IOCTL_TYPE      'd'
#define DMX_IOCTL_NR        1
#define DMX_IOCTL_SIZE      16

/* DMX frame buffer */
typedef struct {
    uint8_t  data[DMX_UNIVERSE_SIZE];
    uint16_t length;          /* Number of valid channels (1-512) */
    uint8_t  priority;        /* sACN priority (0-200) */
    uint8_t  sequence;        /* Sequence number */
    uint32_t source_ip;       /* Source IP for tracking */
    uint32_t last_update_ms;  /* Timestamp of last update */
} dmx_frame_t;

/* Port configuration */
typedef struct {
    int      mode;            /* DMX_MODE_* */
    int      protocol;        /* PROTO_* — which protocol feeds this port */
    uint16_t universe;        /* Universe number (1-based for sACN, 0-based for Art-Net) */
    char     label[9];        /* 8-char label + null */
} port_config_t;

/* Global node configuration */
typedef struct {
    char     hostname[16];
    uint32_t ip_addr;
    uint32_t netmask;
    uint32_t gateway;
    uint8_t  mac[6];

    port_config_t ports[DMX_MAX_PORTS];

    int      active_protocol;  /* PROTO_* — primary protocol */
    uint16_t dmx_hold_time;    /* Seconds to hold last DMX values after source loss */

    uint8_t  addr_mode;          /* ADDR_MODE_* — DHCP vs static */
    uint8_t  lcd_contrast;       /* 0-63 */
    uint8_t  lcd_backlight;      /* LCD_BACKLIGHT_* */
    uint16_t dmx_slot_monitor[DMX_MAX_PORTS]; /* 0=disabled, 1-512=channel */

    uint8_t  dmx_driver;           /* DMX_DRIVER_* — kernel or direct UART */
} node_config_t;

#endif /* SN110_COMMON_H */
