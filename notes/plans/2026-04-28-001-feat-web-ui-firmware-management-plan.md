---
title: "feat: Add firmware management page to web UI"
type: feat
status: active
date: 2026-04-28
origin: notes/brainstorms/2026-04-28-web-ui-firmware-management-requirements.md
---

# feat: Add Firmware Management Page to Web UI

## Overview

Add a dedicated Firmware page to the SN110 web configuration interface showing
firmware version/build info, an install trigger button with confirmation, and a
link to GitHub releases. This gives browser-only users visibility into what's
running and a way to trigger installs without CLI tools.

## Problem Statement

Users currently have no way to see what firmware version is running or trigger
an install without using command-line tools (telnet, FTP, `tools/install.sh`).
The web UI only exposes configuration. Adding firmware visibility and install
controls makes the device more accessible.
(see origin: notes/brainstorms/2026-04-28-web-ui-firmware-management-requirements.md)

## Proposed Solution

A new CGI page at `/cgi-bin/fwget.cgi` / `/cgi-bin/fwpost.cgi` following the
exact same pattern as the existing config page:

- **GET** (`fwget.cgi`): Displays firmware info, install button, and updates link
- **POST** (`fwpost.cgi`): Shows confirmation page, or creates the trigger file

The CGI binary can either be a second entry point in the existing `cgi_config.c`
(dispatched by checking `argv[0]` or an env var) or a separate small binary.
A single binary with dispatch is preferred to save flash space.

## Technical Approach

### How the existing CGI pattern works

1. The device httpd at `/sbin/httpd` serves `/cgi-bin/*.cgi` by executing them
2. Each `.cgi` is a shell script wrapper that calls a binary in `/usr/bin/`
3. The httpd generates HTTP headers -- CGI programs output raw HTML only
4. GET vs POST is determined by `REQUEST_METHOD` env var
5. POST body is read from stdin, length from `CONTENT_LENGTH` env var

### New files needed

| File | Location on device | Purpose |
|------|--------------------|---------|
| `src/cgi/cgi_firmware.c` | (compiled into cgi_config binary) | Firmware page HTML + install trigger logic |
| `tools/web/fwget.cgi` | `/cgi-bin/fwget.cgi` | Shell wrapper: `#!/bin/sh` + `/usr/bin/cgi_config fw` |
| `tools/web/fwpost.cgi` | `/cgi-bin/fwpost.cgi` | Shell wrapper: `#!/bin/sh` + `/usr/bin/cgi_config fw` |

### Dispatch mechanism

The shell wrappers pass an argument (`fw`) to `cgi_config`. The binary checks
`argv[1]`: if `"fw"`, it runs the firmware page logic; otherwise, the existing
config page logic. This avoids a second binary.

### Firmware page behavior

**GET (fwget.cgi):**

```
+--------------------------------------------------+
| SN110-DMX01                    00:E0:01:00:EC:FD  |
+--------------------------------------------------+
|                                                    |
|  [Firmware]                                        |
|                                                    |
|  Version:     sn110dmx v0.3.0                      |
|  Build date:  2026-04-28                           |
|                                                    |
|  [Install Firmware]                                |
|                                                    |
|  Upload a new firmware binary to /tmp/sn110dmx     |
|  via FTP, then click Install.                      |
|                                                    |
|  Check for updates                                 |
|                                                    |
|  Back to configuration                             |
+--------------------------------------------------+
```

- Reuses the same CSS and header bar as the config page
- Shows `FW_VERSION` (from `common.h`, already `"0.3.0"`) and `FW_BUILD_DATE`
  (new `#define`, set by Makefile at compile time)
- "Install Firmware" is a form POST button
- "Check for updates" is `<a href="https://github.com/hubie/sn110/releases"
  target="_blank">`
- "Back to configuration" links to `/cgi-bin/cfgget.cgi`

**POST step 1 -- confirmation page (fwpost.cgi, no `confirm` param):**

```
+--------------------------------------------------+
| SN110-DMX01                    00:E0:01:00:EC:FD  |
+--------------------------------------------------+
|                                                    |
|  Install Firmware                                  |
|                                                    |
|  This will:                                        |
|  - Stop the running daemon                         |
|  - Start the firmware from /tmp/sn110dmx           |
|  - DMX output will be interrupted                  |
|                                                    |
|  [Confirm Install]        [Cancel]                 |
+--------------------------------------------------+
```

- The "Confirm Install" button POSTs with `action=confirm`
- "Cancel" links back to `fwget.cgi`

**POST step 2 -- trigger install (fwpost.cgi, `action=confirm`):**

1. Creates `/tmp/install.arm` (just `open()` + `close()`)
2. Shows a success page: "Install triggered. The device will restart within
   10 seconds." with a meta-refresh back to `fwget.cgi` after 15 seconds

### Version and build date constants

```c
/* common.h — already exists */
#define FW_VERSION "0.3.0"

/* New — set by Makefile via -D flag */
#ifndef FW_BUILD_DATE
#define FW_BUILD_DATE "unknown"
#endif
```

Makefile addition:
```makefile
OABI_CFLAGS += -DFW_BUILD_DATE='"$(shell date +%Y-%m-%d)"'
```

### Navigation between pages

The config page gets a "Firmware" link (in the header bar or after the form).
The firmware page gets a "Configuration" link. Both share the same header bar
and CSS.

### Web deploy updates

`tools/device/install_web.sh` and `tools/deploy_web.py` need to include the
two new `.cgi` wrapper scripts in their file lists.

## Acceptance Criteria

- [ ] GET `/cgi-bin/fwget.cgi` shows firmware version and build date
- [ ] GET `/cgi-bin/fwget.cgi` shows an "Install Firmware" button
- [ ] Clicking "Install Firmware" shows a confirmation page (not a JS alert)
- [ ] Confirming creates `/tmp/install.arm` and shows a success message
- [ ] "Check for updates" link opens GitHub releases page in a new tab
- [ ] Config page links to firmware page and vice versa
- [ ] CSS and header bar are consistent between both pages
- [ ] `FW_BUILD_DATE` is set automatically by the Makefile
- [ ] Web deploy scripts (`install_web.sh`, `deploy_web.py`) updated to
      include the new CGI wrappers
- [ ] Binary size stays within flash budget after adding firmware page code

## Scope Boundaries

- No HTTP file upload (see origin)
- No on-device update checking (see origin)
- No rollback UI (see origin)
- No JavaScript confirmation dialogs (see origin)

## Dependencies & Risks

- **Low risk**: The install trigger (`/tmp/install.arm`) is the same mechanism
  used by `tools/install.sh`. Creating the file is a single syscall.
- **Flash space**: Adding ~100-200 lines of C to the CGI binary adds minimal
  size (~1-2 KB to the bFLT). Well within budget.
- **httpd routing**: The httpd already handles any `/cgi-bin/*.cgi` file. Adding
  new `.cgi` wrapper scripts is the established pattern.

## Sources & References

- **Origin document:** [notes/brainstorms/2026-04-28-web-ui-firmware-management-requirements.md](notes/brainstorms/2026-04-28-web-ui-firmware-management-requirements.md)
  Key decisions carried forward: separate page, HTML confirmation (no JS alerts),
  GitHub link instead of on-device update check
- Existing CGI pattern: `src/cgi/cgi_config.c`, `tools/web/cfgget.cgi`,
  `tools/web/cfgpost.cgi`
- Install mechanism: `tools/device/install.sh`, `SAFETY.md`
- Version constant: `src/common.h:14` (`FW_VERSION`)
- Web deploy: `tools/device/install_web.sh`, `tools/deploy_web.py`
