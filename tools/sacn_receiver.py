#!/usr/bin/env python3
"""
sACN (E1.31) packet receiver/verifier for SN110 end-to-end testing.

Joins the multicast group for a configured universe, parses sACN packets,
and either displays channel values or verifies them against an expected pattern.

Usage:
    python3 tools/sacn_receiver.py [--universe N] [--mode display|verify]
    python3 tools/sacn_receiver.py --universe 1 --mode verify --pattern identify

Modes:
    display  - Show first N channels continuously (default)
    verify   - Compare received DMX against expected pattern, report pass/fail
"""

import socket
import struct
import time
import argparse
import sys

SACN_PORT = 5568

# sACN packet offsets (matching sacn_internal.h)
OFF_PREAMBLE       = 0
OFF_ACN_ID         = 4
OFF_ROOT_VECTOR    = 18
OFF_CID            = 22
OFF_SOURCE_NAME    = 44
OFF_PRIORITY       = 108
OFF_SEQUENCE       = 111
OFF_UNIVERSE       = 113
OFF_DMP_VECTOR     = 117
OFF_DMP_PROP_COUNT = 123
OFF_DMP_START_CODE = 125
OFF_DMP_DMX_DATA   = 126

ACN_IDENTIFIER = b'\x41\x53\x43\x2d\x45\x31\x2e\x31\x37\x00\x00\x00'
VECTOR_ROOT_DATA = 0x00000004
VECTOR_DMP_SET = 0x02

MIN_PACKET_LEN = 126


def sacn_multicast_addr(universe):
    """Calculate sACN multicast address for a universe."""
    return "239.255.{}.{}".format((universe >> 8) & 0xFF, universe & 0xFF)


def parse_sacn(buf):
    """Parse a raw sACN packet. Returns dict or None on invalid packet."""
    if len(buf) < MIN_PACKET_LEN:
        return None

    preamble = struct.unpack_from('>H', buf, OFF_PREAMBLE)[0]
    if preamble != 0x0010:
        return None

    if buf[OFF_ACN_ID:OFF_ACN_ID + 12] != ACN_IDENTIFIER:
        return None

    root_vector = struct.unpack_from('>I', buf, OFF_ROOT_VECTOR)[0]
    if root_vector != VECTOR_ROOT_DATA:
        return None

    if buf[OFF_DMP_VECTOR] != VECTOR_DMP_SET:
        return None

    prop_count = struct.unpack_from('>H', buf, OFF_DMP_PROP_COUNT)[0]
    if prop_count < 1:
        return None

    cid = buf[OFF_CID:OFF_CID + 16]
    source_name = buf[OFF_SOURCE_NAME:OFF_SOURCE_NAME + 64].split(b'\x00')[0].decode('utf-8', errors='replace')
    priority = buf[OFF_PRIORITY]
    sequence = buf[OFF_SEQUENCE]
    universe = struct.unpack_from('>H', buf, OFF_UNIVERSE)[0]
    start_code = buf[OFF_DMP_START_CODE]

    dmx_len = min(prop_count - 1, 512)
    dmx_end = OFF_DMP_DMX_DATA + dmx_len
    if dmx_end > len(buf):
        dmx_len = len(buf) - OFF_DMP_DMX_DATA

    dmx_data = buf[OFF_DMP_DMX_DATA:OFF_DMP_DMX_DATA + dmx_len]

    return {
        'cid': cid,
        'source_name': source_name,
        'priority': priority,
        'sequence': sequence,
        'universe': universe,
        'start_code': start_code,
        'dmx_data': list(dmx_data),
        'dmx_length': dmx_len,
    }


def generate_expected(pattern):
    """Generate expected DMX data for a named pattern."""
    if pattern == 'identify':
        data = [0] * 512
        data[0] = 255
        data[1] = 128
        data[2] = 64
        return data
    elif pattern == 'full':
        return [255] * 512
    elif pattern == 'zero':
        return [0] * 512
    elif pattern == 'ramp':
        return [i & 0xFF for i in range(512)]
    else:
        print(f"Unknown pattern: {pattern}")
        sys.exit(1)


def create_receiver_socket(universe):
    """Create a UDP socket joined to the sACN multicast group."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

    # Some platforms need SO_REUSEPORT
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    except (AttributeError, OSError):
        pass

    sock.bind(('', SACN_PORT))

    # Join multicast group
    mcast_addr = sacn_multicast_addr(universe)
    mreq = struct.pack('4sL', socket.inet_aton(mcast_addr), socket.INADDR_ANY)
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq)

    return sock


def display_mode(sock, universe, channels):
    """Continuously display received DMX channels."""
    print(f"Listening for sACN universe {universe}...")
    print(f"Showing first {channels} channels. Ctrl+C to stop.\n")

    sock.settimeout(5.0)
    pkt_count = 0

    try:
        while True:
            try:
                data, addr = sock.recvfrom(700)
            except socket.timeout:
                sys.stdout.write("\r  [waiting for packets...]")
                sys.stdout.flush()
                continue

            pkt = parse_sacn(data)
            if not pkt or pkt['universe'] != universe:
                continue
            if pkt['start_code'] != 0:
                continue

            pkt_count += 1
            dmx = pkt['dmx_data']
            n = min(channels, len(dmx))
            ch_str = ' '.join(f'Ch{i+1}={dmx[i]:3d}' for i in range(n))
            src = pkt['source_name'][:20]
            sys.stdout.write(f"\r  [{src}] seq={pkt['sequence']:3d} pri={pkt['priority']:3d} | {ch_str}")
            sys.stdout.flush()

    except KeyboardInterrupt:
        print(f"\n\nReceived {pkt_count} packets.")


def verify_mode(sock, universe, pattern, timeout):
    """Verify received DMX matches expected pattern."""
    expected = generate_expected(pattern)
    print(f"Verifying sACN universe {universe}, pattern '{pattern}'")
    print(f"Expected first 5: {expected[:5]}")
    print(f"Timeout: {timeout}s\n")

    sock.settimeout(1.0)
    start = time.time()
    pkt_count = 0
    match_count = 0

    while time.time() - start < timeout:
        try:
            data, addr = sock.recvfrom(700)
        except socket.timeout:
            continue

        pkt = parse_sacn(data)
        if not pkt or pkt['universe'] != universe:
            continue
        if pkt['start_code'] != 0:
            continue

        pkt_count += 1
        dmx = pkt['dmx_data']

        # Check if data matches expected
        match = True
        for i in range(min(len(expected), len(dmx))):
            if dmx[i] != expected[i]:
                match = False
                if match_count == 0 and pkt_count <= 3:
                    print(f"  Mismatch at Ch{i+1}: got {dmx[i]}, expected {expected[i]}")
                break

        if match:
            match_count += 1
            if match_count == 1:
                elapsed = time.time() - start
                print(f"  First match at packet #{pkt_count} ({elapsed:.2f}s)")
                print(f"  Source: {pkt['source_name']}, priority={pkt['priority']}")
            if match_count >= 3:
                print(f"\nPASS: Received {match_count} matching packets ({pkt_count} total)")
                return True

    print(f"\nFAIL: {match_count} matches out of {pkt_count} packets in {timeout}s")
    return False


def main():
    parser = argparse.ArgumentParser(description='sACN receiver/verifier for SN110')
    parser.add_argument('--universe', type=int, default=1,
                        help='DMX universe (default: 1)')
    parser.add_argument('--mode', default='display',
                        choices=['display', 'verify'],
                        help='Operating mode (default: display)')
    parser.add_argument('--channels', type=int, default=10,
                        help='Number of channels to display (default: 10)')
    parser.add_argument('--pattern', default='identify',
                        choices=['identify', 'full', 'zero', 'ramp'],
                        help='Expected pattern for verify mode (default: identify)')
    parser.add_argument('--timeout', type=int, default=10,
                        help='Timeout in seconds for verify mode (default: 10)')
    args = parser.parse_args()

    mcast = sacn_multicast_addr(args.universe)
    print(f"sACN Receiver — universe {args.universe} ({mcast})")

    sock = create_receiver_socket(args.universe)

    if args.mode == 'display':
        display_mode(sock, args.universe, args.channels)
    elif args.mode == 'verify':
        ok = verify_mode(sock, args.universe, args.pattern, args.timeout)
        sock.close()
        sys.exit(0 if ok else 1)

    sock.close()


if __name__ == '__main__':
    main()
