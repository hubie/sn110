# SN110 Hardware Documentation

## NETsilicon NS7520 (NET+ARM)

- **Architecture**: ARM7TDMI (ARMv4T)
- **No MMU** — runs uClinux (Linux without memory management)
- **2 × Serial ports** (UART) — these drive the DMX ports
- **10/100 Ethernet** — built-in MAC
- **DMA** — available for UART transfers
- **GPIO** — for control signals

### Datasheets
- [NS7520 Data Sheet (DigiKey)](https://media.digikey.com/pdf/Data%20Sheets/Digi%20International%20PDFs/NS7520.pdf)
- [NS7520 Data Sheet v3 (Digi)](https://ftp1.digi.com/support/documentation/prd_nap_ns7520_ds.pdf)
- [NS7520 Hardware Reference Manual](https://docs.digi.com/resources/documentation/digidocs/pdfs/90000353.pdf)

### UART Configuration for DMX512
- Baud rate: 250,000 bps
- Data bits: 8
- Stop bits: 2
- Parity: None
- Break signal: ≥ 88 μs
- Mark After Break (MAB): 8-1000 μs

## Flash Memory Layout

| Block | Device | Purpose |
|-------|--------|---------|
| blk0 | `/dev/blk0` | Unknown (possibly boot loader) |
| blk1 | `/dev/blk1` | Unknown |
| blk2 | `/dev/blk2` | Unknown |
| blk3 | `/dev/blk3` | Backup firmware image |
| blk4 | `/dev/blk4` | Unknown |
| blk5 | `/dev/blk5` | Main firmware image |
| blk6 | `/dev/blk6` | Unknown |

The minix root filesystem is mounted from `/dev/root` (likely blk5 or a
composite of blocks).

## DMX Port Hardware

The two DMX ports are exposed as character devices:
- `/dev/dmx0` — Port 1 (5-pin XLR)
- `/dev/dmx1` — Port 2 (5-pin XLR)

### Known ioctl modes (from `dmxtst` analysis):
- `off` — Port disabled
- `silentoff` — Port disabled quietly
- `rx` — Receive mode (blocking or non-blocking)
- `tx` — Transmit mode (blocking or non-blocking)
- `raw` — Raw access mode
- `test` — Hardware test mode

### DMX frame I/O
- **Write**: Write DMX frame data to device file descriptor
- **Read**: Read DMX frame data from device file descriptor
- Frame format: TBD (needs reverse engineering of dmxtst/lxnetdmx)

## Network Configuration

- Default IP: Assigned via BOOTP or configured in `220node.cfg`
- Netmask: 255.255.255.0
- MAC prefix: 00:E0:01 (Strand Lighting OUI)
- Services: HTTP (80), Telnet (23), FTP (21)
- ShowNet: UDP port 2501

## LCD Display

- Device: `/dev/lcd0`
- Controlled via `sn110lcd` utility
- Supports user messages: `sn110lcd /dev/lcd0 usermsg <text>`
- Supports boot messages: `sn110lcd /dev/lcd0 bootupmsg`
