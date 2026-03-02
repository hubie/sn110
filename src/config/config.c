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
#define NUM_MANAGED_KEYS 13

static const char *managed_keys[NUM_MANAGED_KEYS] = {
    "nodeaddr", "hostname", "macaddr", "netmask", "gateway",
    "protocol", "dmx_holdtime",
    "sacn_universe_0", "sacn_universe_1",
    "dmx_port0_mode", "dmx_port1_mode",
    "dmx1_label", "dmx2_label"
};

/* Format one of our managed keys into buf. Returns bytes written. */
static int format_key(char *buf, int bufsize, const char *key,
                      const node_config_t *config)
{
    if (strcmp(key, "nodeaddr") == 0) {
        if (config->ip_addr == 0)
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
    return 0;
}

/*
 * Preserve-and-merge save:
 *  1. Read existing file into memory
 *  2. Rewrite: for each original line, update managed keys or pass through
 *  3. Append any managed keys not already present
 */
int config_save(const char *path, const node_config_t *config)
{
    FILE *f;
    char existing[2048];
    int existing_len = 0;
    char written[NUM_MANAGED_KEYS];
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
                    /* If it's a managed key, write our updated value */
                    for (i = 0; i < NUM_MANAGED_KEYS; i++) {
                        if (strcmp(k, managed_keys[i]) == 0) {
                            char fmtbuf[CONFIG_MAX_LINE];
                            format_key(fmtbuf, sizeof(fmtbuf),
                                       managed_keys[i], config);
                            fputs(fmtbuf, f);
                            written[i] = 1;
                            goto next_line;
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

    /* Step 3: Append any managed keys not already written */
    for (i = 0; i < NUM_MANAGED_KEYS; i++) {
        if (!written[i]) {
            char fmtbuf[CONFIG_MAX_LINE];
            format_key(fmtbuf, sizeof(fmtbuf), managed_keys[i], config);
            fputs(fmtbuf, f);
        }
    }

    fclose(f);
    return 0;
}

int config_generate_ifup(const char *path, const node_config_t *config)
{
    FILE *f;

    f = fopen(path, "w");
    if (!f)
        return -1;

    fprintf(f, "#!/bin/sh\n");
    fprintf(f, "ifconfig eth0 down\n");

    if (config->ip_addr == 0) {
        /* DHCP mode */
        fprintf(f, "/sbin/pump -i eth0\n");
    } else {
        /* Static IP mode */
        uint32_t bcast;
        uint32_t net;

        bcast = (config->ip_addr & config->netmask) |
                (~config->netmask & 0xFFFFFFFF);
        net = config->ip_addr & config->netmask;

        fprintf(f, "ifconfig eth0 %u.%u.%u.%u netmask %u.%u.%u.%u "
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

        fprintf(f, "route add -net %u.%u.%u.%u netmask %u.%u.%u.%u eth0\n",
                (net >> 24) & 0xFF,
                (net >> 16) & 0xFF,
                (net >> 8) & 0xFF,
                net & 0xFF,
                (config->netmask >> 24) & 0xFF,
                (config->netmask >> 16) & 0xFF,
                (config->netmask >> 8) & 0xFF,
                config->netmask & 0xFF);

        if (config->gateway != 0) {
            fprintf(f, "route add default gw %u.%u.%u.%u\n",
                    (config->gateway >> 24) & 0xFF,
                    (config->gateway >> 16) & 0xFF,
                    (config->gateway >> 8) & 0xFF,
                    config->gateway & 0xFF);
        }
    }

    fclose(f);
    return 0;
}
