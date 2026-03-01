# SN110 Safety & Recovery Guide

> **Rule #1: Never flash anything you haven't successfully tested in RAM first.**

This document covers brick prevention, recovery procedures, and the safety
philosophy for this project. The SN110 hardware is irreplaceable — treat every
device as the last one.

---

## Device Recovery Architecture

The SN110 has a surprisingly robust recovery design. Understanding it is key
to working safely.

### What runs independently (always available)

These services are started by `/etc/rc` via `inetd` BEFORE `lxnetdmx` launches,
and they do NOT depend on lxnetdmx:

| Service | Port | Purpose |
|---------|------|---------|
| `telnetd` | TCP 23 | Shell access (no auth!) |
| `aftpd` | TCP 21 | FTP file transfer (anonymous!) |
| `httpd` | TCP 80 | Web configuration UI |

**Even if lxnetdmx is completely broken, deleted, or crashes on startup,
you can still telnet in and FTP files to repair the device.**

### Boot script watchdog (`/etc/rc`)

The boot script loops every 10 seconds and:
1. Checks if `lxnetdmx` exists on disk — if not, skips restart (allows recovery)
2. Checks if `lxnetdmx` is running — restarts it if crashed
3. Checks for `/tmp/install.arm` — triggers install procedure if found

This means:
- If you delete `lxnetdmx`, the watchdog just loops harmlessly
- If your replacement crashes, it gets restarted (which may be undesirable during debugging)
- You can trigger a flash install by uploading files via FTP

### Flash slots

| Block Device | Purpose |
|-------------|---------|
| `/dev/blk5` | **Main firmware** — what boots normally |
| `/dev/blk3` | **Backup firmware** — secondary slot |

The flash tools `eflash` (erase), `wflash` (write), `vflash` (verify) operate on these.

---

## Pre-Work Checklist

Before modifying ANYTHING on the device:

- [ ] **Full firmware backup completed** (check `dump/firmware/` has all files)
- [ ] **All original binaries downloaded** (lxnetdmx, dmxtst, sn110lcd, nodecfg, etc.)
- [ ] **Can successfully telnet to device**
- [ ] **Can successfully FTP to device**
- [ ] **Have documented the device's current IP, netmask, and MAC**
- [ ] **Backup flash slot (`/dev/blk3`) contains working firmware**

### Backing up the flash (CRITICAL — do this first!)

Before any flash modifications, create a backup to the secondary flash slot:

```bash
# Telnet into the device, then:
# This copies the main flash (blk5) to backup (blk3)
# NOTE: Verify this is safe before running — the exact mechanism
# needs confirmation from the flash tools' behavior
```

**Raw flash block dump**: We investigated dumping `/dev/blk5` (the raw flash image)
but the uClinux shell's `cat` doesn't support block devices and `hexdump` is
interactive/paged — impractical for multi-MB images. FTP can't read device files either.

**However, this is OK.** We have a complete file-by-file backup of the entire
filesystem (all 33 files verified with correct sizes). Recovery does NOT require
a raw flash image — we can restore by:
1. FTP uploading individual files back to their original paths
2. Or, if we write a custom flash image builder, we can reconstruct a `.chk` image
   from our file backup

**What we cannot recover from our backup alone:**
- The `.chk` image format/checksum algorithm (needed for `flashsw.sh`)
- The raw flash partition layout / boot block contents

**Mitigations:**
- We preserve `eflash`, `wflash`, `vflash` and `nodecfg` — the tools that
  manage flash — so the device can always self-repair
- File-by-file FTP restore covers the common failure case (bad lxnetdmx binary)
- The kernel and boot loader are in separate flash blocks that we won't touch

---

## Safe Development Workflow

### Level 1: Off-Device Testing (ZERO risk)

Build and test entirely on your development machine:
```bash
make test-host    # Compile for x86, mock DMX devices, run unit tests
make test-sacn    # Test sACN reception with mock multicast
make test-artnet  # Test Art-Net reception with mock UDP
```

Uses POSIX socket API and file-based mock DMX devices on the host.

### Level 2: RAM Testing (very low risk)

Upload binary to `/tmp/` (RAM disk) and run manually:

```bash
# From your development machine:
curl -T build/sn110dmx.bflt -u anonymous:anonymous ftp://192.168.0.71/tmp/sn110dmx

# Telnet in and test:
telnet 192.168.0.71
> # First, stop the existing daemon:
> # (use nodecfg kill or find pid)
> /tmp/sn110dmx
```

**Why this is safe:**
- Nothing on flash is modified
- If it crashes, the watchdog restarts the ORIGINAL lxnetdmx
- Power cycle restores everything to factory state
- `/tmp/` is cleared on reboot

### Level 3: Flash Installation (moderate risk)

Only proceed after Level 2 testing is thoroughly successful.

**Before flashing:**
1. Confirm backup flash slot has working firmware
2. Verify the binary runs correctly from RAM for at least 1 hour
3. Test all protocols you intend to use
4. Test configuration persistence
5. Test reboot behavior

**Install procedure:**
```bash
# One-command deploy from your development machine:
./tools/install.sh 192.168.0.71

# This script:
# 1. Checks connectivity
# 2. Downloads original lxnetdmx as local backup (if not already saved)
# 3. FTPs backup as /usr/bin/lxnetdmx.bak on device
# 4. FTPs new firmware as /usr/bin/lxnetdmx (direct write — no cp/mv needed)
# 5. Uploads device-side install script + trigger
# 6. Device auto-stops old daemon, sets permissions, watchdog starts new firmware
```

**Rollback procedure:**
```bash
# One-command restore from your development machine:
./tools/restore.sh 192.168.0.71

# This restores the original Strand firmware from either:
# - Your local backup (dump/firmware/usr/bin/lxnetdmx)
# - The device's own backup (/usr/bin/lxnetdmx.bak)
```

**Important notes:**
- The device's uClinux shell has NO `cp` or `mv` commands
- All file placement is done via FTP from the host machine
- The device install script only handles stop/start/permissions
- Our binary creates `/var/run/lxnetdmx.pid` for watchdog compatibility
- SIGTERM triggers a clean shutdown (DMX ports set to zero, PID file removed)

---

## Recovery Procedures

### Scenario 1: New binary crashes on startup

**Symptoms:** No DMX output, but telnet/FTP still work.
**Impact:** Low — device is fully accessible.

**Recovery:**
```bash
# Quickest: run the restore script
./tools/restore.sh 192.168.0.71

# Or manually via telnet + FTP:
telnet 192.168.0.71
> rm /usr/bin/lxnetdmx

# From another terminal, upload the original:
curl -T dump/firmware/usr/bin/lxnetdmx -u anonymous:anonymous \
  ftp://192.168.0.71/usr/bin/lxnetdmx

# The watchdog auto-restarts it within 10 seconds
```

### Scenario 2: New binary causes kernel panic / hang

**Symptoms:** Device completely unresponsive to ping, telnet, everything.
**Impact:** Medium — requires power cycle.

**Recovery:**
1. Power cycle the device (unplug PoE or DC power)
2. Device boots from flash — if lxnetdmx on flash is the broken binary, it will
   crash again, but telnet/FTP should come up first (they start before lxnetdmx)
3. Quickly telnet in and delete/replace the broken binary before the watchdog
   restarts it
4. If the crash happens during boot before inetd starts, this won't work —
   see Scenario 3

### Scenario 3: Bricked — nothing responds after power cycle

**Symptoms:** No network response at all after power cycle.
**Impact:** High — but recoverable if you prepared.

**Possible causes:**
- Corrupted flash filesystem
- Corrupted network configuration
- Corrupted kernel (only if you touched `/dev/blk5` directly)

**Recovery options:**

1. **Serial console**: The NS7520 has UART pins. Opening the device and connecting
   a 3.3V serial adapter may give you a boot console where you can interrupt
   the boot process.

2. **BOOTP/DHCP recovery**: The device may attempt BOOTP/DHCP on startup before
   loading its config. Running a BOOTP server on the same network segment might
   allow re-establishing communication.

3. **JTAG**: The NS7520 supports JTAG debugging. This is the nuclear option but
   can reflash the entire device.

### Scenario 4: Wrong firmware flashed, device boots but behaves incorrectly

**Recovery:**
```bash
# If telnet works, just re-flash with original firmware:
# Use the backup flash procedure in reverse
```

---

## Things That Can Actually Brick The Device

Understanding what's truly dangerous:

### 🔴 HIGH RISK — Do NOT do these:
- Erasing `/dev/blk5` without having a verified backup on `/dev/blk3`
- Modifying kernel boot parameters
- Writing garbage to flash block devices directly
- Corrupting the minix filesystem superblock
- Deleting `/sbin/eflash`, `/sbin/wflash`, or `/sbin/vflash` (can't reflash without them!)
- Deleting `/bin/sh` (nothing works without the shell)
- Deleting `/sbin/inetd` or `/sbin/telnetd` (lose remote access)

### 🟡 MODERATE RISK — Be careful:
- Replacing `/usr/bin/lxnetdmx` with an untested binary
- Modifying `/etc/rc` (boot script)
- Modifying `/etc/220node.cfg` (node configuration)
- Filling up the flash filesystem (only 33KB free!)

### 🟢 LOW RISK — Safe to do:
- Uploading files to `/tmp/` (RAM, cleared on reboot)
- Running test binaries from `/tmp/`
- Reading/dumping files via FTP or telnet
- Stopping/starting `lxnetdmx` manually
- Modifying DMX port configuration via the web UI

---

## Flash Space Budget

Total flash: ~2,978 KB
Currently used: ~2,945 KB
Free: ~33 KB

**If replacing lxnetdmx (449 KB), available space becomes: ~482 KB**

Our replacement binary MUST be ≤ 449 KB to safely fit. Target: < 200 KB
to leave room for configuration growth and other improvements.

---

## Emergency Kit

Keep these files accessible at all times when working on the device:

1. `dump/firmware/usr/bin/lxnetdmx` — original daemon binary
2. `dump/firmware/etc/rc` — original boot script
3. `dump/firmware/etc/220node.cfg` — original node configuration
4. `dump/firmware/sbin/*` — all system utilities
5. This document

**Bookmark this command for emergency recovery:**
```bash
curl -T dump/firmware/usr/bin/lxnetdmx -u anonymous:anonymous ftp://192.168.0.71/usr/bin/lxnetdmx
```
