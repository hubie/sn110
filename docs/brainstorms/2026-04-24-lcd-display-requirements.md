---
date: 2026-04-24
topic: lcd-display-redesign
---

# LCD Display Redesign

## Problem Frame

The current LCD implementation (`lcd_splash()`, `lcd_update()`, `lcd_write_screen()` in `src/main.c`) uses a static layout with a configurable Line 1 and a simple left/right port split on Lines 2-3. Line 4 is unused. Now that the LCD hardware is fully characterized (dual-buffer architecture, 96x32 pixels, 16x4 characters with 6px cells, CP437 glyphs 0x01-0x1F confirmed displayable, backlight binary on/off), the display can be redesigned to be data-rich, context-aware, and spatially intuitive relative to the physical port layout.

## Hardware Constraints

These are established facts from on-device testing (2026-04-23/24), not requirements to implement. They bound all layout decisions.

- **Display**: 96x32 pixels, 16 chars x 4 rows, 6px character cells, no inter-character padding
- **Mid-screen pixel gap**: kernel inserts an extra pixel between chars at position 8, useful for MAC display (DE:AD:BE EF:12:34 renders with natural spacing)
- **Character path**: 113 displayable chars (0x01-0x08, 0x0E-0x1F, 0x20-0x7E). Four blanks: 0x09, 0x0B, 0x0C, 0x0D
- **Available CP437 glyphs for UI**: arrows (0x18 ↑, 0x19 ↓, 0x1A →, 0x1B ←), triangles (0x10 ►, 0x11 ◄, 0x1E ▲, 0x1F ▼), heart (0x03 ♥), section (0x15 §), bars, suits
- **Dual buffers**: character buffer (cleared by cmd 2) and pixel framebuffer (cleared by cmd 8) are independent. Full wipe requires both.
- **Backlight**: binary on/off (cmd 18), no dimming
- **Contrast**: 0-63 via cmd 17. Negative values crash the device. Practical range ~0-30.
- **Partial writes**: ioctl cmd 7 writes NUL-terminated string at (col, row) without disturbing other positions

## Requirements

### Display Modes

- R1. **Boot Splash**: Show firmware name and version on startup. Duration ~3 seconds or until initialization completes, whichever is longer.

- R2. **DHCP Negotiating**: When DHCP is enabled, show a DHCP negotiation screen while awaiting lease. Display the MAC address so the operator can locate the device in a DHCP server's lease table. This screen replaces normal display until an IP is acquired (or DHCP times out/fails).
  ```
  DHCP...         
  00E001:00ECFD   
                  
                  
  ```

- R3. **Normal Operation**: The primary runtime display. Layout varies by port state but follows a consistent 4-line structure (see R4-R9).

- R4. **Link Down (Fault)**: When Ethernet link is physically down, replace the IP on Line 2 with `LINK DOWN`. This is the highest-priority fault indicator.

### Normal Operation Layout

- R5. **Line 1 — Hostname**: Display the configured hostname. If no hostname is configured, display the IP address here instead (and skip the Line 2 carousel).

- R6. **Line 2 — IP/MAC Carousel**: When hostname occupies Line 1, carousel between IP address and MAC address on Line 2, alternating every ~10 seconds. When IP occupies Line 1 (no hostname), display MAC on Line 2 without carousel.

- R7. **Line 3 — Port Direction and Universe**: Left half = Port 0, right half = Port 1. Show direction arrow + universe number. Use ↓ (0x19) for TX (pointing down toward the physical port/cable) and ↑ (0x18) for RX (pointing up, away from port/cable). The physical device has Port 0 under the left half of the display and Port 1 under the right half. If a port is disabled/unconfigured, its half is blank. Universe numbers are displayed without padding (e.g., `↑U1`, `↑U12`, `↓U512`).

- R8. **Line 4 — Port State**: Left half = Port 0 state, right half = Port 1 state. Possible states:
  - `LIVE` — actively receiving/transmitting sACN data
  - `IDLE` — no sACN source seen (or hold timer expired), output is blackout
  - `HLD M:SS` — source lost, output frozen at last DMX values, countdown timer showing remaining hold time
  - Blank — port disabled/unconfigured (matches blank Line 3)
  - TX ports show `LIVE` when actively transmitting, `IDLE` otherwise

- R9. **Hold Timer Countdown**: When a port enters HELD state, display a countdown timer in `M:SS` format (e.g., `HLD 4:59`) that counts down to zero. When the timer reaches zero, transition to IDLE state. The hold duration is configurable (default 5 minutes). This is a new feature that must be implemented alongside the display work.

### Transitions

- R10. **Source Lost Flash**: When an sACN source is lost on a port, briefly flash `SRC LOST` on that port's Line 4 position for ~2 seconds before transitioning to the HELD countdown (if hold is enabled) or IDLE.

- R11. **Source Recovered Flash**: When an sACN source resumes on a port (from HELD or IDLE), briefly flash `SRC OK` on that port's Line 4 position for ~2 seconds before showing `LIVE`.

### Design Principles

- R12. **Data-rich and utilitarian**: Use CP437 glyphs (arrows, triangles) for information density, not decoration. Every character on screen should convey useful state.

- R13. **Spatial intuition**: Port 0 information on the left half of the display, Port 1 on the right half, matching the physical port positions below the LCD.

- R14. **Context-dependent content**: Line 4 content varies by port state — clean state label for LIVE/IDLE/TX, countdown timer for HELD. No wasted space showing static labels when dynamic data is available.

## Success Criteria

- All four display lines are used to convey meaningful data during normal operation
- An operator can identify: hostname, IP, MAC, per-port direction, per-port universe, and per-port state from the LCD without web UI access
- Port state transitions (LIVE/HELD/IDLE) are visually apparent without needing to watch the screen continuously (transition flashes)
- The hold countdown timer provides clear feedback on remaining hold time
- Display modes transition automatically based on device state (boot -> DHCP -> normal, link down overlay)

## Scope Boundaries

- **Not in scope**: pixel-layer graphics (cmds 14-16) for this redesign — character path only
- **Not in scope**: packet rate or packets-per-second display (no compelling precedent in commercial devices)
- **Not in scope**: per-source sACN information (source IP, source name) — this belongs in the web UI
- **Not in scope**: firmware version display outside of boot splash
- **Not in scope**: uptime display
- **Not in scope**: contrast or backlight runtime control from display logic (these are configuration-time settings)
- **Not in scope**: multi-universe-per-port display (current firmware supports one universe per port)

## Key Decisions

- **Arrows = physical direction**: ↓ for TX (toward cable), ↑ for RX (away from cable). Chosen because the ports are physically below the display — the arrows indicate data flow direction relative to the device, not abstract input/output.
- **Hostname takes priority over IP on Line 1**: Hostname is the human-friendly identifier; IP is secondary and carousels on Line 2. If no hostname, IP promotes to Line 1.
- **MAC format uses mid-screen gap**: `00E001:00ECFD` (no colons in each half, colon at the midpoint) leverages the kernel's extra pixel at position 8 for natural visual separation.
- **Hold timer counts down**: Countdown chosen over count-up because the operator cares about "how long until blackout," not "how long has it been frozen."
- **HELD state with configurable hold period**: Default 5 minutes. After hold expires, transition to IDLE (blackout). This is a new runtime feature, not just a display feature.
- **Transition flashes are per-port**: SRC LOST and SRC OK flash on the affected port's Line 4 position only, not full-screen.
- **No packet rate display**: Not compelling for this device class. No evidence of commercial lighting nodes showing it.

## Dependencies / Assumptions

- sACN receive state tracking must distinguish LIVE, HELD, and IDLE per port
- Hold timer implementation requires a configurable timeout (default 5m) and per-port countdown
- Hostname availability depends on configuration subsystem (currently stored in flash config)
- Ethernet link state detection must be available to trigger LINK DOWN mode
- DHCP state must be observable to show the negotiation screen

## Outstanding Questions

### Deferred to Planning

- [Affects R6][Technical] What is the best mechanism for the IP/MAC carousel timer? Separate thread, or handled within the existing 1-second lcd_update() loop with a modulo counter?
- [Affects R9][Technical] Where should the hold timer live — in the sACN receive module, the DMX output module, or a shared state structure? Current architecture may inform this.
- [Affects R2][Technical] How should DHCP timeout/failure be handled on the display? Fall back to static IP screen, show error, or retry indefinitely?
- [Affects R4][Needs research] What kernel interface provides Ethernet link state on uClinux/NS7520? `/proc/net/` or ioctl on the network socket?
- [Affects R10-R11][Technical] How should the ~2 second transition flash interact with the 1-second update loop? Should it use a one-shot timer or a state machine with tick counting?

## Next Steps

-> `/ce:plan` for structured implementation planning
