---
title: Recovering Strand SN110 Official Firmware via Wayback Machine and Reverse-Engineering the .chk Checksum Algorithm
category: reverse-engineering
date: 2026-03-24
tags:
  - firmware
  - sn110
  - strand-lighting
  - wayback-machine
  - internet-archive
  - checksum
  - chk-format
  - uclinux
  - minix-filesystem
  - flash-update
  - binary-analysis
component: firmware/build-toolchain
severity: medium
symptoms:
  - No public download links for official Strand SN110 firmware exist on the live web
  - strandlighting.com is offline and redirects to Signify/Philips
  - Forum references to v2.10.8 firmware exist but contain no download links
  - Custom firmware images cannot be validated or flashed without knowing the .chk checksum algorithm
root_cause: >
  Official firmware files were never mirrored publicly but were archived by the
  Wayback Machine in 2003-2004; the .chk format prepends a 4-byte LE checksum
  computed as (sum of all 32-bit LE words in the payload + 9) mod 2^32
related:
  - docs/recovery.md (Option B: Original Strand Firmware Update File)
  - SAFETY.md (flash slot layout, .chk format listed as unknown)
  - PLAN.md (Phase 6 packaging, Open Question #4)
  - dump/firmware/usr/bin/flashsw.sh (on-device update script)
  - tools/analyze_flash.py (Minix filesystem analyzer)
---

## Problem

The SN110 open firmware project needed a way to build Strand-compatible `.chk`
firmware images so that other SN110 owners could flash custom firmware using the
standard Strand update mechanism (`flashsw.sh`). This was blocked by two unknowns:

1. **No firmware files available** — Strand Lighting's website is offline (acquired
   by Philips, then Signify). No public mirrors existed.
2. **Unknown checksum algorithm** — `nodecfg checksum` validates `.chk` files before
   flashing, but the algorithm was undocumented.

## Investigation

### Web searches (mostly failed)

- Searched for "Strand SN110 firmware download", "sn110.chk", ETC support pages.
- Blue Room forum thread mentioned v2.10.8 firmware (12MHz/16MHz variants) but had
  no download links.
- ControlBooth, GoKnight, Parlights had manuals and service info but no firmware files.
- ETC support had only a FAQ about the 6ms DMX idle time requirement — no firmware.

### Wayback Machine CDX API (breakthrough)

Standard Wayback Machine page fetches were blocked by the WebFetch tool, but the
CDX search API accepts direct `curl` requests and can search all archived URLs for
a domain.

**Step 1** — Search the known downloads directory:

```bash
curl -sL "https://web.archive.org/cdx/search/cdx?\
url=strandlighting.com/clientuploads/directory/downloads/*&\
output=text&fl=original,timestamp,statuscode&\
filter=statuscode:200&collapse=urlkey" | grep -i sn110
```

Found manuals, datasheets, and console software — but **no SN110 firmware**.

**Step 2** — Search the entire domain with a wildcard:

```bash
curl -sL "https://web.archive.org/cdx/search/cdx?\
url=strandlighting.com/*sn110*&\
output=text&fl=original,timestamp,statuscode&\
filter=statuscode:200&collapse=urlkey"
```

**Hit.** Two firmware files found in `/program_2003/`:

| File | Archived | Size | Source path |
|------|----------|------|-------------|
| `sn110-2_6_11.chk` | 2003-12-14 | 1,803,888 bytes | `program_2003/lightpalette2.6.11_15/` |
| `sn110.26d` | 2004-01-05 | 1,801,256 bytes | `program_2003/lightpalette2.6.d/` |

**Step 3** — Download raw binaries using the `if_` URL format (bypasses Wayback
HTML injection):

```bash
curl -sL -o sn110-2_6_11.chk \
  "https://web.archive.org/web/20031214090258if_/\
http://www.strandlighting.com:80/program_2003/lightpalette2.6.11_15/sn110-2_6_11.chk"
```

Both downloaded successfully. The v2.6.11 file matches the firmware version on our
device exactly (board rev V2.6.11, date 16/08/2004). (auto memory [claude])

### Binary analysis

- `file` command: "data" (no recognized format header).
- `xxd` revealed: first 4 bytes non-zero, bytes 4–4095 all zeros, data starts at
  offset `0x1000`.
- Data at `0x1000` is ARM machine code — the Linux kernel entry point.
- Strings found: `Linux version 2.0.38.1pre7arm`, Minix filesystem references.

### Checksum reverse engineering

- CRC32, byte sum, 16-bit word sum: none matched the stored header value.
- 32-bit LE word sum of `data[4:]` was **very close** to the stored value (LE
  interpretation): off by exactly **9**.
- Tested the second firmware file — also off by exactly 9.
- Algorithm confirmed: `checksum = (sum_of_32bit_LE_words + 9) & 0xFFFFFFFF`
- Verified: computed checksums match stored values in both files **perfectly**.

## Root Cause / Key Discovery

### `.chk` File Format

```
Offset 0x0000:  [4 bytes]       Checksum (little-endian uint32)
Offset 0x0004:  [4092 bytes]    Zero padding
Offset 0x1000:  [rest of file]  Payload (kernel + Minix filesystem image)
```

The payload is what `wflash` writes directly to `/dev/blk5` (main flash partition)
or `/dev/blk3` (backup partition).

### Checksum Algorithm

```
checksum = (sum of all 32-bit little-endian words from byte 4 to EOF, + 9) & 0xFFFFFFFF
```

The constant `+9` is baked into the original Strand toolchain. Its origin is unknown
(may be an artifact of padding contributing a fixed value, or a deliberate magic
constant), but it is consistent across both firmware versions and must be reproduced
exactly.

### On-Device Flash Update Flow (flashsw.sh)

```
1. FTP sn110.chk to /tmp/sn110.chk     (or sn110bk.chk for backup slot)
2. nodecfg checksum /tmp/sn110.chk      validates checksum (must pass)
3. eflash /dev/blk5                      erase main flash partition
4. vflash . /dev/blk5                    verify erase (block is blank)
5. wflash /tmp/sn110.chk /dev/blk5      write image to flash
6. vflash /tmp/sn110.chk /dev/blk5      verify write matches file
7. reboot
```

## Solution

### Verifying an existing `.chk` file

```python
import struct

def verify_chk(path):
    with open(path, 'rb') as f:
        data = f.read()

    stored = struct.unpack('<I', data[:4])[0]

    s32 = 0
    for i in range(4, len(data), 4):
        if i + 4 <= len(data):
            s32 = (s32 + struct.unpack('<I', data[i:i+4])[0]) & 0xFFFFFFFF

    computed = (s32 + 9) & 0xFFFFFFFF
    return computed == stored
```

### Building a new `.chk` file from a payload

```python
import struct

def build_chk(payload_path, output_path):
    with open(payload_path, 'rb') as f:
        payload = f.read()

    # Pad payload to a multiple of 4 bytes
    if len(payload) % 4:
        payload += b'\x00' * (4 - len(payload) % 4)

    # 4092 bytes of zero padding between header and payload
    padding = b'\x00' * 4092

    # Body = padding + payload (everything after the 4-byte checksum)
    body = padding + payload

    # Sum all 32-bit LE words in the body
    s32 = 0
    for i in range(0, len(body), 4):
        s32 = (s32 + struct.unpack('<I', body[i:i+4])[0]) & 0xFFFFFFFF

    checksum = (s32 + 9) & 0xFFFFFFFF

    with open(output_path, 'wb') as f:
        f.write(struct.pack('<I', checksum))
        f.write(body)
```

### Wayback Machine CDX API pattern (for future firmware archaeology)

```bash
# Search all archived URLs on a domain matching a filename pattern
curl -sL "https://web.archive.org/cdx/search/cdx?\
url=DOMAIN/*PATTERN*&output=text&\
fl=original,timestamp,statuscode&\
filter=statuscode:200&collapse=urlkey"

# Download a raw binary without Wayback HTML injection (note the if_)
curl -sL -o output.bin \
  "https://web.archive.org/web/TIMESTAMPif_/ORIGINAL_URL"
```

## Prevention & Best Practices

### Preserve the reference artifacts

The Wayback Machine is not a permanent archive. The recovered `.chk` files are
stored in this repository at `dump/official-firmware/` with their original filenames.

**SHA-256 hashes should be recorded** for each file to verify integrity if
re-downloaded in the future.

### Formalize the checksum algorithm

The reverse-engineered checksum is currently informal knowledge. A `.chk` image
builder tool (`tools/make_chk.py`) should implement the algorithm and validate it
with a round-trip test against the reference image: extract the payload from the
reference `.chk`, feed it back through the builder, and confirm byte-for-byte
identity with the original file.

### Make firmware images reproducible

- Pin the cross-compilation toolchain (GCC version, OABI flags, linker script)
- Ensure no timestamps or host-dependent metadata enter the binary
- Add a `make verify-chk` target that re-checksums a built image

## Recommended Next Steps

1. **Build `tools/make_chk.py`** — Accept a raw firmware payload, produce a valid
   `.chk` file that `flashsw.sh` will accept. Validate against the reference image.
2. **Extend `tools/analyze_flash.py`** — Add `.chk` parsing and checksum validation
   so it can verify any `.chk` file.
3. **Add `make deploy-flash`** — Build a `.chk` from the open firmware, FTP it to
   the device, and trigger `flashsw.sh` for a full Strand-compatible flash install.
4. **Record SHA-256 hashes** of the reference firmware files.

## Test Cases

| ID | Test | Expected |
|----|------|----------|
| TC-01 | Round-trip: extract payload from reference `.chk`, rebuild, compare | Byte-for-byte identical |
| TC-02 | Checksum field vs. independent computation on reference file | Match |
| TC-03 | Flip one bit in payload, recompute checksum | Checksum changes |
| TC-04 | All-zeros payload | Checksum equals 9 |
| TC-05 | Payload not aligned to 4 bytes | Tool pads and handles correctly |
| TC-06 | Build twice from same input | Deterministic output |

## Cross-References

- **`docs/recovery.md`** — Option B ("Original Strand Firmware Update File") should
  be updated to reference this document and the recovered files.
- **`SAFETY.md`** — Lines 87-88 ("What we cannot recover: .chk format/checksum
  algorithm") should be marked resolved.
- **`PLAN.md`** — Open Question #4 ("Flash checksum format") should be marked
  resolved. Phase 6 deliverable #1 should drop the "if we reverse the checksum
  format" qualifier.
- **`dump/firmware/usr/bin/flashsw.sh`** — The on-device update script that
  consumes `.chk` files. Source of truth for the update flow.
- **`dump/official-firmware/`** — Location of the recovered reference firmware files.
