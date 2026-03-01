#!/bin/sh
#==============================================================================
# SN110 Open Firmware — Device Install Script
#
# This runs ON THE DEVICE via the /etc/rc install mechanism.
# The /etc/rc boot script checks every 10 seconds for /tmp/install.arm;
# when found, it runs /tmp/install.sh (this script).
#
# The host deploy.sh script uploads everything needed before triggering:
#   /usr/bin/lxnetdmx.bak  — backup of original (FTP'd by host)
#   /usr/bin/lxnetdmx      — new firmware binary (FTP'd by host)
#   /tmp/install.sh        — this script
#   /tmp/install.arm       — trigger file
#
# What this does:
#   1. Stops the running daemon
#   2. Verifies the new binary is in place
#   3. Sets permissions
#   4. Displays LCD status
#   5. The /etc/rc watchdog auto-restarts the new binary
#
# Copyright (c) 2026 SN110 Open Firmware Contributors
# SPDX-License-Identifier: MIT
#==============================================================================

echo sn110: Open Firmware Installer v0.1
/usr/bin/sn110lcd /dev/lcd0 usermsg Installing Open_Firmware. Please_wait...

# Stop the running daemon
echo sn110: Stopping running daemon...
if exists /sbin/nodecfg        /sbin/nodecfg kill all
sleep 2

# Verify new binary was placed by host deploy
if not exists /usr/bin/lxnetdmx goto MissingBinary

# Verify backup exists
if not exists /usr/bin/lxnetdmx.bak goto NoBackup
echo sn110: Backup verified at /usr/bin/lxnetdmx.bak
goto SetPerms

NoBackup:
echo sn110: WARNING - no backup at /usr/bin/lxnetdmx.bak
echo sn110: Use restore.sh from your PC to recover original firmware

SetPerms:
echo sn110: Setting permissions...
chmod 755 /usr/bin/lxnetdmx
if exists /usr/bin/lxnetdmx.bak chmod 755 /usr/bin/lxnetdmx.bak

# Clean up temp files
if exists /tmp/sn110dmx        rm /tmp/sn110dmx

echo sn110: Installation complete!
/usr/bin/sn110lcd /dev/lcd0 usermsg Open_Firmware installed! Starting...

# The /etc/rc watchdog will auto-start /usr/bin/lxnetdmx
goto End

MissingBinary:
echo sn110: ERROR - /usr/bin/lxnetdmx not found!
echo sn110: The deploy script must FTP the binary to /usr/bin/lxnetdmx
/usr/bin/sn110lcd /dev/lcd0 usermsg Install FAILED. Binary_missing.
goto End

End:
