#!/usr/bin/env python3
"""
sACN (E1.31) test sender for SN110 end-to-end testing.

Sends sACN packets to a target device on a specified universe.
Can send static values, chases, or ramps for visual verification.

Usage:
    python3 tools/sacn_sender.py [--ip IP] [--universe N] [--mode MODE]

Modes:
    ramp     - Channels 1-512 ramp from 0-255 (repeating)
    chase    - Moving bright channel across 1-10
    full     - All channels at 255
    zero     - All channels at 0
    identify - Ch1=255, Ch2=128, Ch3=64, rest=0 (easy to identify)
"""

import socket
import struct
import time
import argparse
import sys

SACN_PORT = 5568

# sACN sequence counter
_seq = [0]

def sacn_multicast_addr(universe):
    """Calculate sACN multicast address for a universe."""
    return "239.255.{}.{}".format((universe >> 8) & 0xFF, universe & 0xFF)

def build_sacn_packet(universe, dmx_data, priority=100, source_name="SN110 Test"):
    """Build a complete E1.31 sACN data packet."""
    _seq[0] = (_seq[0] + 1) & 0xFF
    
    # Source name (64 bytes, null-padded)
    source_name_bytes = source_name.encode('utf-8')[:63].ljust(64, b'\x00')
    
    # CID (16 bytes - unique source identifier)
    cid = b'\x53\x4e\x31\x31\x30\x54\x65\x73\x74\x00\x00\x00\x00\x00\x00\x01'
    
    # DMX data with start code prefix
    dmx_with_start = b'\x00' + bytes(dmx_data[:512]).ljust(512, b'\x00')
    
    # === Root Layer ===
    # Preamble size (16-bit) + Post-amble size (16-bit)
    preamble = struct.pack('>HH', 0x0010, 0x0000)
    
    # ACN Packet Identifier (12 bytes)
    acn_id = b'\x41\x53\x43\x2d\x45\x31\x2e\x31\x37\x00\x00\x00'
    
    # Flags + Length for root layer (22 + framing_pdu_length)
    # DMP layer: 11 + 513 = 524
    # Framing layer: 77 + 524 = 601  
    # Root layer: 22 + 601 = 623
    
    dmp_length = 0x7000 | (11 + len(dmx_with_start))  # flags=0x7, length
    framing_length = 0x7000 | (77 + 11 + len(dmx_with_start))
    root_length = 0x7000 | (22 + 77 + 11 + len(dmx_with_start))
    
    # Root layer vector (VECTOR_ROOT_E131_DATA = 0x00000004)
    root_vector = struct.pack('>I', 0x00000004)
    
    # Root layer
    root_layer = preamble + acn_id + struct.pack('>H', root_length) + root_vector + cid
    
    # === Framing Layer ===
    framing_vector = struct.pack('>I', 0x00000002)  # VECTOR_E131_DATA_PACKET
    
    framing_layer = (
        struct.pack('>H', framing_length) +
        framing_vector +
        source_name_bytes +
        struct.pack('>B', priority) +        # Priority
        struct.pack('>H', 0) +               # Synchronization address
        struct.pack('>B', _seq[0]) +          # Sequence number
        struct.pack('>B', 0) +               # Options
        struct.pack('>H', universe)           # Universe
    )
    
    # === DMP Layer ===
    dmp_vector = struct.pack('>B', 0x02)     # VECTOR_DMP_SET_PROPERTY
    dmp_layer = (
        struct.pack('>H', dmp_length) +
        dmp_vector +
        struct.pack('>B', 0xA1) +            # Address & Data type
        struct.pack('>H', 0x0000) +          # First property address
        struct.pack('>H', 0x0001) +          # Address increment
        struct.pack('>H', len(dmx_with_start)) +  # Property value count
        dmx_with_start
    )
    
    return root_layer + framing_layer + dmp_layer

def generate_ramp():
    """Generate a ramp pattern: channels cycle 0-255."""
    return bytes([i & 0xFF for i in range(512)])

def generate_chase(step):
    """Generate a chase pattern: bright channel moves across 1-10."""
    data = [0] * 512
    pos = step % 10
    data[pos] = 255
    if pos > 0:
        data[pos - 1] = 64  # trail
    return bytes(data)

def generate_identify():
    """Generate identify pattern: Ch1=255, Ch2=128, Ch3=64."""
    data = [0] * 512
    data[0] = 255   # Channel 1
    data[1] = 128   # Channel 2
    data[2] = 64    # Channel 3
    return bytes(data)

def main():
    parser = argparse.ArgumentParser(description='sACN test sender for SN110')
    parser.add_argument('--ip', default=None,
                        help='Unicast IP (default: multicast)')
    parser.add_argument('--universe', type=int, default=1,
                        help='DMX universe (default: 1)')
    parser.add_argument('--mode', default='identify',
                        choices=['ramp', 'chase', 'full', 'zero', 'identify'],
                        help='Data pattern (default: identify)')
    parser.add_argument('--fps', type=int, default=20,
                        help='Packets per second (default: 20)')
    parser.add_argument('--duration', type=int, default=30,
                        help='Duration in seconds (default: 30, 0=forever)')
    parser.add_argument('--iface', default=None,
                        help='Source interface IP for multicast')
    args = parser.parse_args()
    
    # Determine destination
    if args.ip:
        dest = (args.ip, SACN_PORT)
        print(f"Sending sACN universe {args.universe} → {args.ip}:{SACN_PORT} (unicast)")
    else:
        mcast = sacn_multicast_addr(args.universe)
        dest = (mcast, SACN_PORT)
        print(f"Sending sACN universe {args.universe} → {mcast}:{SACN_PORT} (multicast)")
    
    print(f"Mode: {args.mode}, FPS: {args.fps}, Duration: {args.duration}s")
    
    # Create UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    if not args.ip:
        # Multicast: set TTL and outgoing interface
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 20)
        if args.iface:
            sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF,
                           socket.inet_aton(args.iface))
    
    interval = 1.0 / args.fps
    step = 0
    start = time.time()
    sent = 0
    
    print("Sending... (Ctrl+C to stop)")
    try:
        while True:
            if args.duration > 0 and (time.time() - start) >= args.duration:
                break
            
            # Generate DMX data
            if args.mode == 'ramp':
                dmx = generate_ramp()
            elif args.mode == 'chase':
                dmx = generate_chase(step)
            elif args.mode == 'full':
                dmx = bytes([255] * 512)
            elif args.mode == 'zero':
                dmx = bytes([0] * 512)
            elif args.mode == 'identify':
                dmx = generate_identify()
            
            pkt = build_sacn_packet(args.universe, dmx)
            sock.sendto(pkt, dest)
            sent += 1
            step += 1
            
            if sent % args.fps == 0:
                elapsed = time.time() - start
                sys.stdout.write(f"\r  Sent {sent} packets ({elapsed:.1f}s)")
                sys.stdout.flush()
            
            time.sleep(interval)
    except KeyboardInterrupt:
        pass
    
    # Send a final zero-out packet (clean shutdown)
    print(f"\n  Sending zero-out packet...")
    pkt = build_sacn_packet(args.universe, bytes([0] * 512))
    sock.sendto(pkt, dest)
    
    sock.close()
    elapsed = time.time() - start
    print(f"Done. Sent {sent + 1} packets in {elapsed:.1f}s")

if __name__ == '__main__':
    main()
