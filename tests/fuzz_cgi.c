/*
 * Fuzz harness for parse_formdata() — exercises the CGI form parser
 * and url_decode() with arbitrary input to find crashes, buffer
 * overflows, and UB.
 *
 * Build: make fuzz-cgi (requires clang with libFuzzer)
 * Run:   ./build/fuzz_cgi corpus_cgi/
 *
 * Copyright (c) 2026 SN110 Open Firmware Contributors
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include "../src/common.h"
#include "../src/config/config.h"
#include "../src/cgi/cgi_parse.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    node_config_t cfg;
    char *body;

    /* parse_formdata expects a null-terminated string */
    body = malloc(size + 1);
    if (!body)
        return 0;
    memcpy(body, data, size);
    body[size] = '\0';

    /* Exercise the CGI parser */
    config_defaults(&cfg);
    parse_formdata(body, &cfg);

    /* Also exercise url_decode directly with the raw input */
    {
        char decoded[256];
        url_decode(decoded, body, sizeof(decoded));
    }

    free(body);
    return 0;
}
