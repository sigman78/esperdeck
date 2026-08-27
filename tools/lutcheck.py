"""Prove the per-cell pair LUT reproduces scan_gpair exactly. Check every
glyph row pattern, every pixel position, and a spread of colour pairs."""
import random


def scan_gpair(row, w, p, bg, xf):
    m0 = 0xFFFF if ((row >> (w - 1 - p)) & 1) else 0
    m1 = 0xFFFF if ((row >> (w - 2 - p)) & 1) else 0
    p0 = (bg ^ (xf & m0)) & 0xFFFF
    p1 = (bg ^ (xf & m1)) & 0xFFFF
    return p0 | (p1 << 16)


def build_pair_lut(fg, bg):
    f, b = fg & 0xFFFF, bg & 0xFFFF
    return [b | (b << 16), b | (f << 16), f | (b << 16), f | (f << 16)]


rng = random.Random(20260821)
bad = 0
for name, W in (('8x16', 8), ('10x20', 10), ('12x24', 12)):
    colours = [(0x0000, 0xFFFF), (0xFFFF, 0x0000), (0xF800, 0x001F),
               (0x07E0, 0x07E0)] + [(rng.getrandbits(16), rng.getrandbits(16))
                                    for _ in range(64)]
    checked = 0
    for fg, bg in colours:
        xf = fg ^ bg
        lut = build_pair_lut(fg, bg)
        for row in range(1 << W):            # exhaustive over glyph rows
            for p in range(0, W, 2):
                ref = scan_gpair(row, W, p, bg, xf)
                got = lut[(row >> (W - 2 - p)) & 3]
                checked += 1
                if ref != got:
                    bad += 1
                    print('%s W=%d row=%#x p=%d fg=%04x bg=%04x  ref=%08x got=%08x'
                          % (name, W, row, p, fg, bg, ref, got))
                    break
            if bad:
                break
        if bad:
            break
    print('%-6s W=%2d  %d combinations checked  %s'
          % (name, W, checked, 'FAIL' if bad else 'ok'))

print()
print('FAILED' if bad else 'pair LUT is bit-identical to scan_gpair')
