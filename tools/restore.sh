#!/bin/bash
# SN110 Emergency Restore Script
# Restores original lxnetdmx binary to a device via FTP
#
# Usage: ./restore.sh <ip_address>
#
# This script:
# 1. Verifies the backup binary exists and has correct size
# 2. Uploads the original lxnetdmx via FTP
# 3. Provides instructions for restarting the daemon

set -e

IP="${1:?Usage: $0 <ip_address>}"
BACKUP="dump/firmware/usr/bin/lxnetdmx"
EXPECTED_SIZE=449632
FTP_USER="anonymous"
FTP_PASS="anonymous"

echo "=== SN110 Emergency Restore ==="
echo "Device: $IP"
echo ""

# Verify backup exists
if [ ! -f "$BACKUP" ]; then
    echo "❌ ERROR: Backup file not found: $BACKUP"
    echo "Cannot restore without the original binary."
    exit 1
fi

# Verify backup size
ACTUAL_SIZE=$(wc -c < "$BACKUP")
if [ "$ACTUAL_SIZE" -ne "$EXPECTED_SIZE" ]; then
    echo "⚠️  WARNING: Backup size mismatch!"
    echo "  Expected: $EXPECTED_SIZE bytes"
    echo "  Actual:   $ACTUAL_SIZE bytes"
    echo "  Proceeding anyway (file may be from a different firmware version)..."
fi

# Check connectivity
echo "Checking connectivity..."
if ! ping -c 1 -W 3 "$IP" > /dev/null 2>&1; then
    echo "❌ ERROR: Cannot reach $IP"
    echo "Check network connectivity and IP alias configuration."
    exit 1
fi
echo "  ✅ Device reachable"

# Upload
echo ""
echo "Uploading original lxnetdmx ($ACTUAL_SIZE bytes)..."
if curl -T "$BACKUP" --limit-rate 20k \
     -u "$FTP_USER:$FTP_PASS" \
     "ftp://$IP/usr/bin/lxnetdmx" 2>/dev/null; then
    echo "  ✅ Upload complete"
else
    echo "  ❌ Upload failed!"
    echo ""
    echo "  If FTP is not responding, try:"
    echo "  1. Power cycle the device"
    echo "  2. Wait 30 seconds for it to boot"
    echo "  3. Run this script again"
    exit 1
fi

echo ""
echo "=== Restore complete ==="
echo ""
echo "The original lxnetdmx has been uploaded. To activate it:"
echo "  Option 1: Power cycle the device (safest)"
echo "  Option 2: Telnet in and restart:"
echo "    telnet $IP"
echo "    # The boot script watchdog should restart it automatically"
echo "    # Or manually: /usr/bin/lxnetdmx &"
