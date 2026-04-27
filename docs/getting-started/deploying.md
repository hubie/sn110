# Deploying to the Device

The SN110 runs a minimal Linux environment with FTP and Telnet servers always
available. Deployment is a two-step process: upload the binary via FTP, then
run it via Telnet.

!!! warning "Read the recovery guide first"
    Before deploying custom firmware, make sure you understand the
    [recovery options](../recovery/network-recovery.md) in case something goes wrong.
    The device has multiple safety nets, but knowing them in advance saves stress.

## Prerequisites

- SN110 powered on and connected to your network
- Device IP address known (check the LCD display, or scan with `nmap`)
- `curl` and a Telnet client installed on your workstation

## Upload and Run

### Step 1: Upload via FTP

```bash
curl -s -T build/sn110dmx.bflt ftp://192.168.2.231/tmp/sn110dmx
```

This uploads the binary to `/tmp/` on the device. Files in `/tmp/` are lost on
reboot -- this is the safe way to test.

### Step 2: Run via Telnet

```bash
telnet 192.168.2.231
```

Once connected:

```bash
# Stop the factory daemon and watchdog
kill $(cat /var/run/lxnetdmx.pid)
kill 7 32 33    # watchdog (7) and helper processes (32, 33)

# Make the binary executable and run it
chmod +x /tmp/sn110dmx
/tmp/sn110dmx &
```

!!! note "Why kill PIDs 7, 32, 33?"
    PID 7 is the hardware watchdog process. If left running, it will reboot the
    device when the factory daemon stops responding to it. PIDs 32 and 33 are
    factory helper processes that should also be stopped to free the DMX and LCD
    device files.

The daemon logs to stderr, which you'll see in the Telnet session:

```
[sn110dmx] sn110dmx v0.3.0 -- Open-source DMX gateway
[sn110dmx] Strand SN110 firmware
[sn110dmx] Protocol: sACN
[sn110dmx] Port 0: universe 1, mode TX
[sn110dmx] Port 1: universe 2, mode RX
[sn110dmx] Running. Send SIGTERM to stop.
```

### Step 3: Verify

- Check the LCD display -- it should show the boot splash, then switch to the
  status display
- Send sACN data from your lighting console or software (e.g., QLC+) and verify
  DMX output on the connected port

## Stopping the Daemon

```bash
kill $(cat /var/run/lxnetdmx.pid)
```

The daemon writes a PID file at the standard path that the factory boot scripts
monitor. Killing it cleanly shuts down DMX ports and clears the LCD.

## Persistent Installation

To make the firmware survive reboots, copy it to the flash filesystem:

```bash
# Upload to flash (persistent)
curl -s -T build/sn110dmx.bflt ftp://192.168.2.231/usr/bin/sn110dmx

# Via telnet: make executable, replace the factory startup
chmod +x /usr/bin/sn110dmx
```

!!! danger "Flash writes are permanent"
    Writing to `/usr/bin/` modifies the Minix filesystem on flash. Make sure
    you have a working backup of the original firmware before doing this.
    See [Recovery](../recovery/network-recovery.md) for backup procedures.

## Web Interface

The firmware includes a web-based configuration interface. After the daemon starts,
navigate to:

```
http://192.168.2.231/
```

The web UI allows configuring:

- Hostname, IP address, netmask, gateway
- DHCP / static / DHCP-with-static-fallback addressing
- Protocol selection (sACN; Art-Net and ShowNet planned)
- Per-port mode (TX/RX/Off) and universe assignment
- DMX hold time (how long to hold last values after source loss)
- LCD contrast and backlight
