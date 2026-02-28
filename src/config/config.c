/*
 * Configuration file parser
 *
 * Reads and writes the 220node.cfg configuration format.
 * The original Strand format uses simple KEY=VALUE lines.
 * We extend it with new keys for protocol settings while
 * preserving backward compatibility.
 *
 * Known keys (original Strand):
 *   HOSTNAME, IPADDR, NETMASK, GATEWAY, MAC, DHCP
 *   DMX_PORT0_MODE, DMX_PORT1_MODE
 *   DMX_PORT0_LABEL, DMX_PORT1_LABEL
 *
 * New keys (our extension):
 *   PROTOCOL (sacn|artnet|shownet)
 *   SACN_UNIVERSE_0, SACN_UNIVERSE_1
 *   ARTNET_UNIVERSE_0, ARTNET_UNIVERSE_1
 *   DMX_HOLD_TIME
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

static uint32_t parse_ip(const char *str)
{
    unsigned int a, b, c, d;
    if (sscanf(str, "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
        return (a << 24) | (b << 16) | (c << 8) | d;
    return 0;
}

static int parse_protocol(const char *str)
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
        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        if (sscanf(line, "%63[^=]=%191[^\n\r]", key, value) != 2)
            continue;

        if (strcmp(key, "HOSTNAME") == 0)
            strncpy(config->hostname, value, sizeof(config->hostname) - 1);
        else if (strcmp(key, "IPADDR") == 0)
            config->ip_addr = parse_ip(value);
        else if (strcmp(key, "NETMASK") == 0)
            config->netmask = parse_ip(value);
        else if (strcmp(key, "GATEWAY") == 0)
            config->gateway = parse_ip(value);
        else if (strcmp(key, "PROTOCOL") == 0)
            config->active_protocol = parse_protocol(value);
        else if (strcmp(key, "DMX_HOLD_TIME") == 0)
            config->dmx_hold_time = atoi(value);
        else if (strcmp(key, "SACN_UNIVERSE_0") == 0)
            config->ports[0].universe = atoi(value);
        else if (strcmp(key, "SACN_UNIVERSE_1") == 0)
            config->ports[1].universe = atoi(value);
        else if (strcmp(key, "ARTNET_UNIVERSE_0") == 0)
            config->ports[0].universe = atoi(value);
        else if (strcmp(key, "ARTNET_UNIVERSE_1") == 0)
            config->ports[1].universe = atoi(value);
        else if (strcmp(key, "DMX_PORT0_LABEL") == 0)
            strncpy(config->ports[0].label, value, 8);
        else if (strcmp(key, "DMX_PORT1_LABEL") == 0)
            strncpy(config->ports[1].label, value, 8);
    }

    fclose(f);
    return 0;
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

int config_save(const char *path, const node_config_t *config)
{
    FILE *f;

    f = fopen(path, "w");
    if (!f)
        return -1;

    fprintf(f, "# SN110 Open Firmware Configuration\n");
    fprintf(f, "HOSTNAME=%s\n", config->hostname);
    fprintf(f, "IPADDR=%u.%u.%u.%u\n",
            (config->ip_addr >> 24) & 0xFF, (config->ip_addr >> 16) & 0xFF,
            (config->ip_addr >> 8) & 0xFF, config->ip_addr & 0xFF);
    fprintf(f, "NETMASK=%u.%u.%u.%u\n",
            (config->netmask >> 24) & 0xFF, (config->netmask >> 16) & 0xFF,
            (config->netmask >> 8) & 0xFF, config->netmask & 0xFF);
    fprintf(f, "GATEWAY=%u.%u.%u.%u\n",
            (config->gateway >> 24) & 0xFF, (config->gateway >> 16) & 0xFF,
            (config->gateway >> 8) & 0xFF, config->gateway & 0xFF);
    fprintf(f, "\n# Protocol: sacn, artnet, shownet\n");
    fprintf(f, "PROTOCOL=%s\n", protocol_name(config->active_protocol));
    fprintf(f, "DMX_HOLD_TIME=%d\n", config->dmx_hold_time);
    fprintf(f, "\n# Universe mapping (1-based for sACN, 15-bit for Art-Net)\n");
    fprintf(f, "SACN_UNIVERSE_0=%d\n", config->ports[0].universe);
    fprintf(f, "SACN_UNIVERSE_1=%d\n", config->ports[1].universe);
    fprintf(f, "\n# Port labels (up to 8 characters)\n");
    fprintf(f, "DMX_PORT0_LABEL=%s\n", config->ports[0].label);
    fprintf(f, "DMX_PORT1_LABEL=%s\n", config->ports[1].label);

    fclose(f);
    return 0;
}
