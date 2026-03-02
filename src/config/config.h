/*
 * Configuration file parser
 *
 * Reads and writes the 220node.cfg configuration format used by the SN110.
 * Extends the format with new protocol-related settings while maintaining
 * backward compatibility with the original Strand nodecfg tool.
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef SN110_CONFIG_H
#define SN110_CONFIG_H

#include "../common.h"

#define CONFIG_FILE_PATH    "/etc/220node.cfg"
#define IFUP_FILE_PATH      "/etc/ifup-eth0"
#define CONFIG_MAX_LINE     256

/*
 * Load node configuration from file.
 * Returns 0 on success, -1 on error.
 */
int config_load(const char *path, node_config_t *config);

/*
 * Save node configuration to file.
 * Preserves comments and unknown fields.
 * Returns 0 on success, -1 on error.
 */
int config_save(const char *path, const node_config_t *config);

/*
 * Initialize configuration with defaults.
 */
void config_defaults(node_config_t *config);

/*
 * Generate /etc/ifup-eth0 script from configuration.
 * Static IP writes ifconfig + route; DHCP writes pump.
 * Returns 0 on success, -1 on error.
 */
int config_generate_ifup(const char *path, const node_config_t *config);

/*
 * Parsing helpers — also used by CGI form parser.
 */
uint32_t parse_ip(const char *str);
int parse_mode(const char *str);
int parse_protocol(const char *str);

#endif /* SN110_CONFIG_H */
