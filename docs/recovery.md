# SN110 BOOTP/TFTP Recovery Guide

This document covers network-based recovery for a bricked SN110 that no longer
responds to telnet, FTP, or HTTP. For devices that still have network services
running, see SAFETY.md (Scenarios 1-2).

---

## Background: How the Boot Process Works

The SN110 runs on a Digi NS7520 (NET+ARM) SoC with a NET+OS-derived firmware
architecture. The flash contains two independent images:

| Component | Flash Location | Purpose |
|-----------|---------------|---------|
| **Bootloader** (rom.bin) | Sector 0 (`/dev/blk0`) | Hardware init, image validation, BOOTP/TFTP recovery |
| **Application** (image.bin) | Later sector(s) (`/dev/blk5`) | uClinux kernel + minix filesystem + lxnetdmx |

On power-up, the bootloader:

1. Initializes the NS7520 hardware (memory controller, Ethernet MAC, chip selects)
2. Validates the application image in flash via a **32-bit CRC on the compressed
   image blob** (not on individual files or the filesystem)
3. **If CRC passes**: Copies the application image into RAM, decompresses, jumps to it
4. **If CRC fails**: Enters a BOOTP/TFTP recovery loop (see below)

The bootloader is very small (<64 KB) and lives in a separate flash sector from
the application. It is significantly harder to corrupt than the application —
you would have to overwrite `/dev/blk0` directly.

### Why CRC Validation Matters for Recovery

The bootloader's CRC covers the **raw compressed image blob**, not the filesystem
contents within it. This means:

- **Filesystem corruption** (e.g., from a config save that overflowed the minix
  filesystem) does NOT invalidate the CRC — the compressed image blob in flash
  is unchanged.
- The kernel boots successfully, but panics when it tries to mount the corrupted
  root filesystem.
- The device enters a **3-second boot loop** (kernel boots → mount fails → panic
  → watchdog reset), but never enters BOOTP recovery because the CRC passes.
- Symptoms: PoE power draw is normal, Ethernet port never comes "UP" (no link-
  layer negotiation), no BOOTP packets on the wire.

This is the most likely bricking scenario for the SN110: the flash image CRC is
fine, so the bootloader thinks everything is OK, but the filesystem inside is
corrupted. **BOOTP recovery will not trigger automatically in this case.** See
"Force Recovery Mode" below for hardware-level alternatives.

---

## Step 1: Confirm the Bootloader Is Alive

Connect the bricked device directly to your computer's Ethernet port (or via a
simple unmanaged switch — avoid managed switches with IGMP snooping or port
security that might block BOOTP broadcasts).

Set your computer's Ethernet to a static IP on the same subnet the device
would normally use:

```
IP: 192.168.0.1
Subnet: 255.255.255.0
```

### Monitor for BOOTP Requests

**macOS:**
```bash
sudo tcpdump -i en0 -n 'udp port 67 or udp port 68'
```

**Linux:**
```bash
sudo tcpdump -i eth0 -n 'udp port 67 or udp port 68'
```

**Wireshark (any OS):**
Filter: `bootp || dhcp`

Power cycle the device and watch for **30-60 seconds**. If the bootloader is
alive and the application image is corrupt, you should see BOOTP/DHCP Discover
packets from the device's MAC address.

SN110 MAC addresses use the Strand Lighting OUI: **00:E0:01:xx:xx:xx**

### Interpreting Results

- **BOOTP requests visible**: The bootloader is alive and actively seeking a
  TFTP server to download a replacement image. Proceed to Step 2.

- **No BOOTP requests after 60 seconds**: This is most likely because the
  bootloader's CRC validation passes (the compressed image blob is intact)
  even though the filesystem inside is corrupt. Try a different Ethernet cable
  and port just in case. If still nothing, see "Force Recovery Mode" below
  for hardware-level options (PORTC5/INIT pin, JTAG, serial console).

---

## Step 2: Set Up DHCP + TFTP Server

The bootloader needs two things from the network:

1. **DHCP/BOOTP**: An IP address assignment so it can communicate
2. **TFTP**: A firmware image file to download and flash

### macOS (using dnsmasq)

```bash
# Install dnsmasq
brew install dnsmasq

# Create TFTP root directory
mkdir -p /tmp/tftp-root

# Place your firmware image here (see Step 3)
# cp <your-image-file> /tmp/tftp-root/image.bin

# Set your Ethernet to static IP first:
#   System Settings → Network → Ethernet → Configure IPv4: Manually
#   IP: 192.168.0.1, Subnet: 255.255.255.0, Router: (leave blank)

# Run dnsmasq — adjust en0 to your Ethernet interface name
sudo dnsmasq \
  --no-daemon \
  --interface=en0 \
  --bind-interfaces \
  --dhcp-range=192.168.0.100,192.168.0.110,255.255.255.0,12h \
  --enable-tftp \
  --tftp-root=/tmp/tftp-root \
  --bootp-dynamic \
  --log-dhcp \
  --log-queries
```

### Linux (using dnsmasq)

```bash
sudo apt install dnsmasq    # or: dnf install dnsmasq

mkdir -p /tmp/tftp-root

# Set static IP on your Ethernet interface
sudo ip addr add 192.168.0.1/24 dev eth0
sudo ip link set eth0 up

sudo dnsmasq \
  --no-daemon \
  --interface=eth0 \
  --bind-interfaces \
  --dhcp-range=192.168.0.100,192.168.0.110,255.255.255.0,12h \
  --enable-tftp \
  --tftp-root=/tmp/tftp-root \
  --bootp-dynamic \
  --log-dhcp \
  --log-queries
```

### Windows (using Tftpd64)

1. Download [Tftpd64](https://pjo2.github.io/tftpd64/) and install
2. **TFTP tab**: Set base directory to a folder containing your firmware image
3. **DHCP tab**:
   - IP pool starting address: `192.168.0.100`
   - Size of pool: `10`
   - Subnet mask: `255.255.255.0`
   - Boot file: `image.bin` (or whatever filename the device requests — check logs)
4. Set your Ethernet adapter to static IP `192.168.0.1`, mask `255.255.255.0`

### What to Watch For in the Logs

When you power cycle the device with the server running, the logs should show:

1. A DHCP/BOOTP discover from the device's MAC
2. A DHCP offer with an IP assignment
3. A TFTP read request — **note the requested filename** (likely `image.bin`)
4. TFTP data transfer in progress

If you see the TFTP request but don't have the right file, the transfer will
fail — but now you know the exact filename the bootloader expects.

---

## Step 3: Obtain a Recovery Image

This is the hardest part. The bootloader expects a raw binary image in the
format it would write directly to the application flash sector.

### Option A: Dump from an Identical Working Unit (Best Option)

If you have an identical, working SN110, you can dump its application flash:

```bash
# Telnet into the WORKING unit
telnet 192.168.0.71

# Try to dump the main firmware flash block
cat /dev/blk5 > /tmp/flash_dump.bin

# If cat doesn't work with block devices, try:
dd if=/dev/blk5 of=/tmp/flash_dump.bin
```

Then download the dump via FTP:
```bash
# From your computer
curl -o flash_dump.bin -u anonymous:anonymous ftp://192.168.0.71/tmp/flash_dump.bin
```

Place the dump in your TFTP root:
```bash
cp flash_dump.bin /tmp/tftp-root/image.bin
```

**Note**: The filename may need to match what the bootloader requests. Check
your DHCP/TFTP server logs from Step 2 to see the exact filename.

### Option B: Original Strand Firmware (.chk files recovered)

Two original Strand firmware `.chk` files have been recovered from the Internet
Archive Wayback Machine and are stored in `dump/official-firmware/`:

| File | Version | Size |
|------|---------|------|
| `sn110-2_6_11.chk` | V2.6.11 | 1,803,888 bytes |
| `sn110.26d` | V2.6.d | 1,801,256 bytes |

The `.chk` format is: 4-byte LE checksum + 4092 bytes zero padding + payload
(kernel + Minix filesystem image). The payload starts at offset `0x1000` and is
what `wflash` writes directly to flash.

**To flash via the built-in update mechanism:**

1. FTP the `.chk` file to the device:
   ```
   ftp> binary
   ftp> put sn110-2_6_11.chk /tmp/sn110.chk
   ```
2. Telnet in and run the update script:
   ```
   $ /bin/sh /usr/bin/flashsw.sh
   ```
3. The script validates the checksum (`nodecfg checksum`), erases flash
   (`eflash`), writes the image (`wflash`), verifies (`vflash`), and reboots.

**To extract the raw payload for BOOTP/TFTP recovery:**
```bash
dd if=dump/official-firmware/sn110-2_6_11.chk of=image.bin bs=4096 skip=1
```
This strips the 4096-byte header (checksum + padding), leaving the raw flash
image that the bootloader expects via TFTP.

For full details on the `.chk` format and checksum algorithm, see
`docs/solutions/reverse-engineering/strand-chk-firmware-checksum-algorithm.md`.

### Option C: Reconstruct from File Backup

If you have a complete file-by-file backup of the filesystem (in `dump/firmware/`),
it may be possible to reconstruct a flashable image. This requires understanding:

- The minix filesystem layout used by the SN110's uClinux
- The flash sector size and alignment
- Any header/checksum expected by the bootloader

This is the most complex option and may require writing a custom tool.

---

## Step 4: Flash the Image

Once the DHCP+TFTP server is running with the correct image file:

1. Power cycle the bricked SN110
2. Watch the server logs for BOOTP request → DHCP offer → TFTP transfer
3. **Wait at least 2-3 minutes** — the device is burning the downloaded image
   to flash. Do NOT power cycle during this process.
4. Watch for the **green Ethernet LED** to begin blinking at a steady rate —
   this indicates flashing is complete
5. Power cycle the device
6. Verify recovery: `ping 192.168.0.71` (or whatever IP it received via DHCP)
7. Try `telnet` and `ftp` to confirm full functionality

---

## Force Recovery Mode (Hardware)

When the bootloader's CRC passes but the filesystem is corrupt (the most common
bricking scenario), BOOTP recovery won't trigger automatically. These hardware
methods can force the bootloader into recovery or allow direct reflashing.

### NS7520 Pin Reference (208-QFP Package)

The SN110 uses the NS7520 in the **208-QFP** (quad flat pack) package. Pin
numbers below are from the NET+Works Hardware Reference Guide (8833198D), Table
1-1, 208QFP column.

**JTAG pins** (clustered at pins 170-174):

| Signal | 208-QFP Pin | Description |
|--------|------------|-------------|
| TDO    | 170        | Test Data Out |
| TDI    | 171        | Test Data In |
| TMS    | 172        | Test Mode Select |
| TCK    | 173        | Test Mode Clock |
| TRST*  | 174        | Test Mode Reset (active low) |
| RESET* | 158        | System Reset (active low) |

**GPIO / Port C pins** (pins 56-63):

| Signal | Alt Function | 208-QFP Pin |
|--------|-------------|-------------|
| PORTC7 | OUT2A* / TxCA | 56 |
| PORTC6 | RIA* / IRQ* | 57 |
| PORTC5 | OUT2B* / TxCB | 58 |
| PORTC4 | RIB* / RESET* | 59 |
| PORTC3 | AMUX / CI3 | 60 |
| PORTC2 | CI2 | 61 |
| PORTC1 | CI1 | 62 |
| PORTC0 | CI0 | 63 |

**Bootstrap config pins** (active-low, address bus sampled during reset):

| Signal | 208-QFP Pin | Bootstrap Function |
|--------|------------|-------------------|
| ADDR[27] | (see pinout) | Endian config (0=Little, 1=Big) |
| ADDR[26] | (see pinout) | CPU Bootstrap (0=ARM disabled, 1=ARM enabled) |
| ADDR[25] | (see pinout) | Bus arbiter (0=External, 1=Internal) |
| ADDR[24:23] | (see pinout) | CS0 config (memory type/width) |

### PORTC5 / INIT Pin (Pin 58)

On Digi Connect ME modules running NET+OS, grounding **PORTC5** (pin 58) and
**MFGI** (manufacturing GPIO input) during power-up forces the bootloader into
BOOTP/TFTP download mode regardless of flash image validity. This is implemented
by the `shouldDownloadImage()` function in the NET+OS bootloader (`rom.bin`).

**Important caveat**: This is a **NET+OS software convention**, not a silicon
feature. The NET+ARM chip itself does not have a hardware "force download" mode
— the bootstrap mechanism uses address bus pins (ADDR[27:0]) sampled during
reset for chip configuration only. Whether Strand's bootloader checks PORTC5
is unknown. It's worth trying, but not guaranteed.

**To try**: Locate pin 58 on the 208-QFP package (top side of the chip, counting
counterclockwise from pin 1). Ground it to GND during power-up. If nothing
happens, also try grounding it together with a nearby GND pin. Release after
2-3 seconds, then check for BOOTP packets on the network.

### JTAG

The NS7520 supports JTAG debugging and flash programming via the standard
ARM7TDMI JTAG interface (pins 170-174 on the 208-QFP). On the SN110 PCB
(Strand p/n 220-604-0, rev V2.6.11), there are groups of **unpopulated vias**
near the NS7520 (two columns of vias along the left edge of the board near
pins 157-208). These may be a JTAG breakout.

Requirements:

- Soldering wires or a pin header to the JTAG pads
- A JTAG adapter compatible with ARM7TDMI (e.g., J-Link, Olimex ARM-USB-OCD,
  or any OpenOCD-compatible adapter)
- The complete flash image (bootloader + application) to write
- OpenOCD configuration for the NS7520 (ARM7TDMI core, Intel flash)

```bash
# Example OpenOCD config (untested — adapt to your adapter)
# ns7520.cfg
source [find interface/jlink.cfg]     # or your adapter
transport select jtag
adapter speed 100

set _CHIPNAME ns7520
jtag newtap $_CHIPNAME cpu -irlen 4 -expected-id 0x00000000

set _TARGETNAME $_CHIPNAME.cpu
target create $_TARGETNAME arm7tdmi -chain-position $_CHIPNAME.cpu

# Intel TE28F320 flash (4MB, 16-bit)
flash bank $_CHIPNAME.flash cfi 0x00000000 0x400000 2 2 $_TARGETNAME
```

### Serial Console

The NS7520's two UART ports ARE the DMX ports. Accessing a serial console would
require tapping the UART lines at the chip/board level, not through the XLR
connectors (which have RS-485 transceivers in the signal path). The relevant
pins are:

- **Serial A TX** (TXDA): PORTB6 = pin 46 (208-QFP)
- **Serial A RX** (RXDA): PORTB1 = pin 54 (208-QFP)

This likely requires soldering. The bootloader may output diagnostic messages
on Serial A at boot.

### Digi NetosProg Utility

The `netosprog.exe` utility (found in the NET+OS SDK at `C:\netos75\bin\`)
uses the ADDP (Advanced Device Discovery Protocol) for initial device setup.
However, this only works on devices that already have a functioning bootloader
and EOS (Embedded Operating System) image. It will NOT help if the bootloader
is corrupted.

ADDP uses UDP multicast on **224.0.5.128:2362**. You can try discovering the
device with the open-source ADDP implementations:

```bash
# Python
pip install addp

# Or clone the C implementation
git clone https://github.com/christophgysin/addp
```

ADDP can only discover devices and reconfigure their network settings — it
**cannot upload firmware**.

---

## Key Limitations

- The bootloader validates the **compressed image blob CRC**, not the
  filesystem contents. A corrupt filesystem (e.g., from a config save that
  overflowed the minix partition) will NOT trigger BOOTP recovery — the
  bootloader thinks the image is fine.

- The bootloader **cannot reload itself** via TFTP. It can only replace the
  application image. If the bootloader flash sector is corrupted, network
  recovery is not possible — JTAG is the only option.

- The bootloader writes TFTP-received data **directly to flash**. The data
  must be in the exact binary format expected — not individual files or a
  compressed archive.

- The TFTP filename may be hardcoded in the bootloader. The standard NET+OS
  bootloader expects `image.bin`, but Strand may have customized this. Always
  check the TFTP server logs to see what filename the device requests.

- The bootloader's BOOTP requests use the device's burned-in MAC address.
  If you don't know the MAC, watch for any BOOTP requests from the
  `00:E0:01:xx:xx:xx` range (Strand OUI) in your packet capture.

- The **`/INIT` (PORTC5) force-download** mechanism is a NET+OS software
  convention. The SN110's Strand bootloader may or may not implement it.

---

## References

- [Digi: How firmware downloading works (NET+OS)](https://forums.digi.com/t/how-firmware-downloading-works/5234)
- [Digi: Production firmware update on NET+OS](https://www.digi.com/support/knowledge-base/how-to-production-firmware-update-on-net-os-based)
- [Digi: Why is rom.bin flashing required?](https://forums.digi.com/t/why-is-rom-bin-flashing-required/5099)
- [NS7520 Hardware Reference Manual](https://hub.digi.com/dp/path=/support/asset/ns7520-hardware-reference-manual/)
- [NS7520 Datasheet (Mouser)](https://www.mouser.com/datasheet/2/111/dgncs00013_1-2265329.pdf)
- [NET+Works Hardware Reference Guide (8833198D)](https://ftp1.digi.com/support/documentation/userguide_hwreferenceguide.pdf) — contains 208-QFP pin table
- [NET+Works BSP Porting Guide](https://hub.digi.com/dp/path=/support/asset/net-works-6-3-with-gnu-tools-bsp-porting-guide/)
- [ADDP Protocol documentation](https://github.com/christophgysin/addp/blob/master/doc/protocol)
- [Python ADDP library](https://github.com/zdavkeos/addp)
