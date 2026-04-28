# SN110 Open Firmware

**Open-source DMX gateway firmware for Strand ShowNet SN110 nodes.**

The Strand ShowNet SN110 is a professional DMX-over-Ethernet node from the early 2000s.
Despite being marketed as "ACN Ready," Strand never shipped ACN support. This project
provides open-source replacement firmware that adds modern protocol support, turning
e-waste into functional equipment for theatres, churches, schools, and hobbyists.

## What This Project Does

The SN110 Open Firmware replaces the factory `lxnetdmx` daemon with `sn110dmx`, a
modern multi-protocol DMX gateway that runs on the original hardware. It supports:

| Protocol | Status | Description |
|----------|--------|-------------|
| **sACN (E1.31)** | Working | Primary protocol -- used by ETC Eos, QLC+, most modern systems |
| **Art-Net** | Planned | Widely used alternative, especially for smaller setups |
| **ShowNet** | Planned | Backward compatibility with legacy Strand consoles |

Each DMX port can be independently configured as **input** (DMX-to-network) or **output**
(network-to-DMX), with per-port universe assignment.

## Hardware at a Glance

| Component | Details |
|-----------|---------|
| **CPU** | NETsilicon NS7520 (ARM7TDMI core), ~55 MHz, 14.6 BogoMIPS |
| **RAM** | 14 MB |
| **Flash** | ~3 MB (Minix filesystem) |
| **OS** | uClinux (Linux 2.0.38, no MMU) |
| **Network** | 10/100 Ethernet |
| **DMX** | 2 x DMX512 ports (5-pin XLR), configurable as Input or Output |
| **Display** | 96x32 pixel LCD (16x4 characters, CP437 font) |
| **Binary Format** | bFLT v2 (uClinux flat binary) |

## Quick Start

```bash
# Build the firmware (requires Docker)
make docker-bflt

# Run the test suite
make docker-test

# Deploy to a device on the network
curl -s -T build/sn110dmx.bflt ftp://192.168.2.231/tmp/sn110dmx
```

See [Building](getting-started/building.md) for full setup instructions and
[Deploying](getting-started/deploying.md) for the upload and run workflow.

## Why This Project Exists

The Strand ShowNet SN110 was a well-built piece of professional equipment -- two DMX
universes in a compact rack-mount form factor. But with Strand acquired and ShowNet
obsolete, these devices can't speak modern protocols out of the box.

This firmware fixes that, starting with sACN (E1.31) support.

## Project Status

The firmware is functional and in active development. The sACN (E1.31) DMX gateway
works -- packets are received and output as DMX512. The web configuration interface
is operational. The LCD display shows real-time port status with context-aware modes.
Art-Net and ShowNet support are planned but not yet implemented.

!!! warning "Alpha Software"
    This firmware has been tested on a single SN110 unit (board rev V2.6.11).
    Other hardware revisions may behave differently. Always keep a backup of
    your original firmware before flashing. See [Recovery](recovery/network-recovery.md)
    for how to restore the factory image.
