/*
 * Fuzz harness for config_load() — exercises the config file parser
 * with arbitrary input to find crashes, buffer overflows, and UB.
 *
 * Build: make fuzz-config (requires clang with libFuzzer)
 * Run:   ./build/fuzz_config corpus_config/
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../src/common.h"
#include "../src/config/config.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    node_config_t cfg;
    const char *path = "/tmp/fuzz_config_input.cfg";
    FILE *f;

    /* Write fuzz input to a temp file (config_load reads from FILE*) */
    f = fopen(path, "w");
    if (!f)
        return 0;
    fwrite(data, 1, size, f);
    fclose(f);

    /* Exercise config_load */
    config_load(path, &cfg);

    /* Exercise save round-trip: load → save → load */
    config_save(path, &cfg);
    config_load(path, &cfg);

    /* Exercise Strand-only save */
    config_save_strand("/tmp/fuzz_config_strand.cfg", &cfg);

    /* Exercise ifup generation */
    config_generate_ifup("/tmp/fuzz_config_ifup.sh", &cfg);

    unlink(path);
    unlink("/tmp/fuzz_config_strand.cfg");
    unlink("/tmp/fuzz_config_ifup.sh");

    return 0;
}
