# LCD Display

The SN110 has a 96x32 pixel monochrome LCD panel driven by a custom kernel
module. The display is organized as 16 characters x 4 rows using the CP437
character set, with additional pixel-level drawing commands.

## Display Specifications

| Parameter | Value |
|-----------|-------|
| **Device** | `/dev/lcd0` |
| **Resolution** | 96 x 32 pixels |
| **Characters** | 16 columns x 4 rows |
| **Character cell** | 6 x 8 pixels (no inter-character padding) |
| **Character set** | IBM Code Page 437 (ROM) |
| **Backlight** | Binary on/off (no dimming) |
| **Contrast** | 0-63 (values above ~32 are unreadable) |

## Driver Architecture

The kernel LCD driver maintains **two independent display layers** that render
simultaneously:

| Layer | Written by | Cleared by | Content |
|-------|-----------|-----------|---------|
| Character buffer | `write()`, cmds 6, 7 | Cmd 2 (`lcd_clear`) | Text characters |
| Pixel framebuffer | Cmds 14, 15, 16 | Cmd 8 (reset) | Raw pixel data |

!!! warning "Clearing the display"
    Neither clear command affects the other layer. To fully blank the display,
    issue **both** cmd 2 and cmd 8. Pixel drawings from cmds 14/15/16 will
    persist across character clears, process exits, and device close/reopen
    cycles.

## ioctl Command Table

| Command | Encoding | Size | Description |
|---------|----------|------|-------------|
| Cmd 1 | `_IOW('l', 1, 4)` = `0x40046C01` | 4 bytes | Set write mode: 0=OR, 1=XOR, 3=AND |
| Cmd 2 | `0x6C02` | none | Clear character buffer (fill with spaces, reset cursor) |
| Cmd 4 | `_IOW('l', 4, 4)` = `0x40046C04` | 4 bytes | Set cursor: arg[0]=column, arg[1]=row |
| Cmd 5 | `_IOW('l', 5, 4)` = `0x40046C05` | 4 bytes | Cursor function variant (unknown) |
| Cmd 6 | `_IOW('l', 6, 44)` = `0x402C6C06` | 44 bytes | Write with 2-byte header + 42 bytes content |
| **Cmd 7** | `_IOW('l', 7, 48)` = `0x40306C07` | **48 bytes** | **Cursor + write (primary update mechanism)** |
| Cmd 8 | `_IOW('l', 8, 4)` = `0x40046C08` | 4 bytes | Clear pixel framebuffer (memsets 500 bytes to 0) |
| Cmd 14 | `_IOW('l', 14, 12)` = `0x400C6C0E` | 12 bytes | Pixel rectangle draw (additive) |
| Cmd 15 | `_IOW('l', 15, 8)` = `0x40086C0F` | 8 bytes | Pixel horizontal pattern (additive) |
| Cmd 16 | `_IOW('l', 16, 8)` = `0x40086C10` | 8 bytes | Pixel vertical pattern (additive) |
| Cmd 17 | `_IOW('l', 17, 4)` = `0x40046C11` | 1 byte | Set contrast (0-63) |
| Cmd 18 | `_IOW('l', 18, 4)` = `0x40046C12` | 1 byte | Set backlight (0=off, nonzero=on) |

## Cmd 7: Cursor-Positioned Write

This is the primary mechanism for updating specific areas of the display. It
atomically positions the cursor and writes a NUL-terminated string in a single
syscall, avoiding cursor races between processes.

### Buffer Layout (48 bytes)

| Offset | Contents |
|--------|----------|
| 0-1 | Unused padding |
| 2 | Column (0-15) |
| 3 | Row (0-3, mapping to lines 1-4) |
| 4-47 | NUL-terminated string (max 43 characters) |

The kernel handler:

1. Sets cursor to `(buf[2], buf[3])`
2. Calls `lcd_write_line(&buf[4])` which writes characters until NUL

Characters before the cursor position and after the NUL terminator are
**never touched** -- this is how partial screen updates work.

### Usage Pattern

```c
#define LCD_CMD7  0x40306C07

void lcd_write_at(int col, int row, const char *text)
{
    char buf[48];
    int fd, len;

    memset(buf, 0, 48);
    buf[2] = (char)col;
    buf[3] = (char)row;
    for (len = 0; text[len] && len < 43; len++)
        buf[4 + len] = text[len];

    fd = open("/dev/lcd0", O_RDWR, 0);
    ioctl(fd, LCD_CMD7, buf);
    close(fd);
}
```

!!! tip "Open-ioctl-close pattern"
    Always open and close the device for each update. This matches the factory
    firmware pattern and avoids counter drift from mixing `write()` and
    `ioctl()` on the same file descriptor. The cursor and display state are
    per-device (kernel state), not per-fd.

## Character Handling

The `lcd_write_line` function processes characters byte-by-byte:

| Byte range | Behavior |
|-----------|----------|
| `0x00` | NUL -- terminates write immediately |
| `0x01`-`0x08` | CP437 graphical glyphs (displayed) |
| `0x09`, `0x0B`-`0x0D` | Blank (no glyph displayed) |
| `0x0A` | Newline -- pads rest of line with spaces, advances to next row |
| `0x0E`-`0x1F` | CP437 graphical glyphs (displayed) |
| `0x20`-`0x7F` | Standard ASCII characters |
| `0x80`+ | Replaced with space |

## Available Graphical Characters (0x01-0x07)

These characters have been verified on-device with exact pixel data extracted
from the kernel font table at offset 0x7B056 (column-major, 6 columns x 8 rows,
bit 0 = top row).

```
0x01 - Smiley (outline)          0x02 - Smiley (filled)
       [7E 99 A5 A5 81 7E]             [7E FF DB DB FF 7E]
       .####.                           .####.
       #....#                           ######
       #.##.#                           ##..##
       ##...#                           ######
       ##...#                           ######
       #.##.#                           ##..##
       #....#                           ######
       .####.                           .####.

0x03 - Heart                     0x04 - Diamond
       [1C 3E 7C 3E 1C]                [08 1C 3E 1C 08]
       .....                            .....
       .#.#.                            ..#..
       #####                            .###.
       #####                            #####
       #####                            .###.
       .###.                            ..#..
       ..#..                            .....
       .....                            .....

0x05 - Club                      0x06 - Spade
       [18 5E 67 67 5E 18]             [0C 4E 7F 7F 4E 0C]
       ..##..                           ..##..
       .####.                           .####.
       .####.                           ######
       ##..##                           ######
       ##..##                           ..##..
       ..##..                           ..##..
       .####.                           .####.
       ......                           ......

0x07 - Bullet
       [00 18 3C 3C 18]
       .....
       .....
       ..##.
       .####
       .####
       ..##.
       .....
       .....
```

Note: Characters 0x01, 0x02, 0x05, 0x06 are 6 columns wide. Characters 0x03,
0x04, 0x07 are 5 columns wide. At 6x8 resolution, 0x01 (outlined smiley) reads
as a copyright symbol on the physical display.

## Other Graphical Characters (0x08-0x1F)

These were verified on-device to display as their standard CP437 glyphs.

| Hex | CP437 Name | Notes |
|-----|------------|-------|
| 0x08 | Inverse bullet | |
| 0x0E | Beamed eighth notes | |
| 0x0F | Sun with rays | |
| 0x10 | Right-pointing triangle | |
| 0x11 | Left-pointing triangle | |
| 0x12 | Up-down arrow | |
| 0x13 | Double exclamation mark | |
| 0x14 | Pilcrow / paragraph sign | |
| 0x15 | Section sign | |
| 0x16 | Horizontal bar | Baseline-aligned (rows 6-7) |
| 0x17 | Up-down arrow with base | |
| 0x18 | Up arrow | |
| 0x19 | Down arrow | |
| 0x1A | Right arrow | Used as port TX indicator |
| 0x1B | Left arrow | Used as port RX indicator |
| 0x1C | Right angle | |
| 0x1D | Left-right arrow | |
| 0x1E | Up-pointing triangle | |
| 0x1F | Down-pointing triangle | |

**Blanks** (no glyph): 0x09, 0x0B, 0x0C, 0x0D

**Unreachable**: 0x00 (NUL always terminates the write)

**Total displayable characters**: 113 (0x01-0x08, 0x0E-0x1F, 0x20-0x7E)

Pixel-accurate representations for 0x08-0x1F can be extracted from the kernel
font table at offset 0x7B056 in the firmware image.

## Pixel Framebuffer Layout

```
byte_offset = row + (col / 8) * 100
bit_position = col & 7
```

Each 8-column group has a stride of 100 bytes. The framebuffer lives in
kernel BSS and persists across `close()`/`open()` cycles and process exits.

## Contrast and Backlight

### Contrast (Cmd 17)

Accepts values 0-63. The display is readable in approximately the 0-30 range;
values above ~32 produce black-on-black.

!!! danger "Negative contrast values crash the device"
    The kernel's signed-byte clamp to 0x3F does not protect against negative
    input. Passing -32 caused an immediate device crash and reboot during
    testing. **Always clamp to 0-63 in userspace before calling the ioctl.**

### Backlight (Cmd 18)

Binary on/off only -- no PWM dimming capability.

- `0` = backlight off
- Any nonzero value = backlight on

## Write Modes (OR / XOR / AND)

The driver supports three write modes (set via cmd 1), which perform pixel-level
bitwise operations using the font table:

| Value | Mode |
|-------|------|
| 0 | OR (default) |
| 1 | XOR |
| 3 | AND |

These modes are available but not commonly needed. The firmware's LCD module
uses the default OR mode for all text rendering.

## Kernel Driver Internals

### Key Addresses (in kernel image)

| Function | Address |
|----------|---------|
| LCD init | 0x6B730 |
| LCD open | 0x6B7C4 |
| LCD close | 0x6B864 |
| LCD write | 0x6B880 |
| LCD ioctl | 0x6B9CC |
| lcd_write_line | 0x6C6E0 |
| set_cursor | 0x6C634 |
| clear_screen | 0x6C5D0 |
| pixel_rect (cmd 14) | 0x6C88C |
| pixel_col (cmd 15) | 0x6C97C |
| pixel_col_v2 (cmd 16) | 0x6CA30 |
| set_contrast (cmd 17) | 0x6CAE8 |
| set_backlight (cmd 18) | 0x6CB38 |
| Font table | 0x7B056 |
| Pixel framebuffer | 0x1D1240 (BSS) |
| Device structure | 0x1D1838 (BSS) |

### Device Structure (at 0x1D1838)

| Offset | Size | Purpose |
|--------|------|---------|
| 0x61D | 1 | Dirty/refresh flag |
| 0x61E | 1 | Write mode (0=OR, 1=XOR, 3=AND) |
| 0x61F | 1 | Cursor position |
| 0x620 | 1 | Contrast (0-63) |
| 0x621 | 1 | Backlight state |

### Font Table (at 0x7B056)

128 entries (0x00-0x7F), column-major format. Each entry is 8 bytes: 5-6 data
bytes (one per column, bit 0 = top row) plus zero padding. The first 32 entries
contain the CP437 graphical characters.

## Methodology

All findings were derived from:

- ARM disassembly (Capstone) of the kernel LCD driver in `sn110-2_6_11.chk`
  (kernel offsets 0x6B730-0x6C880)
- ARM disassembly of the heartbeat function in the factory `lxnetdmx` binary
  (file offset 0xF77C-0xF900)
- On-device hardware verification via `tests/lcd_verify.c` (2026-04-23/24)
- Cross-referencing with `tests/lcd_probe.c` empirical results
