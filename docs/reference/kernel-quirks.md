# Kernel Quirks

The SN110 runs uClinux (Linux 2.0.38) on an ARM7TDMI with no MMU. This kernel
predates most modern Linux conventions and has several behaviors that differ
significantly from what contemporary developers expect.

## OABI Syscall Convention

Linux 2.0 on ARM uses the "old ABI" (OABI) where syscalls are invoked via:

```asm
swi 0x900000 + NR
```

This is unlike modern ARM Linux (EABI) which uses `swi 0` with the syscall
number in `r7`. All syscall stubs live in `src/oabi/syscalls.S`.

### Socket Calls

Network syscalls are not direct -- they all go through the `socketcall(2)`
multiplexer (syscall 102). The first argument selects the operation:

| socketcall # | Function |
|-------------|----------|
| 1 | socket() |
| 2 | bind() |
| 3 | connect() |
| 5 | accept() |
| 6 | getsockname() |
| 10 | sendto() |
| 11 | recvfrom() |
| 12 | shutdown() |
| 14 | setsockopt() |

The wrapper is in `src/oabi/minisock.c`.

## No Standard C Library

The kernel's uClinux userspace has no usable libc. The firmware implements a
minimal replacement in `src/oabi/minilib.c`:

- `printf` / `snprintf` -- formatted output
- `memcpy`, `memset`, `memcmp` -- memory operations
- `strlen`, `strcmp`, `strncmp`, `strncpy` -- string operations
- `strtol`, `atoi` -- number parsing
- `ntohl`, `htonl`, `ntohs`, `htons` -- byte order conversion

!!! warning "Don't include standard headers"
    Never `#include <stdlib.h>` or other standard libc headers in device code.
    Use `minilib` functions or add new ones to `src/oabi/minilib.c`.

## bFLT v2 Binary Format

The kernel only supports bFLT (flat binary) executables, not ELF. Key
characteristics:

- **Position Independent Code** via a Global Offset Table (GOT)
- Register `sl` (r10) holds the GOT base, initialized by `src/oabi/crt0.S`
- The `tools/elf2bflt.py` script converts a PIC ARM ELF into bFLT v2 format
  with relocation entries
- Maximum binary size is constrained by flash (~450 KB budget)

## clone() Is Unstable

The `clone()` syscall (used to create threads) is unreliable on this kernel.
The firmware uses a single-threaded polling event loop. Do not attempt to add
threading.

## Filesystem: Minix v1

The root filesystem is Minix v1, the only filesystem supported by this uClinux
build. Key limitations:

- Maximum filename length: 14 characters (Minix v1) or 30 (v2)
- No journaling -- interrupted writes can corrupt the filesystem
- Limited inode count
- The filesystem lives on flash and is very small (~3 MB total)

The firmware uses atomic write-to-tmp-then-rename to mitigate filesystem
corruption risk from config saves.

## select() Behavior

The `select()` implementation in this kernel uses the old-style `fd_set` with a
maximum of 256 file descriptors. The firmware's main loop uses `select()` to
multiplex protocol sockets and the main tick timer.

## Network Interface Detection

Link status detection uses `SIOCGIFFLAGS` on `eth0`, checking the `IFF_RUNNING`
flag. The kernel may not always report `IFF_RUNNING` correctly, so the firmware
falls back to `IFF_UP` and defaults to "link up" if detection fails entirely.

IP address detection uses `SIOCGIFADDR`. When DHCP is active, the firmware polls
this periodically to detect lease acquisition.

## Device Drivers

The DMX and LCD kernel drivers have unusual behaviors documented in their
respective hardware reference pages:

- [DMX Ports](../hardware/dmx-ports.md) -- close-triggered TX, mode ioctl values
- [LCD Display](../hardware/lcd-display.md) -- dual-buffer architecture, contrast crash bug, cmd 7 partial writes
