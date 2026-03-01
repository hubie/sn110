#!/usr/bin/env python3
"""
MagicDMX USB DMX sender for SN110 end-to-end testing.

WARNING: The ChamSys MagicDMX uses a PROPRIETARY closed protocol.
ChamSys has explicitly refused to publish specifications, stating
the device is designed solely for use with their MagicQ software.
See: https://forum.qlcplus.org/viewtopic.php?t=11995

This script attempts to drive the MagicDMX as a CP2110 HID-to-UART
bridge with baud-rate-switching for DMX breaks. This approach may not
produce valid DMX output as the device may require proprietary HID
commands for proper operation.

For reliable DMX output testing, consider:
    - ENTTEC DMX USB Pro (well-documented open protocol)
    - DMXKing ultraDMX (FTDI-based, standard UART)
    - SN110 loopback test (wire Port A to Port B)

Usage:
    source /tmp/sn110-test-env/bin/activate
    python3 tools/magicdmx_sender.py [--mode MODE] [--fps FPS] [--duration SECS]

Modes:
    identify - Ch1=255, Ch2=128, Ch3=64, rest=0 (easy to identify)
    ramp     - Channels 1-512 ramp from 0-255 (repeating)
    chase    - Moving bright channel across 1-10
    full     - All channels at 255
    zero     - All channels at 0

Requires: pip install hidapi
"""

import sys
import time
import struct
import argparse

CHAMSYS_VID = 0x10C4
MAGICDMX_PID = 0x857E

# CP2110 HID report IDs
CP2110_GET_VERSION = 0x46
CP2110_GET_SET_UART_CONFIG = 0x50
CP2110_GET_SET_UART_ENABLE = 0x41
CP2110_PURGE_FIFOS = 0x43
CP2110_SET_TRANSMIT_LINE_BREAK = 0x50  # Overloaded with UART config in some docs

# DMX512 timing
DMX_BAUD = 250000
DMX_BREAK_BAUD = 50000  # At 50kbaud, one 0x00 byte = 200us low (break)
DMX_BREAK_US = 100      # Break: >= 88us
DMX_MAB_US = 12         # Mark After Break: >= 8us

# CP2110 max data per HID report
CP2110_MAX_DATA = 63  # Report ID(1) + length(1) + data(up to 63)


def find_magicdmx():
    """Find and return MagicDMX HID device info."""
    try:
        import hid
    except ImportError:
        print("ERROR: hidapi not installed.")
        print("  source /tmp/sn110-test-env/bin/activate")
        print("  pip install hidapi")
        sys.exit(1)

    devices = hid.enumerate(CHAMSYS_VID, MAGICDMX_PID)
    if not devices:
        print(f"No MagicDMX found (VID=0x{CHAMSYS_VID:04X} PID=0x{MAGICDMX_PID:04X})")
        print("\nAll HID devices:")
        for d in hid.enumerate():
            print(f"  VID=0x{d['vendor_id']:04X} PID=0x{d['product_id']:04X}"
                  f" {d.get('product_string', '')}")
        return None

    # Prefer interface 0 (UART data interface)
    for d in devices:
        if d.get('interface_number', -1) == 0:
            return d
    return devices[0]


def open_magicdmx(dev_info):
    """Open the MagicDMX and configure UART for DMX512."""
    import hid

    h = hid.device()
    h.open_path(dev_info['path'])
    print(f"Opened: {h.get_manufacturer_string()} {h.get_product_string()}")

    # Configure UART: 250000 baud, 8 data bits, no parity, 2 stop bits
    # CP2110 UART config report (0x50):
    #   Byte 0: Report ID (0x50)
    #   Bytes 1-4: Baud rate (32-bit LE)
    #   Byte 5: Parity (0=none)
    #   Byte 6: Flow control (0=none)
    #   Byte 7: Data bits (0x03 = 8 bits)
    #   Byte 8: Stop bits (0x01 = 2 stop bits for short; but 0x00=1, 0x01=2)
    uart_cfg = struct.pack('<BIBBBB',
        CP2110_GET_SET_UART_CONFIG,
        DMX_BAUD,       # baud rate (32-bit LE)
        0x00,           # parity: none
        0x00,           # flow control: none
        0x03,           # data bits: 8
        0x01,           # stop bits: 2
    )
    try:
        h.send_feature_report(uart_cfg)
        print(f"  UART configured: {DMX_BAUD} baud, 8N2")
    except Exception as e:
        print(f"  WARNING: UART config failed: {e}")
        print("  Continuing anyway (device may use stored config)")

    # Enable UART
    try:
        h.send_feature_report(bytes([CP2110_GET_SET_UART_ENABLE, 0x01]))
        print("  UART enabled")
    except Exception as e:
        print(f"  WARNING: UART enable failed: {e}")

    # Purge FIFOs
    try:
        h.send_feature_report(bytes([CP2110_PURGE_FIFOS, 0x03]))
    except Exception:
        pass

    return h


def set_uart_baud(h, baud):
    """Change CP2110 UART baud rate via feature report."""
    uart_cfg = struct.pack('<BIBBBB',
        CP2110_GET_SET_UART_CONFIG,
        baud,
        0x00,  # parity: none
        0x00,  # flow control: none
        0x03,  # data bits: 8
        0x01,  # stop bits: 2
    )
    h.send_feature_report(uart_cfg)


def send_dmx_frame(h, dmx_data):
    """Send a complete DMX512 frame via the MagicDMX.

    DMX512 frame format:
        1. Break (line held low for >= 88us)
        2. Mark After Break (line high for >= 8us)
        3. Start code (0x00) + up to 512 channel bytes at 250kbaud

    We generate the break by temporarily switching to a slow baud rate
    and sending a 0x00 byte. At 50kbaud with 8N2: one byte = 220us,
    with 180us of low signal (start + 8 zero bits) satisfying the
    >= 88us break requirement. The stop bits provide the Mark After Break.
    """
    try:
        # Generate DMX break by switching to slow baud rate.
        # At 9600 baud 8N2: one 0x00 byte produces ~937us of mostly-low
        # signal, well exceeding the 88us break minimum. The stop bits
        # at the end give us the Mark After Break.
        set_uart_baud(h, 9600)
        time.sleep(0.005)  # Let baud change take effect
        h.write(bytes([0x01, 0x00]))  # Send break byte
        time.sleep(0.005)  # Wait for break to complete at 9600 baud

        # Switch back to 250kbaud for DMX data
        set_uart_baud(h, DMX_BAUD)
        time.sleep(0.002)  # Let baud change settle

        # Build DMX packet: start code + channel data
        dmx_packet = bytes([0x00]) + bytes(dmx_data[:512]).ljust(512, b'\x00')

        # Send in CP2110 HID chunks (max 63 bytes per report)
        offset = 0
        total = len(dmx_packet)

        while offset < total:
            chunk_size = min(CP2110_MAX_DATA, total - offset)
            chunk = dmx_packet[offset:offset + chunk_size]
            h.write(bytes([len(chunk)]) + chunk)
            offset += chunk_size

        return True

    except Exception as e:
        print(f"\n  DMX frame error: {e}")
        return False


def generate_pattern(mode, step):
    """Generate DMX data for the given pattern mode."""
    if mode == 'identify':
        data = [0] * 512
        data[0] = 255
        data[1] = 128
        data[2] = 64
        return data
    elif mode == 'ramp':
        return [i & 0xFF for i in range(512)]
    elif mode == 'chase':
        data = [0] * 512
        pos = step % 10
        data[pos] = 255
        if pos > 0:
            data[pos - 1] = 64
        return data
    elif mode == 'full':
        return [255] * 512
    elif mode == 'zero':
        return [0] * 512
    else:
        return [0] * 512


def main():
    parser = argparse.ArgumentParser(description='MagicDMX DMX sender for SN110')
    parser.add_argument('--mode', default='identify',
                        choices=['identify', 'ramp', 'chase', 'full', 'zero'],
                        help='Data pattern (default: identify)')
    parser.add_argument('--fps', type=int, default=20,
                        help='Frames per second (default: 20)')
    parser.add_argument('--duration', type=int, default=30,
                        help='Duration in seconds (default: 30, 0=forever)')
    args = parser.parse_args()

    print("MagicDMX DMX Sender — SN110 Test Tool")
    print("=" * 40)

    dev_info = find_magicdmx()
    if not dev_info:
        sys.exit(1)

    h = open_magicdmx(dev_info)

    interval = 1.0 / args.fps
    step = 0
    sent = 0
    start = time.time()

    print(f"\nSending DMX: mode={args.mode}, fps={args.fps}, duration={args.duration}s")
    print("Press Ctrl+C to stop\n")

    try:
        while True:
            if args.duration > 0 and (time.time() - start) >= args.duration:
                break

            data = generate_pattern(args.mode, step)
            if send_dmx_frame(h, data):
                sent += 1
                step += 1

            if sent % args.fps == 0:
                elapsed = time.time() - start
                sys.stdout.write(f"\r  Sent {sent} frames ({elapsed:.1f}s)")
                sys.stdout.flush()

            time.sleep(interval)

    except KeyboardInterrupt:
        pass

    # Send blackout on exit
    print(f"\n  Sending blackout...")
    send_dmx_frame(h, [0] * 512)
    time.sleep(0.1)

    h.close()
    elapsed = time.time() - start
    print(f"Done. Sent {sent} frames in {elapsed:.1f}s")


if __name__ == '__main__':
    main()
