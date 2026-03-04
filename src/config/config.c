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

    config->addr_mode = ADDR_MODE_DHCP;
    config->lcd_contrast = 15;
    config->lcd_backlight = LCD_BACKLIGHT_ON;
    config->dmx_driver = DMX_DRIVER_KERNEL;
}

uint32_t parse_ip(const char *str)
{
    unsigned int a, b, c, d;
    if (sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
        return (a << 24) | (b << 16) | (c << 8) | d;
    return 0;
}

static int parse_mac(const char *str, uint8_t *mac)
{
    unsigned int m[6];
    if (sscanf(str, "%x:%x:%x:%x:%x:%x",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
        int i;
        for (i = 0; i < 6; i++)
            mac[i] = (uint8_t)m[i];
        return 0;
    }
    return -1;
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

int parse_backlight(const char *str)
{
    if (strcmp(str, "Off") == 0 || strcmp(str, "off") == 0 || strcmp(str, "OFF") == 0)
        return LCD_BACKLIGHT_OFF;
    if (strcmp(str, "Flash") == 0 || strcmp(str, "flash") == 0 || strcmp(str, "FLASH") == 0)
        return LCD_BACKLIGHT_FLASH;
    return LCD_BACKLIGHT_ON; /* default */
}

int parse_addr_mode(const char *str)
{
    if (strcmp(str, "dhcp") == 0 || strcmp(str, "DHCP") == 0)
        return ADDR_MODE_DHCP;
    if (strcmp(str, "dhcp_static") == 0 || strcmp(str, "DHCP_STATIC") == 0)
        return ADDR_MODE_DHCP_STATIC;
    return ADDR_MODE_STATIC;
}

int parse_dmx_driver(const char *str)
{
    if (strcmp(str, "direct") == 0 || strcmp(str, "DIRECT") == 0)
        return DMX_DRIVER_DIRECT;
    return DMX_DRIVER_KERNEL;
}

static const char *backlight_name(int bl)
{
    switch (bl) {
    case LCD_BACKLIGHT_OFF:   return "Off";
    case LCD_BACKLIGHT_FLASH: return "Flash";
    default:                  return "On";
    }
}

int config_load(const char *path, node_config_t *config)
{
    FILE *f;
    char line[CONFIG_MAX_LINE];
    char key[64], value[192];

    config_defaults(config);
    config->addr_mode = 255; /* sentinel: detect if file has explicit addr_mode */

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
        /* LCD keys */
        else if (strcmp(k, "lcd_contrast") == 0) {
            int val = atoi(v);
            if (val < 0) val = 0;
            if (val > 63) val = 63;
            config->lcd_contrast = val;
        }
        else if (strcmp(k, "lcd_backlight") == 0)
            config->lcd_backlight = parse_backlight(v);
        else if (strcmp(k, "dmx1_slot_monitor") == 0)
            config->dmx_slot_monitor[0] = atoi(v);
        else if (strcmp(k, "dmx2_slot_monitor") == 0)
            config->dmx_slot_monitor[1] = atoi(v);
        else if (strcmp(k, "addr_mode") == 0)
            config->addr_mode = parse_addr_mode(v);
        else if (strcmp(k, "dmx_driver") == 0)
            config->dmx_driver = parse_dmx_driver(v);
    }

    /* If addr_mode wasn't in the file, infer from nodeaddr for backward compat */
    if (config->addr_mode == 255)
        config->addr_mode = (config->ip_addr == 0) ? ADDR_MODE_DHCP : ADDR_MODE_STATIC;

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
#define NUM_MANAGED_KEYS 19

static const char *managed_keys[NUM_MANAGED_KEYS] = {
    "nodeaddr", "hostname", "macaddr", "netmask", "gateway",
    "protocol", "dmx_holdtime",
    "sacn_universe_0", "sacn_universe_1",
    "dmx_port0_mode", "dmx_port1_mode",
    "dmx1_label", "dmx2_label",
    "lcd_contrast", "lcd_backlight",
    "dmx1_slot_monitor", "dmx2_slot_monitor",
    "addr_mode",
    "dmx_driver"
};

/*
 * Strand-compatible subset: only keys that Strand's nodecfg recognises.
 * Our extension keys (protocol, sacn_universe_*, dmx_port*_mode, addr_mode)
 * must NOT be written to the file passed to `nodecfg put`, because nodecfg
 * writes to a fixed-size raw flash sector and extra data can overflow into
 * adjacent flash, corrupting the firmware image.
 */
#define NUM_STRAND_KEYS 12

static const char *strand_keys[NUM_STRAND_KEYS] = {
    "nodeaddr", "hostname", "macaddr", "netmask", "gateway",
    "dmx_holdtime",
    "dmx1_label", "dmx2_label",
    "lcd_contrast", "lcd_backlight",
    "dmx1_slot_monitor", "dmx2_slot_monitor"
};

/* Format one of our managed keys into buf. Returns bytes written. */
static int format_key(char *buf, int bufsize, const char *key,
                      const node_config_t *config)
{
    if (strcmp(key, "nodeaddr") == 0) {
        /* Pure DHCP: write 0 so Strand's nodecfg generates a DHCP ifup on boot */
        if (config->addr_mode == ADDR_MODE_DHCP)
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
    if (strcmp(key, "lcd_contrast") == 0)
        return snprintf(buf, bufsize, "lcd_contrast = %d\n",
                config->lcd_contrast);
    if (strcmp(key, "lcd_backlight") == 0)
        return snprintf(buf, bufsize, "lcd_backlight = %s\n",
                backlight_name(config->lcd_backlight));
    if (strcmp(key, "dmx1_slot_monitor") == 0)
        return snprintf(buf, bufsize, "dmx1_slot_monitor = %d\n",
                config->dmx_slot_monitor[0]);
    if (strcmp(key, "dmx2_slot_monitor") == 0)
        return snprintf(buf, bufsize, "dmx2_slot_monitor = %d\n",
                config->dmx_slot_monitor[1]);
    if (strcmp(key, "addr_mode") == 0) {
        const char *mode_str = "static";
        if (config->addr_mode == ADDR_MODE_DHCP)
            mode_str = "dhcp";
        else if (config->addr_mode == ADDR_MODE_DHCP_STATIC)
            mode_str = "dhcp_static";
        return snprintf(buf, bufsize, "addr_mode = %s\n", mode_str);
    }
    if (strcmp(key, "dmx_driver") == 0)
        return snprintf(buf, bufsize, "dmx_driver = %s\n",
                config->dmx_driver == DMX_DRIVER_DIRECT ? "direct" : "kernel");
    return 0;
}

/*
 * Internal preserve-and-merge save:
 *  1. Read existing file into memory
 *  2. Rewrite: for each original line, update keys from key_list or pass through
 *  3. Append any keys from key_list not already present
 *
 * When skip_extension_keys is set, lines whose key is in managed_keys but NOT
 * in key_list are silently dropped (not passed through). This prevents our
 * extension keys from leaking into the Strand-only config.
 */
static int _config_save_internal(const char *path, const node_config_t *config,
                                 const char **key_list, int num_keys,
                                 int skip_extension_keys)
{
    FILE *f;
    char existing[2048];
    int existing_len = 0;
    char written[NUM_MANAGED_KEYS]; /* sized for largest possible key_list */
    int i;

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
                    /* If it's in our key_list, write updated value */
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
                    /* If it's an extension key and we're filtering, drop it */
                    if (skip_extension_keys) {
                        int j;
                        for (j = 0; j < NUM_MANAGED_KEYS; j++) {
                            if (strcmp(k, managed_keys[j]) == 0)
                                goto next_line; /* drop this extension key */
                        }
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
    return _config_save_internal(path, config,
                                 strand_keys, NUM_STRAND_KEYS, 1);
}

int config_generate_ifup(const char *path, const node_config_t *config)
{
    FILE *f;

    f = fopen(path, "w");
    if (!f)
        return -1;

    fprintf(f, "#!/bin/sh\n");
    fprintf(f, "/sbin/ifconfig eth0 down\n");

    if (config->addr_mode == ADDR_MODE_DHCP) {
        /* DHCP mode — netsetup assigns link-local from MAC for
         * immediate reachability, then pump tries DHCP.
         * Second netsetup call updates 220node.cfg with the
         * actual IP so sn110lcd shows the correct address. */
        fprintf(f, "/sbin/ifconfig eth0 up\n");
        fprintf(f, "/usr/bin/netsetup\n");
        fprintf(f, "/sbin/pump -i eth0\n");
        fprintf(f, "/usr/bin/netsetup\n");
    } else {
        /* Static or DHCP+Static — both start with static IP setup */
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

        /* DHCP+Static: after static setup, try DHCP (overrides if successful) */
        if (config->addr_mode == ADDR_MODE_DHCP_STATIC) {
            fprintf(f, "/sbin/pump -i eth0\n");
            fprintf(f, "/usr/bin/netsetup\n");
        }
    }

    fclose(f);
    return 0;
}
