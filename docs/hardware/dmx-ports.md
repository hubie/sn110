# DMX Ports

The SN110 has two DMX512 ports, each exposed as a character device driven by a
custom kernel module. Each port can be independently configured as transmit (TX),
receive (RX), or off.

## Device Files

| Port | Device | Connector |
|------|--------|-----------|
| Port 0 | `/dev/dmx0` | 5-pin XLR (labeled "1") |
| Port 1 | `/dev/dmx1` | 5-pin XLR (labeled "2") |

## UART Configuration (DMX512 Standard)

The NS7520's two UARTs are configured for DMX512 signaling:

| Parameter | Value |
|-----------|-------|
| Baud rate | 250,000 bps |
| Data bits | 8 |
| Stop bits | 2 |
| Parity | None |
| Break signal | >= 88 us |
| Mark After Break (MAB) | 8-1000 us |

## Kernel Driver Interface

### Configuration ioctl

```
Number:  0x40106401
Macro:   _IOW('d', 1, struct dmx_config)
```

| Field | Bits | Description |
|-------|------|-------------|
| Direction | `_IOC_WRITE` (1) | Data flows from userspace to kernel |
| Type | `'d'` (0x64) | DMX device class |
| Number | 1 | Only one ioctl command |
| Struct size | 16 bytes | |

### struct dmx_config

```c
struct dmx_config {         /* 16 bytes total */
    uint8_t  mode;          /* offset 0:  0=OFF, 1=TX, 2=RX, 3=RAW */
    uint8_t  flags;         /* offset 1:  always 0 in dmxtst */
    uint16_t buf_size;      /* offset 2:  512 for TX, 0 otherwise */
    uint16_t rate;          /* offset 4:  100 for TX, 0 otherwise */
    char     label[8];      /* offset 6:  up to 8-char label string */
    uint8_t  reserved;      /* offset 14: always 0 */
    uint8_t  pad;           /* offset 15: struct padding */
};
```

### Mode Values

| Value | Name | Description | buf_size | rate | FIONBIO |
|-------|------|-------------|----------|------|---------|
| 0 | OFF | Disable DMX port | 0 | 0 | No |
| 1 | TX | DMX transmit | 512 | 100 | No |
| 2 | RX | DMX receive | 0 | 0 | Yes |
| 3 | RAW | Raw serial buffer (no wire I/O) | 0 | 0 | Yes |

Additional modes supported by the `dmxtst` test utility:

- **silentoff** -- same as OFF but suppresses console output
- **test** -- direct write loop bypassing the ioctl, generates a scrolling test pattern

## Data I/O

### Transmitting DMX (TX mode)

```c
int fd = open("/dev/dmx0", O_RDWR);
struct dmx_config cfg = { .mode = 1 /* TX */ };
ioctl(fd, 0x40106401, &cfg);
write(fd, dmx_data, 512);  /* 512-byte DMX frame */
```

!!! warning "TX uses close-triggered transmission"
    The kernel driver transmits the DMX frame when the file descriptor is
    **closed**, not when `write()` is called. The firmware uses an
    open-write-close cycle per frame:

    ```c
    fd = open("/dev/dmx0", O_RDWR);
    ioctl(fd, DMX_IOCTL, &tx_cfg);
    write(fd, frame, 512);
    close(fd);  /* This triggers actual DMX transmission */
    ```

    See [DMX driver findings](../reference/reverse-engineering-log.md) for the
    full investigation.

### Receiving DMX (RX mode)

```c
int fd = open("/dev/dmx0", O_RDWR);
struct dmx_config cfg = { .mode = 2 /* RX */ };
ioctl(fd, 0x40106401, &cfg);
int nonblock = 1;
ioctl(fd, 0x5421, &nonblock);  /* FIONBIO */
read(fd, buffer, 512);
```

### Blocking Control

FIONBIO ioctl (0x5421) is used for RX and RAW modes:

- `arg = 0` -- blocking I/O
- `arg = 1` -- non-blocking I/O (default for RX/RAW in `dmxtst`)

## ioctl Number Construction (ARM)

The binary constructs the ioctl number efficiently in ARM assembly:

```asm
mov  r1, #0x6400        ; type 'd' = 0x64, shifted left 8 = 0x6400
add  r1, r1, #1         ; add command number 1 -> r1 = 0x6401
orr  r1, r1, r1, lsl #20  ; merge direction and size bits -> 0x40106401
```

## Syscalls Used by dmxtst

| SVC Instruction | Syscall # | Function |
|-----------------|-----------|----------|
| SVC 0x900001 | 1 | exit() |
| SVC 0x900004 | 4 | write() |
| SVC 0x900005 | 5 | open() |
| SVC 0x900006 | 6 | close() |
| SVC 0x900013 | 19 | lseek() |
| SVC 0x900036 | 54 | ioctl() |
| SVC 0x900052 | 82 | select() |
| SVC 0x90006A | 106 | stat() |

All use the ARM Linux 2.0 old-style `SVC 0x900000+NR` encoding (OABI).

## Source Binary

These findings were reverse-engineered from `dmxtst` (6,072 bytes, bFLT v2,
ARM7TDMI), the factory DMX test utility found on the device filesystem.
