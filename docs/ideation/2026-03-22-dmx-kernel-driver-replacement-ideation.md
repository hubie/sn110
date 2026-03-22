---
date: 2026-03-22
topic: dmx-kernel-driver-replacement
focus: Replacing or fixing the DMX kernel driver's 6ms RCGT timing bug
---

# Ideation: DMX Kernel Driver Replacement

## Codebase Context

The SN110 is an ARM7TDMI (NS7520) DMX-over-Ethernet node running uClinux 2.0.38 (no MMU).
The kernel serial driver uses an RCGT (Receive Character Gap Timer) that requires >6ms idle
between DMX frames to detect frame boundaries. Modern consoles send back-to-back frames with
only 176us break. Result: dropped frames, flicker, unresponsive fixtures.

ETC's official workaround: set console to "slow" DMX mode. Fragile -- a deep clear resets it.
See: https://support.etcconnect.com/ETC/Getting_Started_with_ETC_and_FAQ/Problems_with_Strand_SN110_node_receiving_DMX

**Key constraints**: Cannot recompile kernel (no 2004 uClinux toolchain). No MMU -- userspace
can access all physical memory. HZ=100 (10ms timer tick). 32-byte UART FIFO fills in 1.28ms
at 250kbaud. Single-threaded daemon (clone() unstable).

**Existing work**: A TX-only userspace UART driver exists on `feature/direct-dmx-driver-v2`
using mmap `/dev/mem` for register access. Config key `dmx_driver=kernel|direct` implemented.
mmap/munmap syscalls in OABI layer. Per-port ops dispatch in main.c.

## Ranked Ideas

### 1. RCGT Disable + Parasitic RX
**Description:** Clear the RCGT_EN bit (bit 26 of ctrl_b) via /dev/mem at daemon startup. The kernel ISR continues draining the 32-byte FIFO on RX_RDY/RX_HALF/RX_FULL interrupts (independent of RCGT). Userspace reads the tty stream normally and polls mmap'd RX_BRK status bit for DMX frame boundaries. One register write eliminates the 6ms problem.
**Rationale:** Maximum impact for minimum code. The kernel handles the hard real-time FIFO drain; userspace handles protocol-level frame detection using the DMX BREAK signal (the spec-correct delimiter). No kernel modification needed.
**Downsides:** Kernel may re-enable RCGT on change_speed(). Unknown whether /dev/dmx0's read() semantics depend on RCGT internally. RX_BRK clear semantics need hardware verification.
**Confidence:** 85%
**Complexity:** Low
**Status:** Unexplored

### 2. RX Frame Timing Diagnostics
**Description:** Add instrumentation: frames received, FIFO overruns (STATA_RX_OVERRUN), break detections, inter-frame gap histogram. Expose via web UI and optionally JSON. Poll status_a flags via /dev/mem alongside normal DMX reads.
**Rationale:** Essential for validating any fix. Can't prove the RCGT disable works without measurement. Also makes the firmware more useful than original Strand firmware for debugging DMX issues.
**Downsides:** Adds mmap polling overhead (tiny). Need daemon-to-CGI data sharing mechanism.
**Confidence:** 90%
**Complexity:** Low-Medium
**Status:** Unexplored

### 3. Hardware Watchdog Integration
**Description:** Kick the NS7520 hardware watchdog timer in the main event loop. If the daemon hangs or crashes, the device auto-reboots within seconds.
**Rationale:** SN110 nodes live in inaccessible locations. A single daemon crash means dark fixtures until someone physically reboots. Trivial to implement (one register write per loop), zero risk.
**Downsides:** Need to locate NS7520 watchdog registers (not in current reference headers). Reboot during a show is still disruptive.
**Confidence:** 80%
**Complexity:** Low
**Status:** Unexplored

### 4. Loadable Kernel Module with Custom ISR
**Description:** Write a minimal Linux 2.0 .o module that replaces the serial ISR for DMX channels. New ISR drains FIFO into shared-memory ring buffer with break-detection-based framing, bypassing tty layer and RCGT entirely.
**Rationale:** Textbook correct solution -- kernel module has zero FIFO overflow risk and complete control over frame detection.
**Downsides:** Requires matching cross-toolchain for Linux 2.0 OABI kernel modules. High effort, uncertain toolchain availability.
**Confidence:** 50%
**Complexity:** High
**Status:** Unexplored

### 5. Tighten RCGT Gap Threshold (Fallback)
**Description:** Instead of disabling RCGT, reprogram rx_char_timer to ~250-300us gap. Kernel frame detection continues working with tighter threshold.
**Rationale:** Even simpler than #1 -- no userspace frame detection needed. Good first experiment.
**Downsides:** Tuning risk near DMX break duration. Less robust than full RCGT disable.
**Confidence:** 60%
**Complexity:** Low
**Status:** Unexplored

## Rejection Summary

| # | Idea | Reason Rejected |
|---|------|-----------------|
| 1 | Binary-patch kernel RCGT in RAM | Too fragile -- multiple code paths, cache coherency issues |
| 2 | SIGALRM fast poll | HZ=100 means 10ms minimum; FIFO fills in 1.28ms |
| 3 | DMA-driven RX | DMA register map unknown, board wiring unverified |
| 4 | Hijack ARM IRQ vector | One ISR bug crashes system with no debugger |
| 5 | Full userspace RX takeover | Tight-poll blocks event loop 22ms per frame |
| 6 | rx_match start-code detect | 0x00 matches payload data, not just start codes |
| 7 | rx_buf_timer detection | Poorly documented, kernel ISR resets constantly |
| 8 | SIGIO async notification | Still gated by RCGT -- same 6ms in different wrapper |
| 9 | GPIO bit-bang RX | 197 cycles/bit at 49MHz, no room for interrupts |
| 10 | sACN interpolation | Fabricates control data, violates gateway contract |
| 11 | Reprogram system timer | Changes compiled-in HZ, breaks all kernel timeouts |
| 12 | Adaptive poll rate | Over-engineers a non-solution |
| 13 | Shorter select loop | Only helps after RCGT fixed; alone finds "no data" faster |
| 14 | mDNS/DNS-SD | Resource-heavy for 49MHz, device is statically configured |

## Session Log
- 2026-03-22: Initial ideation -- 23 candidates generated (from 40+ raw across 5 agents), 5 survived adversarial filtering. Clear winner: RCGT Disable + Parasitic RX. Handed off to /ce:plan for experiment + implementation planning.
