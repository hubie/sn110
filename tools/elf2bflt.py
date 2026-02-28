#!/usr/bin/env python3
"""
elf2bflt.py — Convert ARM ELF to bFLT v2 for uClinux

Converts a statically-linked ARM ELF binary (compiled with --emit-relocs
and -fpic) to the bFLT v2 flat binary format used by uClinux on the SN110.

bFLT v2 format (old relocation format):
  [64-byte header]
  [text + rodata]
  [data (including GOT)]
  [relocation table: array of big-endian 32-bit v2 entries]

Each v2 relocation entry encodes:
  bits[26:31] = type: 0=TEXT, 1=DATA, 2=BSS (target section)
  bits[0:25]  = 26-bit offset from data section start (site location)

The kernel's old_reloc() does:
  ptr = datapos + (entry & 0x03FFFFFF);
  *ptr += start_code | start_data | end_data  (based on type)

All relocation SITES must be in the data section (GOT or initialized data).
Text-section R_ARM_ABS32 relocations are rejected — compile with
-fpic -msingle-pic-base to move all address references to GOT.

Usage:
  python3 tools/elf2bflt.py build/sn110dmx_reloc.elf build/sn110dmx.bflt

Copyright (c) 2026 SN110 Open Firmware Contributors
SPDX-License-Identifier: MIT
"""

import struct
import sys

# ELF constants
ELF_MAGIC = b'\x7fELF'
ET_EXEC = 2
EM_ARM = 40
SHT_REL = 9
SHT_PROGBITS = 1
SHT_NOBITS = 8

# ARM relocation types
R_ARM_ABS32 = 2
R_ARM_GOT_BREL = 26

# bFLT constants
BFLT_MAGIC = b'bFLT'
BFLT_VERSION = 2
BFLT_HEADER_SIZE = 64
FLAT_FLAG_RAM = 1
FLAT_FLAG_GOTPIC = 2
BFLT_DEFAULT_STACK = 65536

# v2 relocation types
FLAT_RELOC_TYPE_TEXT = 0
FLAT_RELOC_TYPE_DATA = 1
FLAT_RELOC_TYPE_BSS = 2


def read_elf(data):
    """Parse ELF headers and return sections."""
    if data[:4] != ELF_MAGIC:
        raise ValueError("Not an ELF file")

    ei_class = data[4]  # 1 = 32-bit
    ei_data = data[5]   # 1 = little-endian
    if ei_class != 1 or ei_data != 1:
        raise ValueError("Expected 32-bit little-endian ELF")

    e_type, e_machine = struct.unpack_from('<HH', data, 16)
    if e_machine != EM_ARM:
        raise ValueError(f"Expected ARM ELF, got machine={e_machine}")

    e_entry = struct.unpack_from('<I', data, 24)[0]
    e_shoff = struct.unpack_from('<I', data, 32)[0]
    e_shentsize = struct.unpack_from('<H', data, 46)[0]
    e_shnum = struct.unpack_from('<H', data, 48)[0]
    e_shstrndx = struct.unpack_from('<H', data, 50)[0]

    # Read section headers
    sections = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh = struct.unpack_from('<IIIIIIIIII', data, off)
        sections.append({
            'sh_name': sh[0],
            'sh_type': sh[1],
            'sh_flags': sh[2],
            'sh_addr': sh[3],
            'sh_offset': sh[4],
            'sh_size': sh[5],
            'sh_link': sh[6],
            'sh_info': sh[7],
            'sh_addralign': sh[8],
            'sh_entsize': sh[9],
        })

    # Read section name string table
    shstrtab = sections[e_shstrndx]
    strtab_data = data[shstrtab['sh_offset']:
                       shstrtab['sh_offset'] + shstrtab['sh_size']]

    def get_name(idx):
        end = strtab_data.index(b'\0', idx)
        return strtab_data[idx:end].decode('ascii')

    for s in sections:
        s['name'] = get_name(s['sh_name'])

    return e_entry, sections, data


def find_section(sections, name):
    for s in sections:
        if s['name'] == name:
            return s
    return None


def find_sections_by_prefix(sections, prefix):
    """Find all sections whose name starts with prefix."""
    return [s for s in sections if s['name'].startswith(prefix)]


def extract_relocs_v2(elf_data, sections, text_end_va, data_start_va,
                      bss_start_va, bss_end_va, data_bytes):
    """Extract R_ARM_ABS32 relocations and convert to bFLT v2 format.

    Returns list of (v2_entry, data_offset) tuples.
    Also patches data_bytes in-place with section-relative pre-reloc values.
    """
    relocs = []
    text_reloc_count = 0

    # SHF_ALLOC flag — only process relocations for loaded sections
    SHF_ALLOC = 0x2

    for sec in sections:
        if sec['sh_type'] != SHT_REL:
            continue

        # Only process relocations for sections that are loaded at runtime
        target_sec_idx = sec['sh_info']
        if target_sec_idx < len(sections):
            target_sec = sections[target_sec_idx]
            if not (target_sec['sh_flags'] & SHF_ALLOC):
                continue  # Skip debug/non-loaded sections

        off = sec['sh_offset']
        count = sec['sh_size'] // sec['sh_entsize']

        for i in range(count):
            r_offset, r_info = struct.unpack_from('<II', elf_data,
                                                  off + i * 8)
            r_type = r_info & 0xFF
            if r_type != R_ARM_ABS32:
                continue

            # Determine if site is in text or data segment
            if r_offset < text_end_va:
                text_reloc_count += 1
                print(f"  WARNING: text-section R_ARM_ABS32 at VA 0x{r_offset:x} "
                      f"(from {sec['name']}) — cannot relocate with v2 format!",
                      file=sys.stderr)
                continue

            if r_offset < data_start_va:
                # In gap between text and data (alignment padding)
                print(f"  WARNING: R_ARM_ABS32 at VA 0x{r_offset:x} in "
                      f"text-data gap — skipping", file=sys.stderr)
                continue

            if r_offset >= bss_start_va:
                # BSS shouldn't have initialized relocations
                print(f"  WARNING: R_ARM_ABS32 at VA 0x{r_offset:x} in BSS "
                      f"— skipping", file=sys.stderr)
                continue

            # Site is in data segment
            data_offset = r_offset - data_start_va

            # Read the 32-bit LE value at the relocation site
            target_va = struct.unpack_from('<I', data_bytes, data_offset)[0]

            # Use TEXT type for ALL relocations, matching lxnetdmx.
            # This works because text+data+bss are loaded contiguously
            # (no MAX_SHARED_LIBS gap in Linux 2.0.38), so start_code +
            # link-time VA gives the correct runtime address for any symbol.
            reloc_type = FLAT_RELOC_TYPE_TEXT
            pre_reloc = target_va  # absolute link-time VA = offset from 0

            # Patch the data with the text-relative pre-reloc value
            struct.pack_into('<I', data_bytes, data_offset, pre_reloc)

            # Encode v2 relocation entry
            if data_offset > 0x03FFFFFF:
                raise ValueError(f"Data offset 0x{data_offset:x} exceeds "
                                 f"26-bit limit for v2 relocation")

            v2_entry = (reloc_type << 26) | (data_offset & 0x03FFFFFF)
            relocs.append((v2_entry, data_offset, reloc_type, target_va,
                           pre_reloc))

    if text_reloc_count > 0:
        print(f"\n  ERROR: {text_reloc_count} text-section relocation(s) found!",
              file=sys.stderr)
        print(f"  These cannot be handled by bFLT v2 format.",
              file=sys.stderr)
        print(f"  Compile C code with: -fpic -msingle-pic-base -mpic-register=sl",
              file=sys.stderr)
        print(f"  Remove literal pool symbol references from assembly.",
              file=sys.stderr)
        sys.exit(1)

    # Sort by data offset
    relocs.sort(key=lambda r: r[1])
    return relocs


def extract_got_relocs(elf_data, sections, text_end_va, data_start_va,
                       bss_start_va, data_bytes, existing_offsets):
    """Find GOT entries needing relocation from R_ARM_GOT_BREL references.

    With PIC (-fpic -msingle-pic-base), globals are accessed through GOT
    entries in the data section. The linker fills these entries with absolute
    addresses at link time but --emit-relocs doesn't emit .rel.got entries.
    We find them via R_ARM_GOT_BREL relocations in .rel.text — the resolved
    literal pool value at each site is the data-section-relative offset of
    the GOT entry.
    """
    SHF_ALLOC = 0x2
    relocs = []
    got_entry_offsets = set()

    text_sec = find_section(sections, '.text')
    if not text_sec:
        return relocs

    for sec in sections:
        if sec['sh_type'] != SHT_REL:
            continue
        target_sec_idx = sec['sh_info']
        if target_sec_idx >= len(sections):
            continue
        target_sec = sections[target_sec_idx]
        if not (target_sec['sh_flags'] & SHF_ALLOC):
            continue

        off = sec['sh_offset']
        count = sec['sh_size'] // sec['sh_entsize']

        for i in range(count):
            r_offset, r_info = struct.unpack_from('<II', elf_data,
                                                  off + i * 8)
            r_type = r_info & 0xFF
            if r_type != R_ARM_GOT_BREL:
                continue

            # Read resolved literal pool value = data-section offset of GOT entry
            file_pos = text_sec['sh_offset'] + (r_offset - text_sec['sh_addr'])
            got_data_offset = struct.unpack_from('<I', elf_data, file_pos)[0]

            if got_data_offset not in existing_offsets:
                got_entry_offsets.add(got_data_offset)

    for data_offset in sorted(got_entry_offsets):
        if data_offset + 4 > len(data_bytes):
            print(f"  WARNING: GOT entry at data+0x{data_offset:04x} out of "
                  f"bounds (data_size=0x{len(data_bytes):x})", file=sys.stderr)
            continue

        target_va = struct.unpack_from('<I', data_bytes, data_offset)[0]
        if target_va == 0:
            continue

        # Use TEXT type for all relocations (see extract_relocs_v2 comment)
        reloc_type = FLAT_RELOC_TYPE_TEXT
        pre_reloc = target_va

        struct.pack_into('<I', data_bytes, data_offset, pre_reloc)

        if data_offset > 0x03FFFFFF:
            raise ValueError(f"GOT data offset 0x{data_offset:x} exceeds "
                             f"26-bit limit")

        v2_entry = (reloc_type << 26) | (data_offset & 0x03FFFFFF)
        relocs.append((v2_entry, data_offset, reloc_type, target_va,
                       pre_reloc))

    relocs.sort(key=lambda r: r[1])
    return relocs


def build_bflt(text_data, data_data, bss_size, relocs, stack_size):
    """Build a bFLT v2 binary with old-format relocations."""
    entry = BFLT_HEADER_SIZE  # entry point = start of text
    data_start = BFLT_HEADER_SIZE + len(text_data)
    data_end = data_start + len(data_data)
    bss_end = data_end + bss_size

    # Relocation table follows data
    reloc_start = data_end
    reloc_count = len(relocs)

    # Build header (big-endian, matching device binaries)
    header = struct.pack('>4sIIIIIIII',
        BFLT_MAGIC,
        BFLT_VERSION,
        entry,
        data_start,
        data_end,
        bss_end,
        stack_size,
        reloc_start,
        reloc_count,
    )
    # Flags: FLAT_FLAG_RAM only — matches device binaries (dmxtst, lxnetdmx).
    # FLAT_FLAG_GOTPIC is a newer feature not present in Linux 2.0.38.
    flags = FLAT_FLAG_RAM
    header += struct.pack('<I', flags)
    # Pad header to 64 bytes
    header += b'\x00' * (BFLT_HEADER_SIZE - len(header))

    # Build relocation table (big-endian v2 entries)
    reloc_data = b''
    for v2_entry, _, _, _, _ in relocs:
        reloc_data += struct.pack('>I', v2_entry)

    return header + text_data + bytes(data_data) + reloc_data


def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.elf> <output.bflt>")
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2]

    with open(input_path, 'rb') as f:
        elf_data = f.read()

    e_entry, sections, _ = read_elf(elf_data)

    # Find key sections
    text_sec = find_section(sections, '.text')
    rodata_sec = find_section(sections, '.rodata')
    data_sec = find_section(sections, '.data')
    got_sec = find_section(sections, '.got')
    bss_sec = find_section(sections, '.bss')

    if not text_sec:
        raise ValueError("No .text section found")

    # Build text segment: .text + .rodata (contiguous, not relocated)
    text_data = elf_data[text_sec['sh_offset']:
                         text_sec['sh_offset'] + text_sec['sh_size']]

    if rodata_sec:
        gap = rodata_sec['sh_addr'] - (text_sec['sh_addr'] + text_sec['sh_size'])
        if gap > 0:
            text_data += b'\x00' * gap
        rodata = elf_data[rodata_sec['sh_offset']:
                          rodata_sec['sh_offset'] + rodata_sec['sh_size']]
        text_data += rodata

    # Determine text end VA (where data begins)
    # Account for alignment padding between text and data
    text_end_va = text_sec['sh_addr'] + len(text_data)

    # Find data segment start VA
    # Data = .data (which may include .got via linker script) + separate .got
    data_start_va = None
    if data_sec:
        data_start_va = data_sec['sh_addr']
    if got_sec and (data_start_va is None or got_sec['sh_addr'] < data_start_va):
        data_start_va = got_sec['sh_addr']

    if data_start_va is None:
        data_start_va = text_end_va

    # Pad text to maintain alignment with data
    if data_start_va > text_end_va:
        text_data += b'\x00' * (data_start_va - text_end_va)
        text_end_va = data_start_va

    # Build data segment (mutable bytearray for relocation patching)
    # Include .data and any separate .got section
    data_data = bytearray()
    data_sections = []
    if data_sec and data_sec['sh_size'] > 0:
        data_sections.append(data_sec)
    if got_sec and got_sec['sh_size'] > 0 and got_sec != data_sec:
        data_sections.append(got_sec)

    # Sort data sections by VA
    data_sections.sort(key=lambda s: s['sh_addr'])

    for sec in data_sections:
        # Pad gap between previous data and this section
        expected_offset = sec['sh_addr'] - data_start_va
        if len(data_data) < expected_offset:
            data_data += b'\x00' * (expected_offset - len(data_data))
        sec_data = elf_data[sec['sh_offset']:
                            sec['sh_offset'] + sec['sh_size']]
        data_data += bytearray(sec_data)

    # BSS
    bss_start_va = bss_sec['sh_addr'] if bss_sec else data_start_va + len(data_data)
    bss_size = bss_sec['sh_size'] if bss_sec else 0
    bss_end_va = bss_start_va + bss_size

    # Pad data to reach BSS start
    expected_data_len = bss_start_va - data_start_va
    if len(data_data) < expected_data_len:
        data_data += b'\x00' * (expected_data_len - len(data_data))

    # Extract and convert relocations to v2 format
    print(f"Section layout:")
    print(f"  text:  0x{text_sec['sh_addr']:06x} - 0x{text_end_va:06x} "
          f"({len(text_data)} bytes)")
    print(f"  data:  0x{data_start_va:06x} - 0x{bss_start_va:06x} "
          f"({len(data_data)} bytes)")
    print(f"  bss:   0x{bss_start_va:06x} - 0x{bss_end_va:06x} "
          f"({bss_size} bytes)")
    print()

    relocs = extract_relocs_v2(elf_data, sections, text_end_va,
                               data_start_va, bss_start_va, bss_end_va,
                               data_data)

    # Also find and relocate GOT entries (not covered by .rel.data)
    existing_offsets = {r[1] for r in relocs}
    got_relocs = extract_got_relocs(elf_data, sections, text_end_va,
                                    data_start_va, bss_start_va,
                                    data_data, existing_offsets)
    relocs.extend(got_relocs)
    relocs.sort(key=lambda r: r[1])

    # Build bFLT
    bflt = build_bflt(text_data, data_data, bss_size, relocs,
                      BFLT_DEFAULT_STACK)

    with open(output_path, 'wb') as f:
        f.write(bflt)

    # Print summary
    print(f"ELF → bFLT v2 conversion complete:")
    print(f"  Text:    {len(text_data):6d} bytes (.text + .rodata)")
    print(f"  Data:    {len(data_data):6d} bytes (.data + .got)")
    print(f"  BSS:     {bss_size:6d} bytes")
    print(f"  Relocs:  {len(relocs):6d} entries (v2 old format)")
    print(f"  Stack:   {BFLT_DEFAULT_STACK:6d} bytes")
    print(f"  Total:   {len(bflt):6d} bytes "
          f"({len(bflt) * 100 / 449632:.1f}% of budget)")
    print(f"  Output:  {output_path}")

    # Print relocation details
    type_names = {0: 'TEXT', 1: 'DATA', 2: 'BSS'}
    got_offsets = {r[1] for r in got_relocs}
    print(f"\nRelocation details (v2 old format):")
    for v2_entry, data_off, rtype, target_va, pre_reloc in relocs:
        src = "GOT" if data_off in got_offsets else "rel"
        print(f"  data+0x{data_off:04x}: target=0x{target_va:06x} → "
              f"type={type_names[rtype]} pre_reloc=0x{pre_reloc:06x} "
              f"[entry=0x{v2_entry:08x}] ({src})")

    # Verify header
    print(f"\nbFLT header verification:")
    magic, rev, entry, ds, de, be = struct.unpack('>4sIIIII', bflt[:24])
    ss, rs, rc = struct.unpack('>III', bflt[24:36])
    flags_le = struct.unpack('<I', bflt[36:40])[0]
    print(f"  magic={magic} rev={rev} entry=0x{entry:x} flags=0x{flags_le:x}")
    print(f"  data_start=0x{ds:x} data_end=0x{de:x} bss_end=0x{be:x}")
    print(f"  stack={ss} reloc_start=0x{rs:x} reloc_count={rc}")


if __name__ == '__main__':
    main()
