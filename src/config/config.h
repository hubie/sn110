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

#endif /* SN110_CONFIG_H */
