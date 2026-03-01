#!/usr/bin/env python3
"""
MagicDMX USB DMX reader for SN110 end-to-end testing.

Reads DMX data from a ChamSys MagicDMX USB adapter via HID.
Shows channel values to verify sACN→DMX pipeline.

Usage:
    source /tmp/sn110-test-env/bin/activate
    python3 tools/magicdmx_reader.py

Requires: pip install hidapi
"""

import sys
import time

CHAMSYS_VID = 0x10C4
MAGICDMX_PID = 0x857E

# MagicDMX HID protocol (reverse-engineered):
# The MagicDMX uses 33-byte HID reports.
# Report ID 0: Control/status
# The device can operate in input (DMX→USB) or output (USB→DMX) mode.

def find_magicdmx():
    """Find and open a MagicDMX device."""
    try:
        import hid
    except ImportError:
        print("ERROR: hidapi not installed.")
        print("  source /tmp/sn110-test-env/bin/activate")
        print("  pip install hidapi")
        sys.exit(1)
    
    print("Enumerating HID devices...")
    all_devices = hid.enumerate()
    
    # Find MagicDMX
    magicdmx_devices = [d for d in all_devices
                        if d['vendor_id'] == CHAMSYS_VID
                        and d['product_id'] == MAGICDMX_PID]
    
    if not magicdmx_devices:
        print(f"No MagicDMX found (looking for VID=0x{CHAMSYS_VID:04X} PID=0x{MAGICDMX_PID:04X})")
        print("\nAll HID devices found:")
        for d in all_devices:
            print(f"  VID=0x{d['vendor_id']:04X} PID=0x{d['product_id']:04X}"
                  f" {d.get('manufacturer_string', '')} {d.get('product_string', '')}")
        return None
    
    print(f"Found {len(magicdmx_devices)} MagicDMX interface(s):")
    for d in magicdmx_devices:
        print(f"  Interface {d.get('interface_number', '?')}:"
              f" {d.get('product_string', 'MagicDMX')}"
              f" path={d['path']}")
    
    return magicdmx_devices

def try_read_dmx(devices):
    """Attempt to read DMX data from the MagicDMX."""
    import hid
    
    for dev_info in devices:
        iface = dev_info.get('interface_number', '?')
        print(f"\nTrying interface {iface}...")
        
        try:
            h = hid.Device(path=dev_info['path'])
        except Exception as e:
            print(f"  Cannot open: {e}")
            continue
        
        print(f"  Opened: {h.manufacturer} {h.product}")
        
        # Try to set the device to DMX input mode
        # MagicDMX protocol: send a control report to configure mode
        #
        # Known MagicDMX HID reports (from various reverse engineering efforts):
        # Report 0x00: 33 bytes - DMX data chunks (output mode)
        # The device sends back DMX input data as HID input reports
        #
        # Let's try reading raw reports first to see what comes in
        
        print("  Reading HID reports (5 second window)...")
        print("  (Send DMX data to the MagicDMX's input port to see values)")
        
        h.nonblocking = True
        start = time.time()
        report_count = 0
        dmx_data = [0] * 512
        
        while time.time() - start < 5.0:
            data = h.read(64)  # Read up to 64 bytes
            if data:
                report_count += 1
                if report_count <= 5:
                    hex_str = ' '.join(f'{b:02X}' for b in data[:min(len(data), 16)])
                    print(f"  Report #{report_count} ({len(data)} bytes): {hex_str}...")
                elif report_count == 6:
                    print("  (further reports suppressed, still reading...)")
            time.sleep(0.01)
        
        print(f"  Received {report_count} reports in 5 seconds")
        
        if report_count == 0:
            print("  No data received. The device may be in output-only mode,")
            print("  or it may need a control command to switch to input mode.")
            
            # Try sending an enable-input command
            # Various MagicDMX protocol guesses:
            print("  Trying to enable DMX input mode...")
            try:
                # Try common control commands
                # Mode byte: 0x00=off, 0x01=output, 0x02=input
                for cmd in [
                    bytes([0x00, 0x02]),           # Simple mode switch
                    bytes([0x00, 0x01, 0x02]),     # Alt format
                    bytes([0x02]),                  # Just mode byte
                ]:
                    try:
                        h.write(cmd)
                        time.sleep(0.5)
                        data = h.read(64)
                        if data:
                            hex_str = ' '.join(f'{b:02X}' for b in data[:16])
                            print(f"  Response to {cmd.hex()}: {hex_str}")
                            break
                    except Exception as e:
                        pass
                
                # Read again after control commands
                time.sleep(1)
                got_data = False
                for _ in range(100):
                    data = h.read(64)
                    if data:
                        hex_str = ' '.join(f'{b:02X}' for b in data[:16])
                        print(f"  Post-config report: {hex_str}")
                        got_data = True
                        break
                    time.sleep(0.01)
                
                if not got_data:
                    print("  Still no data after mode switch attempts.")
                    
            except Exception as e:
                print(f"  Control command error: {e}")
        
        h.close()
        
        if report_count > 0:
            return True
    
    return False

def monitor_mode(devices):
    """Continuously monitor DMX data from MagicDMX."""
    import hid
    
    # Use first available interface
    for dev_info in devices:
        try:
            h = hid.Device(path=dev_info['path'])
            break
        except:
            continue
    else:
        print("Cannot open any MagicDMX interface")
        return
    
    print(f"\nMonitoring DMX from {h.manufacturer} {h.product}")
    print("Press Ctrl+C to stop\n")
    
    h.nonblocking = True
    last_print = 0
    
    try:
        while True:
            data = h.read(64)
            if data and time.time() - last_print > 0.5:
                # Print first 10 channels
                channels = data[:10] if len(data) >= 10 else data
                ch_str = ' '.join(f'Ch{i+1}={v:3d}' for i, v in enumerate(channels))
                sys.stdout.write(f'\r  {ch_str}')
                sys.stdout.flush()
                last_print = time.time()
            time.sleep(0.01)
    except KeyboardInterrupt:
        print("\nStopped")
    finally:
        h.close()

def main():
    print("╔══════════════════════════════════════════════╗")
    print("║  MagicDMX DMX Reader — SN110 Test Tool       ║")
    print("╚══════════════════════════════════════════════╝")
    print()
    
    devices = find_magicdmx()
    if not devices:
        sys.exit(1)
    
    print("\n--- Probing device capabilities ---")
    if try_read_dmx(devices):
        print("\n--- Entering monitor mode ---")
        monitor_mode(devices)
    else:
        print("\n⚠️  The MagicDMX Basic is OUTPUT-only (USB→DMX).")
        print("   It cannot read DMX input from the SN110.")
        print()
        print("   Alternatives for verifying DMX output:")
        print("   1. Use a different DMX input adapter (ENTTEC Pro, DMXKing, etc.)")
        print("   2. Verify via daemon logs (telnet to device)")
        print("   3. Connect a DMX fixture and observe visually")
        print("   4. Use the SN110's Port B as a loopback (Port A out → Port B in)")

if __name__ == '__main__':
    main()
