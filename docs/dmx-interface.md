# SN110 DMX Driver Interface — Reverse Engineering Analysis

**Source binary:** `dump/firmware/usr/bin/dmxtst` (6072 bytes, bFLT v2, ARM7TDMI)  
**Disassembly tool:** Capstone (Python bindings), ARM mode  
**Date:** 2026

## Summary

The SN110's DMX ports (`/dev/dmx0`, `/dev/dmx1`) are controlled by a custom Linux 2.0
kernel driver. The userspace interface consists of:

1. A single `ioctl()` call to configure the port mode
2. Standard `read()`/`write()` for DMX frame data
3. `FIONBIO` ioctl for blocking/non-blocking I/O control

## ioctl Interface

### DMX Configuration ioctl

```
Number:  0x40106401
Macro:   _IOW('d', 1, struct dmx_config)
```

Breakdown:
- Direction: `_IOC_WRITE` (1) — data flows from userspace to kernel
- Type: `'d'` (0x64) — DMX device class
- Number: 1 — only one ioctl command
- Struct size: 16 bytes

### struct dmx_config

```c
struct dmx_config {         /* 16 bytes total */
    uint8_t  mode;          /* offset 0:  0=OFF, 1=TX, 2=RX, 3=RAW */
    uint8_t  flags;         /* offset 1:  always 0 in dmxtst */
    uint16_t buf_size;      /* offset 2:  512 for TX, 0 otherwise */
    uint16_t rate;          /* offset 4:  100 for TX, 0 otherwise */
    char     label[8];      /* offset 6:  up to 8-char label string */
    uint8_t  reserved;      /* offset 14: always 0 */
    uint8_t  pad;           /* offset 15: struct padding */
};
```

### Mode Values

| Value | Name | Description | buf_size | rate | FIONBIO |
|-------|------|-------------|----------|------|---------|
| 0 | OFF | Disable DMX port | 0 | 0 | No |
| 1 | TX | DMX transmit | 512 | 100 | No |
| 2 | RX | DMX receive | 0 | 0 | Yes |
| 3 | RAW | Raw serial buffer (local only, no wire I/O) | 0 | 0 | Yes |

Note: `dmxtst` also supports "silentoff" (same as OFF but no console output)
and "test" (direct write loop with no ioctl, generates scrolling test pattern).

### ioctl Number Construction in ARM

The binary constructs the ioctl number efficiently:
```arm
mov  r1, #0x6400        ; type 'd' = 0x64, shifted left 8 = 0x6400
add  r1, r1, #1         ; add command number 1 → r1 = 0x6401
orr  r1, r1, r1, lsl #20  ; merge direction and size bits → 0x40106401
```

## Data I/O

### Writing DMX (TX mode)

```c
int fd = open("/dev/dmx0", O_RDWR);
struct dmx_config cfg = { .mode = 1 /* TX */ };
ioctl(fd, 0x40106401, &cfg);
write(fd, dmx_data, 512);  /* 512-byte DMX frame */
```

### Reading DMX (RX mode)

```c
int fd = open("/dev/dmx0", O_RDWR);
struct dmx_config cfg = { .mode = 2 /* RX */ };
ioctl(fd, 0x40106401, &cfg);
int nonblock = 1;
ioctl(fd, 0x5421, &nonblock);  /* FIONBIO */
read(fd, buffer, 512);
```

### Test Mode Pattern

The "test" mode in `dmxtst` bypasses the DMX ioctl entirely and writes
directly to the file descriptor in a close/sleep/reopen loop:

```c
while (1) {
    write(fd, pattern_512_bytes, 512);
    close(fd);
    usleep(50000);  /* 50ms = ~20fps */
    fd = open("/dev/dmx0", O_RDWR);
}
```

This suggests the kernel driver accepts writes in a default mode
without requiring the configuration ioctl first.

## Blocking Control

FIONBIO ioctl (0x5421) is used for RX and RAW modes:
- arg = 0 → blocking I/O
- arg = 1 → non-blocking I/O

When `dmxtst` is given the "block" parameter (argv[4]), it sets blocking mode.
Default is non-blocking.

## Syscalls Used by dmxtst

| SVC Instruction | Syscall # | Function |
|-----------------|-----------|----------|
| SVC 0x900001 | 1 | exit() |
| SVC 0x900004 | 4 | write() |
| SVC 0x900005 | 5 | open() |
| SVC 0x900006 | 6 | close() |
| SVC 0x900013 | 19 | lseek() |
| SVC 0x900036 | 54 | ioctl() |
| SVC 0x900052 | 82 | select() |
| SVC 0x90006A | 106 | stat() |

ARM Linux 2.0 uses the old-style SVC 0x900000+NR encoding.

## String Table (Data Section 0x15B0-0x17B8)

```
0x15B0: "Usage: %s device off/silentoff/rx/tx/raw/test [label [block]]"
0x15F0: "Error opening %s"
0x1608: "silentoff"
0x1614: "Setting %s to off."
0x162C: "block"
0x1634: "Setting %s to rx - blocking."
0x1654: "Setting %s to rx - non blocking."
0x167C: "Setting %s to tx - blocking."
0x169C: "Setting %s to tx - non blocking."
0x16C4: "Setting %s to raw - blocking."
0x16E4: "Setting %s to raw - non blocking."
0x1708: "test"
0x1710: "/dev/dmx0"
0x171C: "LIBC:PRINTF"
```

## bFLT Binary Layout

```
Offset  Size    Content
0x00    64      bFLT header (magic, entry, data_start, etc.)
0x40    5488    Code section (ARM7 instructions)
0x15B0  520     Data section (strings, constants)
0x17B8  2224    BSS (uninitialized data)
---     0       Relocations (none — flags=0x01000000)
```

Total file size: 6072 bytes
