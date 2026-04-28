# Deploying to the Device

The SN110 runs a minimal Linux environment with FTP and Telnet servers always
available. There are two deployment methods: automated scripts (recommended) and
manual deployment.

!!! warning "Read the recovery guide first"
    Before deploying custom firmware, make sure you understand the
    [recovery options](../recovery/network-recovery.md) in case something goes wrong.
    The device has multiple safety nets, but knowing them in advance saves stress.

## Prerequisites

- SN110 powered on and connected to your network
- Device IP address known (check the LCD display, or scan with `nmap`)
- `curl` installed on your workstation
- Firmware binary built (`make docker-bflt` produces `build/sn110dmx.bflt`)

## Automated Install (Recommended)

The `tools/install.sh` script handles the full deployment in one command:

```bash
./tools/install.sh 192.168.2.231
```

This script:

1. Checks that the firmware binary exists and fits within the size budget
2. Verifies the device is reachable
3. Downloads the original `lxnetdmx` as a local backup (first time only)
4. Uploads the backup to `/usr/bin/lxnetdmx.bak` on the device
5. Uploads the new firmware as `/usr/bin/lxnetdmx`
6. Uploads a device-side install script and trigger file via FTP
7. The device's `/etc/rc` watchdog detects the trigger within ~10 seconds,
   stops the running daemon, sets permissions, and auto-starts the new firmware

The firmware persists across reboots -- the device's boot scripts start
`/usr/bin/lxnetdmx` automatically.

### Verifying

- The LCD display should show the boot splash, then switch to the status display
- Send sACN data from your lighting console or software (e.g., QLC+) and verify
  DMX output on the connected port
- `telnet` to the device -- the process list should show `sn110dmx`

### Rollback

To restore the original Strand firmware:

```bash
./tools/restore.sh 192.168.2.231
```

This restores from either your local backup (`dump/firmware/usr/bin/lxnetdmx`)
or the device's own backup (`/usr/bin/lxnetdmx.bak`), using the same trigger
mechanism.

## Manual Deployment (Testing)

For iterative development, you can upload and run from RAM without touching
flash. Files in `/tmp/` are lost on reboot -- this is the safe way to test.

### Step 1: Upload via FTP

```bash
curl -s -T build/sn110dmx.bflt ftp://192.168.2.231/tmp/sn110dmx
```

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

### Stopping the Daemon

```bash
kill $(cat /var/run/lxnetdmx.pid)
```

The daemon writes a PID file at the standard path that the factory boot scripts
monitor.

## How the Install Mechanism Works

The device's `/etc/rc` boot script runs a watchdog loop that checks every 10
seconds for a trigger file at `/tmp/install.arm`. When found, it executes
`/tmp/install.sh` (the device-side install script uploaded by `tools/install.sh`).

This is the same mechanism the factory firmware uses for updates. The open
firmware install scripts piggyback on it rather than inventing a new process.

Key files:

| File | Runs on | Purpose |
|------|---------|---------|
| `tools/install.sh` | Your computer | Orchestrates the full install over FTP |
| `tools/restore.sh` | Your computer | Orchestrates rollback over FTP |
| `tools/device/install.sh` | The device | Stops daemon, sets permissions (triggered by `/etc/rc`) |
| `tools/device/rollback.sh` | The device | Restores original firmware (triggered by `/etc/rc`) |

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
