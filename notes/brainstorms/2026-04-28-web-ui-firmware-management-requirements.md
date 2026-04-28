---
date: 2026-04-28
topic: web-ui-firmware-management
---

# Web UI Firmware Management

## Problem Frame

SN110 users currently manage firmware updates entirely via command-line tools
(FTP + `tools/install.sh`). The web UI has no firmware information or update
controls. Users who are comfortable with a browser but less so with CLI tools
have no way to trigger an install or check for new releases.

## Requirements

- R1. A dedicated Firmware page in the web UI, separate from the configuration
  page.

- R2. The Firmware page displays the current firmware version, build date, and
  binary name so the user can confirm what's running.

- R3. An "Install" button that triggers the existing `/tmp/install.arm` install
  mechanism. This assumes the user has already FTP'd a new binary to the device.

- R4. Before triggering the install, a confirmation step warns the user that the
  daemon will restart and asks them to confirm. This prevents accidental clicks.

- R5. A "Check for updates" link that opens the GitHub releases page
  (`https://github.com/hubie/sn110/releases`) in the user's browser. The device
  itself does not fetch anything -- the link is a plain HTML anchor.

## Success Criteria

- User can see what firmware version is running without using telnet
- User can trigger a firmware install from the browser after FTP'ing the binary
- User can reach the releases page in one click to check for newer versions
- The install trigger uses the same mechanism as `tools/install.sh` (creating
  `/tmp/install.arm`), so it's no more or less safe than the existing CLI flow

## Scope Boundaries

- No HTTP file upload -- the user FTPs the binary separately. Implementing
  multipart form parsing on this platform is disproportionate to the value.
- No automatic update checking -- the device has no TLS stack and cannot fetch
  from GitHub. The link opens in the user's browser.
- No rollback UI -- rollback remains a CLI operation (`tools/restore.sh`).
- The confirmation step is HTML-based (e.g., a second page or form), not a
  JavaScript alert (which would block the browser extension and automation tools).

## Key Decisions

- **Separate page, not a section on the config page**: Keeps firmware management
  distinct from configuration. The config page is already dense.
- **Confirmation before install**: One accidental click would restart the daemon
  and interrupt DMX output. A confirm step is low-cost and prevents this.
- **GitHub link, not on-device update check**: The device cannot speak HTTPS.
  A static link gives the user the same outcome with zero device-side complexity.

## Dependencies / Assumptions

- The `/etc/rc` watchdog that monitors `/tmp/install.arm` is always running
  (this is part of the factory boot script, not something we control)
- The CGI can create files in `/tmp/` (same permissions as the existing config
  save mechanism)
- Version and build date are compiled into the binary (available as constants)

## Outstanding Questions

### Deferred to Planning

- [Affects R1][Technical] How does the device httpd route to a second CGI page?
  Need to check whether a separate CGI binary or a query parameter on the
  existing one is the right approach.
- [Affects R2][Technical] Where are version/build-date constants currently
  defined, and are they accessible from the CGI binary?
- [Affects R4][Technical] What's the simplest way to implement confirmation
  without JavaScript? A two-step form POST (show confirm page, then trigger)
  is the likely approach.

## Next Steps

-> `/ce:plan` for structured implementation planning
