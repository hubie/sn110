/*
 * Configuration file parser
 *
 * Reads and writes the 220node.cfg configuration in Strand format:
 *   nodeaddr = 192.168.0.71
 *   hostname = SN110
 *   macaddr = 00:E0:01:00:EC:FD
 *
 * We extend the format with keys for protocol settings (protocol,
 * sacn_universe_0, etc.). These are ignored by the original nodecfg
 * tool since it only looks for its own keys.
 *
 * On save, unknown/unmanaged lines (e.g. nodetype, boottest, dmx = ...)
 * are preserved verbatim so nodecfg can still read them.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include "config.h"
#include "../common.h"

#include <string.h>
#include <stdlib.h>

#ifdef HOST_BUILD
#include <stdio.h>
#else
/* uClinux has stdio but limited */
#include <stdio.h>
#endif

/* Strip leading/trailing whitespace in-place. Returns pointer into str. */
static char *trim(char *str)
{
    char *end;

    while (*str == ' ' || *str == '\t')
        str++;
    if (*str == '\0')
        return str;

    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t'))
        end--;
    end[1] = '\0';
    return str;
}

void config_defaults(node_config_t *config)
{
    memset(config, 0, sizeof(*config));
    strncpy(config->hostname, "SN110", sizeof(config->hostname) - 1);
    config->active_protocol = PROTO_SACN;
    config->dmx_hold_time = 5;

    config->ports[0].mode = DMX_MODE_TX;
    config->ports[0].protocol = PROTO_SACN;
    config->ports[0].universe = 1;

    config->ports[1].mode = DMX_MODE_TX;
    config->ports[1].protocol = PROTO_SACN;
    config->ports[1].universe = 2;

    config->addr_mode = ADDR_MODE_SENTINEL; /* will infer from nodeaddr */
    config->lcd_contrast = 128;
    config->lcd_backlight = LCD_BACKLIGHT_ON;
}

uint32_t parse_ip(const char *str)
{
    unsigned int a, b, c, d;
    if (sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) == 4) {
        if (a > 255 || b > 255 || c > 255 || d > 255)
            return 0;
        return (a << 24) | (b << 16) | (c << 8) | d;
    }
    return 0;
}

/* Parse a single hex byte (1-2 hex digits). Returns -1 on failure. */
static int _parse_hex_byte(const char **p)
{
    int val = 0;
    int digits = 0;
    while (digits < 2) {
        char c = **p;
        if (c >= '0' && c <= '9')
            val = (val << 4) | (c - '0');
        else if (c >= 'a' && c <= 'f')
            val = (val << 4) | (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            val = (val << 4) | (c - 'A' + 10);
        else
            break;
        (*p)++;
        digits++;
    }
    return digits > 0 ? val : -1;
}

/*
 * Parse MAC address string "XX:XX:XX:XX:XX:XX" using manual hex parsing.
 * minilib sscanf doesn't support %x, so we can't use sscanf here.
 */
static int parse_mac(const char *str, uint8_t *mac)
{
    const char *p = str;
    int i;

    for (i = 0; i < 6; i++) {
        int val = _parse_hex_byte(&p);
        if (val < 0)
            return -1;
        mac[i] = (uint8_t)val;
        if (i < 5) {
            if (*p != ':')
                return -1;
            p++;
        }
    }
    return 0;
}

int parse_mode(const char *str)
{
    if (strcmp(str, "tx") == 0 || strcmp(str, "TX") == 0)
        return DMX_MODE_TX;
    if (strcmp(str, "rx") == 0 || strcmp(str, "RX") == 0)
        return DMX_MODE_RX;
    if (strcmp(str, "off") == 0 || strcmp(str, "OFF") == 0)
        return DMX_MODE_OFF;
    return DMX_MODE_TX; /* default to TX for backward compatibility */
}

int parse_protocol(const char *str)
{
    if (strcmp(str, "sacn") == 0 || strcmp(str, "sACN") == 0)
        return PROTO_SACN;
    if (strcmp(str, "artnet") == 0 || strcmp(str, "Art-Net") == 0)
        return PROTO_ARTNET;
    if (strcmp(str, "shownet") == 0 || strcmp(str, "ShowNet") == 0)
        return PROTO_SHOWNET;
    return PROTO_NONE;
}

int parse_addr_mode(const char *str)
{
    if (strcmp(str, "dhcp") == 0)
        return ADDR_MODE_DHCP;
    if (strcmp(str, "static") == 0)
        return ADDR_MODE_STATIC;
    if (strcmp(str, "dhcp_static") == 0)
        return ADDR_MODE_DHCP_STATIC;
    return ADDR_MODE_STATIC;
}

int parse_backlight(const char *str)
{
    if (strcmp(str, "off") == 0)
        return LCD_BACKLIGHT_OFF;
    if (strcmp(str, "on") == 0)
        return LCD_BACKLIGHT_ON;
    if (strcmp(str, "auto") == 0)
        return LCD_BACKLIGHT_AUTO;
    return LCD_BACKLIGHT_ON;
}

const char *backlight_name(int mode)
{
    switch (mode) {
    case LCD_BACKLIGHT_OFF:  return "off";
    case LCD_BACKLIGHT_ON:   return "on";
    case LCD_BACKLIGHT_AUTO: return "auto";
    default:                 return "on";
    }
}

static const char *addr_mode_name(int mode)
{
    switch (mode) {
    case ADDR_MODE_DHCP:        return "dhcp";
    case ADDR_MODE_STATIC:      return "static";
    case ADDR_MODE_DHCP_STATIC: return "dhcp_static";
    default:                    return "static";
    }
}

int config_load(const char *path, node_config_t *config)
{
    FILE *f;
    char line[CONFIG_MAX_LINE];
    char key[64], value[192];

    config_defaults(config);

    f = fopen(path, "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof(line), f)) {
        char *k, *v;

        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        if (sscanf(line, "%63[^=]=%191[^\n\r]", key, value) != 2)
            continue;

        k = trim(key);
        v = trim(value);

        /* Strand networking keys */
        if (strcmp(k, "nodeaddr") == 0) {
            if (strcmp(v, "0") == 0)
                config->ip_addr = 0;  /* DHCP */
            else
                config->ip_addr = parse_ip(v);
        }
        else if (strcmp(k, "hostname") == 0)
            strncpy(config->hostname, v, sizeof(config->hostname) - 1);
        else if (strcmp(k, "macaddr") == 0)
            parse_mac(v, config->mac);
        else if (strcmp(k, "netmask") == 0)
            config->netmask = parse_ip(v);
        else if (strcmp(k, "gateway") == 0)
            config->gateway = parse_ip(v);
        /* Our extension keys */
        else if (strcmp(k, "protocol") == 0)
            config->active_protocol = parse_protocol(v);
        else if (strcmp(k, "dmx_holdtime") == 0)
            config->dmx_hold_time = atoi(v);
        else if (strcmp(k, "sacn_universe_0") == 0)
            config->ports[0].universe = atoi(v);
        else if (strcmp(k, "sacn_universe_1") == 0)
            config->ports[1].universe = atoi(v);
        else if (strcmp(k, "dmx1_label") == 0)
            strncpy(config->ports[0].label, v, 8);
        else if (strcmp(k, "dmx2_label") == 0)
            strncpy(config->ports[1].label, v, 8);
        else if (strcmp(k, "dmx_port0_mode") == 0)
            config->ports[0].mode = parse_mode(v);
        else if (strcmp(k, "dmx_port1_mode") == 0)
            config->ports[1].mode = parse_mode(v);
        else if (strcmp(k, "addr_mode") == 0)
            config->addr_mode = parse_addr_mode(v);
        else if (strcmp(k, "lcd_contrast") == 0) {
            int val = atoi(v);
            config->lcd_contrast = val < 0 ? 0 : (val > 255 ? 255 : val);
        }
        else if (strcmp(k, "lcd_backlight") == 0)
            config->lcd_backlight = parse_backlight(v);
        else if (strcmp(k, "dmx_slot_monitor_0") == 0) {
            int val = atoi(v);
            config->dmx_slot_monitor[0] = val < 0 ? 0 : (val > 512 ? 512 : val);
        }
        else if (strcmp(k, "dmx_slot_monitor_1") == 0) {
            int val = atoi(v);
            config->dmx_slot_monitor[1] = val < 0 ? 0 : (val > 512 ? 512 : val);
        }
    }

    /* Backward compat: infer addr_mode from nodeaddr if not set */
    if (config->addr_mode == ADDR_MODE_SENTINEL) {
        config->addr_mode = (config->ip_addr == 0) ?
            ADDR_MODE_DHCP : ADDR_MODE_STATIC;
    }

    fclose(f);
    return 0;
}

static const char *mode_name(int mode)
{
    switch (mode) {
    case DMX_MODE_TX:  return "tx";
    case DMX_MODE_RX:  return "rx";
    case DMX_MODE_OFF: return "off";
    default:           return "tx";
    }
}

static const char *protocol_name(int proto)
{
    switch (proto) {
    case PROTO_SACN:    return "sacn";
    case PROTO_ARTNET:  return "artnet";
    case PROTO_SHOWNET: return "shownet";
    default:            return "none";
    }
}

/*
 * Keys we manage. When saving, we match existing lines by key and
 * update them in place, then append any that weren't already present.
 */
#define NUM_MANAGED_KEYS 18

static const char *managed_keys[NUM_MANAGED_KEYS] = {
    "nodeaddr", "hostname", "macaddr", "netmask", "gateway",
    "protocol", "dmx_holdtime",
    "sacn_universe_0", "sacn_universe_1",
    "dmx_port0_mode", "dmx_port1_mode",
    "dmx1_label", "dmx2_label",
    "addr_mode", "lcd_contrast", "lcd_backlight",
    "dmx_slot_monitor_0", "dmx_slot_monitor_1"
};

/* Keys safe to write via nodecfg put (Strand-compatible subset).
 * Extension keys (addr_mode, lcd_*, slot_monitor, etc.) are excluded
 * because they could cause nodecfg to overflow flash. */
#define NUM_STRAND_KEYS 12

static const char *strand_keys[NUM_STRAND_KEYS] = {
    "nodeaddr", "hostname", "macaddr", "netmask", "gateway",
    "protocol", "dmx_holdtime",
    "sacn_universe_0", "sacn_universe_1",
    "dmx_port0_mode", "dmx_port1_mode",
    "dmx1_label"
};

/* Format one of our managed keys into buf. Returns bytes written. */
static int format_key(char *buf, int bufsize, const char *key,
                      const node_config_t *config)
{
    if (strcmp(key, "nodeaddr") == 0) {
        /* Write 0 for DHCP mode so Strand's nodecfg generates pump ifup */
        if (config->addr_mode == ADDR_MODE_DHCP ||
            config->ip_addr == 0)
            return snprintf(buf, bufsize, "nodeaddr = 0\n");
        return snprintf(buf, bufsize, "nodeaddr = %u.%u.%u.%u\n",
                (config->ip_addr >> 24) & 0xFF,
                (config->ip_addr >> 16) & 0xFF,
                (config->ip_addr >> 8) & 0xFF,
                config->ip_addr & 0xFF);
    }
    if (strcmp(key, "hostname") == 0)
        return snprintf(buf, bufsize, "hostname = %s\n", config->hostname);
    if (strcmp(key, "macaddr") == 0)
        return snprintf(buf, bufsize, "macaddr = %02X:%02X:%02X:%02X:%02X:%02X\n",
                config->mac[0], config->mac[1], config->mac[2],
                config->mac[3], config->mac[4], config->mac[5]);
    if (strcmp(key, "netmask") == 0)
        return snprintf(buf, bufsize, "netmask = %u.%u.%u.%u\n",
                (config->netmask >> 24) & 0xFF,
                (config->netmask >> 16) & 0xFF,
                (config->netmask >> 8) & 0xFF,
                config->netmask & 0xFF);
    if (strcmp(key, "gateway") == 0)
        return snprintf(buf, bufsize, "gateway = %u.%u.%u.%u\n",
                (config->gateway >> 24) & 0xFF,
                (config->gateway >> 16) & 0xFF,
                (config->gateway >> 8) & 0xFF,
                config->gateway & 0xFF);
    if (strcmp(key, "protocol") == 0)
        return snprintf(buf, bufsize, "protocol = %s\n",
                protocol_name(config->active_protocol));
    if (strcmp(key, "dmx_holdtime") == 0)
        return snprintf(buf, bufsize, "dmx_holdtime = %d\n",
                config->dmx_hold_time);
    if (strcmp(key, "sacn_universe_0") == 0)
        return snprintf(buf, bufsize, "sacn_universe_0 = %d\n",
                config->ports[0].universe);
    if (strcmp(key, "sacn_universe_1") == 0)
        return snprintf(buf, bufsize, "sacn_universe_1 = %d\n",
                config->ports[1].universe);
    if (strcmp(key, "dmx_port0_mode") == 0)
        return snprintf(buf, bufsize, "dmx_port0_mode = %s\n",
                mode_name(config->ports[0].mode));
    if (strcmp(key, "dmx_port1_mode") == 0)
        return snprintf(buf, bufsize, "dmx_port1_mode = %s\n",
                mode_name(config->ports[1].mode));
    if (strcmp(key, "dmx1_label") == 0)
        return snprintf(buf, bufsize, "dmx1_label = %s\n",
                config->ports[0].label);
    if (strcmp(key, "dmx2_label") == 0)
        return snprintf(buf, bufsize, "dmx2_label = %s\n",
                config->ports[1].label);
    if (strcmp(key, "addr_mode") == 0)
        return snprintf(buf, bufsize, "addr_mode = %s\n",
                addr_mode_name(config->addr_mode));
    if (strcmp(key, "lcd_contrast") == 0)
        return snprintf(buf, bufsize, "lcd_contrast = %d\n",
                config->lcd_contrast);
    if (strcmp(key, "lcd_backlight") == 0)
        return snprintf(buf, bufsize, "lcd_backlight = %s\n",
                backlight_name(config->lcd_backlight));
    if (strcmp(key, "dmx_slot_monitor_0") == 0)
        return snprintf(buf, bufsize, "dmx_slot_monitor_0 = %d\n",
                config->dmx_slot_monitor[0]);
    if (strcmp(key, "dmx_slot_monitor_1") == 0)
        return snprintf(buf, bufsize, "dmx_slot_monitor_1 = %d\n",
                config->dmx_slot_monitor[1]);
    return 0;
}

/*
 * Preserve-and-merge save:
 *  1. Read existing file into memory
 *  2. Rewrite: for each original line, update managed keys or pass through
 *  3. Append any managed keys not already present
 */
/*
 * Internal save: write a config file using the given key list.
 * If skip_extension_keys is set, lines with keys NOT in key_list
 * are dropped (for strand-safe save). Otherwise unknown lines are preserved.
 */
static int _config_save_internal(const char *path,
                                 const node_config_t *config,
                                 const char **key_list, int num_keys,
                                 int skip_extension_keys)
{
    FILE *f;
    char existing[2048];
    int existing_len = 0;
    char written[20]; /* max keys */
    int i;

    if (num_keys > 20) num_keys = 20;
    memset(written, 0, sizeof(written));

    /* Step 1: Read existing file into memory (if it exists) */
    f = fopen(path, "r");
    if (f) {
        existing_len = fread(existing, 1, sizeof(existing) - 1, f);
        if (existing_len < 0) existing_len = 0;
        existing[existing_len] = '\0';
        fclose(f);
    }

    /* Step 2: Rewrite the file */
    f = fopen(path, "w");
    if (!f)
        return -1;

    if (existing_len > 0) {
        /* Process each line from existing content */
        char *p = existing;
        while (*p) {
            /* Find end of this line */
            char *eol = strchr(p, '\n');
            char linebuf[CONFIG_MAX_LINE];
            int linelen;

            if (eol) {
                linelen = (int)(eol - p + 1);
            } else {
                linelen = (int)strlen(p);
            }
            if (linelen >= (int)sizeof(linebuf))
                linelen = (int)sizeof(linebuf) - 1;

            memcpy(linebuf, p, linelen);
            linebuf[linelen] = '\0';

            /* Check if this line has a key we manage */
            if (linebuf[0] != '#' && linebuf[0] != '\n' && linebuf[0] != '\r') {
                char key_tmp[64], val_tmp[192];
                if (sscanf(linebuf, "%63[^=]=%191[^\n\r]", key_tmp, val_tmp) == 2) {
                    char *k = trim(key_tmp);
                    /* If it's in our key list, write updated value */
                    for (i = 0; i < num_keys; i++) {
                        if (strcmp(k, key_list[i]) == 0) {
                            char fmtbuf[CONFIG_MAX_LINE];
                            format_key(fmtbuf, sizeof(fmtbuf),
                                       key_list[i], config);
                            fputs(fmtbuf, f);
                            written[i] = 1;
                            goto next_line;
                        }
                    }
                    /* Key not in our list */
                    if (skip_extension_keys) {
                        /* Drop extension keys — only preserve
                         * truly unknown Strand keys */
                        int is_extension = 0;
                        for (i = 0; i < NUM_MANAGED_KEYS; i++) {
                            if (strcmp(k, managed_keys[i]) == 0) {
                                is_extension = 1;
                                break;
                            }
                        }
                        if (is_extension)
                            goto next_line; /* skip it */
                    }
                }
            }

            /* Not a managed key — write line verbatim */
            fwrite(linebuf, 1, linelen, f);

next_line:
            p += linelen;
        }
    }

    /* Step 3: Append any keys not already written */
    for (i = 0; i < num_keys; i++) {
        if (!written[i]) {
            char fmtbuf[CONFIG_MAX_LINE];
            format_key(fmtbuf, sizeof(fmtbuf), key_list[i], config);
            fputs(fmtbuf, f);
        }
    }

    fclose(f);
    return 0;
}

int config_save(const char *path, const node_config_t *config)
{
    return _config_save_internal(path, config,
                                managed_keys, NUM_MANAGED_KEYS, 0);
}

int config_save_strand(const char *path, const node_config_t *config)
{
    int ret;

    ret = _config_save_internal(path, config,
                                strand_keys, NUM_STRAND_KEYS, 1);
    if (ret != 0)
        return ret;

    /* Size check — refuse if output is too large for flash */
    {
        FILE *f = fopen(path, "r");
        if (f) {
            char buf[STRAND_MAX_SIZE + 1];
            int n = fread(buf, 1, sizeof(buf), f);
            fclose(f);
            if (n > STRAND_MAX_SIZE)
                return -2; /* too large */
        }
    }

    return 0;
}

/* Helper: write static IP ifconfig + route commands */
static void _ifup_static(FILE *f, const node_config_t *config)
{
    uint32_t bcast;
    uint32_t net;

    bcast = (config->ip_addr & config->netmask) |
            (~config->netmask & 0xFFFFFFFF);
    net = config->ip_addr & config->netmask;

    fprintf(f, "/sbin/ifconfig eth0 %u.%u.%u.%u netmask %u.%u.%u.%u "
            "broadcast %u.%u.%u.%u up\n",
            (config->ip_addr >> 24) & 0xFF,
            (config->ip_addr >> 16) & 0xFF,
            (config->ip_addr >> 8) & 0xFF,
            config->ip_addr & 0xFF,
            (config->netmask >> 24) & 0xFF,
            (config->netmask >> 16) & 0xFF,
            (config->netmask >> 8) & 0xFF,
            config->netmask & 0xFF,
            (bcast >> 24) & 0xFF,
            (bcast >> 16) & 0xFF,
            (bcast >> 8) & 0xFF,
            bcast & 0xFF);

    fprintf(f, "/sbin/route add -net %u.%u.%u.%u netmask %u.%u.%u.%u eth0\n",
            (net >> 24) & 0xFF,
            (net >> 16) & 0xFF,
            (net >> 8) & 0xFF,
            net & 0xFF,
            (config->netmask >> 24) & 0xFF,
            (config->netmask >> 16) & 0xFF,
            (config->netmask >> 8) & 0xFF,
            config->netmask & 0xFF);

    if (config->gateway != 0) {
        fprintf(f, "/sbin/route add default gw %u.%u.%u.%u\n",
                (config->gateway >> 24) & 0xFF,
                (config->gateway >> 16) & 0xFF,
                (config->gateway >> 8) & 0xFF,
                config->gateway & 0xFF);
    }
}

int config_generate_ifup(const char *path, const node_config_t *config)
{
    FILE *f;

    f = fopen(path, "w");
    if (!f)
        return -1;

    fprintf(f, "#!/bin/sh\n");
    fprintf(f, "/sbin/ifconfig eth0 down\n");

    switch (config->addr_mode) {
    case ADDR_MODE_DHCP:
        fprintf(f, "/sbin/pump -i eth0\n");
        break;
    case ADDR_MODE_DHCP_STATIC:
        /* Try DHCP first, fall back to static if it fails */
        fprintf(f, "/sbin/pump -i eth0 || {\n");
        _ifup_static(f, config);
        fprintf(f, "}\n");
        break;
    default: /* ADDR_MODE_STATIC */
        _ifup_static(f, config);
        break;
    }

    fclose(f);
    return 0;
}
