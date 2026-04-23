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
| `_IOW('l', 8, 4)` = `0x40046C08` | 4-byte arg | Reset (memsets 500 bytes to 0) |
| `_IOW('l', 14, 12)` = `0x400C6C0E` | 12-byte arg | Unknown (6 halfword args) |
| `_IOW('l', 15, 8)` = `0x40086C0F` | 8-byte arg | Unknown |
| `_IOW('l', 16, 8)` = `0x40086C10` | 8-byte arg | Unknown |
| `_IOW('l', 17, 4)` = `0x40046C11` | 1-byte arg (signed) | Unknown |
| `_IOW('l', 18, 4)` = `0x40046C12` | 1-byte arg | Unknown |

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
- **Printable ASCII (0x01-0x7F except control chars)** is written directly.
- **Control chars and high-bit chars (0x80+)** are replaced with space.

Important: CGRAM characters 0x01-0x07 pass through (they are not flagged
as control chars by the kernel's ctype table). CGRAM char 0x00 cannot
be written because it is the NUL terminator.

### Write modes (OR / XOR / AND)

The driver stores a write mode at device structure offset 0x61E,
settable via `_IOW('l', 1, 4)`. The mode values are:

- 0 = OR
- 1 = XOR
- 3 = AND

These modes exist in the driver but are **not used by the heartbeat**.
The partial-update mechanism relies entirely on cursor positioning +
NUL termination. The mode may affect a separate rendering layer or
may be vestigial — further investigation needed.

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
- 0x1B = TX active (likely a custom glyph or right-arrow in CGRAM)
- 0x1A = RX active (likely a custom glyph or left-arrow in CGRAM)

### What the display sees

```
Position:  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15
Line 1:   [---- untouched by heartbeat ----] [written]
                                              ^cursor
```

The NUL at buf[8] stops the write after 4 characters (positions 12-15).
Positions 0-11 on line 1 and all of lines 2-4 are never reached.

## Methodology

Findings derived from:
- ARM disassembly (Capstone) of the kernel LCD driver in
  `dump/official-firmware/sn110-2_6_11.chk` (kernel offsets 0x6B730-0x6C880)
- ARM disassembly of the heartbeat function in
  `dump/firmware/usr/bin/lxnetdmx` (file offset 0xF77C-0xF900)
- Cross-referencing with empirical results from `tests/lcd_probe.c`

Key addresses in the kernel image:
- LCD init: 0x6B730
- LCD open: 0x6B7C4
- LCD close: 0x6B864
- LCD write: 0x6B880
- LCD ioctl: 0x6B9CC
- lcd_write_line: 0x6C6E0
- set_mode: 0x6C58C
- set_cursor: 0x6C634
- Mode/status strings: 0x7B4A0
- CGRAM font data: 0x7B360
