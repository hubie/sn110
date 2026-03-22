---
title: "Config Hardening: Strand-Safe Flash Writes, DHCP Rework, and Input Validation"
category: logic-errors
date: 2026-03-21
severity: high
tags:
  - config
  - flash-safety
  - dhcp
  - xss
  - input-validation
  - backward-compatibility
  - embedded
  - arm7tdmi
  - uclinux
components:
  - src/config/config.c
  - src/cgi/cgi_config.c
  - src/cgi/cgi_parse.h
  - src/common.h
  - src/oabi/minilib.c
  - tools/web/cfgpost.cgi
symptoms:
  - Extension keys (lcd_contrast, lcd_backlight, dmx_slot_monitor) written to Strand flash config sector could overflow fixed-size partition
  - parse_mac() used sscanf %x which does not exist in minilib (only %u, %d, %[^...] supported)
  - CGI output had no HTML escaping — user-controlled hostname/label fields vulnerable to XSS
  - DHCP detection relied on ip_addr==0 heuristic instead of explicit addr_mode field
  - No integer bounds checking on CGI form inputs (universe, hold time, contrast, slot numbers)
  - Old config files without addr_mode key would break DHCP behavior on upgrade
root_causes:
  - config_save() wrote all managed keys to both config files — no distinction between Strand-compatible and extension keys
  - minilib sscanf only supports %u, %d, and %[^...] — %x was silently unsupported, causing parse_mac to fail on device
  - CGI output used raw printf for user-controlled fields with no escaping
  - No explicit addressing mode field — DHCP was inferred from ip_addr==0, which was fragile
  - No validation layer between CGI form POST data and config struct
related_issues: []
---

# Config Hardening: Strand-Safe Flash Writes, DHCP Rework, and Input Validation

## Problem Description

The SN110 open firmware's configuration system had several interrelated issues that created risk of flash corruption, on-device failures, and security vulnerabilities:

1. **Flash overflow risk**: The `config_save()` function wrote all managed keys (including new extension keys like `lcd_contrast`, `lcd_backlight`, `dmx_slot_monitor`) to the Strand flash config sector via `nodecfg put`. The Strand config sector is a fixed-size flash partition — writing too many keys could overflow it, potentially bricking the device.

2. **MAC parsing failure on device**: `parse_mac()` used `sscanf(..., "%x", ...)` but the custom minilib only supports `%u`, `%d`, and `%[^...]` format specifiers. This worked in host tests (which use glibc) but silently failed on the actual ARM7TDMI device.

3. **XSS vulnerability**: The CGI web interface output user-controlled fields (hostname, label) directly into HTML without escaping. A malicious hostname like `<script>alert(1)</script>` would execute in the browser.

4. **Fragile DHCP detection**: DHCP mode was inferred from `ip_addr == 0` rather than an explicit config field, making it impossible to distinguish "unconfigured" from "DHCP mode" and blocking the addition of hybrid DHCP+Static fallback mode.

5. **No input validation**: CGI form inputs were stored directly into the config struct with no bounds checking — universe numbers, hold times, contrast values, and slot numbers could be set to arbitrary values.

## Investigation Steps

1. **Identified flash overflow risk** by tracing the config save path: `config_save()` → writes all `managed_keys[]` → `cfgpost.cgi` calls `nodecfg put` on the result. Extension keys not recognized by Strand firmware were being written to the flash sector.

2. **Discovered minilib sscanf limitation** by reviewing `src/oabi/minilib.c` sscanf implementation — the format parser handles `%u`, `%d`, and `%[^...]` but has no hex parsing code. Host tests pass because they link against glibc's sscanf.

3. **Found XSS vulnerability** by reviewing `cgi_config.c` — raw `printf("<input value=\"%s\">", config.hostname)` with no escaping.

4. **Traced DHCP detection fragility** through `config_generate_ifup()` which checked `ip_addr == 0` to decide between DHCP and static scripts, rather than an explicit mode field.

5. **Confirmed no input validation** in `cgi_parse.h` — form field values were parsed and stored directly via `atoi()` with no range checking.

## Root Cause Analysis

The core issue was that the config system was designed for a small, fixed set of Strand-compatible keys and had no separation between "keys safe to write to flash" and "extension keys." As new features (LCD, slot monitoring, address modes) were added, all keys went through the same save path, creating cumulative flash overflow risk.

The secondary issues (MAC parsing, XSS, input validation) stemmed from the gap between host development (glibc, modern browser testing) and the actual device environment (minilib, direct hardware access).

## Solution

### 1. Split Config Save: Strand Keys vs Extension Keys

Refactored `config_save()` into `_config_save_internal()` with explicit key list selection:

```c
/* 12 Strand-compatible keys — safe for flash */
static const char *strand_keys[] = {
    "hostname", "label", "nodeaddr", "netmask", "gateway",
    "universe_0", "universe_1", "hold_0", "hold_1",
    "mode_0", "mode_1", "protocol"
};

/* New: saves only strand keys to /etc/220node.cfg.strand */
int config_save_strand(const node_config_t *config)
{
    int ret = _config_save_internal(STRAND_CONFIG_PATH, config,
                                     strand_keys, NUM_STRAND_KEYS, 1);
    if (ret == 0) {
        /* Size guard — refuse to flash if too large */
        int fd = open(STRAND_CONFIG_PATH, O_RDONLY);
        /* ... check size < STRAND_MAX_SIZE (2048) ... */
    }
    return ret;
}
```

The shell-side `cfgpost.cgi` also got a size guard:

```sh
STRAND_CFG="/etc/220node.cfg.strand"
if [ -f "$STRAND_CFG" ]; then
    SIZE=$(wc -c < "$STRAND_CFG")
    if [ "$SIZE" -lt 4096 ]; then
        /sbin/nodecfg put "$STRAND_CFG"
    fi
fi
```

### 2. Manual Hex Parsing for MAC Addresses

Replaced `sscanf %x` with `_parse_hex_byte()` that works within minilib's constraints:

```c
static int _parse_hex_byte(const char *s, uint8_t *out)
{
    uint8_t val = 0;
    for (int i = 0; i < 2; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9')      val = val * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') val = val * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') val = val * 16 + (c - 'A' + 10);
        else return -1;
    }
    *out = val;
    return 0;
}
```

### 3. HTML Escaping for CGI Output

Added `html_escape()` function and applied it to all user-controlled outputs:

```c
static void html_escape(const char *s)
{
    while (*s) {
        switch (*s) {
            case '<': printf("&lt;"); break;
            case '>': printf("&gt;"); break;
            case '&': printf("&amp;"); break;
            case '"': printf("&quot;"); break;
            default:  putchar(*s); break;
        }
        s++;
    }
}
```

### 4. Explicit addr_mode Field with Backward Compatibility

Added `ADDR_MODE_STATIC/DHCP/DHCP_STATIC` constants and `ADDR_MODE_SENTINEL (255)` for detecting old configs:

```c
#define ADDR_MODE_STATIC      0
#define ADDR_MODE_DHCP        1
#define ADDR_MODE_DHCP_STATIC 2
#define ADDR_MODE_SENTINEL    255  /* Old config — infer from nodeaddr */
```

Backward-compat inference in `config_load()`:

```c
if (config->addr_mode == ADDR_MODE_SENTINEL) {
    /* Old config without addr_mode — infer from nodeaddr */
    if (config->ip_addr == 0)
        config->addr_mode = ADDR_MODE_DHCP;
    else
        config->addr_mode = ADDR_MODE_STATIC;
}
```

### 5. Integer Clamping for CGI Inputs

Added `_clamp()` helper and applied to all numeric form fields:

```c
static int _clamp(int val, int lo, int hi)
{
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

/* Usage in cgi_parse.h */
config->universe[0] = _clamp(atoi(val), 1, 63999);
config->hold[0]     = _clamp(atoi(val), 0, 300);
config->lcd_contrast = _clamp(atoi(val), 0, 255);
config->dmx_slot_monitor[0] = _clamp(atoi(val), 0, 512);
```

### 6. OABI Syscall Additions

Added `mmap`/`munmap`/`rename` syscalls for future direct DMX driver support:

```asm
SYSCALL rename,     38
SYSCALL _sys_mmap,  90    /* old_mmap: takes pointer to 6-arg struct */
SYSCALL munmap,     91
```

With C wrapper in minilib.c that packs the 6-arg struct for ARM Linux 2.0's `old_mmap`.

## Verification Steps

1. **52/52 tests pass** — 15 new tests covering addr_mode round-trip, backward compat, strand key exclusion, size limits, DHCP+Static mode, LCD defaults, MAC hex parsing, IP octet validation, CGI integer bounds
2. **ASan + UBSan clean** — `make test-asan` passes with no sanitizer findings
3. **Fuzzer targets** — `fuzz_config` and `fuzz_cgi` targets added for continuous fuzzing of config parser and CGI input handler
4. **Docker cross-compile** — `make docker-bflt` and `make docker-cgi-bflt` build successfully

## Prevention Strategies

### 1. Separate Flash-Safe Keys at the Type Level

Maintain explicit `strand_keys[]` and `managed_keys[]` arrays. New config fields go into managed_keys only — they must be explicitly added to strand_keys with a size impact assessment.

### 2. Test on minilib, Not Just glibc

Any function using `sscanf`, `printf`, or other minilib-reimplemented functions must be tested on the actual minilib implementation (via Docker ARM build + QEMU), not just host tests that link glibc.

### 3. Always Escape User-Controlled Output

All CGI output of user-controlled data must go through `html_escape()`. The function is available in `cgi_config.c`.

### 4. Use Sentinel Values for Backward Compatibility

When adding new config fields, use a sentinel default (like `ADDR_MODE_SENTINEL = 255`) that triggers inference from existing fields. This ensures old config files work correctly after firmware upgrade.

### 5. Validate at the Boundary

All external inputs (CGI form fields, config file values) should be validated/clamped at the point of entry, not deep in the config logic.

### 6. Size-Guard Flash Writes

Both the C code (`config_save_strand()`) and the shell script (`cfgpost.cgi`) independently verify file size before calling `nodecfg put`. Defense in depth.

## Test Cases

- `test_config_addr_mode_save_load` — addr_mode round-trips through save/load
- `test_config_addr_mode_backward_compat` — SENTINEL triggers inference from ip_addr
- `test_config_save_strand_excludes_extensions` — extension keys absent from strand output
- `test_config_save_strand_preserves_unknown` — unknown keys preserved in strand file
- `test_config_save_strand_size_limit` — oversized config detected and rejected
- `test_config_dhcp_static_mode` — DHCP+Static mode saves and loads correctly
- `test_config_generate_ifup_dhcp_static` — ifup script tries DHCP then falls back
- `test_config_lcd_defaults` — LCD contrast=128, backlight=ON by default
- `test_config_lcd_save_load` — LCD settings round-trip
- `test_config_lcd_backlight_parse` — backlight name parsing (off/on/auto)
- `test_cgi_parse_dhcp_static_mode` — CGI form sets addr_mode correctly
- `test_cgi_parse_lcd_fields` — LCD fields parsed from CGI form
- `test_cgi_parse_integer_bounds` — out-of-range values clamped
- `test_config_mac_hex_parsing` — MAC parsing works without sscanf %x
- `test_config_ip_octet_validation` — octets > 255 rejected

## Cross-References

- Plan: `docs/plans/2026-03-21-001-feat-config-hardening-and-ui-enhancements-plan.md`
- Branch: `feature/config-hardening` (commit `ad05682`)
- Memory: `safety-nodecfg-put.md` — documents flash write safety rules
- Memory: `bricking-investigation.md` — prior incident context for flash caution
