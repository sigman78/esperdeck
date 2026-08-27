#!/usr/bin/env python3
"""check_iram.py -- post-build guard: the render ISR path must be IRAM/DRAM.

With CONFIG_LCD_RGB_ISR_IRAM_SAFE=y the bounce ISR keeps running while the
flash cache is disabled (settings saves, NVS writes). Any flash-resident
function it can reach -- or flash/PSRAM-resident data it reads -- is a
Cache exception at exactly that moment. Placement is decided by the
linker, so this checks the final ELF, not the sources; CMake runs it as a
POST_BUILD step on the device build (see the top-level CMakeLists.txt).

Two symbol classes:
  * REQUIRED_FUNCS must exist AND sit in IRAM -- key entry points; a
    rename that silently drops one out of the audit fails the build too.
  * The pattern lists cover the static helpers the optimizer may or may
    not emit as standalone symbols: present -> placement is enforced,
    inlined-away -> fine.

Usage: check_iram.py --nm <xtensa-nm> <elf>
"""

import argparse
import re
import subprocess
import sys

# ESP32-S3 internal memory windows.
IRAM = (0x40370000, 0x403E0000)   # instruction bus SRAM
DRAM = (0x3FC80000, 0x3FD00000)   # data bus SRAM (flash/PSRAM map at 0x3C...)

# ISR entry points: must exist, must be IRAM.
REQUIRED_FUNCS = [
    "display_render_chunk",
    "render_cache_resolve",
    "render_scan_band",
    "render_fx_frame_tick",
    "font_decode_glyph",
]

# ISR-reachable code: enforced when present (statics may be inlined away).
# NOTE: the app shell's flash-resident render_home/render_menu/... must NOT
# match -- keep these prefixes specific to the render core.
FUNC_PATTERNS = [
    r"render_(cache|underline|dim|cursor|fx)_",
    r"render_chunk_body",
    r"scan_band_",
    r"build_row_cache",
    r"resolve_(overlay|terminal)_cell",
    r"fx_(mono|dim)565",
    r"on_bounce_empty",
    r"display_ansi_to_rgb565",
    r"decode_(record|regular)",
    r"lookup_offset",
    r"read_symbol16",
    r"smear_glyph",
    r"zero_fill",
]

# ISR-read data: enforced when present (a forgotten DRAM_ATTR on a table
# lands it in flash rodata at 0x3C..., cached -- exactly the hazard).
DATA_PATTERNS = [
    r"s_cc$", r"s_cells$", r"s_overlay$", r"s_overlay_(pal|bar|bar_dim)$",
    r"s_cursor$", r"s_bell$", r"s_fx_noise$", r"g_rs$", r"g_fx_snap$",
    r"g_fx_cfg", r"g_fx_frame$", r"g_fx_wobble_lut$", r"g_fx_glow_acc$",
    r"g_fx_row_stamp$", r"g_fx_(wipe|collapse|static)_", r"wob_env",
]

FUNC_RE = re.compile("^(" + "|".join(FUNC_PATTERNS) + ")")
DATA_RE = re.compile("^(" + "|".join(DATA_PATTERNS) + ")")


def base_name(sym):
    """Strip optimizer suffixes: foo$isra$0 / foo.constprop.0 -> foo."""
    return sym.split("$")[0].split(".")[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--nm", default="xtensa-esp32s3-elf-nm")
    ap.add_argument("elf")
    args = ap.parse_args()

    out = subprocess.run([args.nm, args.elf], capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"check_iram: nm failed: {out.stderr.strip()}")

    funcs, data = {}, {}
    for line in out.stdout.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        addr_s, typ, sym = parts
        try:
            addr = int(addr_s, 16)
        except ValueError:
            continue
        name = base_name(sym)
        if typ in "tT":
            if name in REQUIRED_FUNCS or FUNC_RE.match(name):
                funcs[sym] = addr
        elif typ in "dDbBrR":
            if DATA_RE.match(name):
                data[sym] = addr

    errors = []
    for req in REQUIRED_FUNCS:
        if not any(base_name(s) == req for s in funcs):
            errors.append(f"required ISR symbol '{req}' not found in the ELF "
                          f"-- renamed? update tools/check_iram.py")
    for sym, addr in sorted(funcs.items()):
        if not IRAM[0] <= addr < IRAM[1]:
            errors.append(f"ISR-path function in FLASH: {sym} @ {addr:#010x} "
                          f"-- missing IRAM_ATTR")
    for sym, addr in sorted(data.items()):
        if not DRAM[0] <= addr < DRAM[1]:
            errors.append(f"ISR-read data outside internal DRAM: {sym} @ "
                          f"{addr:#010x} -- missing DRAM_ATTR")

    if errors:
        print("check_iram: FAILED -- the render ISR runs with the flash "
              "cache disabled (LCD_RGB_ISR_IRAM_SAFE); these placements "
              "would be a Cache exception during any settings save:",
              file=sys.stderr)
        for e in errors:
            print("  " + e, file=sys.stderr)
        sys.exit(1)

    print(f"check_iram: OK -- {len(funcs)} ISR functions in IRAM, "
          f"{len(data)} data objects in DRAM")


if __name__ == "__main__":
    main()
