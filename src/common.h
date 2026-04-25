/*
 * sn110dmx — Open-source multi-protocol DMX daemon for Strand SN110 nodes
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SN110_COMMON_H
#define SN110_COMMON_H

#include <stdint.h>

/* Firmware version — single source of truth for LOG and LCD splash */
#define FW_VERSION "0.3.0"

/* Source timeout — source considered lost after 2.5s of no packets.
 * Protocol-agnostic: applies to sACN, Art-Net, and ShowNet alike.
 * Used by dmx_output_cycle() and LCD state population. */
#define SOURCE_TIMEOUT_MS   2500

/* DMX constants */
#define DMX_UNIVERSE_SIZE   512
#define DMX_MAX_PORTS       2

/* Address mode (how the node gets its IP) */
#define ADDR_MODE_STATIC    0   /* Static IP from config */
#define ADDR_MODE_DHCP      1   /* DHCP only */
#define ADDR_MODE_DHCP_STATIC 2 /* DHCP with static fallback */
#define ADDR_MODE_SENTINEL  255 /* Unset — infer from nodeaddr */

/* LCD backlight modes */
#define LCD_BACKLIGHT_OFF   0
#define LCD_BACKLIGHT_ON    1
#define LCD_BACKLIGHT_AUTO  2

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
/*
 * DMX mode values — corrected from dmxtst binary reverse-engineering.
 * dmxtst "tx" → ioctl mode=1, "rx" → mode=2, "raw" → mode=3.
 * The original code had these swapped (RAW=1, TX=2, RX=3).
 */
#define DMX_MODE_OFF        0   /* Disable DMX port */
#define DMX_MODE_TX         1   /* DMX transmit */
#define DMX_MODE_RX         2   /* DMX receive */
#define DMX_MODE_RAW        3   /* Raw serial buffer (buf_size=512, rate=100) */

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

    uint8_t  addr_mode;        /* ADDR_MODE_* — how the node gets its IP */
    uint8_t  lcd_contrast;     /* LCD contrast (0-255) */
    uint8_t  lcd_backlight;    /* LCD_BACKLIGHT_* */
    uint16_t dmx_slot_monitor[2]; /* DMX slot to monitor per port (0=disabled, 1-512) */
} node_config_t;

#endif /* SN110_COMMON_H */
