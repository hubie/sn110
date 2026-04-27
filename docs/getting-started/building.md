# Building the Firmware

The SN110 firmware cross-compiles from a modern Linux or macOS host to an ARM7TDMI
bFLT binary. A Docker-based toolchain handles all the cross-compilation complexity.

## Prerequisites

- **Docker** -- the build runs inside a Debian container with the ARM cross-compiler
- **GNU Make**
- **Python 3** -- used by `tools/elf2bflt.py` for ELF-to-bFLT conversion

No ARM toolchain installation is needed on your host machine.

## Building the Device Binary

```bash
make docker-bflt
```

This:

1. Builds a Docker image (`sn110-toolchain`) with `arm-linux-gnueabi-gcc` and QEMU
2. Cross-compiles the firmware as a position-independent ARM ELF
3. Converts the ELF to bFLT v2 format (the only executable format uClinux supports)
4. Outputs `build/sn110dmx.bflt`

The build prints a size summary. The binary must fit within the device's flash budget
(~450 KB). A typical build is around 26 KB.

## Running Tests

```bash
make docker-test
```

This cross-compiles the test suite for ARM, then runs it under QEMU user-mode
emulation. Tests use mock DMX devices and exercise the config parser, protocol
handlers, and CGI form processing.

All tests run deterministically with no network or hardware access.

## Build Targets

| Target | Description |
|--------|-------------|
| `make docker-bflt` | Build the main firmware binary |
| `make docker-test` | Run the test suite under QEMU |
| `make docker-cgi-bflt` | Build the web configuration CGI binary |
| `make docker-shell` | Drop into the build container for debugging |
| `make clean` | Remove all build artifacts |

## How the Build Works

The SN110 runs uClinux (Linux 2.0) on an ARM7TDMI with no MMU. This imposes several
unusual constraints:

- **No standard libc** -- the firmware uses a custom `minilib` (`src/oabi/minilib.c`)
  that implements just enough of libc (printf, string ops, memory ops) via direct
  syscalls
- **OABI syscall convention** -- Linux 2.0 on ARM uses the "old ABI" where syscalls
  are invoked via `swi 0x900000 + NR`. These are in `src/oabi/syscalls.S`
- **bFLT v2 binary format** -- no ELF support. The `tools/elf2bflt.py` script converts
  a position-independent ELF into bFLT with relocation entries
- **PIC via GOT** -- all global accesses go through a Global Offset Table. Register
  `sl` (r10) holds the GOT base, set by `src/oabi/crt0.S` at startup
- **Socket multiplexer** -- network calls go through `socketcall(2)` (`src/oabi/minisock.c`),
  not direct syscalls

!!! info "Why not use an off-the-shelf uClinux toolchain?"
    Historical uClinux toolchains (arm-linux-uclinux-gcc) are difficult to find and
    reproduce. The project uses a standard `arm-linux-gnueabi-gcc` cross-compiler
    with custom startup code and minimal headers, which is reproducible from any
    Debian-based system.

## Local Build (Without Docker)

If you have `arm-linux-gnueabi-gcc` installed natively (e.g., on Debian/Ubuntu):

```bash
sudo apt install gcc-arm-linux-gnueabi binutils-arm-linux-gnueabi qemu-user-static
make bflt        # Build device binary
make arm-test    # Run tests under QEMU
```
