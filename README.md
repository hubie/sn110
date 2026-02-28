# sn110-firmware — Open-Source Firmware for Strand ShowNet SN110 Nodes

**Bringing obsolete theatre DMX gateways back to life with modern protocol support.**

The Strand ShowNet SN110 is a professional DMX-over-Ethernet node from the early 2000s.
Despite being marketed as "ACN Ready," Strand never shipped ACN support. This project
provides open-source replacement firmware that adds modern protocol support, turning
e-waste into functional equipment for theatres, churches, schools, and hobbyists.

## Supported Protocols

| Protocol | Status | Description |
|----------|--------|-------------|
| **sACN (E1.31)** | 🚧 Planned | Primary target — used by ETC Eos, QLC+, most modern systems |
| **Art-Net** | 🚧 Planned | Widely used alternative, especially for smaller setups |
| **ShowNet** | 🚧 Planned | Backward compatibility with legacy Strand consoles |

## Hardware

The SN110 contains:
- **CPU**: NETsilicon NS7520 (ARM7TDMI core), 14.6 BogoMIPS
- **RAM**: 14 MB
- **Flash**: ~3 MB (minix filesystem)
- **OS**: uClinux (Linux 2.0.38, no MMU)
- **Network**: 10/100 Ethernet with 802.3af PoE
- **DMX**: 2 × DMX512 ports (5-pin XLR), configurable as In or Out
- **LCD**: Integral character display for status
- **Binary Format**: bFLT v2 (uClinux flat binary)

## Project Structure

```
sn110-firmware/
├── README.md           # This file
├── PLAN.md             # Detailed implementation plan
├── SAFETY.md           # Recovery procedures and brick prevention
├── LICENSE             # Open-source license
├── Makefile            # Cross-compilation build system
├── src/
│   ├── main.c          # Entry point, thread orchestration
│   ├── sacn/           # sACN (E1.31) receiver
│   ├── artnet/         # Art-Net receiver
│   ├── shownet/        # ShowNet receiver (legacy compatibility)
│   ├── dmx/            # DMX device driver interface (/dev/dmx0, /dev/dmx1)
│   ├── config/         # Configuration file parser (220node.cfg compatible)
│   └── lcd/            # LCD display updates (/dev/lcd0)
├── tests/              # Off-device tests (Linux host, mock DMX devices)
├── tools/              # Build, deploy, and recovery scripts
├── docs/               # Protocol documentation, hardware notes
└── dump/               # Original firmware backup (for reference/recovery)
```

## Building

### Prerequisites

- ARM7 cross-compilation toolchain with bFLT support
- `elf2flt` utility for ELF → bFLT conversion

See [PLAN.md](PLAN.md) for detailed toolchain setup instructions.

### Quick Build

```bash
make            # Cross-compile for ARM7 → bFLT
make test       # Run off-device tests on host
```

## Installation

> ⚠️ **READ [SAFETY.md](SAFETY.md) BEFORE INSTALLING** — contains recovery
> procedures and brick-prevention checklist.

### Safe Testing (RAM-only, lost on reboot)

```bash
make deploy-ram IP=192.168.0.71   # Upload to /tmp/ via FTP, run manually
```

### Persistent Installation (survives reboot)

```bash
make deploy-flash IP=192.168.0.71 # Uses Strand's built-in flash update mechanism
```

## Recovery

If something goes wrong, the device has multiple recovery paths:
1. **Telnet is independent** — always accessible even if the DMX daemon crashes
2. **FTP is independent** — can always upload new files
3. **Backup flash slot** — `/dev/blk3` can hold a known-good firmware image
4. **Boot script watchdog** — automatically restarts crashed daemons
5. **Original firmware backup** — in `dump/firmware/`, can be re-flashed

See [SAFETY.md](SAFETY.md) for step-by-step recovery procedures.

## Contributing

This is an early-stage project. Contributions welcome! Areas where help is needed:
- Reverse engineering the DMX device ioctl interface
- Testing with different SN110 hardware revisions
- sACN/Art-Net protocol implementation
- uClinux/ARM7 cross-compilation expertise

## License

This project's original source code is released under the MIT License.
Original Strand firmware files in `dump/` remain the property of Strand Lighting Ltd.

## Acknowledgments

- The [Open Lighting Project](https://wiki.openlighting.org/) for protocol documentation
- [PyShowNet](https://github.com/nickvsnetworking/PyShowNet) for ShowNet reverse engineering
- [nickvsnetworking](https://nickvsnetworking.com/reverse-engineering-dead-protocols-strand-shownet/) for the definitive ShowNet protocol analysis
- The uClinux project for keeping no-MMU Linux alive
