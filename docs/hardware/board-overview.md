# Board Overview

The Strand ShowNet SN110 is built around the NETsilicon NS7520 system-on-chip,
a specialized ARM-based network processor from the early 2000s.

## NS7520 SoC (NET+ARM)

| Feature | Details |
|---------|---------|
| **Core** | ARM7TDMI (ARMv4T), ~55 MHz |
| **Performance** | 14.6 BogoMIPS |
| **MMU** | None -- runs uClinux (Linux 2.0.38) |
| **UARTs** | 2 x serial -- drive the DMX ports |
| **Ethernet** | Built-in 10/100 MAC |
| **DMA** | Available for UART transfers |
| **GPIO** | Control signals (link LEDs, etc.) |
| **Package** | 208-pin QFP |

## Board Details

| Item | Value |
|------|-------|
| **Manufacturer** | Strand Lighting |
| **Part number** | 220-604-0 |
| **Revision** | V2.6.11 |
| **Date code** | 16/08/2004 |
| **MAC prefix** | 00:E0:01 (Strand Lighting OUI) |

## Memory

| Type | Size | Notes |
|------|------|-------|
| **RAM** | 14 MB | |
| **Flash** | ~3 MB | Minix filesystem |

The flash uses a Minix v1 filesystem, the only filesystem supported by this
uClinux build. Files written to flash persist across reboots; files in `/tmp/`
(RAM disk) do not.

## Flash Memory Layout

| Block | Device | Purpose |
|-------|--------|---------|
| blk0 | `/dev/blk0` | Boot loader region |
| blk1 | `/dev/blk1` | Unknown |
| blk2 | `/dev/blk2` | Unknown |
| blk3 | `/dev/blk3` | Backup firmware image |
| blk4 | `/dev/blk4` | Unknown |
| blk5 | `/dev/blk5` | Main firmware image |
| blk6 | `/dev/blk6` | Unknown |

The Minix root filesystem is mounted from `/dev/root` (likely blk5 or a
composite of blocks).

## Ports and Connectors

| Port | Type | Purpose |
|------|------|---------|
| **DMX 1** | 5-pin XLR | DMX512 I/O (configurable TX/RX) |
| **DMX 2** | 5-pin XLR | DMX512 I/O (configurable TX/RX) |
| **Ethernet** | RJ45 | 10/100 Ethernet |
| **LCD** | 96x32 pixel panel | Status display (16x4 characters, CP437 font) |

## Datasheets

- [NS7520 Data Sheet (DigiKey)](https://media.digikey.com/pdf/Data%20Sheets/Digi%20International%20PDFs/NS7520.pdf)
- [NS7520 Data Sheet v3 (Digi)](https://ftp1.digi.com/support/documentation/prd_nap_ns7520_ds.pdf)
- [NS7520 Hardware Reference Manual](https://docs.digi.com/resources/documentation/digidocs/pdfs/90000353.pdf)
