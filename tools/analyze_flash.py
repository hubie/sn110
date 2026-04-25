#!/usr/bin/env python3
"""
SN110 Flash Dump Analyzer

Scans a raw flash dump for minix filesystems, parses their structure,
and diagnoses corruption — particularly from config file overflow.

Usage:
    python3 tools/analyze_flash.py <flash_dump.bin>
    python3 tools/analyze_flash.py <flash_dump.bin> --extract-fs <hex_offset> <out.img>
    python3 tools/analyze_flash.py <flash_dump.bin> --extract-file <hex_offset> <path> <out>
    python3 tools/analyze_flash.py <flash_dump.bin> --map
"""

import sys
import struct
import os
from collections import defaultdict
from datetime import datetime

# ---------------------------------------------------------------------------
# Minix filesystem constants
# ---------------------------------------------------------------------------

BLOCK_SIZE = 1024

MINIX_MAGIC = {
    0x137F: ("minix v1, 14-char names", 14, 1),
    0x138F: ("minix v1, 30-char names", 30, 1),
    0x2468: ("minix v2, 14-char names", 14, 2),
    0x2478: ("minix v2, 30-char names", 30, 2),
}

# Inode mode bits
S_IFMT  = 0o170000
S_IFDIR = 0o040000
S_IFREG = 0o100000
S_IFLNK = 0o120000
S_IFCHR = 0o020000
S_IFBLK = 0o060000
S_IFIFO = 0o010000


def mode_string(mode):
    """Convert inode mode to ls-style permission string."""
    ft = mode & S_IFMT
    types = {
        S_IFDIR: 'd', S_IFREG: '-', S_IFLNK: 'l',
        S_IFCHR: 'c', S_IFBLK: 'b', S_IFIFO: 'p',
    }
    s = types.get(ft, '?')
    for shift in (6, 3, 0):
        bits = (mode >> shift) & 7
        s += 'r' if bits & 4 else '-'
        s += 'w' if bits & 2 else '-'
        s += 'x' if bits & 1 else '-'
    return s


def format_time(t):
    """Format a Unix timestamp."""
    if t == 0:
        return "(none)"
    try:
        return datetime.fromtimestamp(t).strftime('%Y-%m-%d %H:%M:%S')
    except (OSError, ValueError):
        return f"(invalid: {t})"


# ---------------------------------------------------------------------------
# Minix filesystem parser
# ---------------------------------------------------------------------------

class MinixFS:
    """Parse and analyze a minix filesystem from raw data."""

    def __init__(self, data, base_offset=0):
        self.data = data
        self.base = base_offset
        self._parse_superblock()

    def _parse_superblock(self):
        sb_off = self.base + BLOCK_SIZE  # superblock is at block 1
        sb = self.data[sb_off:sb_off + 20]
        if len(sb) < 20:
            raise ValueError("Data too short for superblock")

        fields = struct.unpack_from('<HHHHHHIHH', sb, 0)
        self.ninodes       = fields[0]
        self.nzones        = fields[1]
        self.imap_blocks   = fields[2]
        self.zmap_blocks   = fields[3]
        self.firstdatazone = fields[4]
        self.log_zone_size = fields[5]
        self.max_size      = fields[6]
        self.magic         = fields[7]
        self.state         = fields[8]

        info = MINIX_MAGIC.get(self.magic)
        if not info:
            raise ValueError(f"Bad magic: 0x{self.magic:04X}")

        self.fs_name, self.max_namelen, self.version = info
        self.zone_size = BLOCK_SIZE << self.log_zone_size

        if self.version == 1:
            self.inode_size    = 32
            self.zone_ptr_fmt  = '<H'
            self.zone_ptr_size = 2
        else:
            self.inode_size    = 64
            self.zone_ptr_fmt  = '<I'
            self.zone_ptr_size = 4

        self.dirent_size = 2 + self.max_namelen  # inode(u16) + name

        # Layout offsets (absolute within self.data)
        self.imap_offset   = self.base + 2 * BLOCK_SIZE
        self.zmap_offset   = self.imap_offset + self.imap_blocks * BLOCK_SIZE
        self.itable_offset = self.zmap_offset + self.zmap_blocks * BLOCK_SIZE
        itable_blocks = (self.ninodes * self.inode_size + BLOCK_SIZE - 1) // BLOCK_SIZE
        self.data_offset   = self.itable_offset + itable_blocks * BLOCK_SIZE

    # -- low-level access --------------------------------------------------

    def _read_zone(self, zone_num):
        if zone_num == 0:
            return b'\x00' * self.zone_size
        off = self.base + zone_num * self.zone_size
        end = off + self.zone_size
        if end > len(self.data):
            return self.data[off:end].ljust(self.zone_size, b'\xff')
        return self.data[off:end]

    def _zone_ptr(self, buf, index):
        return struct.unpack_from(self.zone_ptr_fmt, buf, index * self.zone_ptr_size)[0]

    def get_inode(self, inum):
        """Return inode dict for 1-based inode number, or None."""
        if inum < 1 or inum > self.ninodes:
            return None
        off = self.itable_offset + (inum - 1) * self.inode_size
        raw = self.data[off:off + self.inode_size]
        if len(raw) < self.inode_size:
            return None

        if self.version == 1:
            mode, uid, size, mtime = struct.unpack_from('<HHII', raw, 0)
            gid    = raw[12]
            nlinks = raw[13]
            zones  = list(struct.unpack_from('<9H', raw, 14))  # 7 direct + ind + dbl
        else:
            mode, nlinks, uid, gid, size = struct.unpack_from('<HHHHI', raw, 0)
            mtime = struct.unpack_from('<I', raw, 16)[0]
            zones  = list(struct.unpack_from('<10I', raw, 24))  # 7 direct + ind + dbl + tri

        return {
            'inum': inum, 'mode': mode, 'uid': uid, 'gid': gid,
            'size': size, 'mtime': mtime, 'nlinks': nlinks, 'zones': zones,
        }

    def get_file_data(self, inode):
        """Read all data blocks for an inode, return bytes truncated to size."""
        size = inode['size']
        zones = inode['zones']
        parts = []
        remaining = size
        ptrs_per_zone = self.zone_size // self.zone_ptr_size

        def _append_zone(z):
            nonlocal remaining
            if remaining <= 0:
                return False
            d = self._read_zone(z)
            take = min(len(d), remaining)
            parts.append(d[:take])
            remaining -= take
            return remaining > 0

        # Direct zones (0..6)
        for i in range(7):
            if i >= len(zones):
                break
            if not _append_zone(zones[i]):
                return b''.join(parts)

        # Single indirect (zones[7])
        if len(zones) > 7 and zones[7] != 0:
            ind = self._read_zone(zones[7])
            for i in range(ptrs_per_zone):
                if not _append_zone(self._zone_ptr(ind, i)):
                    return b''.join(parts)

        # Double indirect (zones[8])
        if len(zones) > 8 and zones[8] != 0:
            dbl = self._read_zone(zones[8])
            for i in range(ptrs_per_zone):
                p = self._zone_ptr(dbl, i)
                if p == 0:
                    for _ in range(ptrs_per_zone):
                        if not _append_zone(0):
                            return b''.join(parts)
                    continue
                ind = self._read_zone(p)
                for j in range(ptrs_per_zone):
                    if not _append_zone(self._zone_ptr(ind, j)):
                        return b''.join(parts)

        return b''.join(parts)

    def collect_zones(self, inode):
        """Return set of all non-zero zone numbers referenced by an inode."""
        zones = set()
        ptrs_per_zone = self.zone_size // self.zone_ptr_size

        for i in range(min(7, len(inode['zones']))):
            z = inode['zones'][i]
            if z:
                zones.add(z)

        if len(inode['zones']) > 7 and inode['zones'][7]:
            iz = inode['zones'][7]
            zones.add(iz)
            ind = self._read_zone(iz)
            for i in range(ptrs_per_zone):
                p = self._zone_ptr(ind, i)
                if p:
                    zones.add(p)

        if len(inode['zones']) > 8 and inode['zones'][8]:
            dz = inode['zones'][8]
            zones.add(dz)
            dbl = self._read_zone(dz)
            for i in range(ptrs_per_zone):
                p = self._zone_ptr(dbl, i)
                if not p:
                    continue
                zones.add(p)
                ind = self._read_zone(p)
                for j in range(ptrs_per_zone):
                    q = self._zone_ptr(ind, j)
                    if q:
                        zones.add(q)

        return zones

    # -- directory traversal -----------------------------------------------

    def list_dir(self, inode):
        """Return [(inum, name), ...] for a directory inode."""
        data = self.get_file_data(inode)
        entries = []
        for off in range(0, len(data), self.dirent_size):
            chunk = data[off:off + self.dirent_size]
            if len(chunk) < self.dirent_size:
                break
            inum = struct.unpack_from('<H', chunk, 0)[0]
            if inum == 0:
                continue
            name = chunk[2:].split(b'\x00', 1)[0].decode('ascii', errors='replace')
            entries.append((inum, name))
        return entries

    def walk(self, inum=1, path='/'):
        """Yield (path, inode_dict) for every reachable file."""
        inode = self.get_inode(inum)
        if inode is None:
            return
        yield (path, inode)
        if (inode['mode'] & S_IFMT) == S_IFDIR:
            try:
                for child_inum, name in self.list_dir(inode):
                    if name in ('.', '..'):
                        continue
                    child_path = path.rstrip('/') + '/' + name
                    yield from self.walk(child_inum, child_path)
            except Exception as e:
                yield (path + '/?ERROR', {
                    'inum': inum, 'mode': 0, 'uid': 0, 'gid': 0,
                    'size': 0, 'mtime': 0, 'nlinks': 0, 'zones': [],
                    'error': str(e),
                })

    # -- bitmap analysis ---------------------------------------------------

    def _parse_bitmap(self, offset, n_blocks, max_bits):
        """Return set of bit positions that are set in the bitmap."""
        allocated = set()
        for blk in range(n_blocks):
            base = offset + blk * BLOCK_SIZE
            block = self.data[base:base + BLOCK_SIZE]
            for byte_idx, byte_val in enumerate(block):
                if byte_val == 0:
                    continue
                for bit in range(8):
                    if byte_val & (1 << bit):
                        n = blk * BLOCK_SIZE * 8 + byte_idx * 8 + bit
                        if n < max_bits:
                            allocated.add(n)
        return allocated

    def inode_bitmap(self):
        return self._parse_bitmap(self.imap_offset, self.imap_blocks,
                                  self.ninodes + 1)

    def zone_bitmap(self):
        return self._parse_bitmap(self.zmap_offset, self.zmap_blocks,
                                  self.nzones)

    # -- full analysis -----------------------------------------------------

    def analyze(self):
        """Run full analysis. Returns number of issues found."""
        W = 70
        print(f"\n{'=' * W}")
        print(f"MINIX FILESYSTEM — flash offset 0x{self.base:06X}")
        print(f"{'=' * W}")

        # ---- superblock --------------------------------------------------
        print(f"\nSuperblock:")
        print(f"  Type:             {self.fs_name}")
        print(f"  Magic:            0x{self.magic:04X}")
        state_str = 'clean' if self.state == 0 else 'DIRTY / ERRORS'
        print(f"  State:            {self.state} ({state_str})")
        print(f"  Inodes:           {self.ninodes}")
        print(f"  Zones:            {self.nzones}")
        print(f"  Zone size:        {self.zone_size} bytes")
        print(f"  Imap blocks:      {self.imap_blocks}")
        print(f"  Zmap blocks:      {self.zmap_blocks}")
        print(f"  First data zone:  {self.firstdatazone}")
        print(f"  Max file size:    {self.max_size}")
        total_data = self.nzones - self.firstdatazone
        fs_bytes = self.nzones * self.zone_size
        print(f"  Data zones:       {total_data}")
        print(f"  Filesystem size:  {fs_bytes} bytes ({fs_bytes // 1024} KB)")

        # ---- sanity checks on superblock ---------------------------------
        issues = []
        if self.state != 0:
            issues.append("Filesystem state is DIRTY (not cleanly unmounted)")

        expected_first = (2 + self.imap_blocks + self.zmap_blocks +
                          (self.ninodes * self.inode_size + BLOCK_SIZE - 1) // BLOCK_SIZE)
        if self.firstdatazone != expected_first:
            issues.append(
                f"firstdatazone ({self.firstdatazone}) != expected ({expected_first}) "
                f"— superblock may be damaged")

        # ---- bitmaps -----------------------------------------------------
        print(f"\nBitmaps:")
        imap = self.inode_bitmap()
        imap.discard(0)  # bit 0 is reserved
        zmap = self.zone_bitmap()
        zmap_data = {z for z in zmap if z >= self.firstdatazone}

        used_inodes = len(imap)
        free_inodes = self.ninodes - used_inodes
        used_zones  = len(zmap_data)
        free_zones  = total_data - used_zones
        free_bytes  = free_zones * self.zone_size

        print(f"  Inodes:  {used_inodes} used / {self.ninodes} total ({free_inodes} free)")
        print(f"  Zones:   {used_zones} used / {total_data} total ({free_zones} free)")
        print(f"  Free:    {free_bytes} bytes ({free_bytes // 1024} KB)")

        if free_zones == 0:
            print(f"  >>> FILESYSTEM IS COMPLETELY FULL <<<")
            issues.append("Filesystem is completely full (0 free zones)")
        elif free_zones <= 3:
            print(f"  >>> FILESYSTEM IS NEARLY FULL <<<")
            issues.append(f"Filesystem nearly full (only {free_zones} free zones = {free_bytes} bytes)")

        # ---- file listing ------------------------------------------------
        print(f"\nFiles:")
        all_files = []
        zone_owners = defaultdict(list)
        walk_errors = []

        try:
            for path, inode in self.walk():
                if 'error' in inode:
                    walk_errors.append(f"{path}: {inode['error']}")
                    continue
                all_files.append((path, inode))
                ms = mode_string(inode['mode'])
                print(f"  {ms} {inode['nlinks']:2d} {inode['size']:8d}  "
                      f"ino={inode['inum']:<3d}  {path}")
                try:
                    for z in self.collect_zones(inode):
                        zone_owners[z].append((path, inode['inum']))
                except Exception as e:
                    walk_errors.append(f"zones for {path}: {e}")
        except Exception as e:
            walk_errors.append(f"walk: {e}")
            issues.append(f"Filesystem walk failed: {e}")

        if walk_errors:
            print(f"\n  Errors during walk:")
            for err in walk_errors:
                print(f"    {err}")
            issues.append(f"{len(walk_errors)} error(s) during filesystem walk")

        # ---- integrity checks --------------------------------------------
        print(f"\nIntegrity:")
        all_used = set()
        for z_set in zone_owners:
            all_used.add(z_set)

        # Zone conflicts — same zone claimed by multiple files
        conflicts = {z: owners for z, owners in zone_owners.items()
                     if len(owners) > 1}
        if conflicts:
            print(f"  ZONE CONFLICTS ({len(conflicts)}):")
            for z in sorted(conflicts):
                owners = ', '.join(f"{p} (ino {i})" for p, i in conflicts[z])
                print(f"    zone {z}: {owners}")
            issues.append(f"{len(conflicts)} zone(s) shared by multiple files")
        else:
            print(f"  Zone conflicts:     none")

        # Zones used by files but not marked in bitmap
        file_zones = set(zone_owners.keys())
        file_data_zones = {z for z in file_zones if z >= self.firstdatazone}
        unalloc_used = file_data_zones - zmap
        if unalloc_used:
            print(f"  UNALLOCATED IN USE ({len(unalloc_used)}):")
            for z in sorted(unalloc_used)[:20]:
                owners = ', '.join(p for p, _ in zone_owners[z])
                print(f"    zone {z}: used by {owners}")
            issues.append(f"{len(unalloc_used)} zone(s) used by files but not in bitmap")
        else:
            print(f"  Unallocated usage:  none")

        # Zones in bitmap but not referenced (leaked blocks)
        bitmap_orphans = zmap_data - file_data_zones
        if bitmap_orphans:
            n = len(bitmap_orphans)
            leaked = n * self.zone_size
            print(f"  LEAKED ZONES:       {n} ({leaked} bytes not referenced by any file)")
            if n <= 15:
                print(f"    Zones: {sorted(bitmap_orphans)}")
            issues.append(f"{n} leaked zone(s) ({leaked} bytes) in bitmap but unreferenced")
        else:
            print(f"  Leaked zones:       none")

        # Out-of-range zone pointers
        out_of_range = {z for z in file_zones if z >= self.nzones}
        if out_of_range:
            print(f"  OUT-OF-RANGE ({len(out_of_range)}):")
            for z in sorted(out_of_range)[:10]:
                owners = ', '.join(p for p, _ in zone_owners[z])
                print(f"    zone {z} (max valid: {self.nzones - 1}): {owners}")
            issues.append(f"{len(out_of_range)} zone pointer(s) outside filesystem")
        else:
            print(f"  Out-of-range:       none")

        # ---- config file -------------------------------------------------
        print(f"\n{'─' * W}")
        print(f"Config file: /etc/220node.cfg")
        print(f"{'─' * W}")
        cfg = None
        for path, inode in all_files:
            if path == '/etc/220node.cfg':
                cfg = (path, inode)
                break

        if cfg is None:
            print(f"  NOT FOUND")
            issues.append("/etc/220node.cfg is missing")
        else:
            _, inode = cfg
            print(f"  Inode:   {inode['inum']}")
            print(f"  Size:    {inode['size']} bytes")
            print(f"  Modified:{format_time(inode['mtime'])}")
            nz = [z for z in inode['zones'] if z != 0]
            print(f"  Zones:   {nz}")
            zones_needed = (inode['size'] + self.zone_size - 1) // self.zone_size if inode['size'] > 0 else 0
            print(f"  Blocks:  {zones_needed} needed for {inode['size']} bytes")

            try:
                content = self.get_file_data(inode)

                # Corruption indicators
                nulls = content.count(b'\x00')
                non_ascii = sum(1 for b in content if b > 127)
                if nulls:
                    print(f"  WARNING: {nulls} null byte(s) in content")
                    issues.append(f"220node.cfg contains {nulls} null bytes")
                if non_ascii:
                    print(f"  WARNING: {non_ascii} non-ASCII byte(s)")
                    issues.append(f"220node.cfg contains {non_ascii} non-ASCII bytes")

                text = content.decode('ascii', errors='replace')
                lines = [l for l in text.split('\n') if l.strip()]
                print(f"  Lines:   {len(lines)}")

                if inode['size'] == 0:
                    print(f"\n  >>> CONFIG FILE IS EMPTY (truncated by failed write?) <<<")
                    issues.append("220node.cfg is 0 bytes (truncated)")
                elif not text.strip():
                    print(f"\n  >>> CONFIG FILE IS BLANK <<<")
                    issues.append("220node.cfg exists but is blank")
                else:
                    print(f"\n  Contents:")
                    for line in text.split('\n'):
                        print(f"    {line}")
            except Exception as e:
                print(f"  ERROR reading content: {e}")
                issues.append(f"Cannot read 220node.cfg: {e}")

        # ---- original Strand config for comparison -----------------------
        strand_cfg = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                                  'dump', 'firmware', 'etc', '220node.cfg')
        # Also try relative to cwd
        for candidate in [strand_cfg, 'dump/firmware/etc/220node.cfg']:
            if os.path.exists(candidate):
                orig_size = os.path.getsize(candidate)
                print(f"\n  Original config:  {candidate} ({orig_size} bytes)")
                if cfg and cfg[1]['size'] > 0:
                    diff = cfg[1]['size'] - orig_size
                    if diff > 0:
                        print(f"  Size growth:      +{diff} bytes")
                    elif diff < 0:
                        print(f"  Size shrink:      {diff} bytes")
                    else:
                        print(f"  Size:             unchanged")
                break

        # ---- summary -----------------------------------------------------
        print(f"\n{'=' * W}")
        print(f"SUMMARY")
        print(f"{'=' * W}")

        if not issues:
            print(f"\n  No issues found — filesystem appears healthy.")
            print(f"  If the device is bricked, the cause may be outside the")
            print(f"  filesystem (corrupted kernel, boot script, etc.)")
        else:
            print(f"\n  {len(issues)} issue(s) found:\n")
            for i, issue in enumerate(issues, 1):
                print(f"    {i}. {issue}")

            # Diagnosis
            full = free_zones == 0 or free_zones <= 3
            cfg_empty = cfg and cfg[1]['size'] == 0
            cfg_missing = cfg is None
            has_conflicts = bool(conflicts)

            print(f"\n  Probable cause:")
            if full and (cfg_empty or cfg_missing):
                print(f"    Config save ran out of space. fopen(\"w\") truncated the")
                print(f"    original file, then the write failed partway through,")
                print(f"    leaving a {'missing' if cfg_missing else 'truncated'} config.")
                if has_conflicts:
                    print(f"    Zone conflicts suggest metadata corruption from the")
                    print(f"    failed allocation — this would prevent mounting.")
            elif full:
                print(f"    Filesystem is full. Config may have been written")
                print(f"    successfully but subsequent operations could have")
                print(f"    corrupted metadata.")
            elif has_conflicts or unalloc_used:
                print(f"    Filesystem metadata is inconsistent — zone bitmap")
                print(f"    does not match actual file usage. Likely caused by")
                print(f"    an interrupted write on a full or near-full filesystem.")
            else:
                print(f"    Unable to determine automatically. The issues above")
                print(f"    may provide clues.")

            print(f"\n  Recovery options:")
            print(f"    1. Extract filesystem, repair with fsck.minix, write back")
            print(f"    2. Extract filesystem, mount in Docker, replace config, write back")
            if cfg and cfg[1]['size'] > 0 and not has_conflicts:
                print(f"    3. Config looks intact — problem may be elsewhere")

        return len(issues)


# ---------------------------------------------------------------------------
# Flash-level scanning
# ---------------------------------------------------------------------------

def scan_for_minix(data):
    """Scan raw data for minix superblock magic at block boundaries."""
    found = []
    for offset in range(0, len(data) - 1040, BLOCK_SIZE):
        magic_off = offset + BLOCK_SIZE + 16  # block 1, offset 16
        if magic_off + 2 > len(data):
            break
        magic = struct.unpack_from('<H', data, magic_off)[0]
        if magic not in MINIX_MAGIC:
            continue

        # Sanity-check superblock fields to filter false positives
        sb_off = offset + BLOCK_SIZE
        fields = struct.unpack_from('<HHHHHH', data, sb_off)
        ninodes, nzones, imap, zmap, firstdata, logzs = fields
        if (ninodes == 0 or nzones == 0 or imap == 0 or zmap == 0
                or firstdata < 3 or logzs > 4):
            continue
        found.append((offset, magic))

    return found


def flash_map(data):
    """Print a visual map of the flash contents."""
    size = len(data)
    CHUNK = 0x10000  # 64KB — typical flash sector size

    print(f"\nFlash map ({size} bytes = {size // 1024} KB):")
    print(f"{'Offset':>10s}  {'Content':20s}  {'Fill':>5s}  Detail")
    print(f"{'─' * 60}")

    for off in range(0, size, CHUNK):
        chunk = data[off:off + CHUNK]
        n_ff = chunk.count(b'\xff')
        n_00 = chunk.count(b'\x00')
        pct_ff = 100 * n_ff / len(chunk)
        pct_00 = 100 * n_00 / len(chunk)

        if n_ff == len(chunk):
            kind = "erased (0xFF)"
            fill = "100%"
        elif pct_ff > 95:
            kind = "mostly erased"
            fill = f"{pct_ff:.0f}%"
        elif n_00 == len(chunk):
            kind = "all zeros"
            fill = "0%"
        else:
            # Try to identify content
            fill = f"{100 - pct_ff:.0f}%"
            # Check for text content
            printable = sum(1 for b in chunk if 32 <= b < 127 or b in (10, 13, 9))
            if printable > len(chunk) * 0.7:
                # Mostly text — look for clues
                snippet = chunk[:80].decode('ascii', errors='replace').strip()
                kind = f"text"
            else:
                kind = "data"
            # Check for compressed data (gzip)
            if chunk[:2] == b'\x1f\x8b':
                kind = "gzip compressed"
            # Check for ELF
            elif chunk[:4] == b'\x7fELF':
                kind = "ELF binary"

        detail = ""
        # Check if a minix superblock lives here
        sb_magic_off = off + BLOCK_SIZE + 16
        if sb_magic_off + 2 <= size:
            m = struct.unpack_from('<H', data, sb_magic_off)[0]
            if m in MINIX_MAGIC:
                detail = f"  << MINIX superblock ({MINIX_MAGIC[m][0]})"

        print(f"0x{off:08X}  {kind:20s}  {fill:>5s}  {detail}")

    # Find interesting byte sequences
    print(f"\nNotable signatures:")
    for label, pattern in [
        ("gzip header", b'\x1f\x8b'),
        ("minix v1 magic", struct.pack('<H', 0x138F)),
        ("minix v1 magic (14)", struct.pack('<H', 0x137F)),
        ("minix v2 magic", struct.pack('<H', 0x2468)),
    ]:
        pos = 0
        locs = []
        while True:
            pos = data.find(pattern, pos)
            if pos == -1:
                break
            locs.append(pos)
            pos += 1
            if len(locs) >= 10:
                break
        if locs:
            addrs = ', '.join(f'0x{p:06X}' for p in locs)
            print(f"  {label}: {addrs}")

    print(f"\nString search:")
    for pattern in [b'lxnetdmx', b'220node.cfg', b'#!/bin/sh',
                    b'/etc/rc', b'NET+ARM', b'NETsilicon']:
        pos = data.find(pattern)
        if pos >= 0:
            print(f"  '{pattern.decode()}' at 0x{pos:06X}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 2:
        print("SN110 Flash Dump Analyzer")
        print()
        print("Usage:")
        print(f"  {sys.argv[0]} <dump.bin>                                    Scan and analyze")
        print(f"  {sys.argv[0]} <dump.bin> --map                              Flash content map")
        print(f"  {sys.argv[0]} <dump.bin> --extract-fs <offset> <out.img>    Extract filesystem")
        print(f"  {sys.argv[0]} <dump.bin> --extract-file <offset> <path> <out>  Extract a file")
        print()
        print("Offsets can be decimal or hex (0x prefix).")
        sys.exit(1)

    dump_path = sys.argv[1]
    if not os.path.exists(dump_path):
        print(f"Error: {dump_path} not found")
        sys.exit(1)

    with open(dump_path, 'rb') as f:
        data = f.read()

    size = len(data)
    print(f"Flash dump: {dump_path}")
    print(f"Size:       {size} bytes ({size // 1024} KB / {size / 1048576:.1f} MB)")

    # --map mode
    if len(sys.argv) >= 3 and sys.argv[2] == '--map':
        flash_map(data)
        sys.exit(0)

    # --extract-fs mode
    if len(sys.argv) >= 5 and sys.argv[2] == '--extract-fs':
        offset = int(sys.argv[3], 0)
        output = sys.argv[4]
        fs = MinixFS(data, offset)
        fs_size = fs.nzones * fs.zone_size
        with open(output, 'wb') as f:
            f.write(data[offset:offset + fs_size])
        print(f"Extracted {fs_size} bytes ({fs_size // 1024} KB) "
              f"from offset 0x{offset:06X} → {output}")
        print(f"\nRepair with:")
        print(f"  docker run --rm -v $(pwd):/work debian:bookworm bash -c \\")
        print(f"    'apt-get update -qq && apt-get install -yqq util-linux && "
              f"fsck.minix -fvl /work/{output}'")
        sys.exit(0)

    # --extract-file mode
    if len(sys.argv) >= 6 and sys.argv[2] == '--extract-file':
        offset = int(sys.argv[3], 0)
        filepath = sys.argv[4]
        output = sys.argv[5]
        fs = MinixFS(data, offset)
        for path, inode in fs.walk():
            if path == filepath:
                content = fs.get_file_data(inode)
                with open(output, 'wb') as f:
                    f.write(content)
                print(f"Extracted {path} ({inode['size']} bytes) → {output}")
                sys.exit(0)
        print(f"Error: {filepath} not found in filesystem at 0x{offset:06X}")
        sys.exit(1)

    # Default: scan and analyze
    print(f"\nScanning for minix filesystems...")
    found = scan_for_minix(data)

    if not found:
        print("No minix filesystems found.")
        print("\nRunning flash map to help locate content...")
        flash_map(data)
        sys.exit(1)

    print(f"Found {len(found)} filesystem(s):")
    for offset, magic in found:
        info = MINIX_MAGIC[magic]
        print(f"  0x{offset:06X}: {info[0]} (magic 0x{magic:04X})")

    total_issues = 0
    for offset, magic in found:
        try:
            fs = MinixFS(data, offset)
            n = fs.analyze()
            total_issues += n
        except Exception as e:
            print(f"\nError analyzing filesystem at 0x{offset:06X}: {e}")
            import traceback
            traceback.print_exc()
            total_issues += 1

    # Hints
    if found:
        print(f"\n{'─' * 70}")
        print(f"Next steps:")
        print(f"{'─' * 70}")
        print(f"\nExtract filesystem for repair:")
        for offset, _ in found:
            print(f"  python3 {sys.argv[0]} {dump_path} --extract-fs 0x{offset:06X} rootfs.img")
        print(f"\nExtract a specific file:")
        print(f"  python3 {sys.argv[0]} {dump_path} --extract-file "
              f"0x{found[0][0]:06X} /etc/220node.cfg config_backup.txt")
        print(f"\nView flash layout:")
        print(f"  python3 {sys.argv[0]} {dump_path} --map")

    sys.exit(1 if total_issues > 0 else 0)


if __name__ == '__main__':
    main()
