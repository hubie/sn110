---
title: "DMX loopback failure due to swapped mode enums and driver close-to-transmit semantics"
category: integration-issues
date: 2026-04-04
tags:
  - dmx512
  - kernel-driver
  - reverse-engineering
  - rs485
  - arm7tdmi
  - uclinux
  - ioctl
  - sacn
severity: critical
component: src/dmx/dmx_real.c
related_issues: []
---

# DMX Loopback Failure: Swapped Mode Enums and Close-to-Transmit Semantics

## Problem Statement

The SN110 open-source DMX gateway firmware (ARM7TDMI, Linux 2.0/uClinux, Strand Lighting hardware) could not transmit or receive DMX data through the loopback cable between its two DMX ports. The factory self-test (`dmxtst`/`selftst`) passed, proving the hardware worked, but custom firmware could not achieve wire-level DMX communication.

**Symptoms:**
- RAW mode reads returned 512 bytes of zeros or stale buffer data
- TX/RX modes returned `rd=0` (no data)
- Factory self-test (`selftst dmx12`/`dmx21`) passed but custom firmware failed
- sACN receive path worked fine; DMX wire output was the problem

---

## Investigation Timeline

### What Was Tried (and Why It Failed)

1. **RAW mode (mode=1) on both ports**: Read returned 512 bytes of zeros. RAW mode only provides a local memory buffer with no wire I/O — it never touches the UART.

2. **TX mode (mode=2) / RX mode (mode=3)**: Port 0 set to mode=2 (believed TX), port 1 to mode=3 (believed RX). Read returned 0 bytes. sACN reception was confirmed working (data was reaching the device), so the issue was in the DMX output path. Tried `buf_size=0` and `buf_size=512` with no effect.

3. **Start code handling**: Prepending a 0x00 start code to writes and stripping from reads. Made no functional difference — the byte count shifted from 512 to 511 (confirming the strip logic worked) but data remained zeros.

4. **Factory self-test**: `selftst dmx12` passed when run alone, proving the hardware and loopback cable were fine. It only failed when the daemon held the ports open (resource conflict).

5. **dmxtst pre-initialization**: Running `dmxtst /dev/dmx0 raw` then `dmxtst /dev/dmx1 raw` before the daemon produced a sequential ramp pattern (Ch1=0, Ch2=1, Ch3=2, ...). This turned out to be the RAW buffer's initialization pattern, not wire data — confirmed by setting port 0 to OFF mode and observing port 1 still returned the same pattern.

### The Breakthrough: Disassembling dmxtst

Disassembly of the `dmxtst` binary (6072-byte bFLT, ARM7TDMI) revealed two critical facts:

- **"test" mode does not call the DMX ioctl at all** — it relies on driver defaults after `open()`.
- **TX uses an open, write, close, sleep(50ms), reopen cycle** — the `close()` call triggers the kernel driver to transmit buffered data via the UART.
- **Mode value mapping**: `"tx"` maps to mode=1, `"rx"` maps to mode=2, `"raw"` maps to mode=3.

---

## Root Causes

### Root Cause 1: DMX Mode Values Were Swapped

Our code defined:

| Define | Our Value | Actual Driver Value |
|---|---|---|
| `DMX_MODE_OFF` | 0 | 0 |
| `DMX_MODE_RAW` | 1 | 3 |
| `DMX_MODE_TX` | 2 | 1 |
| `DMX_MODE_RX` | 3 | 2 |

Every time the firmware set "TX mode" it was actually configuring the driver for RX, and vice versa. The original reverse-engineering had the values wrong.

(auto memory [claude]: This finding was previously noted in DMX kernel driver behavior memory — the correction from 1=RAW,2=TX,3=RX to 1=TX,2=RX,3=RAW.)

### Root Cause 2: TX Requires close() Per Frame

The kernel DMX driver buffers `write()` data but only initiates UART transmission when the file descriptor is closed. Our daemon kept the fd open and wrote repeatedly — data accumulated in the buffer but never reached the wire.

(auto memory [claude]: The TX close-reopen pattern was previously noted — dmxtst uses `open() -> write(512) -> close()` per frame with 50ms sleep.)

---

## Working Solution

### Fix 1: Correct Mode Values in `common.h`

```c
/* Before (WRONG) */
#define DMX_MODE_OFF        0
#define DMX_MODE_RAW        1   /* was actually TX in the driver */
#define DMX_MODE_TX         2   /* was actually RX in the driver */
#define DMX_MODE_RX         3   /* was actually RAW in the driver */

/* After (CORRECT — verified from dmxtst binary disassembly) */
#define DMX_MODE_OFF        0
#define DMX_MODE_TX         1   /* DMX transmit */
#define DMX_MODE_RX         2   /* DMX receive */
#define DMX_MODE_RAW        3   /* Raw serial buffer (local only, no wire I/O) */
```

### Fix 2: TX ioctl Configuration in `dmx_real.c`

```c
cfg.mode = mode;
if (mode == DMX_MODE_TX) {
    cfg.buf_size = DMX_UNIVERSE_SIZE;  /* 512 */
    cfg.rate = 100;
}
/* RX and OFF modes: buf_size=0, rate=0 (per dmxtst behavior) */

if (mode == DMX_MODE_RX)
    ioctl(fd, SN110_FIONBIO, &nonblock);  /* non-blocking reads only */
```

### Fix 3: Close-Reopen TX Cycle in `main.c`

```c
static void dmx_tx_write(const dmx_ops_t *ops, int *fds, int port,
                          const uint8_t *data, int len)
{
    const char *dev = (port == 0) ? DMX_DEVICE_0 : DMX_DEVICE_1;
    int fd;

    /* Close previous fd (triggers TX of any buffered data) */
    if (fds[port] >= 0)
        close(fds[port]);

    /* Open fresh, write, close to trigger transmission */
    fd = open(dev, O_RDWR);
    if (fd < 0)
        return;
    write(fd, data, len);
    close(fd);

    /* Reopen for next cycle */
    fds[port] = open(dev, O_RDWR);
}
```

The key insight: the close-reopen cycle mirrors exactly what `dmxtst` does in its TX test mode. The kernel driver treats `close()` as the "send now" signal for any buffered DMX frame data.

---

## Verification

| Test | Result |
|---|---|
| sACN universe 1 (Ch1=255, Ch2=128, Ch3=64) through Port 0 TX, loopback cable, Port 1 RX, out as sACN universe 2 | Received matching data |
| Sender stops | Receiver correctly shows zeros |
| Factory self-test (`selftst dmx12` / `selftst dmx21`) | Still passes |

---

## Prevention Strategies

### 1. Verify Reverse-Engineered Constants Against Reference Binaries

Disassembly is the ground truth. Human-assigned symbolic names are hypotheses until confirmed. For every constant in a reverse-engineered header, trace its usage in at least one known-working binary to confirm semantics. Record the evidence chain: which binary, which offset, which instruction, which register value. Treat the header file as a living document with confidence annotations (e.g., `/* mode=1: TX — confirmed via dmxtst @ 0x1234 */`).

### 2. Test With the Factory Tool as Oracle

When the factory binary works and yours does not, the binary IS the specification. Stop guessing and start disassembling. Extract the complete syscall sequence from the factory binary before writing userspace code: which file is opened, which ioctls are issued, what is written, and how the fd is closed.

### 3. Watch for close()-Triggered I/O

Some kernel drivers use `close()` as the trigger to flush/transmit buffered data. If `write()` succeeds but no data appears on the wire, immediately test the `open -> write -> close` pattern before investigating anything else. When reverse-engineering a driver, always examine the `release` (close) function pointer in the `file_operations` struct, not just `write` and `ioctl`.

### 4. RAW Mode Does Not Mean Raw Wire Access

In this driver, "RAW" mode provides access to a local memory buffer — not raw access to the UART hardware. Never assume a mode's semantics from its name alone. Confirm behavior with wire-level measurement (oscilloscope, logic analyzer, or loopback).

### 5. Treat the Factory Binary as Documentation

For undocumented embedded systems, the factory binary is the canonical reference. Store disassembly notes in version control alongside the reverse-engineered headers. When updating a header constant, cite the binary offset that justifies the change.

---

## Key Lessons

1. **Never trust reverse-engineered constants without binary verification.** The mode enum values were plausible but wrong, and the firmware "worked" (no crashes, no errors) while silently misconfiguring every port.

2. **Driver semantics matter more than API shape.** The `write()` call looked like it should transmit, but the actual transmit trigger was `close()`. This is unusual but internally consistent with how the Strand kernel driver manages UART DMA.

3. **Factory test tools are the ground truth.** Disassembling the 6KB `dmxtst` binary provided both correct constants and the correct I/O pattern in under an hour, after days of black-box experimentation.

---

## Cross-References

- `src/common.h` — Corrected DMX_MODE_* constants
- `src/dmx/dmx_real.c` — Corrected ioctl configuration and simplified driver ops
- `src/main.c` — Added `dmx_tx_write()` close-reopen cycle
- `docs/dmx-interface.md` — Original reverse-engineering analysis (contains stale mode comments at line 35)
- `src/dmx/dmx_ioctl.h` — Reverse-engineered ioctl header (contains stale mode comments at line 28)
- `docs/solutions/reverse-engineering/strand-chk-firmware-checksum-algorithm.md` — Related binary analysis methodology

### Stale Documentation Identified

| File | Location | Issue |
|---|---|---|
| `src/dmx/dmx_ioctl.h` | Line 28 | Comment says `0=OFF, 1=RAW, 2=TX, 3=RX` — should be `0=OFF, 1=TX, 2=RX, 3=RAW` |
| `docs/dmx-interface.md` | Line 35 | Inline comment has old mode mapping |
| `PLAN.md` | Line 8 | Claims "corrected" values but shows old incorrect ones |
