# Ethernet & Network

The SN110 uses the NS7520's built-in Ethernet MAC for 10/100 Mbps networking.

## Network Configuration

| Parameter | Value |
|-----------|-------|
| **Interface** | `eth0` |
| **Speed** | 10/100 Mbps auto-negotiation |
| **MAC prefix** | 00:E0:01 (Strand Lighting OUI) |
| **Default netmask** | 255.255.255.0 |
| **IP assignment** | BOOTP, DHCP, or static (via `220node.cfg`) |

## Services

| Service | Port | Notes |
|---------|------|-------|
| HTTP | 80 | Web configuration interface |
| Telnet | 23 | Shell access |
| FTP | 21 | File upload/download |
| sACN | 5568 (UDP) | E1.31 multicast |

!!! tip "Finding your device"
    If you don't know the device's IP address, check the LCD display after
    boot -- the status screen shows the current IP. You can also scan your
    network with `nmap -sn 192.168.2.0/24` or look for the Strand OUI
    (00:E0:01) in your DHCP server's lease table.

## IP Address Detection

The firmware detects the device's IP address using the `SIOCGIFADDR` ioctl
on the `eth0` interface. Link status is detected via `SIOCGIFFLAGS`, checking
the `IFF_RUNNING` flag (with `IFF_UP` as a fallback for kernels that don't
report `IFF_RUNNING`).

When DHCP is enabled, the firmware polls for IP acquisition and updates the
configuration and LCD display when an address is obtained.

## BOOTP Recovery

If the device cannot boot normally, it may fall back to BOOTP for network
configuration and TFTP for firmware loading. See
[Network Recovery](../recovery/network-recovery.md) for the procedure.
