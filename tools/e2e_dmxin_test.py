#!/usr/bin/env python3
"""
End-to-end DMX-In test for SN110.

Supports two modes:

1. Loopback mode (default):
    Host → sACN → SN110 Port 0 (TX) → DMX cable → SN110 Port 1 (RX) → sACN → Host
    Requires: short XLR cable connecting the two DMX ports

2. MagicDMX mode (--source magicdmx):
    MagicDMX (USB) → DMX → SN110 Port (RX) → sACN → Host
    Requires: ChamSys MagicDMX + hidapi
    NOTE: MagicDMX uses a proprietary closed protocol (ChamSys refuses to
    publish specs). This mode may not work without reverse-engineering the
    device protocol. See: https://forum.qlcplus.org/viewtopic.php?t=11995

Prerequisites:
    - SN110 deployed with DMX-in firmware
    - Loopback: DMX_PORT0_MODE=tx, DMX_PORT1_MODE=rx, cable between ports
    - Host on same network as SN110 for multicast reception

Usage:
    python3 tools/e2e_dmxin_test.py                          # loopback
    python3 tools/e2e_dmxin_test.py --source magicdmx        # MagicDMX
    python3 tools/e2e_dmxin_test.py --tx-universe 1 --rx-universe 2
"""

import sys
import os
import time
import socket
import struct
import argparse
import threading

# Import sibling modules
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sacn_receiver import parse_sacn, create_receiver_socket, sacn_multicast_addr
from sacn_sender import build_sacn_packet

SACN_PORT = 5568


class SacnCapture:
    """Background thread that captures sACN packets."""

    def __init__(self, universe, timeout=10):
        self.universe = universe
        self.timeout = timeout
        self.packets = []
        self.running = False
        self.thread = None

    def start(self):
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=2)

    def _run(self):
        sock = create_receiver_socket(self.universe)
        sock.settimeout(1.0)

        start = time.time()
        while self.running and (time.time() - start) < self.timeout:
            try:
                data, addr = sock.recvfrom(700)
                pkt = parse_sacn(data)
                if pkt and pkt['universe'] == self.universe and pkt['start_code'] == 0:
                    self.packets.append(pkt)
            except socket.timeout:
                continue

        sock.close()

    def get_latest(self):
        if self.packets:
            return self.packets[-1]
        return None

    def find_matching(self, expected_data, channels=3):
        """Find a packet where the first N channels match expected."""
        for pkt in self.packets:
            dmx = pkt['dmx_data']
            if len(dmx) >= channels:
                match = all(dmx[i] == expected_data[i] for i in range(channels))
                if match:
                    return pkt
        return None


def generate_pattern(name, step=0):
    """Generate DMX data for a named pattern."""
    if name == 'identify':
        data = [0] * 512
        data[0] = 255
        data[1] = 128
        data[2] = 64
        return data
    elif name == 'ramp':
        return [i & 0xFF for i in range(512)]
    elif name == 'full':
        return [255] * 512
    elif name == 'zero':
        return [0] * 512
    elif name == 'chase':
        data = [0] * 512
        pos = step % 10
        data[pos] = 255
        return data
    return [0] * 512


def send_sacn(sock, dest, universe, dmx_data, count=20, fps=20):
    """Send sACN packets with the given DMX data."""
    interval = 1.0 / fps
    for i in range(count):
        pkt = build_sacn_packet(universe, bytes(dmx_data))
        sock.sendto(pkt, dest)
        time.sleep(interval)
    return count


def test_loopback_pattern(sock, dest, tx_universe, capture, pattern_name, timeout=8):
    """Send pattern via sACN TX, verify it arrives back via RX sACN.

    Returns (passed, detail_string).
    """
    expected = generate_pattern(pattern_name)
    capture.packets.clear()

    print(f"\n  Pattern: {pattern_name}")
    print(f"    Expected Ch1-3: {expected[0]}, {expected[1]}, {expected[2]}")

    # Send sACN to the TX port
    sent = send_sacn(sock, dest, tx_universe, expected, count=40, fps=20)

    # Give time for DMX → RX → sACN turnaround
    time.sleep(1.5)

    print(f"    Sent {sent} sACN packets (universe {tx_universe})")
    print(f"    Captured {len(capture.packets)} sACN packets back")

    # Check for matching packet
    check_channels = 10 if pattern_name == 'zero' else 3
    pkt = capture.find_matching(expected, channels=check_channels)

    if pkt:
        dmx = pkt['dmx_data']
        print(f"    Received Ch1-3: {dmx[0]}, {dmx[1]}, {dmx[2]}")
        print(f"    Source: {pkt['source_name']}")
        print(f"    PASS")
        return True, f"{pattern_name}: PASS"
    else:
        if capture.packets:
            last = capture.get_latest()
            dmx = last['dmx_data']
            print(f"    Last received Ch1-3: {dmx[0]}, {dmx[1]}, {dmx[2]}")
        else:
            print(f"    No sACN packets received on RX universe!")
        print(f"    FAIL")
        return False, f"{pattern_name}: FAIL"


def main():
    parser = argparse.ArgumentParser(description='SN110 DMX-In end-to-end test')
    parser.add_argument('--source', default='loopback',
                        choices=['loopback', 'magicdmx'],
                        help='DMX source: loopback (cable between ports) or magicdmx')
    parser.add_argument('--tx-universe', type=int, default=1,
                        help='sACN universe for TX port (default: 1)')
    parser.add_argument('--rx-universe', type=int, default=2,
                        help='sACN universe for RX port (default: 2)')
    parser.add_argument('--timeout', type=int, default=30,
                        help='Overall test timeout in seconds (default: 30)')
    parser.add_argument('--device-ip', default=None,
                        help='SN110 IP for unicast sACN (default: multicast)')
    args = parser.parse_args()

    if args.source == 'magicdmx':
        print("ERROR: MagicDMX mode is not currently supported.")
        print("The ChamSys MagicDMX uses a proprietary closed protocol.")
        print("Use --source loopback with a cable between the two DMX ports.")
        sys.exit(1)

    print("=" * 60)
    print("  SN110 DMX-In End-to-End Loopback Test")
    print(f"  sACN(u{args.tx_universe}) → TX → DMX cable → RX → sACN(u{args.rx_universe})")
    print("=" * 60)

    # Create sACN sender socket
    print(f"\n[1/3] Setting up sACN sender (universe {args.tx_universe})...")
    tx_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    tx_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    tx_sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 20)

    if args.device_ip:
        dest = (args.device_ip, SACN_PORT)
        print(f"  Sending to {args.device_ip}:{SACN_PORT} (unicast)")
    else:
        mcast = sacn_multicast_addr(args.tx_universe)
        dest = (mcast, SACN_PORT)
        print(f"  Sending to {mcast}:{SACN_PORT} (multicast)")

    # Start sACN capture on RX universe
    rx_mcast = sacn_multicast_addr(args.rx_universe)
    print(f"\n[2/3] Starting sACN capture (universe {args.rx_universe}, {rx_mcast})...")
    capture = SacnCapture(args.rx_universe, timeout=args.timeout)
    capture.start()
    time.sleep(0.5)

    # Run test patterns
    print(f"\n[3/3] Running test patterns...")
    results = []

    for pattern in ['identify', 'zero', 'ramp']:
        passed, detail = test_loopback_pattern(
            tx_sock, dest, args.tx_universe, capture, pattern)
        results.append((passed, detail))

    # Send blackout
    pkt = build_sacn_packet(args.tx_universe, bytes([0] * 512))
    tx_sock.sendto(pkt, dest)

    capture.stop()
    tx_sock.close()

    # Summary
    print("\n" + "=" * 60)
    print("  Results:")
    total_pass = 0
    for passed, detail in results:
        status = "PASS" if passed else "FAIL"
        print(f"    [{status}] {detail}")
        if passed:
            total_pass += 1

    total = len(results)
    print(f"\n  {total_pass}/{total} tests passed")
    if total_pass < total:
        print("\n  Troubleshooting:")
        print("    - Verify XLR cable connects TX port to RX port")
        print("    - Verify SN110 config: PORT0=tx (universe 1), PORT1=rx (universe 2)")
        print("    - Check SN110 is running DMX-in firmware")
    print("=" * 60)

    sys.exit(0 if total_pass == total else 1)


if __name__ == '__main__':
    main()
