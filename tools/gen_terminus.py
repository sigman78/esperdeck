#!/usr/bin/env python3
"""gen_terminus.py -- generate the compressed glyph tables that ship in components/font.

The script fetches the upstream Terminus Font release (BDF sources),
caching the download once in tools/.cache/. It compresses each size with
the "v1" record format: crop, dedup, PackBits, and a row palette (spec in
components/font/terminus_font.h). It self-verifies the encoded pools:
each pool must decode pixel-exact against the BDF cells. Finally, it
emits one .c translation unit per size into components/font/. Developers
commit the emitted files to the repo; the build performs no generation
step.

Coverage: the regular face carries the full Terminus repertoire (about
1356 glyphs). The script merges adjacent codepoint ranges whose gap is 3
codepoints or less. The gap codepoints then point at the '?' record.

The bold face covers BOLD_SUBSET only. The script synthesizes matching
codepoints at decode time, using the 0xFFFF idx sentinel. A codepoint
matches when smearing the regular glyph one pixel reproduces its true
bold bitmap. Only the exceptions store a record.

Usage:
    python gen_terminus.py                  # all sizes, cached download.
    python gen_terminus.py 8x16 12x24       # only these sizes.
    python gen_terminus.py --src PATH       # local terminus dir or .tar.gz.
    python gen_terminus.py --out DIR        # default: components/font.
"""

import argparse
import struct
import sys
import tarfile
import urllib.request
from collections import Counter
from pathlib import Path

TERMINUS_VERSION = "4.49.1"
TERMINUS_URL = ("https://sourceforge.net/projects/terminus-font/files/"
                "terminus-font-{major}/terminus-font-{ver}.tar.gz/download")

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = REPO_ROOT / "components" / "font"
CACHE_DIR = Path(__file__).resolve().parent / ".cache"

#           WxH:   (normal BDF,     bold BDF)
SIZES = {(8, 16):  ("ter-u16n.bdf", "ter-u16b.bdf"),
         (10, 20): ("ter-u20n.bdf", "ter-u20b.bdf"),
         (12, 24): ("ter-u24n.bdf", "ter-u24b.bdf")}

# Bold coverage -- subset "A": the scripts a terminal actually bolds.
# Codepoints outside this fall back to the normal glyph, unsmeared.
BOLD_SUBSET = [
    (0x0020, 0x007E),   # ASCII
    (0x00A0, 0x017F),   # Latin-1 supplement + Extended-A
    (0x0400, 0x045F),   # Cyrillic (basic Russian + extensions)
]

# gap*2 < 8 (one FontRange directory entry) <=> gap <= 3
MAX_MERGE_GAP = 3

# Consistency check pinned to TERMINUS_VERSION -- update these counts (from
# the tool's own report) when bumping the version.
EXPECTED_BOLD_EXCEPTIONS = {8: 150, 10: 67, 12: 56}


def fetch_bdfs(src):
    """Return a dir containing the six ter-u{16,20,24}{n,b}.bdf files.

    src == None: download the pinned release into tools/.cache/ (once).
    src == dir:  use its BDFs directly (release checkout or extracted tree).
    src == .tar.gz: extract the needed BDFs into the cache."""
    wanted = [f for pair in SIZES.values() for f in pair]

    def bdf_dir_ok(d):
        return all((d / f).is_file() for f in wanted)

    if src is not None:
        src = Path(src)
        if src.is_dir():
            if bdf_dir_ok(src):
                return src
            sub = src / f"terminus-font-{TERMINUS_VERSION}"
            if sub.is_dir() and bdf_dir_ok(sub):
                return sub
            sys.exit(f"{src}: missing one of {', '.join(wanted)}")
        if not src.is_file():
            sys.exit(f"{src}: no such file or directory")
        tarball = src
    else:
        CACHE_DIR.mkdir(exist_ok=True)
        (CACHE_DIR / ".gitignore").write_text("*\n")
        tarball = CACHE_DIR / f"terminus-font-{TERMINUS_VERSION}.tar.gz"
        if not tarball.is_file():
            url = TERMINUS_URL.format(
                major=".".join(TERMINUS_VERSION.split(".")[:2]),
                ver=TERMINUS_VERSION)
            print(f"downloading {url}")
            req = urllib.request.Request(url, headers={"User-Agent": "gen_terminus"})
            with urllib.request.urlopen(req) as r:
                data = r.read()
            tarball.write_bytes(data)
            print(f"  -> {tarball} ({len(data)} bytes)")

    out = CACHE_DIR / f"terminus-font-{TERMINUS_VERSION}"
    if bdf_dir_ok(out):
        return out
    out.mkdir(parents=True, exist_ok=True)
    with tarfile.open(tarball, "r:gz") as tf:
        for m in tf.getmembers():
            name = Path(m.name).name
            if name in wanted and m.isfile():
                (out / name).write_bytes(tf.extractfile(m).read())
    if not bdf_dir_ok(out):
        sys.exit(f"{tarball}: does not contain all of {', '.join(wanted)}")
    return out


def parse_bdf(path):
    """Return (width, height, {codepoint: [row_int; height]})."""
    fbb = None                      # (w, h, xoff, yoff)
    glyphs = {}
    cp = None
    bbx = None
    rows = None
    in_bitmap = False

    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("FONTBOUNDINGBOX"):
            _, w, h, xo, yo = line.split()
            fbb = (int(w), int(h), int(xo), int(yo))
        elif line.startswith("STARTCHAR"):
            cp, bbx, rows, in_bitmap = None, None, [], False
        elif line.startswith("ENCODING"):
            cp = int(line.split()[1])
        elif line.startswith("BBX"):
            _, bw, bh, bx, by = line.split()
            bbx = (int(bw), int(bh), int(bx), int(by))
        elif line.startswith("BITMAP"):
            in_bitmap = True
        elif line.startswith("ENDCHAR"):
            if cp is not None and 0 <= cp <= 0xFFFF:
                glyphs[cp] = compose_cell(fbb, bbx, rows, cp)
            in_bitmap = False
        elif in_bitmap:
            rows.append(line.strip())

    if fbb is None:
        sys.exit("no FONTBOUNDINGBOX in BDF")
    return fbb[0], fbb[1], glyphs


def compose_cell(fbb, bbx, hexrows, cp):
    """Place a BBX-cropped bitmap into the full font cell, right-aligned."""
    fw, fh, fxo, fyo = fbb
    bw, bh, bx, by = bbx
    start_row = (fh + fyo) - (bh + by)
    x_off = bx - fxo
    cell = [0] * fh
    mask = (1 << fw) - 1
    for i, hx in enumerate(hexrows):
        raw = int(hx, 16)
        bits = raw >> (len(hx) * 4 - bw)          # right-align bw bits
        shift = fw - x_off - bw
        val = bits << shift if shift >= 0 else bits >> -shift
        r = start_row + i
        if not 0 <= r < fh:
            if bits:
                print(f"warn: U+{cp:04X} row {r} clipped (set bits outside cell)")
            continue
        if val & ~mask:
            print(f"warn: U+{cp:04X} bits clipped horizontally")
        cell[r] = val & mask
    return cell


def contiguous_ranges(cps):
    cps = sorted(cps)
    out = []
    run = [cps[0], cps[0]]
    for c in cps[1:]:
        if c == run[1] + 1:
            run[1] = c
        else:
            out.append(tuple(run))
            run = [c, c]
    out.append(tuple(run))
    return out


def merge_ranges(ranges, max_gap=MAX_MERGE_GAP):
    ranges = sorted(ranges)
    merged = [list(ranges[0])]
    for lo, hi in ranges[1:]:
        gap = lo - merged[-1][1] - 1
        if gap <= max_gap:
            merged[-1][1] = hi
        else:
            merged.append([lo, hi])
    return [tuple(r) for r in merged]


def bold_ranges_for(font_cps):
    """BOLD_SUBSET clipped to the codepoints the regular font covers."""
    out = []
    for lo, hi in BOLD_SUBSET:
        covered = [c for c in range(lo, hi + 1) if c in font_cps]
        if covered:
            out.extend(contiguous_ranges(covered))
    return out


def crop(rows, height):
    """(start, len, cropped_row_tuple). All-blank: start=0.
    h==16 forces len=1 with a single encoded 0x00 row (the 4-bit len field
    can't express 0); h>16 uses len=0 (no body) for a true blank record."""
    idxs = [i for i, v in enumerate(rows) if v != 0]
    if not idxs:
        return (0, 1, (0,)) if height == 16 else (0, 0, ())
    s, e = idxs[0], idxs[-1]
    return (s, e - s + 1, tuple(rows[s:e + 1]))


def packbits_encode(rows, scost, emit_symbol):
    """Optimal literal/repeat PackBits partition via O(n^2) DP.
    Returns the encoded body bytes (bytearray)."""
    n = len(rows)
    if n == 0:
        return bytearray()
    INF = float("inf")
    dp = [INF] * (n + 1)
    dp[0] = 0
    choice = [None] * (n + 1)
    for i in range(n):
        if dp[i] == INF:
            continue
        base = dp[i] + 1
        # literal runs: length 1..128
        run_cost = 0
        for L in range(1, min(128, n - i) + 1):
            run_cost += scost(rows[i + L - 1])
            total = base + run_cost
            j = i + L
            if total < dp[j]:
                dp[j] = total
                choice[j] = ("lit", i, L)
        # repeat runs: length 2..129, constant symbol
        max_run = 1
        while max_run < min(129, n - i) and rows[i + max_run] == rows[i]:
            max_run += 1
        if max_run >= 2:
            rep_total = base + scost(rows[i])
            for RL in range(2, max_run + 1):
                j = i + RL
                if rep_total < dp[j]:
                    dp[j] = rep_total
                    choice[j] = ("rep", i, RL)
    # reconstruct
    segs = []
    j = n
    while j > 0:
        typ, i, L = choice[j]
        segs.append((typ, i, L))
        j = i
    segs.reverse()
    out = bytearray()
    for typ, i, L in segs:
        if typ == "lit":
            out.append(L - 1)
            for k in range(L):
                emit_symbol(out, rows[i + k])
        else:
            out.append(0x80 | (L - 2))
            emit_symbol(out, rows[i])
    return out


def build_font_data(width, height, normal, real_bold):
    """normal: {cp: [row_int; height]} full regular coverage.
    real_bold: {cp: [row_int; height]} from the bold BDF; a cp absent there
    keeps its normal glyph as the true bold form."""
    rb = 1 if width <= 8 else 2
    font_cps = set(normal)
    reg_ranges = contiguous_ranges(font_cps)
    reg_crop = {cp: crop(rows, height) for cp, rows in normal.items()}

    bold_ranges = bold_ranges_for(font_cps)
    assert len(bold_ranges) == 3, f"expected 3 bold ranges, got {bold_ranges}"
    bold_cps = [cp for lo, hi in bold_ranges for cp in range(lo, hi + 1)]

    def smear_row(r):
        return (r | ((r << 1) & 0xFF)) if width == 8 else (r | (r >> 1))

    exc_crop = {}
    for cp in bold_cps:
        true_bold = real_bold.get(cp, normal[cp])
        smeared = [smear_row(r) for r in normal[cp]]
        if true_bold != smeared:
            exc_crop[cp] = crop(true_bold, height)

    # --- shared row palette (rb==2 sizes only) ---
    palette = []
    row_to_idx = {}
    if rb == 2:
        freq = Counter()
        for rec in set(reg_crop.values()):
            for row in rec[2]:
                freq[row] += 1
        for rec in set(exc_crop.values()):
            for row in rec[2]:
                freq[row] += 1
        ordered = sorted(freq, key=lambda r: (-freq[r], r))
        palette = ordered[:255]
        row_to_idx = {r: i for i, r in enumerate(palette)}

    def scost(sym):
        if rb == 1:
            return 1
        return 1 if sym in row_to_idx else 3

    def emit_symbol(out, sym):
        if rb == 1:
            out.append(sym & 0xFF)
            return
        idx = row_to_idx.get(sym, 0xFF)
        out.append(idx)
        if idx == 0xFF:
            out.append(sym & 0xFF)
            out.append((sym >> 8) & 0xFF)

    def encode_record(rec):
        start, ln, rows = rec
        if ln == 0:
            return bytearray([0, 0])
        if height == 16:
            header = bytearray([((start & 0xF) << 4) | ((ln - 1) & 0xF)])
        else:
            header = bytearray([start, ln])
        return header + packbits_encode(list(rows), scost, emit_symbol)

    def build_pool(crop_map, cp_order):
        pool = bytearray()
        offsets = {}
        n_records = 0
        for cp in cp_order:
            rec = crop_map[cp]
            if rec not in offsets:
                offsets[rec] = len(pool)
                pool += encode_record(rec)
                n_records += 1
        assert len(pool) <= 65534, f"pool too large: {len(pool)} bytes"
        return bytes(pool), offsets, n_records

    reg_pool, reg_offsets, reg_nrec = build_pool(reg_crop, sorted(reg_crop))
    if exc_crop:
        bold_pool, bold_offsets, bold_nrec = build_pool(exc_crop, sorted(exc_crop))
    else:
        bold_pool, bold_offsets, bold_nrec = b"", {}, 0

    assert 0x3F in reg_crop, "'?' (U+003F) missing from regular coverage"
    q_offset = reg_offsets[reg_crop[0x3F]]

    merged_reg_ranges = merge_ranges(reg_ranges)
    reg_range_data = []
    for lo, hi in merged_reg_ranges:
        idx = []
        for cp in range(lo, hi + 1):
            off = reg_offsets[reg_crop[cp]] if cp in reg_crop else q_offset
            assert off < len(reg_pool)
            idx.append(off)
        reg_range_data.append((lo, hi, idx))

    bold_range_data = []
    for lo, hi in bold_ranges:
        idx = []
        for cp in range(lo, hi + 1):
            if cp in exc_crop:
                off = bold_offsets[exc_crop[cp]]
                assert off < len(bold_pool)
                idx.append(off)
            else:
                idx.append(0xFFFF)
        bold_range_data.append((lo, hi, idx))

    return {
        "width": width, "height": height, "rb": rb,
        "reg_pool": reg_pool, "reg_nrec": reg_nrec,
        "reg_range_data": reg_range_data,
        "bold_pool": bold_pool, "bold_nrec": bold_nrec,
        "bold_range_data": bold_range_data,
        "palette": palette,
        "n_glyphs": len(normal), "n_bold_cps": len(bold_cps),
        "exception_count": len(exc_crop),
        "smear_left": 1 if width == 8 else 0,
    }


# Self-verification decodes the encoded pools and compares each codepoint
# against the BDF cells. This module reimplements font_renderer.c's
# decoder in Python, so the check matches the exact renderer semantics.

def decode_record(pool, offset, height, rb, palette):
    """Reimplementation of the v1 decoder for one glyph record."""
    if height == 16:
        b0 = pool[offset]
        start = (b0 >> 4) & 0xF
        ln = (b0 & 0xF) + 1
        pos = offset + 1
    else:
        start = pool[offset]
        ln = pool[offset + 1]
        pos = offset + 2
        if ln == 0:
            return [0] * height

    body = []
    produced = 0
    while produced < ln:
        c = pool[pos]
        pos += 1
        if c < 0x80:
            n = c + 1
            for _ in range(n):
                if rb == 1:
                    sym = pool[pos]
                    pos += 1
                else:
                    pidx = pool[pos]
                    pos += 1
                    if pidx == 0xFF:
                        sym = pool[pos] | (pool[pos + 1] << 8)
                        pos += 2
                    else:
                        sym = palette[pidx]
                body.append(sym)
            produced += n
        else:
            n = (c & 0x7F) + 2
            if rb == 1:
                sym = pool[pos]
                pos += 1
            else:
                pidx = pool[pos]
                pos += 1
                if pidx == 0xFF:
                    sym = pool[pos] | (pool[pos + 1] << 8)
                    pos += 2
                else:
                    sym = palette[pidx]
            body.extend([sym] * n)
            produced += n
    assert produced == ln, f"packbits overrun at offset {offset}"

    rows = [0] * height
    for i, v in enumerate(body):
        rows[start + i] = v
    return rows


def verify_data(data, normal, real_bold):
    width, height, rb = data["width"], data["height"], data["rb"]
    palette = data["palette"] or None
    pool, bpool = data["reg_pool"], data["bold_pool"]
    q_cell = normal[0x3F]

    def smear_row(r):
        return (r | ((r << 1) & 0xFF)) if width == 8 else (r | (r >> 1))

    errors = []
    cp_off = {}
    for lo, hi, idx in data["reg_range_data"]:
        for i, cp in enumerate(range(lo, hi + 1)):
            cp_off[cp] = idx[i]
            want = normal.get(cp, q_cell)     # gap cps decode as '?'
            got = decode_record(pool, idx[i], height, rb, palette)
            if got != want:
                errors.append(f"regular U+{cp:04X}")

    for lo, hi, idx in data["bold_range_data"]:
        for i, cp in enumerate(range(lo, hi + 1)):
            want = real_bold.get(cp, normal[cp])
            if idx[i] == 0xFFFF:
                got = [smear_row(r)
                       for r in decode_record(pool, cp_off[cp], height, rb, palette)]
            else:
                got = decode_record(bpool, idx[i], height, rb, palette)
            if got != want:
                errors.append(f"bold U+{cp:04X}")

    if errors:
        for e in errors[:20]:
            print("MISMATCH:", e)
        sys.exit(f"FAIL {width}x{height}: {len(errors)} decode mismatch(es)")
    print(f"verify {width}x{height}: {len(cp_off)} regular cps + "
          f"{data['n_bold_cps']} bold cps decode pixel-exact")


def face_stats(range_data, pool, nrec, n_real):
    idx_cps = sum(hi - lo + 1 for lo, hi, _ in range_data)
    return {
        "idx_cps": idx_cps, "n_real": n_real, "nrec": nrec,
        "pool_b": len(pool), "idx_b": idx_cps * 2,
        "dir_b": len(range_data) * 8,     # sizeof(FontRange), 4B pointer
        "n_ranges": len(range_data),
    }


def build_stats(data, real_bold_differs):
    gb = data["height"] * data["rb"]
    reg = face_stats(data["reg_range_data"], data["reg_pool"],
                     data["reg_nrec"], data["n_glyphs"])
    bold = face_stats(data["bold_range_data"], data["bold_pool"],
                      data["bold_nrec"], data["exception_count"])
    pal_b = len(data["palette"]) * 2
    reg_total = reg["pool_b"] + reg["idx_b"] + reg["dir_b"]
    bold_total = bold["pool_b"] + bold["idx_b"] + bold["dir_b"]
    total = reg_total + bold_total + pal_b

    # The flat row-array equivalent uses one uncompressed row array over the
    # same merged ranges. It also carries the pre-v1 sparse bold table
    # (true-bold != normal).
    flat = (reg["idx_cps"] + real_bold_differs) * gb \
         + reg["dir_b"] + bold["dir_b"]

    lines = [
        f"regular  {reg['n_real']} glyphs (+{reg['idx_cps'] - reg['n_real']} "
        f"range-gap cps -> '?') in {reg['n_ranges']} ranges",
        f"         pool {reg['pool_b']} B / {reg['nrec']} unique records "
        f"= {reg['pool_b'] / reg['nrec']:.1f} B/record "
        f"({reg['pool_b'] / reg['n_real']:.1f} B/glyph vs {gb} B flat)",
        f"         idx {reg['idx_b']} B, range dir {reg['dir_b']} B",
    ]
    if pal_b:
        lines.append(f"palette  {len(data['palette'])} rows, {pal_b} B "
                     f"(shared regular/bold)")
    smear = data["n_bold_cps"] - data["exception_count"]
    lines += [
        f"bold     {data['n_bold_cps']} cps in {bold['n_ranges']} ranges: "
        f"{smear} smear-synthesized, {data['exception_count']} stored "
        f"({bold['nrec']} unique records)",
        f"         pool {bold['pool_b']} B, idx {bold['idx_b']} B, "
        f"range dir {bold['dir_b']} B",
        f"total    {total} B   "
        f"(flat row tables: {flat} B -> {100.0 * (total - flat) / flat:+.1f}%)",
    ]
    return lines, total


C_HEADER = """\
/*
 * {w}x{h} Terminus glyph tables — compressed format v1 (spec in
 * terminus_font.h, encoder in tools/gen_terminus.py).
 *
 * Derived from the Terminus Font {ver} ({nsrc} + {bsrc}).
 * Copyright (c) 2020 Dimitar Zhekov <dimitar.zhekov@gmail.com>,
 * with Reserved Font Name "Terminus Font".
 * Licensed under the SIL Open Font License, Version 1.1 — full text in
 * components/font/LICENSE. This file is NOT covered by the repository's
 * MIT license.
 *
 * GENERATED file, but COMMITTED: regenerate with
 *     python tools/gen_terminus.py {w}x{h}
 * instead of editing by hand.
 *
{stats}
 */
#include "terminus_font.h"

#if FONT_RT_{w}X{h}

"""


def _hex_lines(vals, fmt, per_line, indent="    "):
    out = []
    for i in range(0, len(vals), per_line):
        chunk = ", ".join(fmt.format(v) for v in vals[i:i + per_line])
        out.append(f"{indent}{chunk},\n")
    return out


def _emit_face(parts, prefix, range_data, pool):
    parts.append(f"static const uint8_t {prefix}pool[] = {{\n")
    parts.extend(_hex_lines(list(pool), "0x{:02X}", 16))
    parts.append("};\n\n")
    for lo, hi, idx in range_data:
        parts.append(f"static const uint16_t {prefix}idx_{lo:04X}_{hi:04X}[] = {{\n")
        parts.extend(_hex_lines(idx, "0x{:04X}", 8))
        parts.append("};\n\n")
    parts.append(f"static const FontRange {prefix}ranges[] = {{\n")
    for lo, hi, _ in range_data:
        parts.append(f"    {{0x{lo:04X}, 0x{hi:04X}, {prefix}idx_{lo:04X}_{hi:04X}}},\n")
    parts.append("};\n\n")


def emit_c(out_path, data, stats_lines, nsrc, bsrc):
    w, h = data["width"], data["height"]
    base = f"terminus{w}x{h}"
    has_pal = bool(data["palette"])
    stats = "\n".join(f" * {ln}".rstrip() for ln in
                      ["Size stats:"] + ["  " + s for s in stats_lines])

    parts = [C_HEADER.format(w=w, h=h, ver=TERMINUS_VERSION,
                             nsrc=nsrc, bsrc=bsrc, stats=stats)]

    _emit_face(parts, "r_", data["reg_range_data"], data["reg_pool"])

    if has_pal:
        parts.append("static const uint16_t pal[] = {\n")
        parts.extend(_hex_lines(data["palette"], "0x{:04X}", 8))
        parts.append("};\n\n")

    parts.append(
        f"const FontFace {base}_regular = {{\n"
        f"    .ranges      = r_ranges,\n"
        f"    .pool        = r_pool,\n"
        f"    .palette     = {'pal' if has_pal else 'NULL'},\n"
        f"    .num_ranges  = {len(data['reg_range_data'])},\n"
        f"    .pool_bytes  = {len(data['reg_pool'])},\n"
        f"    .palette_len = {len(data['palette'])},\n"
        f"    .smear_left  = 0,\n"
        f"}};\n\n")

    parts.append("#if FONT_BOLD_ENABLED\n\n")
    _emit_face(parts, "b_", data["bold_range_data"], data["bold_pool"])
    parts.append(
        f"const FontFace {base}_bold = {{\n"
        f"    .ranges      = b_ranges,\n"
        f"    .pool        = b_pool,\n"
        f"    .palette     = {'pal' if has_pal else 'NULL'},\n"
        f"    .num_ranges  = {len(data['bold_range_data'])},\n"
        f"    .pool_bytes  = {len(data['bold_pool'])},\n"
        f"    .palette_len = {len(data['palette'])},\n"
        f"    .smear_left  = {data['smear_left']},\n"
        f"}};\n\n"
        f"#endif /* FONT_BOLD_ENABLED */\n\n"
        f"#endif /* FONT_RT_{w}X{h} */\n")

    Path(out_path).write_text("".join(parts), encoding="utf-8", newline="\n")


def parse_wh(spec):
    w, h = spec.lower().split("x")
    wh = (int(w), int(h))
    if wh not in SIZES:
        sys.exit(f"unknown size {spec}; known: "
                 + " ".join(f"{a}x{b}" for a, b in SIZES))
    return wh


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("sizes", nargs="*", default=[],
                    help="sizes to generate (default: all): 8x16 10x20 12x24")
    ap.add_argument("--src", metavar="PATH",
                    help="local terminus dir or .tar.gz (default: cached download)")
    ap.add_argument("--out", metavar="DIR", type=Path, default=DEFAULT_OUT,
                    help="output directory (default: components/font)")
    args = ap.parse_args()

    sizes = [parse_wh(s) for s in args.sizes] or list(SIZES)
    bdf_dir = fetch_bdfs(args.src)

    for wh in sizes:
        w, h = wh
        nsrc, bsrc = SIZES[wh]
        nw, nh, normal = parse_bdf(bdf_dir / nsrc)
        bw, bh, bold = parse_bdf(bdf_dir / bsrc)
        if (nw, nh) != wh or (bw, bh) != wh:
            sys.exit(f"{nsrc}/{bsrc}: size mismatch (normal {nw}x{nh}, "
                     f"bold {bw}x{bh}, expected {w}x{h})")

        data = build_font_data(w, h, normal, bold)
        want_exc = EXPECTED_BOLD_EXCEPTIONS[w]
        if data["exception_count"] != want_exc:
            sys.exit(f"bold exception count mismatch for {w}x{h}: got "
                     f"{data['exception_count']}, expected {want_exc} — update "
                     f"EXPECTED_BOLD_EXCEPTIONS if TERMINUS_VERSION changed")

        verify_data(data, normal, bold)

        real_bold_differs = sum(
            1 for lo, hi, _ in data["bold_range_data"]
            for cp in range(lo, hi + 1) if bold.get(cp, normal[cp]) != normal[cp])
        stats_lines, total = build_stats(data, real_bold_differs)

        out_path = args.out / f"terminus{w}x{h}.c"
        emit_c(out_path, data, stats_lines, nsrc, bsrc)
        print(f"wrote {out_path} ({total} B of table data)")
        for ln in stats_lines:
            print(f"  {ln}")


if __name__ == "__main__":
    main()
