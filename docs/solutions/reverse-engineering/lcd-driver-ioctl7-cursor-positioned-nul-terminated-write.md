---
title: "LCD Partial Write: Cursor-Positioned NUL-Terminated Writes via ioctl cmd 7"
category: reverse-engineering
date_solved: 2026-04-22
date_documented: 2026-04-22
tags:
  - lcd
  - ioctl
  - kernel-driver
  - lxnetdmx
  - arm-disassembly
  - uclinux
  - partial-write
severity: informational
component: lcd-driver
symptoms:
  - "lxnetdmx heartbeat icon blinks at row 0 col 12-15 without disturbing surrounding display content"
  - "content written by other processes (sn110lcd) survives lxnetdmx LCD writes"
  - "prior documentation described ioctl as passthrough to write() — could not explain positional targeting"
related_issues:
  - "auto memory lcd-driver-model.md — prior doc described cmd 7 as passthrough; this session corrects that"
---

## Problem

The factory firmware process `lxnetdmx` blinks a heart icon at LCD line 1, positions 12-15, without disturbing any other content on the 4x16 character display — including content written by a separate process (`sn110lcd`). The `write()` syscall to `/dev/lcd0` uses a circular buffer with a persistent position counter, making targeted positional writes appear impossible without a shadow buffer. The mechanism was undocumented.

## Investigation

1. Examined the `lxnetdmx` and `sn110lcd` factory binaries (BFLT format) using strings and hex analysis
2. Located kernel LCD driver strings ("Mode: OR", "Mode: XOR", "Mode: AND", "Cursor:", "BackLight:") in the firmware image (`sn110-2_6_11.chk`) at offset 0x7B4A0
3. Found literal pool references to these strings, pinpointing the driver code region (0x6B730–0x6C880)
4. Installed Capstone and performed ARM disassembly of the kernel LCD driver's ioctl handler at 0x6B9CC
5. Discovered a 12-entry ioctl command table with specific handlers — not a passthrough to `write()`
6. Disassembled key functions: `lcd_write_line` (0x6C6E0), `set_cursor` (0x6C634), `set_mode` (0x6C58C)
7. Traced the `lxnetdmx` heartbeat function at binary offset 0xF77C — confirmed it uses ioctl cmd 7 with col=12, row=0
8. Built and ran on-device test program (`tests/lcd_partial.c`) confirming partial writes work exactly as predicted

## Root Cause

The kernel LCD driver exposes ioctl command 7 (`_IOW('l', 7, 48)` = `0x40306C07`) which atomically positions the cursor and writes a NUL-terminated string — all in a single syscall. This is the only way to write to an arbitrary LCD position without clobbering the cursor state shared with other processes.

The kernel handler at offset 0x6BC48:
1. Extracts column from `buf[2]` and row from `buf[3]`
2. Calls `set_cursor(col, row)` at 0x6C634
3. Calls `lcd_write_line(&buf[4])` at 0x6C6E0, which writes characters until it hits a NUL byte

Characters before the cursor position and after the NUL terminator are never touched.

Prior documentation (auto memory [claude]) described the ioctl as "functionally identical to write()" with an open question about counter alignment across blink cycles. The kernel disassembly proved that ioctl cmd 7 has a specific handler that sets cursor position before writing — it is not a passthrough to the circular buffer. The cursor positioning resolves the alignment question completely.

## Solution

```c
#define LCD_CMD7  ((1U << 30) | (48U << 16) | ('l' << 8) | 7)  /* 0x40306C07 */

void lcd_partial_write(int col, int row, const char *text)
{
    char buf[48];
    int fd, len;

    memset(buf, 0, 48);
    buf[2] = (char)col;
    buf[3] = (char)row;
    for (len = 0; text[len] && len < 43; len++)
        buf[4 + len] = text[len];
    /* buf[4+len] is already 0 from memset — NUL terminator */

    fd = open("/dev/lcd0", O_RDWR, 0);
    ioctl(fd, LCD_CMD7, buf);
    close(fd);
}
```

The lxnetdmx heartbeat pattern: `buf[2]=12, buf[3]=0, buf[4]=0x03` (CGRAM heart glyph) or `' '` (space), `buf[8]=0x00` (NUL stop). Open and close the fd each cycle — driver state is per-device, not per-fd.

## Key Technical Details

### 48-byte buffer layout for ioctl cmd 7

| Bytes | Contents |
|-------|----------|
| 0-1 | Unused padding |
| 2 | Column (0-15) |
| 3 | Row (0-3, mapping to lines 1-4) |
| 4-47 | NUL-terminated string payload (max 43 chars) |

### lcd_write_line character handling

| Byte value | Behavior |
|-----------|----------|
| `0x00` | NUL — terminates write immediately |
| `0x01`-`0x07` | CGRAM custom glyphs — pass through unchanged |
| `0x0A` | Newline — pads remainder of current line with spaces, advances to next row |
| `0x20`-`0x7F` | Printable ASCII — written directly |
| `0x80`+ | Replaced with space |
| Other control chars | Replaced with space |

### Ioctl command table (selected entries)

| Command | Encoding | Description |
|---------|----------|-------------|
| `_IOW('l', 1, 4)` | `0x40046C01` | Set write mode: arg[0] = 0=OR, 1=XOR, 3=AND |
| `0x6C02` | simple | Clear screen (memset to space, reset cursor) |
| `_IOW('l', 4, 4)` | `0x40046C04` | Set cursor: arg[0]=col, arg[1]=row |
| `_IOW('l', 7, 48)` | `0x40306C07` | **Cursor + write (the partial update mechanism)** |
| `_IOW('l', 8, 4)` | `0x40046C08` | Reset (memsets 500 bytes to 0) |

The OR/XOR/AND write modes (stored at device offset 0x61E) exist in the driver but are **not used** by the heartbeat. The partial-update mechanism relies entirely on cursor positioning + NUL termination.

## Best Practices

**Use ioctl cmd 7 for all partial updates.** It positions the cursor and writes in a single syscall, avoiding races with other processes that share the device's cursor state.

**Always NUL-terminate payloads.** Place `0x00` at `buf[4 + len]`. The driver uses NUL as a stop — termination is mandatory, not optional. Maximum safe payload: 43 characters.

**Use open-ioctl-close per update.** This matches the factory firmware pattern and avoids counter drift from mixing `write()` and `ioctl()` on the same fd.

**Reserve `write()` for full-screen redraws.** A 64-byte `write()` with all positions filled is cleaner for full repaints and wraps the counter cleanly. Use ioctl cmd 7 only when part of one line changes.

## Pitfalls to Avoid

- **CGRAM char 0x00 is unreachable** — NUL is always a terminator. Use slots 0x01-0x07 for custom glyphs.
- **Cursor state is per-device, not per-fd** — two processes writing to `/dev/lcd0` share one cursor. Use ioctl cmd 7 (which sets cursor explicitly) to avoid races.
- **Newline pads to end-of-line** — `\n` fills the rest of the current line with spaces. Never use `\n` for partial line updates.
- **Control chars and 0x80+ are silently replaced with space** — do not encode status using high bytes.

## Testing

On-device test program: `tests/lcd_partial.c` (build with `make docker-lcd-partial-bflt`)

| Mode | What it tests |
|------|--------------|
| `proof` | Writes known screen, then ioctl cmd 7 at specific positions — confirms only targeted positions change |
| `blink` | Replicates factory heartbeat — 20 cycles of toggling position 12, rest of screen frozen |
| `modes` | Probes whether OR/XOR/AND modes affect ioctl cmd 7 |
| `cursors` | Verifies row 0-3 maps to lines 1-4 |

When modifying LCD code: run your update loop in parallel with another process writing to the display to confirm no position corruption.

## Cross-References

- [docs/lcd-partial-write.md](/docs/lcd-partial-write.md) — Full ioctl command table and kernel address reference
- [docs/solutions/integration-issues/dmx-kernel-driver-mode-values-and-close-triggered-tx.md](/docs/solutions/integration-issues/dmx-kernel-driver-mode-values-and-close-triggered-tx.md) — Parallel finding: DMX kernel driver ioctl semantics discovered by the same ARM disassembly methodology
- [docs/solutions/reverse-engineering/strand-chk-firmware-checksum-algorithm.md](/docs/solutions/reverse-engineering/strand-chk-firmware-checksum-algorithm.md) — Source of the kernel image used for LCD driver disassembly
- [docs/ideation/2026-04-04-lcd-display-ideation.md](/docs/ideation/2026-04-04-lcd-display-ideation.md) — LCD display design using this mechanism
