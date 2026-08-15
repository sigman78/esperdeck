#!/usr/bin/env python3
"""fontbin2c.py -- expand a compressed glyph blob into its C translation unit.

Build-time half of the font data pipeline: the repo commits compact binary
blobs (components/font/data/terminusWxH[b].bin, container "CDF1" below);
this script regenerates the terminusWxH[b].c array file into the build tree,
matching the externs declared in components/font/terminus_font.h. Run
automatically by CMake (see components/font/font_data.cmake) for the device,
simulator and unit-test builds -- never commit its output.

Container layout (all little-endian), one blob per translation unit:

    0   char[4]  magic  "CDF1"
    4   u8       version (1)
    5   u8       width
    6   u8       height
    7   u8       face          0 = regular, 1 = bold
    8   u8       smear_left    bold only; 0 for regular
    9   u8       reserved (0)
    10  u16      num_ranges
    12  u16      palette_len   regular uint16-row sizes only; else 0
    14  u16      pool_bytes
    16  num_ranges x (u16 first, u16 last)
        per range, (last - first + 1) x u16 idx (pool byte offsets;
            0xFFFF in bold faces = "smear the regular glyph")
        palette_len x u16 rows
        pool_bytes x u8 pool

Usage:
    python fontbin2c.py in.bin out.c
"""

import struct
import sys
from pathlib import Path


def parse_blob(path):
    """Parse a CDF1 blob -> dict. Validates sizes exactly (no trailing junk)."""
    b = Path(path).read_bytes()
    if b[:4] != b"CDF1":
        sys.exit(f"{path}: bad magic {b[:4]!r}")
    version, width, height, face, smear_left, _res = struct.unpack_from("<BBBBBB", b, 4)
    if version != 1:
        sys.exit(f"{path}: unsupported version {version}")
    num_ranges, palette_len, pool_bytes = struct.unpack_from("<HHH", b, 10)
    pos = 16

    bounds = []
    for _ in range(num_ranges):
        lo, hi = struct.unpack_from("<HH", b, pos)
        pos += 4
        if hi < lo:
            sys.exit(f"{path}: inverted range {lo:04X}..{hi:04X}")
        bounds.append((lo, hi))

    ranges = []
    for lo, hi in bounds:
        n = hi - lo + 1
        idx = list(struct.unpack_from(f"<{n}H", b, pos))
        pos += 2 * n
        ranges.append((lo, hi, idx))

    palette = list(struct.unpack_from(f"<{palette_len}H", b, pos))
    pos += 2 * palette_len
    pool = b[pos:pos + pool_bytes]
    pos += pool_bytes
    if len(pool) != pool_bytes or pos != len(b):
        sys.exit(f"{path}: truncated or oversized blob "
                  f"(parsed {pos} of {len(b)} bytes)")

    return {
        "width": width, "height": height, "face": face,
        "smear_left": smear_left, "ranges": ranges,
        "palette": palette, "pool": pool,
    }


HEADER = """\
/*
 * {w}x{h}{bold_tag} glyph data derived from the Terminus Font.
 * Compressed pool, format v1 -- see components/font/terminus_font.h and
 * tools/gen_terminus.py for the record/PackBits layout.
 *
 * Copyright (c) 2020 Dimitar Zhekov <dimitar.zhekov@gmail.com>,
 * with Reserved Font Name "Terminus Font".
 *
 * Licensed under the SIL Open Font License, Version 1.1 -- see the full text
 * in components/font/LICENSE. This data is NOT covered by the repository's
 * MIT license.
 *
 * Expanded from {src} by tools/fontbin2c.py at BUILD time.
 * Generated file -- do not edit, do not commit.
 */
#include "terminus_font.h"
#include <stddef.h>

#if {guard}

"""


def _hex_lines(vals, fmt, per_line, indent="    "):
    out = []
    for i in range(0, len(vals), per_line):
        chunk = ", ".join(fmt.format(v) for v in vals[i:i + per_line])
        out.append(f"{indent}{chunk},\n")
    return out


def emit_c(blob, out_path, src_name):
    w, h = blob["width"], blob["height"]
    bold = blob["face"] == 1
    base = f"terminus{w}x{h}"
    stem = f"{base}_bold" if bold else base
    idx_prefix = "font_bidx" if bold else "font_idx"
    guard = (f"FONT_RT_{w}X{h} && FONT_BOLD_ENABLED" if bold
             else f"FONT_RT_{w}X{h}")

    parts = [HEADER.format(w=w, h=h, bold_tag=" BOLD" if bold else "",
                           src=src_name, guard=guard)]

    parts.append(f"const uint8_t {stem}_pool[] = {{\n")
    parts.extend(_hex_lines(list(blob["pool"]), "0x{:02X}", 16))
    parts.append("};\n\n")
    parts.append(f"const uint16_t {stem}_pool_bytes = {len(blob['pool'])};\n\n")

    if blob["palette"]:
        parts.append(f"const uint16_t {base}_palette[] = {{\n")
        parts.extend(_hex_lines(blob["palette"], "0x{:04X}", 8))
        parts.append("};\n\n")
        parts.append(f"const uint16_t {base}_palette_len = "
                     f"{len(blob['palette'])};\n\n")

    for lo, hi, idx in blob["ranges"]:
        parts.append(f"static const uint16_t {idx_prefix}_{lo:04X}_{hi:04X}[] = {{\n")
        parts.extend(_hex_lines(idx, "0x{:04X}", 8))
        parts.append("};\n\n")

    parts.append(f"const FontRange {stem}_ranges[] = {{\n")
    for lo, hi, _ in blob["ranges"]:
        parts.append(f"    {{0x{lo:04X}, 0x{hi:04X}, {idx_prefix}_{lo:04X}_{hi:04X}}},\n")
    parts.append("};\n\n")
    num_name = f"{base}_num_bold_ranges" if bold else f"{base}_num_ranges"
    parts.append(f"const int {num_name} = {len(blob['ranges'])};\n\n")

    if bold:
        parts.append(f"const uint8_t {stem}_smear_left = "
                     f"{blob['smear_left']};\n\n")

    parts.append(f"#endif /* {guard} */\n")
    Path(out_path).write_text("".join(parts), encoding="utf-8", newline="\n")


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    in_bin, out_c = sys.argv[1], sys.argv[2]
    blob = parse_blob(in_bin)
    Path(out_c).parent.mkdir(parents=True, exist_ok=True)
    emit_c(blob, out_c, Path(in_bin).name)


if __name__ == "__main__":
    main()
