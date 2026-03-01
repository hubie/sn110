#!/usr/bin/env python3
"""
SN110 Web UI Deploy Script

Uploads the CGI binary and shell scripts to an SN110 device.
Uses FTP (active mode) to stage files in /tmp/, then telnet to
copy them to their final locations with the shell `cp` command.

Usage: python3 tools/deploy_web.py [ip_address]
       Default IP: 192.168.0.71

Prerequisites:
  - make docker-cgi-bflt   (builds build/cgi_config.bflt)

Copyright (c) 2026 SN110 Open Firmware Contributors
SPDX-License-Identifier: MIT
"""

import ftplib
import socket
import sys
import time
import os

IP = sys.argv[1] if len(sys.argv) > 1 else "192.168.0.71"
BFLT = "build/cgi_config.bflt"
IAC, DO, WONT = 255, 253, 252


def recv_clean(sock, timeout=2):
    """Receive from telnet socket, stripping IAC sequences."""
    sock.settimeout(timeout)
    buf = b""
    while True:
        try:
            data = sock.recv(4096)
            if data:
                buf += data
            else:
                break
        except socket.timeout:
            break
    clean = b""
    i = 0
    while i < len(buf):
        if buf[i] == IAC and i + 2 < len(buf):
            if buf[i + 1] == DO:
                sock.send(bytes([IAC, WONT, buf[i + 2]]))
            i += 3
        else:
            clean += bytes([buf[i]])
            i += 1
    return clean.decode("ascii", errors="replace")


def telnet_cmd(sock, cmd, wait=1):
    """Send a command via telnet and return the response."""
    sock.send(f"{cmd}\n".encode())
    time.sleep(wait)
    return recv_clean(sock, wait).strip()


def main():
    if not os.path.exists(BFLT):
        print(f"Error: {BFLT} not found. Run: make docker-cgi-bflt")
        sys.exit(1)

    bflt_size = os.path.getsize(BFLT)
    print(f"SN110 Web UI Deploy")
    print(f"  Device: {IP}")
    print(f"  Binary: {BFLT} ({bflt_size} bytes)")
    print()

    # ── Step 1: FTP upload to /tmp/ ──
    print("Step 1: Uploading files via FTP (active mode)...")
    ftp = ftplib.FTP(IP)
    ftp.login("anonymous", "anonymous")
    ftp.set_pasv(False)

    with open(BFLT, "rb") as f:
        ftp.storbinary("STOR /tmp/cgi_config", f)
    print(f"  cgi_config ({bflt_size} bytes) -> /tmp/")

    for name in ["cfgget.cgi", "cfgpost.cgi", "index.html"]:
        path = f"tools/web/{name}"
        with open(path, "rb") as f:
            ftp.storbinary(f"STOR /tmp/{name}", f)
        print(f"  {name} -> /tmp/")

    ftp.quit()

    # ── Step 2: Install via telnet ──
    print("\nStep 2: Installing via telnet...")
    tn = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tn.settimeout(5)
    tn.connect((IP, 23))
    time.sleep(1)
    recv_clean(tn, 1)

    # Free space: remove old Strand CGI binaries if present
    telnet_cmd(tn, "rm /cgi-bin/cfg2html", 0.5)
    telnet_cmd(tn, "rm /cgi-bin/html2cfg", 0.5)
    # Remove any previous empty install
    telnet_cmd(tn, "rm /usr/bin/cgi_config", 0.5)

    # Copy files to final locations
    for src, dst in [
        ("/tmp/cgi_config", "/usr/bin/cgi_config"),
        ("/tmp/cfgget.cgi", "/cgi-bin/cfgget.cgi"),
        ("/tmp/cfgpost.cgi", "/cgi-bin/cfgpost.cgi"),
        ("/tmp/index.html", "/index.html"),
    ]:
        r = telnet_cmd(tn, f"cp {src} {dst}")
        telnet_cmd(tn, f"chmod 755 {dst}", 0.5)
        telnet_cmd(tn, f"rm {src}", 0.3)
        print(f"  {src} -> {dst}")

    # Verify
    r = telnet_cmd(tn, "ls -l /usr/bin/cgi_config")
    if f"{bflt_size}" in r:
        print(f"\n  Binary verified: {bflt_size} bytes")
    else:
        print(f"\n  WARNING: Binary size mismatch!")
        print(f"  {r}")

    tn.close()

    print(f"\nDone! Browse to http://{IP}/")


if __name__ == "__main__":
    main()
