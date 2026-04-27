---
title: "feat: Config hardening, DHCP rework, LCD controls, and safety improvements"
type: feat
status: active
date: 2026-03-21
---

# Config Hardening, DHCP Rework, LCD Controls, and Safety Improvements

## Overview

Extract and implement all non-driver changes from the `feature/direct-dmx-driver` branch onto a clean branch off `main`. This includes config safety improvements (Strand-only save, addr_mode rework), new UI features (LCD, slot monitors, DHCP+Static), test infrastructure (ASan, fuzzing), review fixes (input validation, XSS, parse_mac), and minilib additions (fread/fwrite/fputs/mmap).

These changes are independently valuable — they harden the config system, fix the DHCP boot-brick bug, add missing web UI controls, and strengthen testing. They are not coupled to the direct UART driver.

## Problem Statement

The `feature/direct-dmx-driver` branch bundles several independent improvements into one commit alongside the UART driver. The driver has P1 issues (timing, RX) that need more research. Meanwhile, the non-driver changes are ready and needed:

1. **DHCP boot-brick prevention** — `config_save_strand()` and `addr_mode` field prevent flash overflow and ensure Strand's `nodecfg process` generates correct ifup scripts
2. **Missing web UI controls** — LCD contrast/backlight, slot monitors, DHCP+Static mode have no UI on main
3. **No sanitizer/fuzz testing** — main branch only has `make test`
4. **Input validation gaps** — CGI integer fields lack server-side clamping, HTML output has XSS, `parse_mac` is broken on-device
5. **minilib gaps** — `fread`, `fwrite`, `fputs` missing; `mmap` wrapper needed for future use

## Proposed Solution

Create branch `feature/config-hardening` off `main`. Re-implement the non-driver changes in logical commits. Incorporate the P2/P3 review fixes (todos 006-008, 012) at the same time since they touch the same files.

## Implementation Phases

### Phase 1: Config struct + addr_mode + Strand-safe save

Extend `node_config_t` with all new fields **except** `dmx_driver`. Add the config_save_strand safety mechanism.

**Files:**
- `src/common.h` — add `ADDR_MODE_*`, `LCD_BACKLIGHT_*` constants, extend struct with `addr_mode`, `lcd_contrast`, `lcd_backlight`, `dmx_slot_monitor[2]`
- `src/config/config.h` — add `STRAND_CONFIG_PATH`, `config_save_strand()`, new parser declarations
- `src/config/config.c` — add `parse_backlight()`, `parse_addr_mode()`, `backlight_name()`, extend `config_defaults()`, `config_load()`, `format_key()`; add `strand_keys[]`, `_config_save_internal()`, `config_save_strand()`; rework `config_generate_ifup()` for addr_mode + DHCP+Static; fix `format_key("nodeaddr")` to write `0` for DHCP mode; add backward-compat addr_mode inference from nodeaddr=0

**Tests (new):**
- `test_config_addr_mode_save_load`
- `test_config_addr_mode_backward_compat`
- `test_config_save_strand_excludes_extensions`
- `test_config_save_strand_preserves_unknown`
- `test_config_save_strand_size_limit`
- `test_config_dhcp_static_mode`
- `test_config_generate_ifup_dhcp_static`
- `test_config_lcd_defaults`
- `test_config_lcd_save_load`
- `test_config_lcd_backlight_parse`

**Tests (updated):**
- `test_config_defaults_values` — assert addr_mode
- `test_config_save_load` — set addr_mode=STATIC
- `test_config_dhcp_save_load` — verify nodeaddr=0 in file
- `test_cgi_parse_dhcp_mode` — assert addr_mode field, IPs preserved
- `test_cgi_parse_static_mode` — assert addr_mode field
- `test_config_preserve_unknown_fields` — change lcd_contrast to lcd_timeout (lcd_contrast is now a managed key)
- `test_config_dhcp_nodeaddr_zero` — backward compat, assert addr_mode=dhcp
- `test_config_generate_ifup_static` — set addr_mode=STATIC, use /sbin paths
- `test_config_generate_ifup_dhcp` — use addr_mode, verify netsetup calls

### Phase 2: CGI web UI enhancements

Add LCD, slot monitor, DHCP+Static, and Advanced fieldsets to the web config page. Wire up form parsing.

**Files:**
- `src/cgi/cgi_config.c` — addr_mode radio buttons (Static/DHCP/DHCP+Static), LCD fieldset (contrast + backlight), slot monitor inputs, auto-refresh on save, `config_save_strand()` call on POST
- `src/cgi/cgi_parse.h` — replace `use_dhcp` logic with `parse_addr_mode()`, add lcd_contrast (with clamping), lcd_backlight, slot_monitor parsing

**Tests (new):**
- `test_cgi_parse_dhcp_static_mode`
- `test_cgi_parse_lcd_fields`

### Phase 3: Review fixes (todos 006, 007, 008, 012)

Incorporate the P2/P3 fixes identified by the code review since they touch the same files.

**006 — Fix parse_mac on-device:**
- `src/config/config.c` — rewrite `parse_mac()` with manual hex parsing instead of `sscanf %x`
- Test: add a `test_config_mac_hex_parsing` that verifies parse_mac works without libc `%x`

**007 — XSS in CGI HTML:**
- `src/cgi/cgi_config.c` — add `html_escape()` function, apply to hostname and label outputs

**008 — Integer range validation:**
- `src/cgi/cgi_parse.h` — clamp universe (1-63999), dmx_hold_time (0-300), slot_monitor (0-512)
- Test: `test_cgi_parse_integer_bounds`

**012 — Minor validation:**
- `src/config/config.c` — validate parse_ip octets 0-255
- `src/cgi/cgi_parse.h` — explicit null-terminate after label strncpy

### Phase 4: minilib additions

Add the libc functions needed by the enhanced config system and prepare mmap for future use.

**Files:**
- `src/oabi/minilib.c` — add `fread()`, `fwrite()`, `fputs()`, `mmap()` wrapper (old_mmap 6-arg struct)
- `src/oabi/syscalls.S` — add `_sys_mmap` (NR 90), `munmap` (NR 91) (already done on feature branch)
- `src/oabi/include/sys/mman.h` — new: PROT_READ, PROT_WRITE, MAP_SHARED, MAP_FAILED
- `src/oabi/include/fcntl.h` — add O_SYNC

### Phase 5: Build system + test infrastructure

Add sanitizer testing, fuzzing, and the netsetup build target.

**Files:**
- `Makefile` — add `test-asan` target, `fuzz-config`/`fuzz-cgi` targets with Homebrew LLVM detection, `NETSETUP_SRCS`, `oabi-netsetup`/`netsetup-bflt` targets, update `deploy-web` dependency, update `.PHONY`
- `tests/fuzz_config.c` — libFuzzer harness for config_load/save round-trip
- `tests/fuzz_cgi.c` — libFuzzer harness for parse_formdata/url_decode
- `tests/corpus_config/` — seed corpus (basic.cfg, dhcp.cfg, strand_full.cfg)
- `tests/corpus_cgi/` — seed corpus (basic_post.txt, encoded.txt)

### Phase 6: cfgpost.cgi size guard

Add the shell-level safety check before `nodecfg put`.

**Files:**
- `tools/web/cfgpost.cgi` — size guard (< 4096 bytes), use strand config path, backgrounded ifup

## What is NOT included

- `src/dmx/dmx_direct.c` / `src/dmx/dmx_direct.h` — the direct UART driver (has P1 issues)
- `DMX_DRIVER_*` constants and `dmx_driver` config field — no point without the driver
- `dmx_get_ops(int driver)` signature change — stays as `dmx_get_ops(void)` on this branch
- Changes to `src/dmx/dmx_real.c`, `src/dmx/dmx_mock.c`, `src/main.c` driver dispatch

## Acceptance Criteria

- [ ] All existing 38 tests on main still pass
- [ ] ~15 new tests added and passing (targeting ~53 total, minus 4 dmx_driver tests = ~49)
- [ ] `make test-asan` clean (0 ASan/UBSan findings)
- [ ] Both fuzzers build and run without crashes (5-minute smoke test each)
- [ ] `make docker-cgi-bflt` cross-compiles successfully
- [ ] Config round-trip: DHCP, Static, DHCP+Static modes all save/load correctly
- [ ] `config_save_strand()` output contains no extension keys
- [ ] Strand config worst-case size < 2048 bytes
- [ ] CGI HTML escapes hostname and labels
- [ ] CGI integer fields clamped to valid ranges
- [ ] ifup scripts use absolute paths (/sbin/ifconfig, /sbin/route, /sbin/pump)
- [ ] Backward compat: old config files without addr_mode load correctly (inferred from nodeaddr)

## File Summary

| File | Change |
|------|--------|
| `src/common.h` | Add ADDR_MODE_*, LCD_BACKLIGHT_*, extend node_config_t |
| `src/config/config.h` | Add STRAND_CONFIG_PATH, config_save_strand, parse function declarations |
| `src/config/config.c` | Strand-safe save, addr_mode, LCD keys, parse_mac fix, parse_ip validation |
| `src/cgi/cgi_config.c` | LCD/slot/DHCP+Static UI, html_escape, auto-refresh, strand save on POST |
| `src/cgi/cgi_parse.h` | addr_mode parsing, LCD fields, integer clamping, null-terminate labels |
| `src/oabi/minilib.c` | fread, fwrite, fputs, mmap wrapper |
| `src/oabi/syscalls.S` | _sys_mmap (NR 90), munmap (NR 91) |
| `src/oabi/include/sys/mman.h` | New: mmap constants |
| `src/oabi/include/fcntl.h` | Add O_SYNC |
| `Makefile` | test-asan, fuzz targets, netsetup targets |
| `tests/test_basics.c` | ~15 new tests, updates to existing tests |
| `tests/fuzz_config.c` | New: config fuzzer harness |
| `tests/fuzz_cgi.c` | New: CGI fuzzer harness |
| `tests/corpus_config/` | New: 3 seed corpus files |
| `tests/corpus_cgi/` | New: 2 seed corpus files |
| `tools/web/cfgpost.cgi` | Size guard, strand config path, backgrounded ifup |

## Sources

- Code review findings: `todos/006-008, 012`
- Feature branch reference: `feature/direct-dmx-driver` (commit 78c18b0)
- Prior work: DHCP rework plan (now implemented on feature branch)
