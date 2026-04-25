# LCD Partial Write Mechanism

How the factory firmware (lxnetdmx) updates a blinking heart at
line 1 positions 12-15 without disturbing the rest of the display.

## Summary

The kernel LCD driver supports an ioctl that combines **cursor positioning**
with a **NUL-terminated string write**. Characters are written starting at
the cursor position and stop at the first NUL byte. Positions before the
cursor and after the NUL are never touched.

## Kernel LCD driver ioctl interface

Device: `/dev/lcd0`

### Ioctl commands (discovered via disassembly of kernel at 0x6B9CC)

| Command | Encoding | Description |
|---------|----------|-------------|
| `_IOW('l', 1, 4)` = `0x40046C01` | 4-byte arg | **Set write mode**: arg[0] = mode (0=OR, 1=XOR, 3=AND) |
| `0x6C02` | no arg | **Clear screen**: fills buffer with spaces, resets cursor to 0 |
| `_IOW('l', 4, 4)` = `0x40046C04` | 4-byte arg | **Set cursor**: arg[0]=column, arg[1]=row |
| `_IOW('l', 5, 4)` = `0x40046C05` | 4-byte arg | Unknown (calls cursor function variant) |
| `_IOW('l', 6, 44)` = `0x402C6C06` | 44-byte arg | Write with 2-byte header + 42 bytes content |
| `_IOW('l', 7, 48)` = `0x40306C07` | 48-byte arg | **Cursor + write** (used by heartbeat) |
| `_IOW('l', 8, 4)` = `0x40046C08` | 4-byte arg | **Clear pixel framebuffer** (memsets 500 bytes to 0; does not touch character buffer) |
| `_IOW('l', 14, 12)` = `0x400C6C0E` | 12-byte arg | **Pixel rectangle draw** — writes pixel data directly to framebuffer (6 halfword args: pixel data byte, x_start, y_start, x_end, y_end, flags) |
| `_IOW('l', 15, 8)` = `0x40086C0F` | 8-byte arg | **Pixel column draw** — sets/clears individual pixels in framebuffer |
| `_IOW('l', 16, 8)` = `0x40086C10` | 8-byte arg | **Pixel column draw variant** — same structure as cmd 15 |
| `_IOW('l', 17, 4)` = `0x40046C11` | 1-byte arg (signed) | **Set contrast** — value clamped to 0x3F (63). **WARNING: negative values crash the device** (confirmed on hardware 2026-04-23; use 0-63 only) |
| `_IOW('l', 18, 4)` = `0x40046C12` | 1-byte arg | **Set backlight**: 0=off, any nonzero=on (no dimming; confirmed on device 2026-04-24) |

### Ioctl command 7 — cursor + write (48-byte buffer)

This is the command lxnetdmx uses for the heartbeat.

```
Offset  Purpose
------  -------
0-1     Unused (padding; not read by kernel driver)
2       Column (0-15)
3       Row (0-3: line 1-4)
4-47    NUL-terminated string, written starting at (col, row)
```

The kernel handler (at 0x6BC48) does two things:
1. Calls the cursor setter with `buf[2]` (column) and `buf[3]` (row)
2. Calls `lcd_write_line(&buf[4])` which writes characters into the
   display buffer starting at the cursor position

`lcd_write_line` (at 0x6C6E0) processes the string byte by byte:
- **NUL (0x00)** terminates the write — nothing at or after the NUL
  position is modified. This is the key to partial updates.
- **Newline (0x0A)** pads the current line with spaces and advances
  to the next line.
- **All other bytes (0x01-0x7F)** are written directly to the display.
  The LCD controller renders them using its built-in CP437 ROM.
- **High-bit chars (0x80+)** are replaced with space.

**On-device testing (2026-04-23) disproved the earlier disassembly
prediction that control chars 0x08-0x1F would be filtered to space.**
Most 0x01-0x1F pass through and display as CP437 graphical glyphs.
Only 0x00 (NUL), 0x0A (newline), and four blanks (0x09, 0x0B, 0x0C,
0x0D) receive special handling.

### Character table (CP437, confirmed on device)

The LCD controller has the IBM Code Page 437 character set in ROM.
All characters 0x20-0x7E verified against standard ASCII on device
(2026-04-24). The kernel driver also includes a software font table
at offset 0x7B056 (128 entries, column-major) used by the OR/XOR/AND
write modes.

Note: char 0x7E (tilde) renders only in the top pixel rows of the
cell — correct for CP437 but visually subtle on the physical display.

```
Char 0 (0x00) — NUL (cannot be written; terminates ioctl cmd 7)
  (empty)

Char 1 (0x01) — ☺ smiley (outline)    [6 cols: 7E 99 A5 A5 81 7E]
  .####.
  #....#
  #.##.#
  ##...#
  ##...#
  #.##.#
  #....#
  .####.

Char 2 (0x02) — ☻ smiley (filled)     [6 cols: 7E FF DB DB FF 7E]
  .####.
  ######
  ##..##
  ######
  ######
  ##..##
  ######
  .####.

Char 3 (0x03) — ♥ heart               [5 cols: 1C 3E 7C 3E 1C]
  .....
  .#.#.
  #####
  #####
  #####
  .###.
  ..#..
  .....

Char 4 (0x04) — ♦ diamond             [5 cols: 08 1C 3E 1C 08]
  .....
  ..#..
  .###.
  #####
  .###.
  ..#..
  .....
  .....

Char 5 (0x05) — ♣ club                [6 cols: 18 5E 67 67 5E 18]
  ..##..
  .####.
  .####.
  ##..##
  ##..##
  ..##..
  .####.
  ......

Char 6 (0x06) — ♠ spade               [6 cols: 0C 4E 7F 7F 4E 0C]
  ..##..
  .####.
  ######
  ######
  ..##..
  ..##..
  .####.
  ......

Char 7 (0x07) — • bullet              [5 cols: 00 18 3C 3C 18]
  .....
  .....
  ..##.
  .####
  .####
  ..##.
  .....
  .....
```

Note: Characters 1, 2, 5, 6 use 6 columns; characters 3, 4, 7 use 5.
The physical display cell width determines how many columns are visible.

### Graphical characters 0x08-0x1F (confirmed on device 2026-04-23)

Most of these display as their CP437 glyphs. Four display as blanks
(marked below). All were verified via `tests/lcd_verify.c` filter mode.

| Char | Hex  | CP437 name                | Notes |
|------|------|---------------------------|-------|
| ◘    | 0x08 | Inverse bullet            | |
|      | 0x09 | **(blank)**                | No glyph displayed |
|      | 0x0A | Newline                   | Special: pads line + advances row |
|      | 0x0B | **(blank)**                | No glyph displayed |
|      | 0x0C | **(blank)**                | No glyph displayed |
|      | 0x0D | **(blank)**                | No glyph displayed |
| ♫    | 0x0E | Beamed eighth notes       | |
| ☼    | 0x0F | Sun with rays             | |
| ►    | 0x10 | Right-pointing triangle   | |
| ◄    | 0x11 | Left-pointing triangle    | |
| ↕    | 0x12 | Up-down arrow             | |
| ‼    | 0x13 | Double exclamation mark   | |
| ¶    | 0x14 | Pilcrow / paragraph sign  | |
| §    | 0x15 | Section sign              | |
| ▬    | 0x16 | Horizontal bar            | Baseline-aligned (rows 6-7) |
| ↨    | 0x17 | Up-down arrow with base   | |
| ↑    | 0x18 | Up arrow                  | |
| ↓    | 0x19 | Down arrow                | |
| →    | 0x1A | Right arrow               | Used by heartbeat as port indicator |
| ←    | 0x1B | Left arrow                | Used by heartbeat as port indicator |
| ∟    | 0x1C | Right angle               | |
| ↔    | 0x1D | Left-right arrow          | |
| ▲    | 0x1E | Up-pointing triangle      | |
| ▼    | 0x1F | Down-pointing triangle    | |

### Write modes (OR / XOR / AND)

The driver stores a write mode at device structure offset 0x61E,
settable via `_IOW('l', 1, 4)`. The mode values are:

- 0 = OR
- 1 = XOR
- 3 = AND

These modes exist in the driver but are **not used by the heartbeat**.
The partial-update mechanism relies entirely on cursor positioning +
NUL termination. The write modes operate on the pixel framebuffer
using the font table for character-to-pixel conversion (see below).

### Cursor position persistence

The cursor position is stored at device structure offset 0x61F.
It persists across `close()`/`open()` cycles — it is per-device
kernel state, not per-fd. The ioctl command 7 sets the cursor
explicitly each time, so this persistence doesn't affect its behavior.

## How lxnetdmx uses this

The heartbeat function (lxnetdmx file offset 0xF77C) runs on a
periodic timer. Each cycle:

```
1. open("/dev/lcd0", O_RDWR)

2. Prepare 48-byte buffer:
     buf[2]  = 12          // column 12 (0-indexed)
     buf[3]  = 0           // row 0 (line 1)
     buf[8]  = 0x00        // NUL terminator — always set

3. Toggle content:
     Heart ON:                    Heart OFF:
       buf[4] = 0x03  (heart)      buf[4] = ' '
       buf[5] = 0x03 or ' '        buf[5] = ' '
       buf[6] = port0 indicator    buf[6] = ' '
       buf[7] = port1 indicator    buf[7] = ' '

4. ioctl(fd, 0x40306C07, buf)

5. close(fd)
```

The heart glyph is CGRAM character 3 (byte value 0x03).

Port indicators (when present):
- 0x1A (→ right arrow) and 0x1B (← left arrow) appear in the heartbeat
  buffer as port direction indicators. On-device testing confirmed these
  display as CP437 arrow glyphs despite being in the 0x08-0x1F range.

### What the display sees

```
Position:  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
Line 1:   [---- untouched by heartbeat ----] [written]
                                              ^cursor
```

The NUL at buf[8] stops the write after 4 characters (positions 12-15).
Positions 0-11 on line 1 and all of lines 2-4 are never reached.

## Driver architecture: pixel framebuffer + font table

The kernel LCD driver does **software rendering**. It maintains two
key data structures:

### Pixel framebuffer (kernel address 0x001D1240)

A bit-addressed framebuffer for the physical display. Layout:

```
byte_offset = row + (col / 8) * 100
bit_position = col & 7
```

Each 8-column group has a stride of 100 bytes.

**Display geometry (confirmed on device 2026-04-24):** Character cells
are **6 pixels wide** (no inter-character padding). The display is
**96 pixel columns × 32 pixel rows** (16 chars × 6px, 4 rows × 8px).

Ioctl commands 14, 15, 16 write directly to this framebuffer,
**bypassing all character filtering**. This is the path for displaying
arbitrary pixel patterns (arrows, box-drawing, custom icons).

**The character and pixel buffers have separate clear mechanisms
(confirmed on device 2026-04-24):**

| Command | Character buffer | Pixel framebuffer |
|---------|-----------------|-------------------|
| Cmd 2 (`lcd_clear`, 0x6C02) | Clears (fills with spaces, resets cursor) | Not touched |
| Cmd 8 (reset, 0x40046C08) | Not touched | Clears (memsets 500 bytes to 0) |

To fully blank the display, issue both cmd 2 and cmd 8.

### Pixel command behavior (confirmed on device 2026-04-24)

Argument layouts are partially understood. On-device results:

| Command | Observed behavior |
|---------|-------------------|
| Cmd 15 | Draws horizontal pixel patterns. `(0xFF, 0, 0, 31)` drew a 32px horizontal line at the top of the display. |
| Cmd 14 | Draws filled rectangles. `(0, 0, 7, 4, 0xFF)` drew a box in the upper left. `(0, 16, 7, 20, 0xAA)` drew 3 horizontal stripes (alternating rows) partway down. |
| Cmd 16 | Draws vertical pixel patterns. `(0xFF, 0, 0, 31)` drew a vertical bar along the left edge. |

All three commands write additively — there is no observed erase/XOR
behavior by default (write mode may affect this; not yet tested).

### Font table (kernel offset 0x7B056)

A 128-entry (0x00-0x7F) read-only font table in column-major format.
Each entry is 8 bytes: 5-6 data bytes (one per column, bit 0 = top
row) + zero padding. The first 32 entries (0x00-0x1F) contain the
IBM Code Page 437 graphical characters. See the glyph table above
for characters 0x00-0x07.

The character write path (`lcd_write_line` via cmds 6, 7) uses this
table to convert character codes to pixels before writing to the
framebuffer. The OR/XOR/AND write modes perform pixel-level bitwise
operations using font table lookups.

### Character filtering vs. pixel bypass

| Path | Commands | Filtering | Use case |
|------|----------|-----------|----------|
| Character | 6, 7 | 0x00=NUL terminates, 0x0A=newline, 0x80+=space; most 0x01-0x7F pass through | Normal text |
| Pixel | 14, 15, 16 | None — raw pixel data | Custom graphics |
| Hardware | 17, 18 | N/A | Contrast, backlight |

**On-device testing (2026-04-23) corrected the disassembly prediction:**
the original analysis predicted all 0x08-0x1F would be replaced with
space, but most display as CP437 glyphs. Only 0x09, 0x0B, 0x0C, 0x0D
display as blanks. See the graphical characters table above for details.

### Device structure (kernel address 0x001D1838)

Per-device state within the LCD driver:

| Offset | Size | Purpose |
|--------|------|---------|
| 0x61D | 1 | Dirty/refresh flag (set by every ioctl) |
| 0x61E | 1 | Write mode (0=OR, 1=XOR, 3=AND) |
| 0x61F | 1 | Cursor position |
| 0x620 | 1 | Contrast (0-63, set by cmd 17) |
| 0x621 | 1 | Backlight (set by cmd 18) |

## Methodology

Findings derived from:
- ARM disassembly (Capstone) of the kernel LCD driver in
  `dump/official-firmware/sn110-2_6_11.chk` (kernel offsets 0x6B730-0x6C880)
- ARM disassembly of the heartbeat function in
  `dump/firmware/usr/bin/lxnetdmx` (file offset 0xF77C-0xF900)
- Cross-referencing with empirical results from `tests/lcd_probe.c`
- On-device hardware verification via `tests/lcd_verify.c` (2026-04-23):
  character filtering, contrast range, graphical character display

Key addresses in the kernel image:
- LCD init: 0x6B730
- LCD open: 0x6B7C4
- LCD close: 0x6B864
- LCD write: 0x6B880
- LCD ioctl: 0x6B9CC
- lcd_write_line: 0x6C6E0
- set_mode: 0x6C58C
- set_cursor: 0x6C634
- clear_screen: 0x6C5D0
- reset: 0x6C854
- pixel_rect (cmd 14): 0x6C88C
- pixel_col (cmd 15): 0x6C97C
- pixel_col_v2 (cmd 16): 0x6CA30
- set_contrast (cmd 17): 0x6CAE8
- set_backlight (cmd 18): 0x6CB38
- Font table: 0x7B056 (128 entries × 8 bytes, column-major, CP437)
- Mode/status strings: 0x7B4A0
- Pixel framebuffer: 0x1D1240 (runtime BSS)
- Device structure: 0x1D1838 (runtime BSS)
