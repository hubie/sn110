---
date: 2026-04-04
topic: lcd-display
focus: What the SN110 LCD screen should display at runtime
---

# Ideation: LCD Runtime Display

## Codebase Context

- **Device:** Strand SN110, ARM7TDMI, uClinux 2.0.38, **4-line × 16-character** character LCD at `/dev/lcd0`
- **Display confirmed:** Photo of original Strand boot screen shows 4 lines: brand/version (lines 1-2), blank (line 3), MAC address (line 4)
- **Original boot screen:** `Strand  Lighting` / `v2.10.7` / *(blank)* / `00E001:  00:EC:FD`
- **Interface:** Kernel driver at `/dev/lcd0` supports `write()` (64-byte circular buffer) and `ioctl _IOW('l', 7, 48)` for cursor-positioned partial writes. See `docs/lcd-partial-write.md` for full ioctl command table.
- **Config fields wired:** `lcd_contrast` (0-255), `lcd_backlight` (off/on/auto), `dmx_slot_monitor[2]`
- **Daemon knows at runtime:** active protocol, universe per port, port mode (TX/RX), IP address, DHCP/static, packet receive rate, DMX channel values (all 512), source IP, hold state + elapsed time, uptime
- **Known PoE instability:** device reboots intermittently — a watchdog restart counter would surface this
- **Deployment context:** theatre/events, dark racks, operators under time pressure, no laptop in hand
- **Design constraint:** no buttons — read-only output only, no interaction model
- **Key implication of 4 lines:** identity, routing, AND both port statuses can all be shown simultaneously — no rotation needed

## Past Learnings

- `lcd_contrast` and `lcd_backlight` config fields are already round-tripped correctly through config save/load
- LCD config keys must remain in the extension key set (not Strand flash `strand_keys[]`)
- `/dev/lcd0` write semantics fully documented: `write()` uses a 64-byte circular buffer; `ioctl _IOW('l', 7, 48)` does cursor-positioned NUL-terminated partial writes. See `docs/lcd-partial-write.md` and `docs/solutions/reverse-engineering/lcd-driver-ioctl7-cursor-positioned-nul-terminated-write.md`

## Ranked Ideas

### 1. Per-Port Health Display *(primary screen)*
**Description:** The always-visible runtime screen. Line 1 = Port 0 status, Line 2 = Port 1 status. Status words: `LIVE 44pkt/s` → `HELD 01:23` → `IDLE` → `NO SRC`. When a port is in hold state, the packet rate is replaced by an elapsed hold timer counting up. A restart-count badge (`R:2`) appears in the corner if watchdog reboots have occurred this session.

**Rationale:** Answers the three most important field questions in a single glance: is each port alive, is it actively receiving, and has anything crashed recently. The LIVE→HELD→IDLE state progression surfaces the hold-time feature that is otherwise completely invisible. The PoE instability makes the restart badge especially valuable on this specific hardware.

**Downsides:** Two ports × two states (normal vs. hold) means the display layout switches between two string formats — small but real implementation complexity. Restart count requires writing/reading `/tmp/wdcount` across restarts.

**Confidence:** 92%
**Complexity:** Low-Medium
**Status:** Unexplored

---

### 2. Fault Override Policy *(mandatory cross-cutting layer)*
**Description:** Not a screen — a priority rule. When any fault is detected (DHCP failure, total source loss on both ports, or network link down), the display immediately overrides everything with a full-width fault message: `DHCP FAILED` / `NO DMX P0+P1` / `NO LINK`. Clears automatically when the fault resolves.

**Rationale:** In a show environment the display is only consulted when something is wrong. A display that requires knowing "which rotation screen has fault info?" is useless under show conditions. Fault override makes the LCD an active alarm, not a passive dashboard. Must ship alongside any content screens.

**Downsides:** Must be implemented before content screens to avoid fault info being buried. Adds a priority layer to the display update logic.

**Confidence:** 95%
**Complexity:** Low
**Status:** Unexplored

---

### 3. Network Identity Screen *(baseline/idle screen)*
**Description:** When both ports are IDLE (no sources seen since boot), shows: Line 1 = hostname + IP (`SN110 192.168.2.231`), Line 2 = protocol + universe routing (`sACN U1→P0 U2→P1`). Also shown for a few seconds on startup before switching to the health screen.

**Rationale:** The first question a tech asks when plugging in a node is "what's the IP?" The second is "what universe is it on?" This screen answers both without any tool, exactly when you need it — before sources attach and when idling.

**Downsides:** During an active show with live sources, this screen may never appear. Operators needing the IP mid-show have no easy access to it.

**Confidence:** 85%
**Complexity:** Low
**Status:** Unexplored

---

### 4. Source-Lost Event Ticker *(transition state display)*
**Description:** When a previously-LIVE source drops out, the display shows: Line 1 = `SRC LOST P0`, Line 2 = `HELD 01:23` (counting up). When source returns, shows `SRC OK P0` for 3 seconds before resuming normal status. Triggered on edge transitions (LIVE→LOST), not a continuously rotating screen.

**Rationale:** The difference between a 2-second console glitch and a 2-minute crash is not visible with a binary LIVE/IDLE display. The elapsed counter lets an operator gauge urgency. Particularly useful during hold-time — the operator sees the DMX is frozen and counts how long it's been without a tool.

**Downsides:** Requires per-port edge detection and a persistent timestamp of when the drop occurred. Partially overlaps with hold timer in #1 — could be merged into one implementation.

**Confidence:** 80%
**Complexity:** Low-Medium
**Status:** Unexplored

---

### 5. Firmware Version Boot Splash *(startup only)*
**Description:** On daemon start (including watchdog restarts), display `sn110dmx v0.2.0` / `OPEN FW` for 2 seconds, then switch to the health screen. A startup-only one-shot — not a carousel entry.

**Rationale:** Every watchdog restart fires this banner, making the open-source daemon visually distinct from the original Strand binary. In a rack of mixed nodes, this is the only visual differentiator without SSH access.

**Downsides:** Adds a startup-phase state machine (or a 2s blocking sleep — acceptable at init). Low value in an all-open-source deployment.

**Confidence:** 65%
**Complexity:** Low
**Status:** Unexplored

---

## Rejection Summary

| # | Idea | Reason Rejected |
|---|------|-----------------|
| 1 | Protocol/universe routing (standalone) | Config echo, never changes — merged into #3 |
| 2 | DMX slot monitor | Requires pre-configuration nobody does under pressure |
| 3 | Session health (dedicated screen) | Too much on one screen — wdcount badge merged into #1 |
| 4 | Source IP screen | Raw IP unreadable to operators; "NO SRC" merged into #1 |
| 5 | Rotating carousel | Actively hostile in rushed environments — operators can't wait |
| 6 | CGI display mode selector | Premature generalization; implement fixed layout first |
| 7 | Universe conflict warning | Multi-source tracking table is heavyweight for marginal value |
| 8 | Loopback self-test indicator | Topology-specific; false failures in single-direction use |
| 9 | Frame rate history bar | Wrong tool for the environment |
| 10 | Showtime event log (broad) | Too heavy for v1; scoped to source-lost events only → becomes #4 |
| 11 | Source priority winner | Only relevant in multi-console edge cases |
| 12 | /tmp/dbg.txt display pipeline | Wrong abstraction level; daemon owns the display directly |
| 13 | Hostname nameplate (standalone) | Will never be configured; use existing hostname instead → merged into #3 |

## Session Log
- 2026-04-04: Initial ideation — 40 candidates generated across 5 frames, 18 unique after dedupe, 5 survivors
