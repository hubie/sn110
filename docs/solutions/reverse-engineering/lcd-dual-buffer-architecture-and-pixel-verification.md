---
title: "SN110 LCD Driver: Dual-Buffer Architecture, Pixel Commands, and Display Geometry"
category: reverse-engineering
date_solved: 2026-04-24
date_documented: 2026-04-24
tags:
  - lcd
  - ioctl
  - pixel-framebuffer
  - hardware-verification
  - uClinux
  - ns7520
  - display-geometry
  - backlight
  - ascii
severity: informational
component: SN110 LCD driver (/dev/lcd0)
symptoms:
  - "Pixel drawings from cmds 14/15/16 survived lcd_clear() and persisted across program restarts"
  - "Display dimensions (pixel width per character cell) unknown — needed for pixel coordinate math"
  - "Backlight and pixel command behaviors predicted from disassembly but never tested on device"
related_issues:
  - "lcd-driver-partial-update-verification-and-safety.md — companion doc covering character filtering, contrast safety, CP437 glyph availability"
  - "lcd-driver-ioctl7-cursor-positioned-nul-terminated-write.md — ioctl cmd 7 API contract"
---

## Problem

On-device testing of the LCD character filter (2026-04-23) revealed that pixel
drawings made via cmds 14/15/16 were **not cleared** by `lcd_clear()` (cmd 2).
The drawings persisted across clear operations, character writes, and even
across separate program invocations. This raised three questions:

1. Is there a mechanism to clear the pixel framebuffer?
2. What are the exact display dimensions (pixels per character cell)?
3. What do the backlight and pixel commands actually do on hardware?

## Investigation

1. **Pixel test (cmds 14/15/16)**: Ran `lcd_verify pixel` with multiple argument
   layout hypotheses. Cmd 15 with `(0xFF, 0, 0, 31)` drew a horizontal line
   ~1/3 of screen width (~32px). Since 32/96 = 1/3, this established the display
   as 96px wide. Cmd 14 drew filled rectangles; 0xAA pattern produced alternating
   stripes confirming pixel-level fill control. Cmd 16 drew vertical bars.
   Pixel drawings accumulated additively and were never cleared by the test's
   `lcd_clear()` calls between steps.

2. **Width test**: Filled display rows with 5-column chars (heart, 0x03) vs
   6-column chars (0x01). No clipping on 6-column chars. No inter-character
   padding — 6-column chars butted directly together. Confirmed character cell
   width = **6 pixels**. Total display: **96×32 pixels** (16×6, 4×8).

3. **Allchars test**: Displayed all 0x20–0x7E. All matched standard ASCII.
   Tilde (0x7E) renders only in the top pixel rows of the cell — correct for
   CP437 but visually subtle on the physical display.

4. **Reset test (cmd 8)**: Hypothesized that cmd 8 ("memsets 500 bytes to 0")
   targets the pixel framebuffer. Drew pixel patterns + text, then fired cmd 8.
   Result: **pixel data cleared, text remained**. Wrote new text afterward — it
   appeared alongside the surviving text from before the reset.

5. **Backlight test (cmd 18)**: Swept values 0, 1, 32, 64, 128, 255. Value 0 =
   off, any nonzero = on. No brightness variation — binary on/off only.

## Root Cause

The LCD driver maintains **two independent display layers**:

| Layer | Cleared by | Content |
|-------|-----------|---------|
| Character buffer | Cmd 2 (`lcd_clear`) | Text from `write()` and cmd 6/7 |
| Pixel framebuffer | Cmd 8 (reset) | Raw pixels from cmds 14/15/16 |

Neither clear operation affects the other layer. Both layers render simultaneously
on the physical display — character text appears visually on top of pixel data.
The pixel framebuffer lives in kernel BSS (address 0x001D1240) and persists across
`close()`/`open()` cycles and process exits.

This dual-buffer architecture is the root cause of the "pixel artifacts survive
clear" behavior. Calling `lcd_clear()` alone is insufficient when pixel content
has been drawn.

## Solution

### Full screen wipe

```c
#define LCD_CLEAR  0x6C02
#define LCD_CMD8   ((1U << 30) | (4 << 16) | ('l' << 8) | 8)

void lcd_full_clear(void) {
    char buf[4] = {0};
    int fd = open("/dev/lcd0", O_RDWR);
    ioctl(fd, LCD_CMD8, buf);    /* clear pixel framebuffer */
    ioctl(fd, LCD_CLEAR, 0);     /* clear character buffer + reset cursor */
    close(fd);
}
```

### Display geometry constants

```c
#define LCD_CHARS_COLS  16
#define LCD_CHARS_ROWS   4
#define LCD_CHAR_W_PX    6   /* no inter-character padding */
#define LCD_CHAR_H_PX    8
#define LCD_WIDTH_PX    96   /* 16 × 6 */
#define LCD_HEIGHT_PX   32   /* 4 × 8 */
```

### Pixel command summary

| Command | Behavior | Example |
|---------|----------|---------|
| Cmd 14 | Filled rectangle | `(0, 0, 7, 4, 0xFF)` → box in upper left |
| Cmd 15 | Horizontal pattern | `(0xFF, 0, 0, 31)` → 32px line across top |
| Cmd 16 | Vertical pattern | `(0xFF, 0, 0, 31)` → bar down left edge |

All three write additively to the pixel framebuffer. Pattern byte controls
fill: 0xFF = solid, 0xAA = alternating stripes, 0x00 = erase.

### Backlight

```c
#define LCD_CMD18  ((1U << 30) | (4 << 16) | ('l' << 8) | 18)

void lcd_backlight(int on) {
    char buf[4] = {0};
    buf[0] = on ? 1 : 0;   /* 0=off, nonzero=on; no dimming */
    int fd = open("/dev/lcd0", O_RDWR);
    ioctl(fd, LCD_CMD18, buf);
    close(fd);
}
```

## Key Findings

- **Dual-buffer architecture**: character buffer (cmd 2) and pixel framebuffer
  (cmd 8) are independent. Full wipe requires both commands.
- **Display geometry**: 96×32 pixels. Character cells are 6px wide, 8px tall,
  with no inter-character padding.
- **Pixel framebuffer persists across process lifecycle**: drawings survive
  `close()`, `open()`, and program exit. Only cmd 8 clears them.
- **Backlight is binary**: cmd 18, 0=off, nonzero=on. No PWM dimming.
- **ASCII verified**: 0x20–0x7E all correct. Tilde (0x7E) renders in top rows only.
- **Char 0x01 appearance**: at 6×8 resolution, the CP437 outlined smiley (☺)
  reads as a copyright symbol (©) on the physical display.

## Prevention Strategies

- **Always clear both buffers on program init.** Issue cmd 8 then cmd 2 at
  startup. Never assume the display is clean — a previous process may have
  left pixel artifacts.
- **Register cleanup on exit.** Any process that draws pixels should issue
  cmd 8 before exiting (atexit handler or signal trap), since the pixel
  framebuffer outlives the process.
- **Hardcode `LCD_CHAR_W_PX = 6`** in any code converting character positions
  to pixel coordinates. Wrong assumptions (e.g., 8px cells) produce
  misaligned graphics silently.
- **Choose one layer per screen region.** Don't mix character writes and pixel
  draws in the same area — the two layers render simultaneously and
  coordination is manual.
- **Backlight is all-or-nothing.** Design power management as fully on or
  fully off; do not attempt dimming schemes through the driver.

## Testing

On-device test program: `tests/lcd_verify.c` (build with `make docker-lcd-verify-bflt`)

| Mode | What it tests | Status |
|------|--------------|--------|
| `pixel` | Cmds 14, 15, 16 argument layouts | Verified 2026-04-24 |
| `width` | Character cell width | Verified 2026-04-24 — 6px, no padding |
| `allchars` | Full 0x20–0x7E range | Verified 2026-04-24 — all correct |
| `reset` | Cmd 8 pixel framebuffer clear | Verified 2026-04-24 — clears pixels, not text |
| `backlight` | Cmd 18 | Verified 2026-04-24 — binary on/off |

## Cross-References

- [lcd-driver-partial-update-verification-and-safety.md](lcd-driver-partial-update-verification-and-safety.md) — Companion: character filtering, contrast safety, CP437 glyph availability
- [lcd-driver-ioctl7-cursor-positioned-nul-terminated-write.md](lcd-driver-ioctl7-cursor-positioned-nul-terminated-write.md) — Cmd 7 API contract and partial write mechanism
- [docs/lcd-partial-write.md](/docs/lcd-partial-write.md) — Authoritative ioctl command table and kernel address reference
