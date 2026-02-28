#!/bin/bash
# SN110 Device Backup Script
# Downloads all files from an SN110 node via FTP
#
# Usage: ./backup.sh <ip_address> [output_dir]

set -e

IP="${1:?Usage: $0 <ip_address> [output_dir]}"
OUT="${2:-dump/firmware}"
FTP_USER="anonymous"
FTP_PASS="anonymous"
RATE_LIMIT="20k"  # Limit transfer rate to avoid OOM on device (only 920KB free RAM)

echo "=== SN110 Firmware Backup ==="
echo "Device: $IP"
echo "Output: $OUT"
echo ""

# Files to back up (in order of importance)
CRITICAL_FILES=(
    "sbin/eflash"
    "sbin/wflash"
    "sbin/vflash"
    "sbin/nodecfg"
    "sbin/inetd"
    "sbin/telnetd"
    "sbin/aftpd"
    "sbin/httpd"
    "usr/bin/lxnetdmx"
    "usr/bin/sn110lcd"
    "usr/bin/dmxtst"
    "etc/rc"
    "etc/inittab"
    "etc/inetd.conf"
    "etc/220node.cfg"
    "etc/ifup-eth0"
)

SECONDARY_FILES=(
    "usr/bin/selftst"
    "usr/bin/swinstal.sh"
    "usr/bin/flashsw.sh"
    "usr/bin/flashtst.sh"
    "usr/bin/mkspace.sh"
    "cgi-bin/cfgget.cgi"
    "cgi-bin/cfgnet.cgi"
    "cgi-bin/cfgpost.cgi"
    "cgi-bin/reboot.cgi"
    "cgi-bin/cfg2html"
    "cgi-bin/html2cfg"
    "index.html"
    "netstat.html"
    "licence.txt"
    "images/sn110.gif"
)

LARGE_FILES=(
    "sbin/ifconfig"
    "sbin/mount"
    "sbin/pump"
    "sbin/route"
)

download_file() {
    local file="$1"
    local dir
    dir=$(dirname "$file")
    mkdir -p "$OUT/$dir"

    echo -n "  Downloading $file... "
    if curl -s --max-time 180 --limit-rate "$RATE_LIMIT" \
         -u "$FTP_USER:$FTP_PASS" \
         -o "$OUT/$file" \
         "ftp://$IP/$file" 2>/dev/null; then
        local size
        size=$(wc -c < "$OUT/$file")
        echo "OK ($size bytes)"
    else
        echo "FAILED"
        return 1
    fi
}

echo "--- Critical files ---"
for f in "${CRITICAL_FILES[@]}"; do
    download_file "$f" || echo "  ⚠️  WARNING: Critical file $f failed!"
done

echo ""
echo "--- Secondary files ---"
for f in "${SECONDARY_FILES[@]}"; do
    download_file "$f" || echo "  (non-critical, continuing)"
done

echo ""
echo "--- Large files (slow, rate-limited to protect device) ---"
for f in "${LARGE_FILES[@]}"; do
    download_file "$f" || echo "  (non-critical, continuing)"
done

echo ""
echo "=== Backup complete ==="
echo "Files saved to: $OUT/"
find "$OUT" -type f | wc -l | tr -d ' '
echo " files backed up"
du -sh "$OUT"
