#!/bin/sh
#==============================================================================
# SN110 Open Firmware — Device Rollback Script
#
# This runs ON THE DEVICE via the /etc/rc install mechanism.
# Restores the original Strand firmware from the backup.
#
# The host rollback.sh uploads this + trigger to initiate rollback.
# The host must also FTP:
#   /usr/bin/lxnetdmx — copy of lxnetdmx.bak (since no cp/mv on device)
#
# Copyright (c) 2026 SN110 Open Firmware Contributors
# SPDX-License-Identifier: MIT
#==============================================================================

echo sn110: Rolling back to original Strand firmware...
/usr/bin/sn110lcd /dev/lcd0 usermsg Rolling back firmware. Please_wait...

# Stop the running daemon
if exists /sbin/nodecfg        /sbin/nodecfg kill all
sleep 2

# Verify backup exists
if not exists /usr/bin/lxnetdmx.bak goto NoBackup

# Host deploy must have copied .bak to /usr/bin/lxnetdmx via FTP
if not exists /usr/bin/lxnetdmx goto MissingRestore

echo sn110: Restoring original firmware...
chmod 755 /usr/bin/lxnetdmx

echo sn110: Rollback complete!
/usr/bin/sn110lcd /dev/lcd0 usermsg Original firmware restored!
goto End

NoBackup:
echo sn110: ERROR - No backup found at /usr/bin/lxnetdmx.bak
echo sn110: Use restore.sh from your PC to upload the original binary.
/usr/bin/sn110lcd /dev/lcd0 usermsg Rollback FAILED. No_backup found.
goto End

MissingRestore:
echo sn110: ERROR - /usr/bin/lxnetdmx not in place
echo sn110: Host must FTP lxnetdmx.bak to /usr/bin/lxnetdmx
/usr/bin/sn110lcd /dev/lcd0 usermsg Rollback FAILED. Restore_incomplete.
goto End

End:
