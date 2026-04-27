# Configuration System

The firmware's configuration system manages persistent settings stored in
`/etc/220node.cfg` on the device's Minix flash filesystem.

## Config File Format

The `220node.cfg` file uses a simple `key=value` format, one setting per line.
This is the same format used by Strand's original `nodecfg` tool, extended with
additional keys for the open firmware's features.

```ini
# Strand-compatible keys (safe for nodecfg)
nodeaddr=192.168.2.231
nodemask=255.255.255.0
nodegate=192.168.2.1
hostname=SN110

# Extension keys (open firmware only)
protocol=sacn
port0_mode=tx
port0_universe=1
port1_mode=rx
port1_universe=2
dmx_hold=10
lcd_contrast=32
lcd_backlight=on
addr_mode=dhcp_static
```

## Strand Compatibility

The config system maintains backward compatibility with the factory `nodecfg`
tool, which reads and writes `/etc/220node.cfg` to flash. This is important
because `nodecfg` only understands a fixed set of keys.

The firmware uses a **split-save strategy**:

| Operation | File | Contents |
|-----------|------|----------|
| `config_save()` | `/etc/220node.cfg` | All keys (Strand + extension) |
| `config_save_strand()` | `/etc/220node.cfg.strand` | Strand-compatible keys only |

The Strand-only file is safe to pass to `nodecfg put` for flash persistence.
The full config file includes extension keys that `nodecfg` would reject.

!!! warning "Flash write safety"
    Config saves use an atomic write-to-tmp-then-rename pattern to prevent
    filesystem corruption from interrupted writes. The `rename()` syscall was
    added to the OABI syscall table specifically for this purpose.

## node_config_t

The central configuration structure (defined in `common.h`):

```c
typedef struct {
    char     hostname[16];
    uint32_t ip_addr;
    uint32_t netmask;
    uint32_t gateway;
    uint8_t  mac[6];

    port_config_t ports[DMX_MAX_PORTS];

    int      active_protocol;   /* PROTO_SACN / PROTO_ARTNET / PROTO_SHOWNET */
    uint16_t dmx_hold_time;     /* Seconds to hold last DMX values */

    uint8_t  addr_mode;         /* ADDR_MODE_STATIC / DHCP / DHCP_STATIC */
    uint8_t  lcd_contrast;      /* 0-63 */
    uint8_t  lcd_backlight;     /* LCD_BACKLIGHT_OFF / ON / AUTO */
    uint16_t dmx_slot_monitor[2];
} node_config_t;
```

## Configuration API

| Function | Purpose |
|----------|---------|
| `config_defaults(config)` | Initialize with safe defaults |
| `config_load(path, config)` | Parse config file into struct |
| `config_save(path, config)` | Write all settings to file (atomic) |
| `config_save_strand(path, config)` | Write Strand-compatible subset |
| `config_generate_ifup(path, config)` | Generate `/etc/ifup-eth0` network script |

## Web Configuration (CGI)

The firmware includes a web-based configuration interface served by the device's
built-in HTTP server. The CGI binary (`cgi_config.c`) generates HTML forms and
processes POST submissions.

Configurable settings via the web UI:

- Hostname, IP address, netmask, gateway
- DHCP / static / DHCP-with-static-fallback addressing
- Protocol selection (sACN, Art-Net, ShowNet)
- Per-port mode (TX/RX/Off) and universe assignment
- DMX hold time
- LCD contrast (0-63) and backlight (off/on/auto)

## Address Modes

| Mode | Constant | Behavior |
|------|----------|----------|
| Static | `ADDR_MODE_STATIC` | Use IP from config file |
| DHCP | `ADDR_MODE_DHCP` | Request IP via DHCP |
| DHCP+Static | `ADDR_MODE_DHCP_STATIC` | Try DHCP, fall back to static IP |

When DHCP is active, the firmware polls for IP acquisition using `SIOCGIFADDR`
on `eth0`. Once an address is obtained, the configuration and LCD display are
updated.

## Defaults

| Setting | Default | Notes |
|---------|---------|-------|
| Protocol | sACN | |
| Port 0 | TX, universe 1 | |
| Port 1 | RX, universe 2 | |
| DMX hold time | 10 seconds | |
| LCD contrast | 32 | Hardware range 0-63 |
| LCD backlight | On | |
| Address mode | Inferred | Derived from nodeaddr presence |
