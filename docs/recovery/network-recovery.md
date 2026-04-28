# Network Recovery (BOOTP/TFTP)

This guide covers network-based recovery for an SN110 that no longer responds
to telnet, FTP, or HTTP. For devices that still have network services running,
you can upload firmware directly via FTP -- see
[Deploying](../getting-started/deploying.md).

!!! warning "Untested procedure"
    The BOOTP/TFTP recovery procedure described here is based on Digi NS7520
    documentation and has not been fully verified on SN110 hardware. Use it as
    a starting point, not a guaranteed recipe.

---

## Background: How the Boot Process Works

The SN110 runs on a Digi NS7520 (NET+ARM) SoC. The flash contains two
independent images:

| Component | Flash Location | Purpose |
|-----------|---------------|---------|
| **Bootloader** (rom.bin) | Sector 0 (`/dev/blk0`) | Hardware init, image validation, BOOTP/TFTP recovery |
| **Application** (image.bin) | Later sector(s) (`/dev/blk5`) | uClinux kernel + Minix filesystem + lxnetdmx |

On power-up, the bootloader:

1. Initializes the NS7520 hardware (memory controller, Ethernet MAC, chip selects)
2. Validates the application image in flash via a **32-bit CRC on the compressed
   image blob** (not on individual files or the filesystem)
3. **If CRC passes**: Copies the application image into RAM, decompresses, jumps to it
4. **If CRC fails**: Enters a BOOTP/TFTP recovery loop (see below)

The bootloader is very small (<64 KB) and lives in a separate flash sector from
the application.

---

## Step 1: Confirm the Bootloader Is Alive

Connect the device directly to your computer's Ethernet port (or via a simple
unmanaged switch).

Set your computer's Ethernet to a static IP on the same subnet:

```
IP: 192.168.0.1
Subnet: 255.255.255.0
```

### Monitor for BOOTP Requests

=== "macOS"

    ```bash
    sudo tcpdump -i en0 -n 'udp port 67 or udp port 68'
    ```

=== "Linux"

    ```bash
    sudo tcpdump -i eth0 -n 'udp port 67 or udp port 68'
    ```

=== "Wireshark (any OS)"

    Filter: `bootp || dhcp`

Power cycle the device and watch for **30-60 seconds**. If the bootloader is
alive and the application image CRC fails, you should see BOOTP/DHCP Discover
packets from the device's MAC address.

!!! tip
    SN110 MAC addresses use the Strand Lighting OUI: **00:E0:01:xx:xx:xx**

### Interpreting Results

- **BOOTP requests visible**: The bootloader is seeking a TFTP server to
  download a replacement image. Proceed to Step 2.

- **No BOOTP requests after 60 seconds**: The bootloader's CRC validation
  likely passes (the compressed image blob is intact). Try a different Ethernet
  cable and port. If still nothing, see
  [Force Recovery Mode](#force-recovery-mode-hardware) or
  [JTAG Recovery](jtag-recovery.md).

---

## Step 2: Set Up DHCP + TFTP Server

The bootloader needs two things from the network:

1. **DHCP/BOOTP**: An IP address assignment
2. **TFTP**: A firmware image file to download and flash

=== "macOS (dnsmasq)"

    ```bash
    brew install dnsmasq

    mkdir -p /tmp/tftp-root
    # cp <your-image-file> /tmp/tftp-root/image.bin

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

=== "Linux (dnsmasq)"

    ```bash
    sudo apt install dnsmasq

    mkdir -p /tmp/tftp-root

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

=== "Windows (Tftpd64)"

    1. Download [Tftpd64](https://pjo2.github.io/tftpd64/)
    2. **TFTP tab**: Set base directory to a folder containing your firmware image
    3. **DHCP tab**: Pool starting at `192.168.0.100`, size `10`, mask `255.255.255.0`,
       boot file `image.bin`
    4. Set your Ethernet adapter to static IP `192.168.0.1`, mask `255.255.255.0`

### What to Watch For in the Logs

When you power cycle the device with the server running:

1. A DHCP/BOOTP discover from the device's MAC
2. A DHCP offer with an IP assignment
3. A TFTP read request -- **note the requested filename** (likely `image.bin`)
4. TFTP data transfer in progress

If you see the TFTP request but don't have the right file, the transfer will
fail -- but now you know the exact filename the bootloader expects.

---

## Step 3: Obtain a Recovery Image

### Option A: Dump from an Identical Working Unit

If you have an identical, working SN110:

```bash
# Telnet into the WORKING unit
telnet 192.168.0.71

# Dump the main firmware flash block
cat /dev/blk5 > /tmp/flash_dump.bin
```

Then download the dump via FTP:

```bash
curl -o flash_dump.bin ftp://192.168.0.71/tmp/flash_dump.bin
```

Place the dump in your TFTP root as `image.bin` (or whatever filename the
bootloader requested in Step 2).

### Option B: Original Strand Firmware (.chk files)

Two original Strand firmware `.chk` files have been recovered:

| File | Version | Size |
|------|---------|------|
| `sn110-2_6_11.chk` | V2.6.11 | 1,803,888 bytes |
| `sn110.26d` | V2.6.d | 1,801,256 bytes |

The `.chk` format is: 4-byte LE checksum + 4092 bytes zero padding + payload.
The payload starts at offset `0x1000`.

**To extract the raw payload for BOOTP/TFTP recovery:**

```bash
dd if=sn110-2_6_11.chk of=image.bin bs=4096 skip=1
```

**To flash via the built-in update mechanism** (requires a working device):

```bash
curl -s -T sn110-2_6_11.chk ftp://192.168.2.231/tmp/sn110.chk
# Telnet in and run:
/bin/sh /usr/bin/flashsw.sh
```

---

## Step 4: Flash the Image

Once the DHCP+TFTP server is running with the correct image file:

1. Power cycle the SN110
2. Watch the server logs for BOOTP request -> DHCP offer -> TFTP transfer
3. **Wait at least 2-3 minutes** -- the device may be burning the image to flash.
   Do NOT power cycle during this process.
4. Power cycle the device
5. Verify recovery: `ping` the device's IP, then try `telnet` and `ftp`

---

## Force Recovery Mode (Hardware)

If BOOTP recovery doesn't trigger automatically, these hardware methods may
help.

### PORTC5 / INIT Pin (NS7520 Pin 58)

On Digi Connect ME modules running NET+OS, grounding **PORTC5** (pin 58) and
**MFGI** during power-up forces the bootloader into BOOTP/TFTP download mode
regardless of flash image validity.

!!! warning "Not guaranteed on SN110"
    This is a **NET+OS software convention**, not a silicon feature. Whether
    Strand's custom bootloader checks PORTC5 is unknown.

### JTAG

For direct flash access regardless of software state, see
[JTAG Recovery](jtag-recovery.md).

### Serial Console

The NS7520's UART ports are the DMX ports. Accessing a serial console requires
tapping the UART lines at the chip level:

- **Serial A TX** (TXDA): PORTB6 = pin 46 (208-QFP)
- **Serial A RX** (RXDA): PORTB1 = pin 54 (208-QFP)

The bootloader may output diagnostic messages on Serial A at boot.

---

## Key Limitations

- The bootloader validates the **compressed image blob CRC**, not the filesystem
  contents.
- The bootloader **cannot reload itself** via TFTP. If the bootloader flash
  sector is corrupted, JTAG is the only option.
- The TFTP filename may be hardcoded. Check your server logs for the exact
  filename the device requests.

---

## References

- [Digi: How firmware downloading works (NET+OS)](https://forums.digi.com/t/how-firmware-downloading-works/5234)
- [Digi: Production firmware update on NET+OS](https://www.digi.com/support/knowledge-base/how-to-production-firmware-update-on-net-os-based)
- [NS7520 Hardware Reference Manual](https://hub.digi.com/dp/path=/support/asset/ns7520-hardware-reference-manual/)
