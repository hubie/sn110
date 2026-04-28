/*
 * lcd.h — LCD display module for SN110
 *
 * Context-aware display with multiple modes:
 *   BOOT      — firmware name + version splash
 *   DHCP      — "DHCP..." + MAC while awaiting lease
 *   NORMAL    — hostname, IP/MAC carousel, per-port state
 *   LINK_DOWN — same as NORMAL but Line 2 shows "LINK DOWN"
 *
 * The caller (main.c) populates an lcd_state_t snapshot each tick
 * and passes it to lcd_update(). This module owns no network or DMX
 * state — it only reads the snapshot and drives the display.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SN110_LCD_H
#define SN110_LCD_H

#include <stdint.h>

#define LCD_PORT_COUNT 2

/* State snapshot passed from main.c each tick */
typedef struct {
    /* Network identity */
    char     hostname[16];        /* empty string = no hostname configured */
    uint32_t ip_addr;             /* 0 = not yet assigned (host byte order) */
    uint8_t  mac[6];
    uint8_t  addr_mode;           /* ADDR_MODE_* from common.h */
    int      link_up;             /* 1 = Ethernet link active, 0 = down */

    /* Per-port state */
    struct {
        int      mode;            /* DMX_MODE_OFF / TX / RX */
        uint16_t universe;        /* 1-based */
        int      live;            /* 1 = actively receiving/sending data */
        int      held;            /* 1 = source lost, holding last values (TX ports only) */
        uint32_t hold_remaining_ms; /* ms until hold expires (0 if not held; TX ports only) */
    } port[LCD_PORT_COUNT];
} lcd_state_t;

/*
 * Initialize the LCD: clear both buffers, set contrast/backlight,
 * show boot splash. Call once at startup.
 */
void lcd_init(int contrast, int backlight);

/*
 * Update the display based on current state.
 * Call once per second from the main loop.
 * Manages mode transitions, carousel, and flash state internally.
 */
void lcd_update(const lcd_state_t *state);

/*
 * Clean shutdown: clear screen and turn off backlight.
 * Call on SIGTERM / exit.
 */
void lcd_shutdown(void);

#endif /* SN110_LCD_H */
