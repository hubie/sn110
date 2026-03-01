#!/bin/bash
# SN110 Open Firmware — Host Deploy Script
#
# Runs on your COMPUTER (not the device) to install the open firmware.
# Orchestrates the full install via FTP + the device's built-in update system.
#
# Usage: ./tools/install.sh [ip_address]
#        Default IP: 192.168.0.71
#
# What this does:
#   1. Checks device connectivity
#   2. Downloads original lxnetdmx as backup (if not already saved)
#   3. Uploads backup copy as /usr/bin/lxnetdmx.bak on device
#   4. Uploads new firmware as /usr/bin/lxnetdmx on device
#   5. Uploads device install script + trigger
#   6. The device auto-runs install.sh within ~10 seconds
#
# Copyright (c) 2026 SN110 Open Firmware Contributors
# SPDX-License-Identifier: MIT

set -e

IP="${1:-192.168.0.71}"
BINARY="build/sn110dmx.bflt"
DEVICE_INSTALL="tools/device/install.sh"
BACKUP_DIR="dump/firmware/usr/bin"
BACKUP_FILE="$BACKUP_DIR/lxnetdmx"
FTP_USER="anonymous"
FTP_PASS="anonymous"
EXPECTED_ORIG_SIZE=449632

echo "╔══════════════════════════════════════════════╗"
echo "║  SN110 Open Firmware — Install               ║"
echo "╚══════════════════════════════════════════════╝"
echo ""
echo "  Device:   $IP"
echo "  Binary:   $BINARY"
echo ""

# ── Pre-flight checks ──────────────────────────────────────────────

if [ ! -f "$BINARY" ]; then
    echo "❌ ERROR: Firmware binary not found: $BINARY"
    echo "   Build it first:  make docker-bflt"
    exit 1
fi

if [ ! -f "$DEVICE_INSTALL" ]; then
    echo "❌ ERROR: Device install script not found: $DEVICE_INSTALL"
    exit 1
fi

BINARY_SIZE=$(wc -c < "$BINARY" | tr -d ' ')
echo "  FW size:  $BINARY_SIZE bytes"

if [ "$BINARY_SIZE" -gt 449632 ]; then
    echo "❌ ERROR: Binary too large! Max 449632 bytes (original lxnetdmx size)"
    exit 1
fi

echo ""
echo "Checking connectivity..."
if ! ping -c 1 -W 3 "$IP" > /dev/null 2>&1; then
    echo "❌ Cannot reach $IP"
    echo ""
    echo "   If you're on a different subnet, add a route:"
    echo "     sudo ip addr add 192.168.0.100/24 dev en7"
    echo "   or (macOS):"
    echo "     sudo ifconfig en7 alias 192.168.0.100 255.255.255.0"
    exit 1
fi
echo "  ✅ Device reachable"

# ── Backup original firmware ────────────────────────────────────────

echo ""
if [ -f "$BACKUP_FILE" ]; then
    echo "✅ Local backup already exists: $BACKUP_FILE"
else
    echo "Downloading original firmware as backup..."
    mkdir -p "$BACKUP_DIR"
    if curl -s -o "$BACKUP_FILE" "ftp://$FTP_USER:$FTP_PASS@$IP/usr/bin/lxnetdmx"; then
        DL_SIZE=$(wc -c < "$BACKUP_FILE" | tr -d ' ')
        echo "  ✅ Downloaded $DL_SIZE bytes → $BACKUP_FILE"
    else
        echo "  ⚠️  Download failed — proceeding without local backup"
        echo "     (original may already be backed up on device)"
    fi
fi

# ── Upload backup to device ────────────────────────────────────────

echo ""
echo "Step 1/4: Uploading backup to /usr/bin/lxnetdmx.bak..."
if [ -f "$BACKUP_FILE" ]; then
    if curl -s -T "$BACKUP_FILE" --limit-rate 50k \
         -u "$FTP_USER:$FTP_PASS" \
         "ftp://$IP/usr/bin/lxnetdmx.bak"; then
        echo "  ✅ Backup uploaded"
    else
        echo "  ⚠️  Backup upload failed (continuing — device may already have it)"
    fi
else
    echo "  ⚠️  No local backup to upload (continuing)"
fi

# ── Upload new firmware ─────────────────────────────────────────────

echo ""
echo "Step 2/4: Uploading new firmware to /usr/bin/lxnetdmx..."
if curl -s -T "$BINARY" --limit-rate 50k \
     -u "$FTP_USER:$FTP_PASS" \
     "ftp://$IP/usr/bin/lxnetdmx"; then
    echo "  ✅ Firmware uploaded ($BINARY_SIZE bytes)"
else
    echo "  ❌ Firmware upload failed!"
    echo "     The device may be busy. Try power-cycling and running again."
    exit 1
fi

# ── Upload install script ──────────────────────────────────────────

echo ""
echo "Step 3/4: Uploading install script..."
if curl -s -T "$DEVICE_INSTALL" \
     -u "$FTP_USER:$FTP_PASS" \
     "ftp://$IP/tmp/install.sh"; then
    echo "  ✅ Install script uploaded"
else
    echo "  ❌ Install script upload failed!"
    exit 1
fi

# ── Trigger install ─────────────────────────────────────────────────

echo ""
echo "Step 4/4: Triggering device install..."
echo "trigger" | curl -s -T - \
     -u "$FTP_USER:$FTP_PASS" \
     "ftp://$IP/tmp/install.arm"
echo "  ✅ Install triggered"

# ── Wait for result ─────────────────────────────────────────────────

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "  Install triggered! The device will:"
echo "    1. Detect /tmp/install.arm (within ~10 seconds)"
echo "    2. Stop the running daemon"
echo "    3. Set permissions on new binary"
echo "    4. Auto-start the new firmware via watchdog"
echo ""
echo "  Watch the device LCD for status messages."
echo "  The device should be running open firmware within ~20 seconds."
echo ""
echo "  To verify: telnet $IP"
echo "  You should see the sn110dmx banner in the process list."
echo ""
echo "  To rollback: ./tools/restore.sh $IP"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
