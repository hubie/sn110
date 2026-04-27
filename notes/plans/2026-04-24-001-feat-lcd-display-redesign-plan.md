---
title: "feat: LCD display redesign with context-aware modes and hold countdown"
type: feat
status: active
date: 2026-04-24
origin: docs/brainstorms/2026-04-24-lcd-display-requirements.md
---

# LCD Display Redesign

## Overview

Replace the current static LCD layout (`lcd_update()` in `src/main.c:225-327`) with a context-aware, multi-mode display that shows data-rich per-port status. The redesign adds display modes (boot, DHCP, normal, link-down), an IP/MAC carousel, per-port state with direction arrows (CP437 glyphs), transition flashes (SRC LOST/SRC OK), and a hold-timer countdown. It also introduces a new runtime feature: configurable hold-to-blackout with visible countdown.

## Problem Statement

The current LCD implementation uses a single static layout with a configurable Line 1 and Lines 2-3 showing protocol/universe and mode/state. Line 4 is unused. Now that the LCD hardware is fully characterized (96x32px, 16x4 chars, dual-buffer architecture, CP437 glyphs confirmed — see origin), the display can be much more useful: context-dependent modes, spatial alignment with physical ports, and a hold countdown timer that gives operators real-time feedback.

## Proposed Solution

Extract LCD logic from `src/main.c` into a new `src/lcd/` module. Implement a state machine that drives display mode transitions. The existing 1-second update tick (`LCD_UPDATE_MS`) remains the heartbeat, with sub-second events (transition flashes) tracked via a tick counter.

## Technical Approach

### Architecture

```
src/lcd/
  lcd.h          — Public API: lcd_init(), lcd_update(), lcd_shutdown()
  lcd.c          — State machine, mode rendering, ioctl wrappers
  lcd_hw.h       — Low-level: cmd 7 partial write, cmd 2/8 clear, cmd 18 backlight
  lcd_hw.c       — ioctl helpers (open-ioctl-close pattern per call)
```

The LCD module receives a read-only snapshot of display-relevant state each tick. It owns no network or DMX state — `main.c` passes a struct:

```c
/* src/lcd/lcd.h */

#define LCD_PORT_COUNT 2

typedef struct {
    /* Network identity */
    char     hostname[16];        /* empty string = no hostname */
    uint32_t ip_addr;             /* 0 = not yet assigned */
    uint8_t  mac[6];
    uint8_t  addr_mode;           /* ADDR_MODE_* */
    int      link_up;             /* 1 = Ethernet link active */

    /* Per-port state */
    struct {
        int      mode;            /* DMX_MODE_OFF / TX / RX */
        uint16_t universe;        /* 1-based */
        int      live;            /* 1 = actively receiving/sending data */
        int      held;            /* 1 = source lost, holding last values (TX-output ports only) */
        uint32_t hold_remaining_ms; /* ms until hold expires (0 if not held; TX-output ports only) */
    } port[LCD_PORT_COUNT];
} lcd_state_t;
```

`main.c` computes this struct from existing globals (`g_config`, `g_dmx_out`, `now_ms()`) each second and passes it to `lcd_update()`. The LCD module never touches `g_config` or `g_dmx_out` directly.

### Display Mode State Machine

```
                  ┌─────────┐
                  │  BOOT   │ (splash, ~3-5s)
                  └────┬────┘
                       │
            ┌──────────┴──────────┐
            │ addr_mode==DHCP     │ addr_mode==STATIC
            │ && ip_addr==0       │ or ip_addr!=0
            ▼                     ▼
       ┌─────────┐          ┌─────────┐
       │  DHCP   │──ip!=0──▶│ NORMAL  │◀──link restored
       └─────────┘          └────┬────┘
                                 │ link_up==0
                                 ▼
                            ┌─────────┐
                            │LINK_DOWN│──link restored──▶ NORMAL
                            └─────────┘
```

States:
- **BOOT**: Show splash. Transition to DHCP or NORMAL when `g_lcd_hold` expires.
- **DHCP**: Show `DHCP...` + MAC. Transition to NORMAL when `ip_addr != 0`.
- **NORMAL**: Primary runtime display (Lines 1-4). Transition to LINK_DOWN when `link_up == 0`.
- **LINK_DOWN**: Same as NORMAL but Line 2 shows `LINK DOWN` instead of IP/MAC.

### Normal Mode Line Rendering

Each line is rendered independently via ioctl cmd 7 partial writes (no more 64-byte bulk write):

**Line 1 — Identity** (`src/lcd/lcd.c`)
```c
/* If hostname configured, show it. Otherwise show IP. */
if (state->hostname[0])
    lcd_write_line(0, state->hostname);
else
    lcd_write_line(0, format_ip(state->ip_addr));
```

**Line 2 — IP/MAC Carousel** (`src/lcd/lcd.c`)
```c
/* Carousel toggles every 10 ticks (10s at 1Hz update rate) */
static int carousel_tick = 0;
carousel_tick++;

if (!state->hostname[0]) {
    /* No hostname → IP is on Line 1, MAC always on Line 2 */
    lcd_write_line(1, format_mac(state->mac));
} else if ((carousel_tick / 10) % 2 == 0) {
    lcd_write_line(1, format_ip(state->ip_addr));
} else {
    lcd_write_line(1, format_mac(state->mac));
}
```

MAC format: `00E001:00ECFD` (13 chars, colon at midpoint leverages kernel's extra pixel at position 8).

**Line 3 — Port Direction + Universe** (`src/lcd/lcd.c`)

Left half (cols 0-7) = Port 0, right half (cols 8-15) = Port 1.

```c
/* Arrow glyphs: 0x18 = ↑ (RX, away from port), 0x19 = ↓ (TX, toward port) */
#define GLYPH_RX  0x18  /* ↑ */
#define GLYPH_TX  0x19  /* ↓ */

for (i = 0; i < 2; i++) {
    if (state->port[i].mode == DMX_MODE_OFF) {
        snprintf(half[i], 9, "        ");
    } else {
        char arrow = (state->port[i].mode == DMX_MODE_TX) ? GLYPH_TX : GLYPH_RX;
        snprintf(half[i], 9, "%cU%-5u", arrow, state->port[i].universe);
    }
}
lcd_write_line(2, combine_halves(half[0], half[1]));
```

**Line 4 — Port State** (`src/lcd/lcd.c`)

Per-port state label with context-dependent extras:

| State | Display | Condition |
|-------|---------|-----------|
| LIVE | `LIVE` | `port.live && !port.held` |
| HELD | `HLD M:SS` | `port.held && hold_remaining_ms > 0` (TX-output ports only) |
| IDLE | `IDLE` | `!port.live && !port.held` |
| SRC LOST | `SRC LOST` | Transition flash, ~2s (TX-output ports only) |
| SRC OK | `SRC OK` | Transition flash, ~2s (TX-output ports only) |
| (blank) | `        ` | `port.mode == DMX_MODE_OFF` |

Note: HELD, SRC LOST, and SRC OK only apply to TX-output ports (receiving sACN to output DMX). RX-input ports (reading DMX hardware to send sACN) have no upstream "source" to lose — they are simply LIVE when DMX data is being read, IDLE otherwise.

### Transition Flash State Machine

Each port has its own flash state, tracked in the LCD module:

```c
typedef enum {
    FLASH_NONE,
    FLASH_SRC_LOST,
    FLASH_SRC_OK
} flash_state_t;

typedef struct {
    flash_state_t state;
    int           ticks_remaining;  /* counts down from 2 */
    int           prev_live;        /* previous tick's live state */
} port_flash_t;

static port_flash_t g_flash[LCD_PORT_COUNT];
```

Each tick:
1. Compare `state->port[i].live` against `g_flash[i].prev_live`
2. If transition detected (live→!live or !live→live), set flash state and `ticks_remaining = 2`
3. If `ticks_remaining > 0`, render flash text on Line 4 instead of normal state
4. Decrement `ticks_remaining`; when 0, return to normal rendering

Edge case: if source is lost and recovered within the same 1s tick, no flash is shown (state didn't change from the LCD module's perspective). This is acceptable — sub-second blips shouldn't alarm the operator.

### Hold Timer

The hold timer is a **new runtime feature**, not just a display feature (see origin: R9).

**Current behavior** (`src/main.c:447-458`): `dmx_output_cycle()` checks `age > dmx_hold_time * 1000` where `age = now - last_update_ms` (time since last packet). This means the hold period is measured from the last packet received, which includes the ~2.5s sACN timeout (`SACN_TIMEOUT_MS`) before the source is considered lost. The display would show HELD, but the timer origin doesn't match what the operator sees.

**Change**: Add `hold_start_ms` to track when hold began, and use it as the single source of truth for both the blackout decision and the display countdown:

```c
/* In main.c, alongside existing g_dmx_out[] */
static uint32_t g_hold_start_ms[DMX_MAX_PORTS];  /* 0 = not held */
```

When `dmx_output_cycle()` detects source timeout (age > `SACN_TIMEOUT_MS`) but hold hasn't expired yet:
- Set `g_hold_start_ms[i] = now` on first detection
- Compute `remaining = (dmx_hold_time * 1000) - (now - g_hold_start_ms[i])`
- Pass `hold_remaining_ms` in `lcd_state_t`

**Critical**: The blackout check must also switch to `g_hold_start_ms`:

```c
/* OLD: age measured from last packet — includes sACN timeout period */
int timed_out = (g_dmx_out[i].last_update_ms > 0) &&
                (age > (uint32_t)g_config.dmx_hold_time * 1000);

/* NEW: hold measured from source-lost detection — display and output agree */
int timed_out = (g_hold_start_ms[i] > 0) &&
                ((t - g_hold_start_ms[i]) > (uint32_t)g_config.dmx_hold_time * 1000);
```

This ensures the display countdown and the actual blackout fire at the same instant. Without this change, they'd disagree by ~2.5 seconds (the sACN timeout period).

When hold expires, existing blackout logic fires and `g_hold_start_ms[i]` resets to 0. When source recovers during hold, `g_hold_start_ms[i]` also resets to 0.

Display format: `HLD M:SS` (8 chars max). Hold times >9:59 show `HLD 9:59` until countdown drops below 10 minutes. Default hold is 5 minutes (already in config as `dmx_hold_time`).

### IP Address Detection (DHCP Transition)

The daemon currently loads `g_config.ip_addr` once at startup (`src/main.c:542`) and never updates it. For the DHCP display mode to transition to Normal, the daemon needs to detect when an IP is assigned.

**Solution**: Periodic `SIOCGIFADDR` ioctl on the `eth0` socket, checked once per LCD update tick (1s). This is the same mechanism `netsetup.c` uses.

```c
/* In lcd_state_t population (main.c) */
static uint32_t detect_ip(void) {
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 0;
    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ);
    if (ioctl(fd, SIOCGIFADDR, &ifr) < 0) { close(fd); return 0; }
    close(fd);
    return ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
}
```

When `addr_mode` is DHCP and `ip_addr` transitions from 0 to non-zero, also update `g_config.ip_addr` so the rest of the daemon sees it.

### Ethernet Link State Detection

No link state detection currently exists. Add periodic `SIOCGIFFLAGS` check:

```c
static int detect_link(void) {
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return 1;  /* assume up if can't check */
    strncpy(ifr.ifr_name, "eth0", IFNAMSIZ);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) { close(fd); return 1; }
    close(fd);
    return (ifr.ifr_flags & IFF_RUNNING) ? 1 : 0;
}
```

**Research needed**: Verify `IFF_RUNNING` is available on Linux 2.0 / uClinux with the NS7520 Ethernet driver. If not, an alternative is checking carrier via the hardware registers directly. This should be tested on-device early in implementation.

### LCD Hardware Helpers

Extract from `main.c` into `src/lcd/lcd_hw.c`:

```c
/* Open-ioctl-close per write (matches factory pattern, avoids cursor races) */
void lcd_cmd7_write(int col, int row, const char *text);

/* Clear character buffer (cmd 2) */
void lcd_clear_chars(void);

/* Clear pixel framebuffer (cmd 8) */
void lcd_clear_pixels(void);

/* Set backlight (cmd 18): 0=off, nonzero=on */
void lcd_backlight(int on);

/* Set contrast (cmd 17): CLAMPS to 0-63, never passes negative */
void lcd_contrast(int value);
```

Safety: `lcd_contrast()` must clamp to 0-63 in userspace. Negative values crash the device (see origin: Hardware Constraints).

### Boot Splash Changes

Current splash (`src/main.c:342-368`) uses 64-byte bulk write with alignment tricks. The new module should:

1. Clear both buffers on init (cmd 8 then cmd 2) — prevents pixel artifacts from prior process
2. Write splash via cmd 7 partial writes (no alignment counter dependency)
3. Set backlight per config
4. Hold for `g_lcd_hold` ticks (existing mechanism)

This removes the fragile 64-byte counter alignment requirement entirely. All writes go through cmd 7.

### Removing lcd_write_screen()

The current `lcd_write_screen()` (`src/main.c:195-213`) writes 64 bytes in one `write()` call to maintain counter alignment. Once all LCD writes use cmd 7 partial writes, this function and the alignment bookkeeping become unnecessary. Remove it.

## System-Wide Impact

- **main.c shrinkage**: ~180 lines of LCD code (`lcd_splash`, `lcd_write_screen`, `lcd_update`, LCD globals) move to `src/lcd/`. Replaced by ~15 lines: build `lcd_state_t`, call `lcd_update()`.
- **Hold timer computation**: Small addition to `dmx_output_cycle()` to track `g_hold_start_ms[]`. No change to existing blackout behavior.
- **IP detection**: New periodic ioctl in main loop. Cost: one `socket()+ioctl()+close()` per second when in DHCP mode. Negligible.
- **Link detection**: Same cost profile as IP detection. One ioctl per second.
- **No protocol changes**: sACN, Art-Net, ShowNet paths are untouched.
- **Build system**: Add `src/lcd/lcd.c` and `src/lcd/lcd_hw.c` to `SRC_DIRS` or explicit source list in Makefile.

## Implementation Phases

### Phase 1: Extract LCD Module (Foundation)

Create `src/lcd/` with `lcd.h`, `lcd.c`, `lcd_hw.h`, `lcd_hw.c`.

Tasks:
- [ ] `src/lcd/lcd_hw.h` + `src/lcd/lcd_hw.c` — ioctl cmd 7 partial write, cmd 2/8 clear, cmd 17 contrast (clamped), cmd 18 backlight. Open-ioctl-close per call.
- [ ] `src/lcd/lcd.h` — Define `lcd_state_t`, public API (`lcd_init`, `lcd_update`, `lcd_shutdown`)
- [ ] `src/lcd/lcd.c` — Mode state machine (BOOT/DHCP/NORMAL/LINK_DOWN), tick counter, carousel counter, per-port flash state
- [ ] `src/main.c` — Remove `lcd_splash()`, `lcd_write_screen()`, `lcd_update()`, LCD globals. Replace with `lcd_init()` at startup, `lcd_state_t` population + `lcd_update(&state)` in main loop, `lcd_shutdown()` at exit.
- [ ] `Makefile` — Add `src/lcd/*.c` to build

**Success criteria**: Firmware builds and runs. LCD shows boot splash, then current port state. Visually identical output to current behavior — though the underlying write path changes from 64-byte bulk `write()` to cmd 7 `ioctl()`, the on-screen result should match.

### Phase 2: Normal Mode Layout

Implement the new 4-line layout for normal operation.

Tasks:
- [ ] Line 1: hostname or IP (from `lcd_state_t`)
- [ ] Line 2: IP/MAC carousel with 10s toggle (modulo counter on 1Hz tick)
- [ ] Line 3: Direction arrow + universe per port, left/right split. Use `\x18` (↑) for RX, `\x19` (↓) for TX.
- [ ] Line 4: Port state labels (LIVE, IDLE, HLD)
- [ ] MAC format: `00E001:00ECFD` (colon at midpoint)

**Success criteria**: On-device, LCD shows hostname, rotating IP/MAC, per-port arrows+universe, per-port state. Visual inspection matches mockups from requirements doc.

### Phase 3: Hold Timer Countdown

Implement the runtime hold countdown and display it.

Tasks:
- [ ] `src/main.c` — Add `g_hold_start_ms[]` tracking in `dmx_output_cycle()`. Set on first source timeout detection, clear on hold expiry or source recovery.
- [ ] `src/main.c` — Compute `hold_remaining_ms` when populating `lcd_state_t`
- [ ] `src/lcd/lcd.c` — Render `HLD M:SS` on Line 4 when `port.held && hold_remaining_ms > 0`. Format: minutes (1 digit) + colon + seconds (2 digits, zero-padded).
- [ ] Handle edge: hold times >9:59 display as `HLD 9:59` until countdown drops below 10 min

**Success criteria**: When sACN source stops, Line 4 shows countdown from configured hold time. When countdown reaches 0, transitions to IDLE. When source resumes during hold, returns to LIVE.

### Phase 4: Transition Flashes

Implement SRC LOST and SRC OK flashes.

Tasks:
- [ ] `src/lcd/lcd.c` — Per-port `port_flash_t` state. Detect live→!live and !live→live transitions on each tick.
- [ ] Flash `SRC LOST` for 2 ticks when source lost, then show HLD or IDLE
- [ ] Flash `SRC OK` for 2 ticks when source recovered, then show LIVE
- [ ] Verify both ports can flash independently

**Success criteria**: On-device, stopping sACN source briefly shows SRC LOST before hold countdown. Resuming source shows SRC OK before LIVE.

### Phase 5: DHCP and Link-Down Modes

Tasks:
- [ ] `src/main.c` — Add `detect_ip()` using `SIOCGIFADDR`. Call once per LCD tick when `addr_mode` is DHCP and `ip_addr == 0`. Update `g_config.ip_addr` on detection.
- [ ] `src/main.c` — Add `detect_link()` using `SIOCGIFFLAGS` / `IFF_RUNNING`. Call once per LCD tick. Pass result in `lcd_state_t.link_up`.
- [ ] `src/lcd/lcd.c` — DHCP mode: show `DHCP...` on Line 1, `00E001:00ECFD` on Line 2, Lines 3-4 blank. Transition to NORMAL when `ip_addr != 0`.
- [ ] `src/lcd/lcd.c` — LINK_DOWN mode: replace Line 2 with `LINK DOWN`. Lines 3-4 continue showing port state naturally (sources will timeout through normal sACN path).
- [ ] **On-device test**: Verify `IFF_RUNNING` works on uClinux/NS7520. If not, investigate alternative (direct register check or `/proc/net/` parsing).

**Success criteria**: Unplugging Ethernet shows LINK DOWN on Line 2. DHCP mode shows MAC during negotiation and transitions to normal when IP acquired.

### Phase 6: Cleanup and Polish

Tasks:
- [ ] Ignore `lcd_info_line` config key — still parsed by `config.c` (no config system change needed), but not acted on. Hostname-first logic supersedes it.
- [ ] Boot splash: clear both buffers (cmd 8 + cmd 2) on init
- [ ] Shutdown: clear display on SIGTERM (`lcd_shutdown()`)
- [ ] Update `docs/lcd-partial-write.md` with new module architecture
- [ ] On-device verification of all display modes and transitions

## Acceptance Criteria

### Functional (from origin: R1-R14)

- [ ] R1: Boot splash shows firmware name and version for ~3-5s
- [ ] R2: DHCP screen shows `DHCP...` + MAC address while negotiating
- [ ] R3-R8: Normal mode shows hostname, IP/MAC carousel, per-port ↑/↓+universe, per-port state
- [ ] R9: Hold timer counts down in M:SS format, transitions to IDLE at zero
- [ ] R10: SRC LOST flashes ~2s on affected port when source lost
- [ ] R11: SRC OK flashes ~2s on affected port when source recovered
- [ ] R4: LINK DOWN replaces IP on Line 2 when Ethernet link is down
- [ ] R7: ↓ for TX (0x19), ↑ for RX (0x18) — physical direction convention
- [ ] R12: No decorative glyphs — all characters convey information
- [ ] R13: Left half = Port 0, right half = Port 1

### Non-Functional

- [ ] LCD module is self-contained in `src/lcd/` with no direct access to `g_config` or `g_dmx_out`
- [ ] Contrast function clamps 0-63 (negative values crash device)
- [ ] Both buffers cleared on init (cmd 8 + cmd 2)
- [ ] No 64-byte alignment dependency — all writes via cmd 7

## Dependencies & Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| `IFF_RUNNING` not available on Linux 2.0/NS7520 | Medium | Phase 5 blocked | Test early on device. Fallback: direct Ethernet register check or skip link detection for v1 |
| DHCP IP detection adds latency to main loop | Low | Negligible | One socket+ioctl+close per second is ~1ms |
| Carousel/flash timing feels off at 1Hz | Low | UX annoyance | Tune tick counts on device. Could increase to 2Hz if needed (500ms update) |
| `lcd_info_line` config removal breaks existing setups | Low | Config migration | Keep reading the key but ignore it — hostname-first logic supersedes |

## Sources & References

### Origin

- **Origin document:** [docs/brainstorms/2026-04-24-lcd-display-requirements.md](../brainstorms/2026-04-24-lcd-display-requirements.md) — Key decisions carried forward: ↓TX/↑RX arrow convention, hostname-first layout, hold countdown (not count-up), MAC format with midpoint colon, no packet rate display.

### Internal References

- Current LCD code: `src/main.c:195-368` (lcd_write_screen, lcd_update, lcd_splash)
- LCD ioctl reference: `docs/lcd-partial-write.md`
- DMX output/hold logic: `src/main.c:426-461` (dmx_output_cycle)
- sACN timeout: `src/sacn/sacn.h:20` (SACN_TIMEOUT_MS = 2500)
- Config struct: `src/common.h:79-96` (node_config_t)
- Port config: `src/common.h:71-76` (port_config_t)
- Empty LCD directory: `src/lcd/` (ready for new module)

### Documented Learnings

- LCD dual-buffer architecture: `docs/solutions/reverse-engineering/lcd-dual-buffer-architecture-and-pixel-verification.md`
- Character filtering and contrast crash: `docs/solutions/reverse-engineering/lcd-driver-partial-update-verification-and-safety.md`
- Cmd 7 API contract: `docs/solutions/reverse-engineering/lcd-driver-ioctl7-cursor-positioned-nul-terminated-write.md`
- DMX driver modes (1=TX, 2=RX): `docs/solutions/integration-issues/dmx-kernel-driver-mode-values-and-close-triggered-tx.md`
- DHCP addr_mode: `docs/solutions/logic-errors/config-hardening-strand-safe-flash-dhcp-rework.md`
