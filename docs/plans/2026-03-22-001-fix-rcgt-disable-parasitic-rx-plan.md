---
title: "fix: RCGT Disable + Parasitic RX for DMX Frame Reception"
type: fix
status: active
date: 2026-03-22
---

# fix: RCGT Disable + Parasitic RX for DMX Frame Reception

## Overview

The SN110's kernel serial driver uses an RCGT (Receive Character Gap Timer) that
requires >6ms of silence between DMX frames before it delivers data to userspace.
Modern consoles send back-to-back frames with only 176us of break. This causes
the SN110 to drop frames, flicker, or stop responding entirely.

ETC documents this as a known issue:
https://support.etcconnect.com/ETC/Getting_Started_with_ETC_and_FAQ/Problems_with_Strand_SN110_node_receiving_DMX

The fix: disable the RCGT via a single register write from userspace, keep the
kernel ISR draining the FIFO at interrupt speed, and detect DMX frame boundaries
in userspace using the hardware BREAK status bit.

## Problem Statement

The RCGT is configured in three places in the kernel serial driver
(`docs/reference/serial_netarm.c`):

- `startup()` line 786: `regs->ctrl_b = RCGT_EN | UART_MODE`
- `change_speed()` line 936: same
- `rs_init()` line 2560: same

The RCGT threshold is set via `rx_char_timer = RXGAP(baud)`. For 250kbaud with
PLL bypass and 18.432MHz crystal:

```
RXGAP(250000) = RX_GAP_TIMER_EN | (((10 * 18432000) / (250000 * 5 * 512)) - 1)
              = RX_GAP_TIMER_EN | (((184320000) / (640000000)) - 1)
              = RX_GAP_TIMER_EN | ~719
```

719 counts at the gap timer clock rate produces ~3.7ms. The ETC article reports
">6ms" which suggests the PLL'd clock path may be active, or the actual crystal
frequency differs. Either way, the gap is multi-millisecond when DMX only needs
176us of break between frames.

## Proposed Solution

### Architecture

```
BEFORE:
  UART HW → Kernel ISR drains FIFO → Kernel waits 6ms gap → Delivers frame → read()
  Problem: 6ms gap never happens with fast consoles → data stuck in kernel

AFTER:
  UART HW → Kernel ISR drains FIFO (still works — RX_RDY interrupts are independent)
           → Userspace reads byte stream from /dev/dmx0 (or /dev/ttyS1)
           → Userspace polls RX_BRK via mmap'd status register
           → Frame boundary detected in <1ms via BREAK, not 6ms via silence
```

### Key Insight

The kernel ISR's FIFO draining does NOT depend on RCGT. The ISR fires on three
independent interrupt sources:

- `RX_RDY` — at least 1 byte in FIFO
- `RX_HALF` — 16+ bytes in FIFO
- `RX_FULL` — 32 bytes in FIFO

These are controlled by interrupt enable bits in `ctrl_a` (bits 9-11), completely
separate from the `RCGT_EN` bit in `ctrl_b`. Disabling RCGT does not affect the
ISR's ability to drain the FIFO at interrupt speed.

## Technical Approach

### Implementation Phases

#### Phase 1: On-Device Validation Experiment

**Prerequisite:** Device power supply repair (replacement capacitors on order).

Run these experiments sequentially on the device via telnet. Each builds on the
previous result. Stop and reassess if any step fails.

##### Experiment 1a: Verify register access

```sh
# On the device, verify we can read UART registers via /dev/mem
# This should already work (the TX direct driver does it)
# Check ctrl_b current value for CH1 (offset 0x04 from 0xFFD00000)
devmem 0xFFD00004  # Should show a value with bit 26 (0x04000000) set = RCGT_EN
```

If `devmem` is not available on the device, write a tiny test binary that mmaps
`/dev/mem` at `0xFFD00000`, reads `ctrl_b`, and prints it. Cross-compile as bFLT
and FTP to `/tmp/`.

**Success criteria:** Can read ctrl_b, and it contains `RCGT_EN` (bit 26 set).

##### Experiment 1b: Disable RCGT and test kernel RX

```sh
# Clear RCGT_EN in ctrl_b (read-modify-write: clear bit 26)
# Then send DMX from a console and read /dev/dmx0
```

Write a test binary (`tests/rcgt_probe.c`) that:

1. Opens `/dev/mem`, mmaps `0xFFD00000` (serial module base)
2. Reads `ctrl_b` for the target channel, prints it
3. Clears `CTLB_RCGT_EN` (bit 26), writes back
4. Reads `ctrl_b` again to confirm the bit is cleared
5. Opens `/dev/dmx0` in non-blocking mode
6. Loops for 10 seconds:
   - Reads from `/dev/dmx0` with `read(fd, buf, 513)`
   - Prints bytes received per read (count + first few bytes)
   - Polls `status_a` via mmap, prints `RX_BRK` and `RX_OVERRUN` flags
   - Sleeps 5ms between iterations (`usleep(5000)`)

**Success criteria (best case):** `read()` returns bytes from the DMX stream.
`RX_BRK` toggles when frames arrive. No `RX_OVERRUN`.

**If read() returns 0 or -1 consistently:** The DMX char driver may depend on
RCGT internally for frame delivery. Proceed to experiment 1c.

##### Experiment 1c: Bypass /dev/dmx, use raw tty

If `/dev/dmx0` doesn't deliver bytes without RCGT, try the raw serial port:

1. Close `/dev/dmx0`
2. Open `/dev/ttyS1` (or `/dev/ttyS0` — try both to find which maps to which
   DMX connector)
3. Configure via termios: 250000 baud, 8N2, raw mode, no echo
4. If termios can't set 250000 (non-standard baud), configure via mmap register
   write to `REG_BITRATE` (same as the TX direct driver does)
5. Repeat the read loop from 1b

**Success criteria:** Raw tty delivers bytes when RCGT is disabled.

##### Experiment 1d: Verify RX_BRK semantics

While DMX is flowing:

1. Poll `status_a` via mmap in a tight loop for ~100ms
2. Count how many times `RX_BRK` transitions from 0→1
3. Compare with expected DMX frame rate (e.g., 44 frames/sec = ~4-5 breaks per 100ms)
4. Test whether `RX_BRK` auto-clears on read, or requires write-1-to-clear
   (write `STATA_RX_BRK` to `status_a` and check if it clears)

**Success criteria:** `RX_BRK` fires once per DMX frame. Clear semantics understood.

##### Experiment 1e: Channel mapping verification

Test which physical DMX connector corresponds to which UART channel:

1. Send DMX on connector A only, read CH1 and CH2 — which has data?
2. Send DMX on connector B only, read CH1 and CH2 — which has data?

This resolves the TODO in `dmx_direct.h`: "verify this mapping on hardware —
the kernel minor numbers (dmx0=minor 1, dmx1=minor 0) hint it could be reversed."

**Success criteria:** Channel mapping documented with certainty.

#### Phase 2: Minimal RX Implementation (if Phase 1 validates)

##### 2a: Add RCGT disable to direct driver open

In `src/dmx/dmx_direct.c`, modify `direct_open()` to optionally disable RCGT
when the port will be used for RX in hybrid mode:

```c
// New function: disable RCGT on a channel without taking over TX
void direct_disable_rcgt(int port)
{
    uint32_t ctlb = reg_read(port, REG_CTRL_B);
    ctlb &= ~CTLB_RCGT_EN;
    reg_write(port, REG_CTRL_B, ctlb);
}
```

##### 2b: Add RX_BRK polling to read_frame

Create a new read path (either in `dmx_direct.c` or a new `dmx_hybrid.c`) that:

1. Reads raw bytes from the kernel device fd (kernel ISR drains FIFO)
2. Polls `status_a` via mmap for `RX_BRK`
3. When BRK detected: the accumulated bytes are the previous frame
4. Returns the completed frame via `read_frame()` interface

```c
// Pseudo-code for hybrid read_frame
static int hybrid_read_frame(int fd, uint8_t *buf, int max_len)
{
    static uint8_t accum[513];
    static int accum_len = 0;

    // Check for break (new frame boundary)
    uint32_t sta = reg_read(port, REG_STATUS_A);
    if (sta & STATA_RX_BRK) {
        // Clear the break flag
        reg_write(port, REG_STATUS_A, STATA_RX_BRK);

        // Deliver accumulated frame
        if (accum_len > 0) {
            int n = (accum_len < max_len) ? accum_len : max_len;
            memcpy(buf, accum, n);
            accum_len = 0;

            // Read any bytes that arrived with/after the break
            int r = read(fd, accum, sizeof(accum));
            if (r > 0) accum_len = r;

            return n;
        }
    }

    // Accumulate bytes from kernel
    int space = sizeof(accum) - accum_len;
    if (space > 0) {
        int r = read(fd, accum + accum_len, space);
        if (r > 0) accum_len += r;
    }

    return 0;  // No complete frame yet
}
```

##### 2c: RCGT re-enablement guard

Add a periodic re-check in the main loop (once per iteration, ~44Hz):

```c
// In main loop, after dmx_input_cycle():
if (g_config.dmx_driver == DMX_DRIVER_DIRECT) {
    // Re-disable RCGT in case kernel re-enabled it
    for (int i = 0; i < DMX_MAX_PORTS; i++) {
        if (g_config.ports[i].mode == DMX_MODE_RX)
            direct_disable_rcgt(i);
    }
}
```

Cost: one register read + conditional write per port per loop = negligible.

##### 2d: Wire into dmx_ops_t

Add a new ops variant (or extend the existing direct ops):

```c
// Option A: Extend existing direct driver with RX support
static const dmx_ops_t hybrid_ops = {
    .open        = hybrid_open,       // opens kernel fd + mmap for BRK polling
    .close       = hybrid_close,
    .set_mode    = hybrid_set_mode,   // disables RCGT for RX mode
    .write_frame = direct_write_frame, // reuse TX direct driver
    .read_frame  = hybrid_read_frame,  // new: kernel bytes + mmap BRK
};
```

This eliminates the TX/RX ops split in main.c — both directions use the hybrid
driver when `dmx_driver=direct`.

##### 2e: Update config and web UI

The `dmx_driver=direct` config already exists. Add a note to the web UI:

```
Direct mode: precise TX timing + fast RX frame detection.
Kernel mode: original driver behavior (requires >6ms inter-frame gap for RX).
```

#### Phase 3: Diagnostics

Add to `dmx_ops_t` or as a standalone stats struct:

```c
typedef struct {
    uint32_t frames_received;
    uint32_t frames_overrun;     // STATA_RX_OVERRUN count
    uint32_t breaks_detected;
    uint32_t bytes_received;
    uint32_t min_gap_us;         // Shortest inter-frame gap observed
    uint32_t max_gap_us;
    uint32_t last_frame_len;
} dmx_rx_stats_t;
```

Expose via web UI status display and a `/cgi-bin/status.json` endpoint.

#### Phase 4: Fallback — Tighten RCGT Threshold

If Phase 1 shows that disabling RCGT entirely causes problems (e.g., kernel
driver enters an unexpected state), the fallback is to just lower the threshold:

```c
// Set RCGT to ~250us instead of ~4ms
// Formula: count = (10 * XTAL_FREQ) / (baud * 5 * 512) - 1
// For 250us target with 18.432MHz crystal:
//   count ≈ 250us * (18432000 / 512) ≈ 9
uint32_t short_rcgt = NETARM_SER_RX_GAP_TIMER_EN | 9;
reg_write(port, REG_RX_CHAR_TMR, short_rcgt);
```

This keeps the kernel's frame detection working, just with a tighter threshold
that accommodates fast consoles. No userspace frame detection needed.

## Alternative Approaches Considered

See `docs/ideation/2026-03-22-dmx-kernel-driver-replacement-ideation.md` for the
full ideation with 23 candidates evaluated. Key rejects:

- **SIGALRM fast poll**: HZ=100 means 10ms minimum timer — FIFO overflows in 1.28ms
- **DMA-driven RX**: Theoretically optimal but DMA register map unknown
- **Full userspace RX takeover**: Tight-poll blocks event loop for 22ms per frame
- **Kernel module replacement**: Correct solution but needs matching 2004 toolchain
- **Interrupt vector hijack**: One ISR bug = system crash with no debugger

## Acceptance Criteria

### Phase 1 (Experiment)

- [ ] Can read/write UART ctrl_b register via /dev/mem on the actual device
- [ ] Clearing RCGT_EN does not crash the device or stop the kernel ISR
- [ ] Bytes flow from DMX console to userspace read() after RCGT disable
- [ ] RX_BRK status bit fires on DMX break conditions
- [ ] RX_BRK clear semantics documented (auto-clear vs write-1-to-clear)
- [ ] Channel mapping (CH1/CH2 → DMX connector) verified and documented
- [ ] Document which device path works: /dev/dmx0 vs /dev/ttyS1

### Phase 2 (Implementation)

- [ ] Hybrid RX driver passes DMX frames to sACN TX path at full speed
- [ ] No FIFO overruns reported in diagnostics with fast console
- [ ] RCGT re-enablement guard prevents kernel from reverting the fix
- [ ] Existing host tests pass (56/56)
- [ ] New tests: hybrid read_frame with mocked BRK detection
- [ ] Cross-compile succeeds (Docker bFLT + CGI bFLT)
- [ ] `dmx_driver=kernel` still works identically to current behavior

### Phase 3 (Diagnostics)

- [ ] Web UI shows frame count, overrun count, break count
- [ ] Stats reset on page load or explicit reset button
- [ ] JSON endpoint returns machine-readable stats

## Dependencies & Risks

### Dependencies

- **Device power supply repair**: Capacitor replacement on order. All on-device
  testing blocked until this is resolved.
- **DMX test source**: Need a DMX console or USB-DMX adapter that sends at
  "fast" rate (no inter-frame idle >6ms). An ETC Eos or similar would be ideal.

### Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| /dev/dmx0 read() depends on RCGT internally | Medium | Must use /dev/ttyS1 instead (more code) | Experiment 1c tests this path |
| Kernel re-enables RCGT on error recovery | Low | Momentary frame drops | RCGT guard in main loop re-clears the bit |
| RX_BRK doesn't fire on this hardware revision | Low | Cannot detect frame boundaries | Fall back to tightened RCGT threshold (Phase 4) |
| Channel mapping is reversed | Medium | Wrong data on wrong port | Experiment 1e verifies before implementation |
| Stop bit errata affects RX framing | Low | Framing errors on received bytes | Monitor STATA_RX_FRMERR in diagnostics |

## Files Changed

| File | Change |
|------|--------|
| `tests/rcgt_probe.c` | New: on-device validation experiment binary |
| `src/dmx/dmx_direct.c` | Add `direct_disable_rcgt()`, hybrid read_frame |
| `src/dmx/dmx_direct.h` | Add RX-related status bit definitions (already present) |
| `src/dmx/dmx.h` | Add `dmx_rx_stats_t`, possibly hybrid ops |
| `src/dmx/dmx_real.c` | Update `dmx_get_ops()` to return hybrid ops when direct |
| `src/main.c` | RCGT guard in main loop, simplify per-port ops if hybrid works |
| `src/cgi/cgi_config.c` | Diagnostics display in web UI |
| `Makefile` | Add rcgt_probe build target |
| `tests/test_basics.c` | Tests for hybrid read_frame with mocked BRK |

## Sources & References

- **ETC Support Article**: https://support.etcconnect.com/ETC/Getting_Started_with_ETC_and_FAQ/Problems_with_Strand_SN110_node_receiving_DMX
- **Ideation**: `docs/ideation/2026-03-22-dmx-kernel-driver-replacement-ideation.md`
- **Kernel driver**: `docs/reference/serial_netarm.c` — lines 786, 936 (RCGT config), 364-505 (receive_chars), 581-620 (ISR)
- **Register header**: `docs/reference/netarm_ser_module.h` — lines 308-333 (RXGAP macro), 149-230 (status bits)
- **Direct driver (feature branch)**: `src/dmx/dmx_direct.h`, `src/dmx/dmx_direct.c` on `feature/direct-dmx-driver-v2`
- **ANSI E1.11 (DMX512-A)**: Break >= 88us, MAB >= 8us, start code + up to 512 channels
