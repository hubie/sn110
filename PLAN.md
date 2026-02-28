# SN110 Open Firmware — Implementation Plan

## Current Status (2026-02-28)

### ✅ Completed
- Device investigation, profiling, full firmware backup (33 files)
- **DMX driver reverse engineering** — ioctl `_IOW('d', 1, 16-byte struct)` = 0x40106401
  - Modes: 0=OFF, 1=RAW, 2=TX, 3=RX (corrected from original guesses)
  - Data I/O via read()/write() of 512-byte DMX frames
  - Full analysis in `docs/dmx-interface.md`
- All protocol implementations: sACN (E1.31), Art-Net, ShowNet (with RLE decoder)
- DMX driver layer (real + mock), config parser, main threaded daemon
- Host test suite: **18/18 passing** (macOS native)
- Docker ARM toolchain: **18/18 passing** under QEMU ARM emulation
- ARM daemon binary starts and runs correctly under QEMU

### 🔲 Remaining
- [ ] bFLT/OABI toolchain (elf2flt + OABI syscalls for device binary)
- [ ] Device-ready bFLT binary (must be ≤ 449KB)
- [ ] RAM-resident test on real SN110 hardware
- [ ] Web UI / LCD integration
- [ ] Flash installation packaging

### Build Commands
```bash
make test          # Host tests (macOS/Linux native)
make docker-build  # Build Docker ARM toolchain
make docker-test   # ARM tests under QEMU in Docker
make docker-shell  # Interactive Docker shell
```

---

## Overview

Replace the proprietary `lxnetdmx` daemon on Strand ShowNet SN110 nodes with
an open-source multi-protocol DMX gateway supporting sACN (E1.31), Art-Net,
and ShowNet (backward compatibility).

## Device Profile

| Property | Value |
|----------|-------|
| CPU | NETsilicon NS7520 (ARM7TDMI), 14.6 BogoMIPS |
| RAM | 14 MB total, ~920 KB free at runtime |
| Flash | ~3 MB minix filesystem, ~482 KB available after removing lxnetdmx |
| OS | uClinux (Linux 2.0.38, no MMU) |
| Binary Format | bFLT v2 (uClinux flat binary) |
| Kernel GCC | 2.96 (Dec 2004) |
| DMX Ports | 2 × `/dev/dmx0`, `/dev/dmx1` (character devices) |
| LCD | `/dev/lcd0` |
| Network | 10/100 Ethernet, UDP/TCP, multicast support TBD |

## Existing Architecture

The current `lxnetdmx` daemon uses 4 pthreads:

```
┌─────────────────────────────────────────────┐
│                 lxnetdmx                     │
│                                              │
│  Thread_NetDmxGet ──recv()──→ ShowNet UDP    │
│         │ sem_post                           │
│         ▼                                    │
│  Thread_DevDmxPut ──write()──→ /dev/dmxN     │
│                                              │
│  Thread_DevDmxGet ──read()───→ /dev/dmxN     │
│         │ sem_post                           │
│         ▼                                    │
│  Thread_NetDmxPut ──send()──→ ShowNet UDP    │
│                                              │
│  [Unused ACN SDT session layer also linked]  │
└─────────────────────────────────────────────┘
```

DMX device ioctl modes (from `dmxtst` binary strings):
- `off` — Port disabled
- `silentoff` — Port disabled, quiet
- `rx` — DMX input (read from device, blocking or non-blocking)
- `tx` — DMX output (write to device, blocking or non-blocking)
- `raw` — Raw access
- `test` — Hardware test mode

## New Architecture

```
┌──────────────────────────────────────────────────────┐
│                    sn110dmx                           │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐           │
│  │  sACN    │  │ Art-Net  │  │ ShowNet  │  Network   │
│  │ Receiver │  │ Receiver │  │ Receiver │  Listeners │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘           │
│       │              │              │                 │
│       └──────┬───────┴──────────────┘                │
│              ▼                                        │
│       ┌──────────────┐                               │
│       │  DMX Merger   │  Priority/HTP merge          │
│       │  & Router     │  Universe → Port mapping     │
│       └──────┬───────┘                               │
│              │                                        │
│       ┌──────┴──────┐                                │
│       ▼              ▼                                │
│  ┌─────────┐  ┌─────────┐                           │
│  │/dev/dmx0│  │/dev/dmx1│  DMX Output               │
│  └─────────┘  └─────────┘                           │
│                                                      │
│  ┌─────────────────────┐                             │
│  │ Config Manager      │  220node.cfg compatible     │
│  │ Web UI (CGI)        │  Protocol/universe config   │
│  │ LCD Display         │  Status via /dev/lcd0       │
│  └─────────────────────┘                             │
└──────────────────────────────────────────────────────┘
```

## Phase 1: Toolchain & DMX Driver Reverse Engineering

### 1a. Cross-compilation toolchain

**Goal**: Produce a "hello world" bFLT binary that runs on the SN110.

**Approach** (in order of preference):
1. **Buildroot** with ARM7TDMI + uClibc-ng + bFLT output format
   - Most likely to produce compatible binaries
   - Can target Linux 2.0 ABI
2. **Bootlin prebuilt** `armv5-uclibc` toolchain + manual elf2flt
   - Faster to set up
   - May have ABI compatibility issues with Linux 2.0
3. **Docker-based build environment** with vintage toolchain
   - Build the same GCC 2.96 + binutils Strand used
   - Most compatible but hardest to set up

**Verification**:
- Compile minimal C program → bFLT
- Upload to SN110 `/tmp/` via FTP
- Execute via telnet
- Confirm "Hello from sn110" output

### 1b. Reverse-engineer DMX device interface

**Goal**: Understand how to open, configure, and write DMX data via `/dev/dmxN`.

**Approach**:
1. Disassemble `dmxtst` (only 6KB, very simple) using ARM7 disassembler
   - It's a command-line tool: `dmxtst <device> <mode> [label [block]]`
   - Contains the exact ioctl calls needed
2. Identify ioctl numbers for mode setting (off/rx/tx/raw/test)
3. Determine DMX frame format for write() calls (raw 512-byte frames? headers?)
4. Test with minimal program: open dmx0, set tx mode, write test pattern

**Tools**: Ghidra, radare2, or `arm-none-eabi-objdump` on the bFLT binaries
(may need bFLT→ELF conversion first, or direct binary analysis).

### 1c. Off-device testing infrastructure

**Goal**: Build and test on macOS/Linux host without needing the SN110.

**Approach**:
- Abstract DMX device behind an interface (`dmx_device_t`)
- Host implementation uses files/pipes as mock DMX devices
- Cross-compiled implementation uses real `/dev/dmxN`
- Abstract network reception behind similar interface
- Host tests use loopback UDP sockets

```c
// Platform abstraction
typedef struct {
    int (*open)(const char *device, int mode);
    int (*write_frame)(int fd, const uint8_t *dmx_data, int len);
    int (*read_frame)(int fd, uint8_t *dmx_data, int len);
    void (*close)(int fd);
} dmx_ops_t;

// Two implementations:
extern dmx_ops_t dmx_ops_real;    // /dev/dmxN on SN110
extern dmx_ops_t dmx_ops_mock;    // File-based mock for host testing
```

**Testing levels**:
1. **Unit tests** — Pure logic (frame parsing, universe mapping, config parsing)
2. **Integration tests** — Mock DMX + real UDP sockets on loopback
3. **QEMU tests** — ARM7 emulation running the bFLT binary (stretch goal)
4. **Device tests** — Real hardware, RAM-resident binary

## Phase 2: sACN (E1.31) Receiver

**Goal**: Receive sACN multicast packets, extract DMX data, write to `/dev/dmxN`.

**Protocol summary** (ANSI E1.31-2018):
- UDP multicast on `239.255.{universe_high}.{universe_low}`, port 5568
- Each packet: Root Layer → Framing Layer → DMP Layer → 512 DMX slots
- Priority system (0-200), sequence numbering
- Universe range: 1-63999

**Implementation**:
1. Join multicast group for configured universe(s)
2. Parse E1.31 packet (fixed offsets, simple binary format)
3. Extract 512-byte DMX payload
4. Write to appropriate `/dev/dmxN`
5. Handle priority (highest priority source wins)
6. Handle sequence numbers (detect packet loss)
7. Handle source timeout (no data for 2.5 seconds → blackout or hold)

**Multicast concern**: Linux 2.0 kernel multicast support may be limited.
Need to verify `IP_ADD_MEMBERSHIP` socket option works. Fallback: receive
on broadcast address, filter by source.

**Size budget**: sACN receiver should be < 30KB of code.

## Phase 3: Art-Net Receiver

**Goal**: Also accept Art-Net (widely used, especially with lower-end gear).

**Protocol summary** (Art-Net 4):
- UDP broadcast/unicast on port 6454
- `ArtDmx` packet: 18-byte header + 512 DMX slots
- Universe addressing: Net/SubNet/Universe (15 bits)
- Much simpler than sACN — no multicast, no priority

**Implementation**:
1. Bind UDP socket to port 6454
2. Parse `ArtDmx` packets (identify by "Art-Net\0" magic header)
3. Extract DMX payload
4. Route to appropriate `/dev/dmxN` based on universe mapping
5. Reply to `ArtPoll` with `ArtPollReply` (device discovery)

**Size budget**: Art-Net receiver should be < 20KB of code.

## Phase 4: ShowNet Backward Compatibility

**Goal**: Maintain compatibility with legacy Strand consoles.

**Protocol reference**: PyShowNet (MIT license), nickvsnetworking reverse engineering.

**Protocol summary**:
- UDP port 2501 (0x09C5)
- RLE-compressed DMX data
- Up to 18,432 addressable DMX slots (36 universes)
- Net slot mapping to physical DMX channels

**Implementation**:
1. Port ShowNet decoder from PyShowNet (Python → C)
2. Decode RLE-compressed DMX data
3. Route to DMX ports using existing slot/priority configuration

**Size budget**: ShowNet receiver should be < 15KB of code.

## Phase 5: Web UI & Configuration

**Goal**: Configure protocol, universe mapping, and priority via web browser.

**Approach**: Extend existing CGI system.
- The web UI uses CGI scripts (`cfgget.cgi`, `cfgpost.cgi`, `cfgnet.cgi`)
- `cfg2html` and `html2cfg` are bFLT binaries that translate between
  `220node.cfg` format and HTML forms
- We need to either extend or replace these

**New configuration parameters**:
```ini
# Protocol selection
protocol = sacn          # sacn, artnet, shownet, all
# sACN universe mapping
sacn_universe_1 = 1      # Universe for DMX port 1
sacn_universe_2 = 2      # Universe for DMX port 2
sacn_priority = 100      # Priority threshold
# Art-Net universe mapping
artnet_universe_1 = 0    # Art-Net universe for port 1
artnet_universe_2 = 1    # Art-Net universe for port 2
```

**LCD display**: Show active protocol, universe, and frame rate.

## Phase 6: Packaging & Distribution

**Goal**: Make it easy for anyone to flash their SN110.

**Deliverables**:
1. Pre-built `.chk` firmware image (if we reverse the checksum format)
2. Pre-built bFLT binary + install script (simpler alternative)
3. Step-by-step install guide with photos
4. Recovery guide
5. Source code + Makefile + Dockerfile for reproducible builds

**Install methods**:
1. **Automated**: `make deploy-flash IP=x.x.x.x` from any computer
2. **Manual**: FTP upload + telnet commands (documented step-by-step)
3. **Web-based**: If we add a firmware upload page to the web UI

---

## Open Questions

1. **Linux 2.0 multicast support** — Does `IP_ADD_MEMBERSHIP` work? If not,
   sACN falls back to broadcast reception with software filtering.

2. **DMX frame format** — Is it raw 512-byte writes, or does the driver expect
   a header/structure? The `dmxtst` binary analysis will answer this.

3. **DMX timing** — Does the kernel driver handle DMX timing (break, MAB, etc.)
   or do we need to bit-bang? Likely handled by driver given the NS7520's UART.

4. **Flash checksum format** — The `nodecfg checksum` command validates `.chk` files.
   We need to reverse this to create proper flash images. Alternative: use
   file-by-file FTP upload instead of the `.chk` flash mechanism.

5. **Thread model** — Pthreads on uClinux with Linux 2.0: are these real pthreads
   or LinuxThreads (clone-based)? The existing binary uses them, so they work.

6. **Binary size** — Can we fit sACN + Art-Net + ShowNet + config in < 449KB bFLT?
   Almost certainly yes — these are simple protocols.

7. **DMX IN support** — Do we also want to support DMX IN → sACN/Art-Net output?
   This would make the node bidirectional. Phase 2+ goal.

---

## Size Estimates

| Component | Estimated Size |
|-----------|---------------|
| Main + threading | ~10 KB |
| sACN receiver | ~25 KB |
| Art-Net receiver | ~15 KB |
| ShowNet receiver | ~15 KB |
| DMX device interface | ~5 KB |
| Config parser | ~10 KB |
| LCD driver | ~5 KB |
| Web CGI updates | ~10 KB |
| uClibc linked overhead | ~100-150 KB |
| **Total** | **~200-250 KB** |

Well within the 449 KB budget (the space freed by removing lxnetdmx).
