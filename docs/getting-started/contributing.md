# Contributing

Contributions are welcome! This project benefits from a wide range of skills --
you don't need to be an embedded systems expert to help.

## Areas Where Help Is Needed

- **Testing on other hardware revisions** -- the firmware has only been tested on
  board rev V2.6.11. If you have an SN110, we'd love to hear how it works on yours.
- **Protocol testing** -- testing with different lighting consoles and software
  (ETC Eos, MA Lighting, Chamsys, QLC+, xLights, etc.)
- **Documentation** -- improving these docs, adding diagrams, writing tutorials
- **Web UI improvements** -- the configuration interface is functional but basic
- **Reverse engineering** -- there's still plenty to discover about the factory
  firmware and kernel driver behavior

## Development Workflow

### 1. Fork and Clone

```bash
git clone https://github.com/YOUR_USERNAME/sn110.git
cd sn110
```

### 2. Create a Feature Branch

```bash
git checkout -b feat/your-feature-name
```

### 3. Build and Test

```bash
make docker-bflt    # Build the device binary
make docker-test    # Run the test suite (must pass before submitting)
```

### 4. Submit a Pull Request

Push your branch and open a PR against `main`. Include:

- What the change does and why
- How you tested it (on-device if applicable, or test suite)
- Any hardware-specific observations

## Code Style

- **C99** with `-Wall -Wextra` clean
- 4-space indentation, no tabs
- Braces on same line for control structures, next line for functions
- Comments explain *why*, not *what*
- Keep functions short -- most fit on one screen
- No dynamic memory allocation (`malloc`/`free`) -- everything is static or stack

## Architecture Guidelines

- **Single-threaded** -- the daemon uses a polling event loop, not threads.
  `clone()` is unstable on this kernel. Don't add threading.
- **No standard libc** -- use `minilib` functions or add new ones to
  `src/oabi/minilib.c`. Don't `#include <stdlib.h>` in device code.
- **Module isolation** -- each subsystem (`src/sacn/`, `src/lcd/`, etc.) should
  communicate through well-defined interfaces, not by reaching into other modules'
  global state.
- **`#ifndef HOST_BUILD`** -- device-only code (ioctls, `/dev/` access, LCD, network
  detection) is guarded so the test suite can build on the host.

## Testing

The test suite lives in `tests/test_basics.c` and runs under QEMU emulating ARM.
Tests use mock DMX devices (`src/dmx/dmx_mock.c`) for deterministic behavior.

To add a test:

1. Write a `static void test_your_feature(void)` function
2. Add it to the `main()` runner at the bottom of `test_basics.c`
3. Use `assert()` for checks -- the test framework is intentionally minimal

!!! tip "On-device testing"
    Some things can only be verified on real hardware (LCD rendering, DMX timing,
    network behavior on Linux 2.0). Document what you observe -- these findings
    are valuable to the project even if they're not automated tests.

## Documenting Findings

If you discover something about the hardware or kernel behavior, please document it.
The [Reverse Engineering Log](../reference/reverse-engineering-log.md) collects
these findings so future contributors don't have to rediscover them.

Key things worth documenting:

- Kernel driver behavior that differs from standard Linux
- Hardware quirks (timing, register values, pin behavior)
- ioctl commands and their effects
- Anything that took you more than 30 minutes to figure out
