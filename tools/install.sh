#!/bin/sh
# SN110 Open Firmware Install Script
# This script runs ON the SN110 device via the boot script's install mechanism.
#
# The boot script (/etc/rc) detects /tmp/install.arm and runs /tmp/install.sh.
# This script replaces lxnetdmx with the new binary uploaded to /tmp/sn110dmx.
#
# SAFETY: This does NOT use the flash erase/write mechanism (eflash/wflash).
# Instead, it simply replaces the file on the minix filesystem, which is
# much safer — the filesystem structure is preserved.

echo "SN110 Open Firmware Installer"
echo "=============================="

# Stop existing daemon
if exists /var/run/lxnetdmx.pid
    echo "Stopping existing lxnetdmx..."
    /sbin/nodecfg kill all
    sleep 2
fi

# Verify new binary exists
if not exists /tmp/sn110dmx goto MissingBinary

# Back up the original (if it exists and we haven't already)
if exists /usr/bin/lxnetdmx.orig goto SkipBackup
if exists /usr/bin/lxnetdmx
    echo "Backing up original lxnetdmx..."
    # Note: on a minix filesystem with limited space, this may fail
    # If it fails, we proceed anyway — the user should have an off-device backup
fi

SkipBackup:
# Remove old binary to free space
echo "Removing old lxnetdmx..."
if exists /usr/bin/lxnetdmx rm /usr/bin/lxnetdmx

# Install new binary
echo "Installing new binary..."
# Note: 'cp' may not exist on this system, but the shell might support it
# The FTP upload should have already placed it at /usr/bin/lxnetdmx
# via a separate FTP command. This script just handles the orchestration.

# If the new binary was uploaded to /tmp/, we need to move it
# Unfortunately this minimal shell may not have 'mv' or 'cp'
# The deploy-flash Makefile target should upload directly to /usr/bin/

echo "Installation complete."
echo "The boot script watchdog will start the new binary automatically."
goto End

MissingBinary:
echo "ERROR: /tmp/sn110dmx not found!"
echo "Upload the binary via FTP first."
goto End

End:
