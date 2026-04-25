# SN110 JTAG Recovery Guide

JTAG recovery for a bricked SN110 using a SEGGER J-Link EDU Mini and OpenOCD.
This is the last-resort recovery method when the bootloader's CRC passes but the
filesystem is corrupt (so BOOTP recovery doesn't trigger).

For background on how the SN110 gets into this state, see `recovery.md`.

---

## Hardware

- **SoC**: Digi NS7520 (ARM7TDMI), 208-QFP package
- **Flash**: Intel TE28F320 (4MB, 16-bit, CFI-compatible)
- **JTAG adapter**: SEGGER J-Link EDU Mini (USB-C, 10-pin 1.27mm Cortex Debug connector)

---

## Wiring

### J-Link EDU Mini 10-Pin Connector

```
         ┌─────────┐
  VTref  │ 1     2 │  SWDIO/TMS
   GND   │ 3     4 │  SWCLK/TCK
   GND   │ 5     6 │  SWO/TDO
  (key)  │ 7     8 │  TDI
  GNDdet │ 9    10 │  nRESET
         └─────────┘
```

### Connections to NS7520

| J-Link Pin | Signal | NS7520 208-QFP Pin | Notes |
|------------|--------|-------------------|-------|
| 1 (VTref) | VTref | 3.3V rail | Tap from a VCC pin (e.g., pin 26) or any 3.3V test point. Tells the J-Link what voltage the target uses. |
| 2 (TMS) | TMS | **172** | |
| 3 or 5 (GND) | GND | **157** (or any GND) | |
| 4 (TCK) | TCK | **173** | |
| 6 (TDO) | TDO | **170** | |
| 8 (TDI) | TDI | **171** | |
| 10 (nRESET) | RESET* | **158** | Optional but recommended |

TRST* (NS7520 pin 174) has no dedicated pin on the 10-pin connector. This is
usually fine — OpenOCD can work without it. If JTAG scan fails, tie TRST* high
(to VCC through a 10k resistor) to keep it deasserted.

### NS7520 208-QFP JTAG Pin Cluster

The JTAG pins are clustered together on the QFP package, which makes wiring
easier:

```
Pin 170 — TDO
Pin 171 — TDI
Pin 172 — TMS
Pin 173 — TCK
Pin 174 — TRST*
    ...
Pin 157 — GND
Pin 158 — RESET*
```

### Soldering Tips

- Use **30 AWG wire-wrap wire** or enameled magnet wire for QFP pins
- Keep wires **under 10cm** — JTAG is noise-sensitive at higher clock speeds
- Check the **via pads** near the NS7520 first — two columns of vias along
  the left edge of the board near pins 157-208 may be JTAG breakouts. Use a
  multimeter continuity test from each via to pins 170-174 before committing
  to soldering directly on the QFP leads
- If using the vias, standard 0.1" pin headers can be soldered in for a
  reusable connection

### Recommended Wiring Order

1. **VTref** — find a 3.3V test point on the board (easiest connection,
   confirms J-Link sees target power)
2. **GND** — connect to any ground point
3. **TMS, TCK, TDO** — these 3 wires are enough for a JTAG scan test
4. **TDI** — needed for actual read/write operations
5. **nRESET** — add last, lets OpenOCD reset the CPU without power cycling

---

## Software Setup

### Install OpenOCD

```bash
# macOS
brew install open-ocd

# Linux (Debian/Ubuntu)
sudo apt install openocd

# Verify
openocd --version
```

### OpenOCD Configuration File

Save as `sn110.cfg` in your working directory:

```tcl
# SEGGER J-Link
source [find interface/jlink.cfg]
transport select jtag

# Start slow — increase after verifying the chain works
adapter speed 100

# NS7520 = ARM7TDMI, JTAG IR length = 4 bits
set _CHIPNAME ns7520
jtag newtap $_CHIPNAME cpu -irlen 4

# Target
set _TARGETNAME $_CHIPNAME.cpu
target create $_TARGETNAME arm7tdmi -chain-position $_CHIPNAME.cpu

# Intel CFI flash at address 0x00000000
# Parameters: base size chip_width bus_width target
# chip_width=2 (16-bit), bus_width=2 (16-bit)
flash bank $_CHIPNAME.flash cfi 0x00000000 0x400000 2 2 $_TARGETNAME
```

---

## Procedure

### Step 1: Verify JTAG Connection

Power on the SN110 (via PoE or DC), then start OpenOCD:

```bash
openocd -f sn110.cfg
```

**Success** looks like:
```
Info : J-Link EDU Mini V1 compiled ...
Info : Hardware version: 1.00
Info : JTAG tap: ns7520.cpu tap/device found: 0x????????
Info : ns7520.cpu: hardware has 2 breakpoints, 1 watchpoint
```

**Failure** — "JTAG scan chain interrogation failed":
- Check wiring, especially GND
- Try `adapter speed 10` (slower clock)
- Verify VTref is reading ~3.3V (J-Link LED should be green)
- Check that TRST* is not floating low (tie high via 10k if needed)

Leave OpenOCD running for the following steps.

### Step 2: Connect to OpenOCD

In a second terminal:

```bash
telnet localhost 4444
```

You'll get an OpenOCD command prompt.

### Step 3: Halt the CPU

```
> halt
```

The CPU must be halted before any memory/flash operations. If the device is in
a boot loop, this may take a couple of tries — the CPU resets every ~3 seconds.
Just run `halt` again if it doesn't take on the first attempt.

### Step 4: Dump the Entire Flash (DO THIS FIRST)

**This is the most important step.** Before touching anything, dump the full
flash contents to a file:

```
> flash probe 0
> dump_image sn110_full_flash.bin 0x00000000 0x400000
```

This reads 4MB (0x400000 bytes) starting at address 0. Adjust the size if
`flash probe` reports a different geometry.

The dump takes a few minutes at 100kHz JTAG speed. You can increase speed
after the first successful read:

```
> adapter speed 1000
> dump_image sn110_full_flash_fast.bin 0x00000000 0x400000
```

**Verify both dumps match:**
```bash
md5 sn110_full_flash.bin sn110_full_flash_fast.bin
```

Store this dump safely — it's your complete backup of the device, including the
bootloader, kernel, and filesystem.

### Step 5: Analyze the Dump

In a regular terminal, examine the dump:

```bash
# Look at the first bytes — bootloader starts here
hexdump -C sn110_full_flash.bin | head -20

# Search for minix filesystem magic number (0x138F little-endian)
# The magic is at offset 0x410 within the filesystem partition
hexdump -C sn110_full_flash.bin | grep "8f 13"

# Search for known strings to locate the filesystem
strings -t x sn110_full_flash.bin | grep "lxnetdmx"
strings -t x sn110_full_flash.bin | grep "220node.cfg"
strings -t x sn110_full_flash.bin | grep "#!/bin/sh"

# Find the boundary between bootloader and application image
# Look for a compressed data signature (gzip magic: 1f 8b)
hexdump -C sn110_full_flash.bin | grep "1f 8b"
```

Key things to identify:
- **Bootloader region**: offset 0x00000 to ~0x10000 (first 64KB)
- **Application image start**: where the compressed kernel+filesystem blob begins
- **Filesystem region**: where the minix filesystem lives after decompression
- **Config file location**: where `/etc/220node.cfg` data sits in the filesystem

### Step 6: Repair Strategy

Choose one based on what the dump analysis reveals:

#### Option A: Minimal Filesystem Repair (Best)

If the dump shows the filesystem is mostly intact with just corrupted metadata
from the config overflow:

```bash
# Extract just the filesystem portion (offsets determined from Step 5)
dd if=sn110_full_flash.bin of=filesystem.img bs=1 skip=<fs_offset> count=<fs_size>

# Try to repair with minix fsck
fsck.minix filesystem.img

# Or mount and inspect (Linux only — macOS can't mount minix)
mkdir /tmp/sn110fs
sudo mount -t minix -o loop filesystem.img /tmp/sn110fs
ls -la /tmp/sn110fs/etc/
```

Then patch the repaired filesystem back into the full dump and write it back.

#### Option B: Replace the Config File

If the corruption is limited to `/etc/220node.cfg` growing too large:

1. Find the config file's data blocks in the hex dump
2. Replace with the original config from `dump/firmware/etc/220node.cfg`
3. Fix any inode size fields that reference the old (larger) size
4. Patch the modified filesystem back into the full flash dump

#### Option C: Write a Known-Good Full Image

If you have a flash dump from a working identical SN110:

```
> halt
> flash write_image erase good_flash.bin 0x00000000
> verify_image good_flash.bin 0x00000000
> reset run
```

### Step 7: Write the Repaired Image

Once you have a repaired flash image:

```
> halt

# Erase and write — this takes several minutes for 4MB
> flash write_image erase repaired_flash.bin 0x00000000

# Verify the write was correct
> verify_image repaired_flash.bin 0x00000000

# Boot the device
> reset run
```

If `flash write_image erase` is too slow or fails, you can erase and write
individual sectors:

```
# Erase a specific sector (sector numbers from flash probe output)
> flash erase_sector 0 <first> <last>

# Write just a portion at a specific offset
> flash write_image erase repaired_section.bin 0x<offset>
```

### Step 8: Verify Recovery

After `reset run`:

1. Watch the Ethernet port — the link LED should come up within 10-15 seconds
2. Try `ping 192.168.0.71` (or the device's known IP)
3. Try `telnet 192.168.0.71`
4. If telnet works, verify the filesystem: `ls /etc/`, `cat /etc/220node.cfg`
5. Verify DMX operation

---

## Troubleshooting

### "Error: timed out while waiting for target halted"

The CPU is in a tight boot loop. Try:
- Run `halt` repeatedly — you need to catch it between resets
- Use `reset halt` if nRESET is connected — this resets and immediately halts
- Increase JTAG speed to `adapter speed 1000` so the halt command is faster

### "Error: flash bank not probed"

Run `flash probe 0` before any flash operations. If probe fails:
- Verify the flash bank config (base address, chip/bus width)
- Try `adapter speed 100` or slower
- The flash base address might not be 0x00000000 — check the NS7520 memory
  map (flash can be at 0x00000000 or 0x10000000 depending on CS0 config)

### "Error: error writing to flash"

- Flash write operations require the CPU to be halted
- Some flash sectors may be write-protected — check with `flash protect_check 0`
- Verify VCC is stable (flash erase/write draws more current)

### Dump succeeded but image is all 0xFF

You're reading erased/unmapped flash. The base address is likely wrong. Try:
```
> dump_image test.bin 0x10000000 0x100
```
Check the NS7520 memory map for the correct flash mapping.

### J-Link LED is red

VTref is not detected. The J-Link needs to see the target's voltage on pin 1
to enable its output drivers. Verify 3.3V is present on the VTref wire.

---

## References

- [OpenOCD User's Guide](https://openocd.org/doc/html/index.html)
- [OpenOCD Flash Commands](https://openocd.org/doc/html/Flash-Commands.html)
- [SEGGER J-Link documentation](https://www.segger.com/products/debug-probes/j-link/)
- [NET+Works Hardware Reference Guide (208-QFP pinout)](https://ftp1.digi.com/support/documentation/userguide_hwreferenceguide.pdf)
- [NS7520 Hardware Reference Manual](https://hub.digi.com/dp/path=/support/asset/ns7520-hardware-reference-manual/)
