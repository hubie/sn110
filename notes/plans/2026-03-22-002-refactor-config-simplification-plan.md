---
title: "refactor: Simplify Config Architecture — Eliminate Dual-Save if Safe"
type: refactor
status: active
date: 2026-03-22
---

# refactor: Simplify Config Architecture — Eliminate Dual-Save if Safe

## Overview

The config system currently has two save paths: `config_save()` writes all 18
managed keys to `/etc/220node.cfg`, and `config_save_strand()` writes only 12
Strand-compatible keys to `/etc/220node.cfg.strand`. The `.strand` file exists
solely to be passed to `nodecfg put` for flash persistence.

This dual-save was implemented as a safety measure against the theory that
writing extension keys to flash could overflow the fixed-size config sector and
brick the device. That theory has been debunked — the "bricking" was caused by
failing electrolytic capacitors on the 22-year-old board, not config overflow.

With the fear removed, we can investigate whether the dual-save is necessary and
simplify the architecture if the risk is actually negligible.

## Problem Statement

1. **Unnecessary complexity**: Two key lists (`managed_keys[18]`,
   `strand_keys[12]`), a `skip_extension_keys` flag, two config file paths
   (`CONFIG_FILE_PATH`, `STRAND_CONFIG_PATH`), and two public save functions
   (`config_save`, `config_save_strand`) — all to guard against a risk that
   turned out to be hardware failure.

2. **Extension keys likely don't survive reboots**: If Strand's `nodecfg process`
   runs at boot and regenerates `/etc/220node.cfg` from flash (the expected boot
   sequence, but not yet verified on-device), extension keys (`addr_mode`,
   `lcd_contrast`, `lcd_backlight`, `dmx_slot_monitor_*`, `dmx_driver`) would be
   lost on every reboot since they never reach flash. This needs on-device
   verification — it's possible our firmware's init sequence differs.

3. **Backward compat is already fine**: The original Strand `nodecfg` tool
   ignores unknown keys (confirmed by the hybrid config observed on-device:
   our uppercase `HOSTNAME=` keys coexisted with Strand's lowercase
   `hostname =` keys). Extension keys are safe to include in the config file.

## Proposed Solution

### Key Question: Does `nodecfg process` Preserve Unknown Keys?

The entire simplification hinges on this:

- **If yes**: Collapse to a single file and single save. Extension keys go to
  flash, survive reboots, and come back through `nodecfg process`. Remove
  `config_save_strand()`, `strand_keys[]`, `STRAND_CONFIG_PATH`, and the
  `skip_extension_keys` mechanism entirely.

- **If no**: Keep two save paths but add a secondary persistent file
  (`/etc/sn110.ext`) for extension keys so they survive reboots. Our daemon
  loads the base config then overlays extension keys from the secondary file.

Evidence is suggestive but not conclusive — the bricking investigation found a
hybrid config on the device where an earlier firmware's uppercase keys
(`HOSTNAME=`, `IPADDR=`) coexisted alongside Strand's lowercase keys
(`hostname =`, `nodeaddr =`). This could mean `nodecfg process` passed through
unknown lines, or that those keys were in a region of flash `nodecfg` doesn't
touch. Definitive verification requires on-device experiments.

### Architecture (Best Case — Single Save)

```
BEFORE (current):
  Web POST → cgi_config:
    config_save("/etc/220node.cfg", &cfg)           [18 keys]
    config_save_strand("/etc/220node.cfg.strand", &cfg) [12 keys]
  cfgpost.cgi:
    nodecfg put /etc/220node.cfg.strand              [flash]

AFTER (simplified):
  Web POST → cgi_config:
    config_save("/etc/220node.cfg", &cfg)            [all keys]
  cfgpost.cgi:
    nodecfg put /etc/220node.cfg                     [flash]
```

## Technical Approach

### Phase 1: On-Device Validation (Blocked by Cap Replacement)

These experiments run sequentially on the device via telnet.

#### Experiment 1a: Inspect Boot Sequence and Measure Flash Config Sector

First, understand what actually happens at boot:

```sh
# What init system runs?
cat /etc/inittab
ls /etc/rc.d/ 2>/dev/null || ls /etc/init.d/ 2>/dev/null

# Does nodecfg process run at boot? Search init scripts
grep -r "nodecfg" /etc/rc* /etc/init* 2>/dev/null

# What does nodecfg process actually do?
strings /sbin/nodecfg | head -40    # look for file paths, flash partitions
```

Then measure the flash config sector:

```sh
# Find flash partitions
ls -la /dev/flash*
cat /proc/mtd    # if available — shows partition sizes

# Check current flash config size
cp /etc/220node.cfg /tmp/cfg_backup
nodecfg get /tmp/cfg_from_flash
wc -c < /tmp/cfg_from_flash
```

**Success criteria:** Boot sequence documented (does `nodecfg process` run?).
Flash sector size documented. Current config well below limit (expected:
sector is 4KB-64KB, current config is ~780 bytes).

#### Experiment 1b: Test Unknown Key Preservation

```sh
# Add a test extension key to the config
echo "test_extension_key = hello_world" >> /etc/220node.cfg

# Write to flash
nodecfg put /etc/220node.cfg

# Read back from flash
nodecfg get /tmp/cfg_readback

# Check if extension key survived
grep "test_extension_key" /tmp/cfg_readback
```

**Success criteria:** `test_extension_key = hello_world` appears in the readback.

#### Experiment 1c: Test nodecfg process Round-Trip

```sh
# Write config with extension key to flash (from 1b)
# Then run nodecfg process to regenerate /etc/220node.cfg
nodecfg process

# Check if extension key survived the full cycle
grep "test_extension_key" /etc/220node.cfg
```

**Success criteria:** Extension key present in regenerated config after
`nodecfg process`.

#### Experiment 1d: Test Full Reboot Cycle

```sh
# Add all our extension keys to the config, write to flash
# (Use our daemon's config_save to write a proper full config)
# Then reboot
reboot

# After reboot, check
grep "addr_mode\|lcd_contrast\|lcd_backlight\|dmx_slot_monitor\|dmx_driver" /etc/220node.cfg
```

**Success criteria:** Extension keys survive a full power cycle.

#### Experiment 1e: Size Stress Test

```sh
# Write a config padded to 2KB, 3KB, 4KB
# Test at what size nodecfg put fails or misbehaves
dd if=/dev/zero bs=1 count=2048 | tr '\0' 'x' > /tmp/big_cfg
nodecfg put /tmp/big_cfg
nodecfg get /tmp/big_readback
wc -c < /tmp/big_readback
```

**Success criteria:** Maximum safe config size documented.

### Phase 2: Implementation

#### Path A: If nodecfg Preserves Unknown Keys (Expected)

**Delete:**
- `config_save_strand()` function
- `strand_keys[]` array and `NUM_STRAND_KEYS`
- `STRAND_CONFIG_PATH` constant
- `skip_extension_keys` parameter from `_config_save_internal()`
- Size check in `config_save_strand()` (move to `config_save()` with the
  measured flash sector limit)

**Simplify:**
- `_config_save_internal()` drops the `skip_extension_keys` flag — always
  preserves unknown lines, always writes all managed keys
- `cgi_config.c` POST handler: one call to `config_save()` instead of two
- `cfgpost.cgi`: `nodecfg put /etc/220node.cfg` directly (no `.strand` file)

**Keep:**
- Size guard — but based on measured flash sector size, not the conservative
  `STRAND_MAX_SIZE 2048`
- Atomic write (write-to-tmp, rename) already in place
- Unknown line preservation — important for forward/backward compat

**Files changed:**

| File | Change |
|------|--------|
| `src/config/config.h` | Remove `config_save_strand()`, `STRAND_CONFIG_PATH`, `STRAND_MAX_SIZE` |
| `src/config/config.c` | Remove `strand_keys[]`, simplify `_config_save_internal()`, add measured size guard to `config_save()` |
| `src/cgi/cgi_config.c` | Remove `config_save_strand()` call from POST handler |
| `tools/web/cfgpost.cgi` | `nodecfg put /etc/220node.cfg` instead of `.strand` |
| `tests/test_basics.c` | Remove `test_config_save_strand_*` tests, update remaining |

**Estimated diff:** ~80 lines removed, ~10 lines modified.

#### Path B: If nodecfg Drops Unknown Keys (Fallback)

Keep the dual-save for flash but add extension key persistence:

1. Add `/etc/sn110.ext` as a persistent file for extension keys only
2. On save: write `/etc/220node.cfg` (all keys) + `/etc/220node.cfg.strand`
   (strand keys to flash) + `/etc/sn110.ext` (extension keys)
3. On load: read `/etc/220node.cfg` first, then overlay `/etc/sn110.ext`
   (extension keys win if both present, since `/etc/220node.cfg` may have been
   regenerated by `nodecfg process` with defaults)
4. Store `/etc/sn110.ext` in a separate flash partition or JFFS2 area if
   available, or just accept it lives only in the filesystem (survives soft
   reboot but not flash reflash)

This is MORE complex than current, but solves extension key persistence.

**Note:** On uClinux 2.0, the root filesystem is typically RAM-based (romfs or
cramfs). `/etc/sn110.ext` would need to live on a writable flash partition
(JFFS2 or raw) to actually survive reboots. Experiment 1a's boot sequence
inspection will reveal what writable storage is available.

**Files changed:**

| File | Change |
|------|--------|
| `src/config/config.h` | Add `config_load_extensions()`, `config_save_extensions()`, `EXT_CONFIG_PATH` |
| `src/config/config.c` | Add extension-only save/load functions |
| `src/main.c` | Call `config_load_extensions()` after `config_load()` |
| `src/cgi/cgi_config.c` | Add `config_save_extensions()` call |

## Backward Compatibility

Core principle: **old Strand configs must still load correctly.**

This is already handled by `config_load()`:
- Unknown keys (Strand's `nodetype`, `boottest`, `dmx = ...`) are ignored
  during load and preserved verbatim on save
- Missing extension keys get defaults via `config_defaults()`
- `ADDR_MODE_SENTINEL (255)` triggers inference from `nodeaddr` for old configs
  without `addr_mode`

Neither Path A nor Path B changes any of this. The backward compat layer is in
the load path, not the save path.

The one thing that changes: in Path A, `nodecfg put` receives a file with
extension keys. Strand's `nodecfg` should ignore them (it only looks for its own
keys). But this needs the Phase 1 experiments to confirm.

## Acceptance Criteria

### Phase 1 (Experiment)

- [ ] Boot sequence documented (does `nodecfg process` run? what regenerates `/etc/220node.cfg`?)
- [ ] Flash config sector size documented
- [ ] `nodecfg put` + `nodecfg get` round-trip preserves unknown keys (or not)
- [ ] `nodecfg process` preserves unknown keys in `/etc/220node.cfg` (or not)
- [ ] Full reboot cycle tested with extension keys
- [ ] Maximum safe config size documented
- [ ] Writable flash partitions identified (relevant if Path B needed)

### Phase 2 (Implementation — Path A)

- [ ] `config_save_strand()` removed from public API
- [ ] `strand_keys[]` and dual-save mechanism removed
- [ ] Single `config_save()` call in CGI POST handler
- [ ] `cfgpost.cgi` writes `/etc/220node.cfg` to flash directly
- [ ] Size guard uses measured flash sector limit
- [ ] Extension keys survive reboot cycle on device
- [ ] All existing tests pass (update strand-specific tests)
- [ ] New test: full config round-trip including extension keys
- [ ] Cross-compile succeeds (Docker bFLT + CGI bFLT)
- [ ] `dmx_driver=kernel` still works identically to current behavior

### Phase 2 (Implementation — Path B, if needed)

- [ ] Extension key overlay file (`/etc/sn110.ext`) implemented
- [ ] Extension keys survive reboot via overlay
- [ ] Load order correct: base config + overlay
- [ ] All existing tests pass
- [ ] New tests for extension overlay round-trip

## Dependencies & Risks

### Dependencies

- **Device power supply repair**: Capacitor replacement on order. All on-device
  experiments blocked until this is resolved. (Same dependency as the RCGT plan.)

### Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| `nodecfg put` truncates at sector boundary | Low | Data loss if config > sector | Experiment 1e measures limit; keep size guard |
| `nodecfg process` drops unknown keys | Medium | Must use Path B (more complex) | Experiment 1c tests this directly |
| `nodecfg put` corrupts flash if it sees unknown keys | Very Low | Device needs JTAG recovery | Test on device with JTAG connected for recovery |
| Extension keys in flash confuse Strand firmware | Low | Strand daemon ignores unknown keys | Already demonstrated by hybrid config on device |

## Sources & References

- **Bricking investigation**: Memory `bricking-investigation.md` — root cause was capacitor failure, not config overflow
- **Flash safety rules**: Memory `safety-nodecfg-put.md` — current safety rules to be updated based on experiments
- **Config hardening**: `docs/solutions/logic-errors/config-hardening-strand-safe-flash-dhcp-rework.md` — history of the dual-save implementation
- **Current code**: `src/config/config.c` (lines 298-538), `src/config/config.h`, `tools/web/cfgpost.cgi`
- **Device config snapshot**: Bricking investigation found 780-byte hybrid config with both formats coexisting
