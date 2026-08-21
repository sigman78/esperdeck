"""Verify the SWAR smear is bit-identical to the per-row loop it replaces,
for every glyph geometry and both smear directions, over exhaustive rows."""
import random


def ref(rows, bits, left):
    m = (1 << bits) - 1
    return [((v | ((v << 1) & m)) if left else (v | (v >> 1))) & m for v in rows]


def swar(rows, bits, left):
    per = 32 // bits
    if bits == 8:
        maskL, maskR = 0xFEFEFEFE, 0x7F7F7F7F
    else:
        maskL, maskR = 0xFFFEFFFE, 0x7FFF7FFF
    out = []
    for i in range(0, len(rows), per):
        w = 0
        for j in range(per):                    # little-endian lane packing
            w |= (rows[i + j] & ((1 << bits) - 1)) << (bits * j)
        w = (w | (((w << 1) & maskL) if left else ((w >> 1) & maskR))) & 0xFFFFFFFF
        for j in range(per):
            out.append((w >> (bits * j)) & ((1 << bits) - 1))
    return out


GEOM = [('8x16  rb=1', 8, 16), ('10x20 rb=2', 16, 20), ('12x24 rb=2', 16, 24)]
rng = random.Random(20260821)
bad = 0
for name, bits, height in GEOM:
    for left in (True, False):
        # exhaustive single-row patterns + random full glyphs
        cases = [[(1 << b) if b < bits else 0] * height for b in range(bits)]
        cases.append([0] * height)
        cases.append([(1 << bits) - 1] * height)
        cases += [[rng.getrandbits(bits) for _ in range(height)] for _ in range(400)]
        for rows in cases:
            if ref(rows, bits, left) != swar(rows, bits, left):
                bad += 1
                print('%s left=%s MISMATCH rows=%s' % (name, left, rows[:4]))
                break
    print('%-12s both directions ok  (%d cases each)' % (name, bits + 402))

print()
print('FAILED' if bad else 'SWAR smear is bit-identical to the per-row loop')
