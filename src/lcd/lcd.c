/*
 * lcd.c — LCD display state machine and rendering
 *
 * Display modes:
 *   BOOT      → DHCP (if DHCP+ip==0) or NORMAL
 *   DHCP      → NORMAL (when ip acquired)
 *   NORMAL    ↔ LINK_DOWN (when link drops/restores)
 *
 * Normal layout (16 chars × 4 rows):
 *   Line 0: Hostname (or IP if no hostname)
 *   Line 1: IP/MAC carousel ~10s (or MAC only if no hostname)
 *   Line 2: ↑U1      ↑U2     (direction + universe, left/right split)
 *   Line 3: LIVE     HLD 4:59 (port state, left/right split)
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include "lcd.h"
#include "lcd_hw.h"
#include "../common.h"

#include <string.h>
#include <stdio.h>

_Static_assert(LCD_PORT_COUNT == DMX_MAX_PORTS,
               "LCD_PORT_COUNT must match DMX_MAX_PORTS");

/* ========================================================================= */
/* Display mode state machine                                                */
/* ========================================================================= */

typedef enum {
    MODE_BOOT,
    MODE_DHCP,
    MODE_NORMAL,
    MODE_LINK_DOWN
} lcd_mode_t;

/* ========================================================================= */
/* Transition flash state (per-port)                                         */
/* ========================================================================= */

typedef enum {
    FLASH_NONE,
    FLASH_SRC_LOST,
    FLASH_SRC_OK
} flash_state_t;

typedef struct {
    flash_state_t state;
    int           ticks_remaining;   /* counts down from 2 */
    int           prev_live;         /* previous tick's live state */
} port_flash_t;

/* ========================================================================= */
/* Module state                                                              */
/* ========================================================================= */

static lcd_mode_t   g_mode = MODE_BOOT;
static int          g_boot_ticks = 0;    /* counts up; splash ends at 5 */
static unsigned int g_carousel_tick = 0; /* IP/MAC toggle counter */
static port_flash_t g_flash[LCD_PORT_COUNT];

/* CP437 direction arrows — physical orientation relative to ports below display */
#define GLYPH_RX  '\x18'  /* ↑ up arrow: data coming UP from port/cable */
#define GLYPH_TX  '\x19'  /* ↓ down arrow: data going DOWN to port/cable */

/* ========================================================================= */
/* Formatting helpers                                                        */
/* ========================================================================= */

static void format_ip(char *buf, int bufsize, uint32_t ip)
{
    snprintf(buf, bufsize, "%u.%u.%u.%u",
             (ip >> 24) & 0xff, (ip >> 16) & 0xff,
             (ip >> 8) & 0xff, ip & 0xff);
}

/*
 * MAC format: "00E001:00ECFD" (13 chars)
 * Colon at midpoint leverages the kernel's extra pixel at screen position 8.
 */
static void format_mac(char *buf, int bufsize, const uint8_t *mac)
{
    snprintf(buf, bufsize, "%02X%02X%02X:%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/*
 * Combine two 8-char halves into a 16-char line.
 * Each half is padded/truncated to exactly 8 chars.
 */
static void combine_halves(char *out, const char *left, const char *right)
{
    char l[9], r[9];
    int i, len;

    len = strlen(left);
    if (len > 8) len = 8;
    memcpy(l, left, len);
    for (i = len; i < 8; i++) l[i] = ' ';
    l[8] = '\0';

    len = strlen(right);
    if (len > 8) len = 8;
    memcpy(r, right, len);
    for (i = len; i < 8; i++) r[i] = ' ';
    r[8] = '\0';

    memcpy(out, l, 8);
    memcpy(out + 8, r, 8);
    out[16] = '\0';
}

/* ========================================================================= */
/* Mode: BOOT                                                                */
/* ========================================================================= */

static void render_boot(void)
{
    /* Splash is written once in lcd_init(); just count ticks here */
    g_boot_ticks++;
}

static lcd_mode_t boot_next_mode(const lcd_state_t *state)
{
    if (g_boot_ticks < 5)
        return MODE_BOOT;

    /* DHCP mode if configured for DHCP and no IP yet */
    if ((state->addr_mode == ADDR_MODE_DHCP ||
         state->addr_mode == ADDR_MODE_DHCP_STATIC) &&
        state->ip_addr == 0)
        return MODE_DHCP;

    return MODE_NORMAL;
}

/* ========================================================================= */
/* Mode: DHCP                                                                */
/* ========================================================================= */

static void render_dhcp(const lcd_state_t *state)
{
    char mac[16];

    lcd_hw_write_line(0, "DHCP...");
    format_mac(mac, sizeof(mac), state->mac);
    lcd_hw_write_line(1, mac);
    lcd_hw_write_line(2, "");
    lcd_hw_write_line(3, "");
}

/* ========================================================================= */
/* Mode: NORMAL / LINK_DOWN                                                  */
/* ========================================================================= */

/* Line 0: hostname or IP */
static void render_line0(const lcd_state_t *state)
{
    char ip[16];

    if (state->hostname[0]) {
        lcd_hw_write_line(0, state->hostname);
    } else {
        format_ip(ip, sizeof(ip), state->ip_addr);
        lcd_hw_write_line(0, ip);
    }
}

/* Line 1: IP/MAC carousel (or LINK DOWN) */
static void render_line1(const lcd_state_t *state, lcd_mode_t mode)
{
    char buf[17];

    if (mode == MODE_LINK_DOWN) {
        lcd_hw_write_line(1, "LINK DOWN");
        return;
    }

    if (!state->hostname[0]) {
        /* No hostname → IP is on Line 0, MAC always on Line 1 */
        format_mac(buf, sizeof(buf), state->mac);
    } else if ((g_carousel_tick / 10) % 2 == 0) {
        format_ip(buf, sizeof(buf), state->ip_addr);
    } else {
        format_mac(buf, sizeof(buf), state->mac);
    }
    lcd_hw_write_line(1, buf);
}

/* Line 2: direction arrow + universe per port */
static void render_line2(const lcd_state_t *state)
{
    char half[2][9];
    char line[LCD_COLS + 1];
    int i;

    for (i = 0; i < LCD_PORT_COUNT; i++) {
        if (state->port[i].mode == DMX_MODE_OFF) {
            memset(half[i], ' ', 8);
            half[i][8] = '\0';
        } else {
            char arrow = (state->port[i].mode == DMX_MODE_TX)
                         ? GLYPH_TX : GLYPH_RX;
            snprintf(half[i], sizeof(half[i]), "%cU%u",
                     arrow, (unsigned)state->port[i].universe);
        }
    }

    combine_halves(line, half[0], half[1]);
    lcd_hw_write_line(2, line);
}

/* Line 3: port state with flash overlay and hold countdown */
static void render_line3(const lcd_state_t *state)
{
    char half[2][9];
    char line[LCD_COLS + 1];
    int i;

    for (i = 0; i < LCD_PORT_COUNT; i++) {
        if (state->port[i].mode == DMX_MODE_OFF) {
            memset(half[i], ' ', 8);
            half[i][8] = '\0';
            continue;
        }

        /* Check for transition flash (overrides normal state) */
        if (g_flash[i].ticks_remaining > 0) {
            if (g_flash[i].state == FLASH_SRC_LOST)
                snprintf(half[i], sizeof(half[i]), "SRC LOST");
            else
                snprintf(half[i], sizeof(half[i]), "SRC OK");
            g_flash[i].ticks_remaining--;
            continue;
        }

        /* Normal state rendering */
        if (state->port[i].held && state->port[i].hold_remaining_ms > 0) {
            /* Hold countdown: HLD M:SS */
            uint32_t secs = state->port[i].hold_remaining_ms / 1000;
            uint32_t mins = secs / 60;
            secs = secs % 60;
            if (mins > 9) mins = 9; /* cap display at 9:59 */
            snprintf(half[i], sizeof(half[i]), "HLD %u:%02u",
                     (unsigned)mins, (unsigned)secs);
        } else if (state->port[i].live) {
            snprintf(half[i], sizeof(half[i]), "LIVE");
        } else {
            snprintf(half[i], sizeof(half[i]), "IDLE");
        }
    }

    combine_halves(line, half[0], half[1]);
    lcd_hw_write_line(3, line);
}

/* Detect live→!live and !live→live transitions, trigger flash */
static void update_flash(const lcd_state_t *state)
{
    int i;

    for (i = 0; i < LCD_PORT_COUNT; i++) {
        /* Flash only applies to TX-output ports */
        if (state->port[i].mode != DMX_MODE_TX) {
            g_flash[i].prev_live = state->port[i].live;
            continue;
        }

        if (state->port[i].live && !g_flash[i].prev_live) {
            /* Source recovered */
            g_flash[i].state = FLASH_SRC_OK;
            g_flash[i].ticks_remaining = 2;
        } else if (!state->port[i].live && g_flash[i].prev_live) {
            /* Source lost */
            g_flash[i].state = FLASH_SRC_LOST;
            g_flash[i].ticks_remaining = 2;
        }

        g_flash[i].prev_live = state->port[i].live;
    }
}

static void render_normal(const lcd_state_t *state, lcd_mode_t mode)
{
    g_carousel_tick++;
    update_flash(state);

    render_line0(state);
    render_line1(state, mode);
    render_line2(state);
    render_line3(state);
}

/* ========================================================================= */
/* Public API                                                                */
/* ========================================================================= */

void lcd_init(int contrast, int backlight)
{
    /* Clear both buffers to remove any prior pixel/character artifacts */
    lcd_hw_clear_pixels();
    lcd_hw_clear_chars();

    /* Apply config */
    lcd_hw_contrast(contrast);
    lcd_hw_backlight(backlight);

    /* Show boot splash */
    lcd_hw_write_line(0, "sn110dmx v" FW_VERSION);
    lcd_hw_write_line(1, " Open Firmware");
    lcd_hw_write_line(2, "");
    lcd_hw_write_line(3, "");

    g_mode = MODE_BOOT;
    g_boot_ticks = 0;
    g_carousel_tick = 0;
    memset(g_flash, 0, sizeof(g_flash));
}

void lcd_update(const lcd_state_t *state)
{
    switch (g_mode) {
    case MODE_BOOT:
        render_boot();
        g_mode = boot_next_mode(state);
        /* If transitioning out of boot, render first frame immediately */
        if (g_mode != MODE_BOOT)
            lcd_update(state);
        break;

    case MODE_DHCP:
        render_dhcp(state);
        if (state->ip_addr != 0)
            g_mode = MODE_NORMAL;
        break;

    case MODE_NORMAL:
        render_normal(state, MODE_NORMAL);
        if (!state->link_up)
            g_mode = MODE_LINK_DOWN;
        break;

    case MODE_LINK_DOWN:
        render_normal(state, MODE_LINK_DOWN);
        if (state->link_up)
            g_mode = MODE_NORMAL;
        break;
    }
}

void lcd_shutdown(void)
{
    lcd_hw_clear_pixels();
    lcd_hw_clear_chars();
    lcd_hw_backlight(0);
}
