---
title: "LCD Driver On-Device Verification: Character Filtering, Contrast Safety, and CP437 Glyph Availability"
category: reverse-engineering
date_solved: 2026-04-23
date_documented: 2026-04-23
tags:
  - lcd
  - ioctl
  - cp437
  - character-filter
  - partial-write
  - uClinux
  - kernel-driver
  - disassembly
  - arm7tdmi
  - ns7520
  - contrast
  - on-device-testing
severity: informational
component: lcd-driver
symptoms:
  - "Disassembly predicted 0x08-0x1F characters would be replaced with space — needed hardware verification"
  - "Contrast and backlight ioctl commands (17, 18) identified in disassembly but never tested on device"
  - "Port indicator bytes 0x1A/0x1B in heartbeat code had unknown display behavior"
related_issues:
  - "lcd-driver-ioctl7-cursor-positioned-nul-terminated-write.md — companion doc covering cmd 7 API contract"
  - "auto memory lcd-driver-model.md — updated with corrected filtering and contrast safety"
---

## Problem

ARM disassembly of the SN110 kernel LCD driver (2026-04-22) produced a complete ioctl command table and identified `lcd_write_line` character handling logic. However, several predictions from static analysis needed hardware verification:

1. Would characters 0x08-0x1F be replaced with space, as the disassembly suggested?
2. What do ioctl cmds 17 (contrast) and 18 (backlight) actually control?
3. Are the port indicator bytes 0x1A/0x1B in the heartbeat code vestigial or displayable?
4. What is the safe input range for contrast values?

## Investigation

1. **Built multi-mode test program** (`tests/lcd_verify.c`) — cross-compiled as BFLT for ARM7TDMI/uClinux. Six test modes: `filter` (character display), `contrast`, `backlight`, `pixel`, `width`, `allchars`.

2. **Character filter test**: Wrote each byte 0x08-0x1F to the LCD via ioctl cmd 7 with hex labels (format `XX=?`), observing which displayed a glyph vs. blank.

3. **Contrast test**: Swept ioctl cmd 17 with values 0-63, then tested negative values (-1, -32, -64).

4. **Compared results against disassembly predictions** and updated documentation with corrections.

## Root Cause

The disassembly-based prediction for character filtering was **partially wrong**. The kernel's `lcd_write_line` function handles characters as follows:

| Byte value | Behavior | Source |
|-----------|----------|--------|
| `0x00` | NUL - terminates write | Disassembly + confirmed |
| `0x01`-`0x08` | CP437 glyphs displayed | **On-device (corrects disassembly)** |
| `0x09` | Blank (no glyph) | **On-device** |
| `0x0A` | Newline - pads line + advances row | Disassembly + confirmed |
| `0x0B`-`0x0D` | Blank (no glyph) | **On-device** |
| `0x0E`-`0x1F` | CP437 glyphs displayed | **On-device (corrects disassembly)** |
| `0x20`-`0x7F` | Standard ASCII | Disassembly + confirmed |
| `0x80`+ | Replaced with space | Disassembly + confirmed |

The original disassembly predicted **all** 0x08-0x1F would be replaced with space. In reality, only 4 specific codes (0x09, 0x0B, 0x0C, 0x0D) display as blanks. The rest render their CP437 graphical glyphs — arrows, triangles, musical notes, etc.

The contrast command (cmd 17) accepts values 0-63 as expected, but the signed-byte clamp to 0x3F **does not protect against negative input**. Passing -32 caused an immediate device crash and reboot.

## Solution

### Displayable graphical characters (0x08-0x1F)

These CP437 glyphs are available via standard ioctl cmd 7 writes:

| Char | Hex  | CP437 name                | Notes |
|------|------|---------------------------|-------|
| `\x08` | 0x08 | Inverse bullet            | |
|        | 0x09 | **(blank)**               | |
|        | 0x0B | **(blank)**               | |
|        | 0x0C | **(blank)**               | |
|        | 0x0D | **(blank)**               | |
| `\x0E` | 0x0E | Beamed eighth notes       | |
| `\x0F` | 0x0F | Sun with rays             | |
| `\x10` | 0x10 | Right-pointing triangle   | |
| `\x11` | 0x11 | Left-pointing triangle    | |
| `\x12` | 0x12 | Up-down arrow             | |
| `\x13` | 0x13 | Double exclamation mark   | |
| `\x14` | 0x14 | Pilcrow / paragraph sign  | |
| `\x15` | 0x15 | Section sign              | |
| `\x16` | 0x16 | Horizontal bar            | Baseline-aligned (rows 6-7) |
| `\x17` | 0x17 | Up-down arrow with base   | |
| `\x18` | 0x18 | Up arrow                  | |
| `\x19` | 0x19 | Down arrow                | |
| `\x1A` | 0x1A | Right arrow               | Used by heartbeat as port indicator |
| `\x1B` | 0x1B | Left arrow                | Used by heartbeat as port indicator |
| `\x1C` | 0x1C | Right angle               | |
| `\x1D` | 0x1D | Left-right arrow          | |
| `\x1E` | 0x1E | Up-pointing triangle      | |
| `\x1F` | 0x1F | Down-pointing triangle    | |

### Contrast safety

```c
/* SAFE: 0-63 only */
void lcd_set_contrast(int value) {
    int fd, arg;
    if (value < 0 || value > 63) return;  /* CRITICAL: negative values crash device */
    fd = open("/dev/lcd0", O_RDWR);
    arg = value;
    ioctl(fd, LCD_CMD17, &arg);
    close(fd);
}
```

Values above ~32 make the display unreadable (black-on-black). Practical range is approximately 0-30.

### Port indicators confirmed

The heartbeat bytes 0x1A and 0x1B are **not vestigial** — they display as right arrow (→) and left arrow (←) CP437 glyphs, serving as port direction indicators.

## Key Findings

- **Character filtering prediction was wrong**: disassembly predicted 0x08-0x1F would all be filtered to space. On-device testing showed 20 of 24 chars in this range display as CP437 glyphs.
- **Only 4 blanks**: 0x09, 0x0B, 0x0C, 0x0D display nothing. These correspond to ASCII control codes (HT, VT, FF, CR).
- **Contrast range**: cmd 17 accepts 0-63, but **negative values crash the device** (confirmed: -32 caused immediate reboot).
- **Partial write isolation confirmed**: the open-ioctl-close pattern with cursor positioning allows independent screen regions to be updated by separate processes with no interference.
- **Available glyph count**: combined with 0x01-0x07 (card suits) and 0x20-0x7E (ASCII), 113 distinct displayable characters are available through the character path alone.
- **Display geometry**: character cells are 6 pixels wide with no inter-character padding. Total display: 96×32 pixels (16 chars × 6px, 4 rows × 8px). Confirmed by width test (2026-04-24).
- **Character and pixel buffers have separate clear commands**: cmd 2 (`lcd_clear`) clears only the character buffer; cmd 8 (reset) clears only the pixel framebuffer. Neither affects the other. Use both for a full screen wipe.
- **Pixel commands produce visible output**: cmd 15 draws horizontal patterns, cmd 14 draws filled rectangles, cmd 16 draws vertical patterns. All write additively. Exact argument layouts are partially understood (see `docs/lcd-partial-write.md`).
- **ASCII 0x20-0x7E all render correctly**: verified against standard ASCII table (2026-04-24). Tilde (0x7E) renders only in top pixel rows — correct for CP437 but subtle on the physical display.

## Prevention Strategies

- **Treat disassembly as hypothesis, not ground truth.** Mark every disassembly-derived claim as `[PREDICTED]` until hardware confirms it. ARM mixed-mode code and compiler optimizations can mislead static analysis.
- **Test character/value ranges exhaustively.** Blanket assumptions like "all 0x08-0x1F are filtered" collapse when the actual behavior is sparse. Sweep the full range and record each result individually.
- **Anticipate signed/unsigned mismatch at ioctl boundaries.** The signed-byte clamp to 0x3F only works for positive values. Always clamp inputs in userspace before passing to ioctl.
- **Have a recovery path ready before testing new ioctls.** Confirm BOOTP/TFTP is staged before testing any command that hasn't been previously exercised on hardware.
- **Log test results to a file with fflush after each step.** If a test crashes the device, the last flushed entry is the only forensic evidence.

## Testing

On-device test program: `tests/lcd_verify.c` (build with `make docker-lcd-verify-bflt`)

| Mode | What it tests | Status |
|------|--------------|--------|
| `filter` | Writes chars 0x08-0x1F with hex labels | Verified 2026-04-23 |
| `contrast` | Sweeps cmd 17 values 0-63 and negative | Verified 2026-04-23 |
| `backlight` | Tests cmd 18 | Verified 2026-04-24 — binary on/off (0=off, nonzero=on), no dimming |
| `pixel` | Tests cmds 14, 15, 16 argument layouts | Verified 2026-04-24 — all three cmds produce visible output; pixel FB not cleared by lcd_clear() |
| `width` | Measures character cell width | Verified 2026-04-24 — 6px cells, no padding |
| `allchars` | Displays full 0x20-0x7E range | Verified 2026-04-24 — all match standard ASCII |

## Cross-References

- [docs/lcd-partial-write.md](/docs/lcd-partial-write.md) — Authoritative ioctl command table and kernel address reference (updated with these findings)
- [lcd-driver-ioctl7-cursor-positioned-nul-terminated-write.md](lcd-driver-ioctl7-cursor-positioned-nul-terminated-write.md) — Companion doc: ioctl cmd 7 API contract and partial write mechanism
- [docs/solutions/reverse-engineering/strand-chk-firmware-checksum-algorithm.md](/docs/solutions/reverse-engineering/strand-chk-firmware-checksum-algorithm.md) — Source of the kernel image used for disassembly
- [docs/solutions/integration-issues/dmx-kernel-driver-mode-values-and-close-triggered-tx.md](/docs/solutions/integration-issues/dmx-kernel-driver-mode-values-and-close-triggered-tx.md) — Parallel ARM disassembly methodology
