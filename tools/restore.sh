#!/bin/bash
# SN110 Open Firmware — Host Restore Script
#
# Runs on your COMPUTER to restore the original Strand firmware.
# Can restore from either:
#   - Local backup in dump/firmware/usr/bin/lxnetdmx
#   - Device backup at /usr/bin/lxnetdmx.bak
#
# Usage: ./tools/restore.sh [ip_address]
#        Default IP: 192.168.0.71
#
# Copyright (c) 2026 SN110 Open Firmware Contributors
# SPDX-License-Identifier: MIT

set -e

IP="${1:-192.168.0.71}"
DEVICE_ROLLBACK="tools/device/rollback.sh"
LOCAL_BACKUP="dump/firmware/usr/bin/lxnetdmx"
FTP_USER="anonymous"
FTP_PASS="anonymous"
EXPECTED_SIZE=449632

echo "╔══════════════════════════════════════════════╗"
echo "║  SN110 Open Firmware — Restore Original      ║"
echo "╚══════════════════════════════════════════════╝"
echo ""
echo "  Device: $IP"
echo ""

# ── Check connectivity ──────────────────────────────────────────────

echo "Checking connectivity..."
if ! ping -c 1 -W 3 "$IP" > /dev/null 2>&1; then
    echo "❌ Cannot reach $IP"
    exit 1
fi
echo "  ✅ Device reachable"

# ── Find the backup to restore ──────────────────────────────────────

RESTORE_SOURCE=""

if [ -f "$LOCAL_BACKUP" ]; then
    BACKUP_SIZE=$(wc -c < "$LOCAL_BACKUP" | tr -d ' ')
    echo "  ✅ Local backup found: $LOCAL_BACKUP ($BACKUP_SIZE bytes)"
    RESTORE_SOURCE="$LOCAL_BACKUP"
else
    echo "  ⚠️  No local backup at $LOCAL_BACKUP"
    echo "     Will try to use device-side backup (/usr/bin/lxnetdmx.bak)"
fi

# ── Upload original binary ──────────────────────────────────────────

echo ""
if [ -n "$RESTORE_SOURCE" ]; then
    echo "Step 1/3: Uploading original firmware to /usr/bin/lxnetdmx..."
    if curl -s -T "$RESTORE_SOURCE" --limit-rate 50k \
         -u "$FTP_USER:$FTP_PASS" \
         "ftp://$IP/usr/bin/lxnetdmx"; then
        echo "  ✅ Original firmware uploaded"
    else
        echo "  ❌ Upload failed!"
        exit 1
    fi
else
    # No local backup — try to download .bak from device and re-upload as lxnetdmx
    echo "Step 1/3: Downloading device backup to re-upload..."
    TEMP_BACKUP=$(mktemp)
    if curl -s -o "$TEMP_BACKUP" "ftp://$FTP_USER:$FTP_PASS@$IP/usr/bin/lxnetdmx.bak"; then
        BAK_SIZE=$(wc -c < "$TEMP_BACKUP" | tr -d ' ')
        echo "  ✅ Downloaded lxnetdmx.bak ($BAK_SIZE bytes)"

        echo "     Re-uploading as /usr/bin/lxnetdmx..."
        if curl -s -T "$TEMP_BACKUP" --limit-rate 50k \
             -u "$FTP_USER:$FTP_PASS" \
             "ftp://$IP/usr/bin/lxnetdmx"; then
            echo "  ✅ Original firmware restored"
        else
            echo "  ❌ Re-upload failed!"
            rm -f "$TEMP_BACKUP"
            exit 1
        fi
        rm -f "$TEMP_BACKUP"
    else
        echo "  ❌ No backup found on device either!"
        echo "     Cannot restore without a backup copy."
        rm -f "$TEMP_BACKUP"
        exit 1
    fi
fi

# ── Upload rollback script + trigger ─────────────────────────────────

echo ""
echo "Step 2/3: Uploading rollback script..."
if [ -f "$DEVICE_ROLLBACK" ]; then
    curl -s -T "$DEVICE_ROLLBACK" \
         -u "$FTP_USER:$FTP_PASS" \
         "ftp://$IP/tmp/install.sh"
    echo "  ✅ Rollback script uploaded"
else
    # Minimal inline rollback: just stop + set perms
    printf 'echo sn110: Restoring original firmware\nif exists /sbin/nodecfg /sbin/nodecfg kill all\nsleep 2\nchmod 755 /usr/bin/lxnetdmx\necho sn110: Rollback complete\n/usr/bin/sn110lcd /dev/lcd0 usermsg Original firmware restored!\n' | \
    curl -s -T - -u "$FTP_USER:$FTP_PASS" "ftp://$IP/tmp/install.sh"
    echo "  ✅ Inline rollback script uploaded"
fi

echo ""
echo "Step 3/3: Triggering rollback..."
echo "trigger" | curl -s -T - \
     -u "$FTP_USER:$FTP_PASS" \
     "ftp://$IP/tmp/install.arm"
echo "  ✅ Rollback triggered"

# ── Done ────────────────────────────────────────────────────────────

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Rollback triggered! The device will:"
echo "    1. Detect /tmp/install.arm (within ~10 seconds)"
echo "    2. Stop the open firmware"
echo "    3. Start the original Strand lxnetdmx"
echo ""
echo "  The device should be running original firmware within ~20 seconds."
echo "  To verify: telnet $IP"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
