# Reverse Engineering Log

This page collects key reverse engineering findings about the SN110 hardware,
kernel drivers, and factory firmware. Each entry represents something that took
significant effort to discover and would be valuable for future contributors to
know.

---

## DMX Kernel Driver (2026-04-04)

**Finding**: The DMX driver's mode values were incorrectly documented in early
code. The correct mapping (from `dmxtst` binary disassembly):

| Mode | Value | Description |
|------|-------|-------------|
| OFF | 0 | Disable port |
| TX | 1 | Transmit |
| RX | 2 | Receive |
| RAW | 3 | Raw serial buffer |

**Critical discovery**: The driver uses **close-triggered transmission** -- the
DMX frame is transmitted on the wire when `close()` is called on the file
descriptor, not when `write()` is called. The factory `dmxtst` test mode
confirms this with an explicit `write → close → sleep → open` loop.

**Source**: ARM disassembly of `dmxtst` (6,072 bytes, bFLT v2).

**Impact**: The original firmware had TX and RX modes swapped, causing complete
DMX communication failure. Correcting the mode values and adopting the
open-write-close pattern fixed DMX loopback.

---

## LCD Driver ioctl Command Table (2026-04-22)

**Finding**: The kernel LCD driver exposes 12 ioctl commands, not just `write()`
and `clear`. The most important is **cmd 7** (`0x40306C07`): a 48-byte buffer
that atomically positions the cursor and writes a NUL-terminated string.

This is the mechanism the factory `lxnetdmx` uses for its heartbeat blink --
it writes to positions 12-15 of line 1 without disturbing the rest of the
display.

**Source**: ARM disassembly of the kernel LCD driver (offsets 0x6B730-0x6C880 in
the firmware image) and the `lxnetdmx` heartbeat function (offset 0xF77C).

See [LCD Display](../hardware/lcd-display.md) for the full command table.

---

## LCD Dual-Buffer Architecture (2026-04-24)

**Finding**: The LCD driver maintains two independent display layers -- a
character buffer and a pixel framebuffer. Each has its own clear command, and
neither clear affects the other layer. Pixel drawings persist across `close()`,
`open()`, and process exits.

**Source**: On-device testing with `tests/lcd_verify.c`.

**Impact**: Any code that draws pixels must issue cmd 8 to clean up. A character
clear (cmd 2) alone will leave pixel artifacts on screen.

---

## LCD Character Filtering (2026-04-23)

**Finding**: Disassembly predicted that all characters 0x08-0x1F would be
replaced with space by `lcd_write_line`. On-device testing proved this wrong --
only 4 of 24 characters in this range display as blanks (0x09, 0x0B, 0x0C,
0x0D). The rest render as their CP437 graphical glyphs, including the arrows
(0x1A, 0x1B) used by the factory heartbeat as port indicators.

**Source**: On-device character sweep via `tests/lcd_verify.c` filter mode.

**Lesson**: Treat disassembly as hypothesis, not ground truth. Test character
and value ranges exhaustively on hardware.

---

## LCD Contrast Safety (2026-04-23)

**Finding**: The contrast ioctl (cmd 17) accepts values 0-63, but negative
values **crash the device** immediately (confirmed: -32 caused instant reboot).
The signed-byte clamp to 0x3F only works for positive inputs.

**Source**: On-device testing. Recovery required power cycle.

**Impact**: All userspace code must clamp contrast to 0-63 before calling the
ioctl. The firmware enforces this in `config.c` (parse clamp) and `cgi_parse.h`
(form validation).

---

## LCD Display Geometry (2026-04-24)

**Finding**: Character cells are **6 pixels wide** with no inter-character
padding. The display is **96 x 32 pixels** (16 chars x 6px, 4 rows x 8px).

**Source**: On-device width test comparing 5-column and 6-column CP437
characters.

---

## .chk Firmware Checksum Algorithm (2026-03-24)

**Finding**: The `.chk` firmware format has a 4096-byte header (4-byte LE
checksum + 4092 bytes padding) followed by the payload. The checksum is computed
as:

```
(sum of all 32-bit LE words in the payload + 9) mod 2^32
```

The constant `+9` is specific to Strand's implementation.

**Source**: Binary analysis of two recovered `.chk` files (`sn110-2_6_11.chk`
and `sn110.26d`) from the Internet Archive Wayback Machine.

**Impact**: Enables creation of valid .chk files for the factory update
mechanism (`flashsw.sh`).

---

## Boot Process and CRC Validation (2026-03)

**Finding**: The bootloader validates the **compressed image blob** CRC, not the
filesystem contents within it. This means filesystem corruption from a config
overflow does not trigger BOOTP recovery -- the device enters a 3-second boot
loop instead.

**Source**: Investigation of a bricking incident where the device's filesystem
was corrupted by an oversized config write.

**Impact**: Led to implementing atomic config saves (write-to-tmp, rename) and
the comprehensive recovery documentation.

---

## Original Firmware Recovery (2026-03-24)

**Finding**: Two official Strand firmware images were recovered from the Internet
Archive Wayback Machine, where they had been archived from `strandlighting.com`
in 2003-2004. No other public source for these files exists.

| File | Version | Archive Date |
|------|---------|-------------|
| `sn110-2_6_11.chk` | V2.6.11 | 2004 |
| `sn110.26d` | V2.6.d | 2003 |

---

## Contributing Findings

If you discover something about the hardware or kernel behavior, please add it
to this log. Key things worth documenting:

- Kernel driver behavior that differs from standard Linux
- Hardware quirks (timing, register values, pin behavior)
- ioctl commands and their effects
- Anything that took you more than 30 minutes to figure out

!!! tip "Format"
    Each entry should include: what you found, how you found it (source/method),
    and why it matters (impact on the firmware or future development).
