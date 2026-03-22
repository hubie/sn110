/*
 * CGI form parsing — URL decode and form data parser
 *
 * Shared between cgi_config.c (device binary) and test_basics.c (host tests).
 * All functions are static to avoid multiple-definition issues.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef CGI_PARSE_H
#define CGI_PARSE_H

#include "../common.h"
#include "../config/config.h"

#include <string.h>
#include <stdlib.h>

static int _hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Decode URL-encoded string: %XX → byte, '+' → space.
 * Writes at most maxlen-1 chars plus null terminator.
 */
static void url_decode(char *dst, const char *src, int maxlen)
{
    int i = 0;

    while (*src && i < maxlen - 1) {
        if (*src == '%' && src[1] && src[2]) {
            int hi = _hex_digit(src[1]);
            int lo = _hex_digit(src[2]);
            if (hi >= 0 && lo >= 0) {
                dst[i++] = (char)((hi << 4) | lo);
                src += 3;
                continue;
            }
        }
        if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

/*
 * Parse URL-encoded form body into config struct.
 * Body format: key=value&key=value&...
 * Values are URL-decoded before being stored.
 */
/* Clamp integer to range [lo, hi] */
static int _clamp(int val, int lo, int hi)
{
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

static void parse_formdata(const char *body, node_config_t *cfg)
{
    char key[64], raw[192], val[192];

    while (*body) {
        /* Extract key */
        int ki = 0;
        while (*body && *body != '=' && ki < 63)
            key[ki++] = *body++;
        key[ki] = '\0';

        if (*body == '=') body++;

        /* Extract raw value (still URL-encoded) */
        int vi = 0;
        while (*body && *body != '&' && vi < 191)
            raw[vi++] = *body++;
        raw[vi] = '\0';

        if (*body == '&') body++;

        /* URL-decode value */
        url_decode(val, raw, sizeof(val));

        /* Map to config fields */
        if (strcmp(key, "hostname") == 0) {
            strncpy(cfg->hostname, val, sizeof(cfg->hostname) - 1);
            cfg->hostname[sizeof(cfg->hostname) - 1] = '\0';
        }
        else if (strcmp(key, "ipaddr") == 0)
            cfg->ip_addr = parse_ip(val);
        else if (strcmp(key, "netmask") == 0)
            cfg->netmask = parse_ip(val);
        else if (strcmp(key, "gateway") == 0)
            cfg->gateway = parse_ip(val);
        else if (strcmp(key, "protocol") == 0)
            cfg->active_protocol = parse_protocol(val);
        else if (strcmp(key, "port0_mode") == 0)
            cfg->ports[0].mode = parse_mode(val);
        else if (strcmp(key, "port1_mode") == 0)
            cfg->ports[1].mode = parse_mode(val);
        else if (strcmp(key, "port0_universe") == 0)
            cfg->ports[0].universe = _clamp(atoi(val), 1, 63999);
        else if (strcmp(key, "port1_universe") == 0)
            cfg->ports[1].universe = _clamp(atoi(val), 1, 63999);
        else if (strcmp(key, "port0_label") == 0) {
            strncpy(cfg->ports[0].label, val, 8);
            cfg->ports[0].label[8] = '\0';
        }
        else if (strcmp(key, "port1_label") == 0) {
            strncpy(cfg->ports[1].label, val, 8);
            cfg->ports[1].label[8] = '\0';
        }
        else if (strcmp(key, "dmx_hold_time") == 0)
            cfg->dmx_hold_time = _clamp(atoi(val), 0, 300);
        else if (strcmp(key, "addr_mode") == 0)
            cfg->addr_mode = parse_addr_mode(val);
        else if (strcmp(key, "lcd_contrast") == 0)
            cfg->lcd_contrast = _clamp(atoi(val), 0, 255);
        else if (strcmp(key, "lcd_backlight") == 0)
            cfg->lcd_backlight = parse_backlight(val);
        else if (strcmp(key, "slot_monitor_0") == 0)
            cfg->dmx_slot_monitor[0] = _clamp(atoi(val), 0, 512);
        else if (strcmp(key, "slot_monitor_1") == 0)
            cfg->dmx_slot_monitor[1] = _clamp(atoi(val), 0, 512);
        else if (strcmp(key, "dmx_driver") == 0) {
            if (strcmp(val, "direct") == 0)
                cfg->dmx_driver = DMX_DRIVER_DIRECT;
            else
                cfg->dmx_driver = DMX_DRIVER_KERNEL;
        }
    }

    /* DHCP mode zeros out address fields for Strand compat */
    if (cfg->addr_mode == ADDR_MODE_DHCP) {
        cfg->ip_addr = 0;
        cfg->netmask = 0;
        cfg->gateway = 0;
    }
}

#endif /* CGI_PARSE_H */
