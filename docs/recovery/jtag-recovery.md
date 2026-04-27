# JTAG Recovery

JTAG recovery for a bricked SN110 using OpenOCD. This is the last-resort
recovery method that gives direct read/write access to the flash chip through
the CPU's debug interface.

For background on how the SN110 gets into a state requiring this, see
[Network Recovery](network-recovery.md).

---

## Hardware

| Component | Details |
|-----------|---------|
| **SoC** | Digi NS7520 (ARM7TDMI), 208-QFP package |
| **Flash** | Intel TE28F320 (4MB, 16-bit, CFI-compatible) |
| **JTAG adapter** | Any ARM JTAG adapter supported by OpenOCD (e.g., J-Link, ST-Link, FTDI-based) |

---

## Wiring

### Connections to NS7520

| Signal | NS7520 208-QFP Pin | Notes |
|--------|-------------------|-------|
| TMS | **172** | |
| TCK | **173** | |
| TDO | **170** | |
| TDI | **171** | |
| TRST* | **174** | Optional -- tie high via 10k if JTAG scan fails |
| RESET* | **158** | Optional but recommended |
| GND | **157** (or any GND) | |
| VTref | 3.3V rail | Tap from a VCC pin (e.g., pin 26) |

The JTAG pins are clustered together on the QFP package (pins 170-174), with
GND at pin 157 and RESET* at pin 158.

Check the **via pads** near the NS7520 first -- two columns of vias along the
left edge of the board near pins 157-208 may be JTAG breakouts. Use a
multimeter continuity test from each via to pins 170-174 before soldering
directly on the QFP leads.

---

## Software Setup

### Install OpenOCD

=== "macOS"

    ```bash
    brew install open-ocd
    ```

=== "Linux"

    ```bash
    sudo apt install openocd
    ```

### OpenOCD Configuration File

Save as `sn110.cfg` in your working directory:

```tcl
# Adapter -- change this to match your hardware
source [find interface/jlink.cfg]
transport select jtag

# Start slow -- increase after verifying the chain works
adapter speed 100

# NS7520 = ARM7TDMI, JTAG IR length = 4 bits
set _CHIPNAME ns7520
jtag newtap $_CHIPNAME cpu -irlen 4

# Target
set _TARGETNAME $_CHIPNAME.cpu
target create $_TARGETNAME arm7tdmi -chain-position $_CHIPNAME.cpu

# Intel CFI flash at address 0x00000000
# Parameters: base size chip_width bus_width target
flash bank $_CHIPNAME.flash cfi 0x00000000 0x400000 2 2 $_TARGETNAME
```

---

## Procedure

### Step 1: Verify JTAG Connection

Power on the SN110, then start OpenOCD:

```bash
openocd -f sn110.cfg
```

**Success** looks like:

```
Info : JTAG tap: ns7520.cpu tap/device found: 0x????????
Info : ns7520.cpu: hardware has 2 breakpoints, 1 watchpoint
```

**Failure** -- "JTAG scan chain interrogation failed":

- Check wiring, especially GND
- Try `adapter speed 10` (slower clock)
- Verify VTref is reading ~3.3V
- Check that TRST* is not floating low (tie high via 10k if needed)

Leave OpenOCD running for the following steps.

### Step 2: Connect to OpenOCD

In a second terminal:

```bash
telnet localhost 4444
```

### Step 3: Halt the CPU

```
> halt
```

The CPU must be halted before any memory/flash operations. If the device is in
a boot loop, this may take a couple of tries -- the CPU resets every ~3 seconds.
Just run `halt` again if it doesn't take on the first attempt.

### Step 4: Dump the Entire Flash (DO THIS FIRST)

!!! danger "This is the most important step"
    Before touching anything, dump the full flash contents to a file. This is
    your complete backup of the device, including the bootloader, kernel, and
    filesystem.

```
> flash probe 0
> dump_image sn110_full_flash.bin 0x00000000 0x400000
```

This reads 4MB (0x400000 bytes) starting at address 0. The dump takes a few
minutes at 100kHz JTAG speed. You can increase speed after the first read:

```
> adapter speed 1000
> dump_image sn110_full_flash_fast.bin 0x00000000 0x400000
```

**Verify both dumps match:**

```bash
md5 sn110_full_flash.bin sn110_full_flash_fast.bin
```

### Step 5: Analyze the Dump

```bash
# Look at the first bytes -- bootloader starts here
hexdump -C sn110_full_flash.bin | head -20

# Search for Minix filesystem magic number (0x138F little-endian)
hexdump -C sn110_full_flash.bin | grep "8f 13"

# Search for known strings
strings -t x sn110_full_flash.bin | grep "lxnetdmx"
strings -t x sn110_full_flash.bin | grep "220node.cfg"

# Find compressed data (gzip magic: 1f 8b)
hexdump -C sn110_full_flash.bin | grep "1f 8b"
```

Key things to identify:

- **Bootloader region**: offset 0x00000 to ~0x10000 (first 64KB)
- **Application image start**: where the compressed kernel+filesystem blob begins
- **Filesystem region**: where the Minix filesystem lives after decompression

### Step 6: Write a Known-Good Image

If you have a flash dump from a working identical SN110:

```
> halt
> flash write_image erase good_flash.bin 0x00000000
> verify_image good_flash.bin 0x00000000
> reset run
```

!!! tip "Sector-level operations"
    If full-image write is too slow or fails, you can operate on individual
    sectors:

    ```
    > flash erase_sector 0 <first> <last>
    > flash write_image erase repaired_section.bin 0x<offset>
    ```

### Step 7: Verify Recovery

After `reset run`:

1. Watch the Ethernet port -- link LED should come up within 10-15 seconds
2. Try `ping` the device's known IP
3. Try `telnet` to verify full functionality
4. Verify DMX operation

---

## Troubleshooting

### "Error: timed out while waiting for target halted"

The CPU is in a tight boot loop. Try:

- Run `halt` repeatedly -- you need to catch it between resets
- Use `reset halt` if nRESET is connected
- Increase JTAG speed to `adapter speed 1000`

### "Error: flash bank not probed"

Run `flash probe 0` before any flash operations. If probe fails:

- Verify the flash bank config (base address, chip/bus width)
- Try `adapter speed 100` or slower
- The flash base address might not be 0x00000000 -- try 0x10000000

### "Error: error writing to flash"

- Verify the CPU is halted
- Check for write-protected sectors: `flash protect_check 0`
- Verify VCC is stable (flash erase/write draws more current)

### Dump is all 0xFF

You're reading erased/unmapped flash. The base address is likely wrong. Try
`dump_image test.bin 0x10000000 0x100` and check the NS7520 memory map.

---

## References

- [OpenOCD User's Guide](https://openocd.org/doc/html/index.html)
- [OpenOCD Flash Commands](https://openocd.org/doc/html/Flash-Commands.html)
- [NS7520 Hardware Reference Manual](https://hub.digi.com/dp/path=/support/asset/ns7520-hardware-reference-manual/)
