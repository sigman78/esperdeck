"""Validate render_scan.inc's wobble offset and clipping against the reference
it replaces. The reference renders the line straight, then word-shifts it by
koff with a black fill. Both must agree for every cell geometry and every
displacement.

Words carry a distinctive tag per (cell, word) so any misplacement shows.
"""
NW = 400                                   # destination words per scanline
BLACK = 0


def tag(c, w):
    return (c + 1) * 100 + w + 1           # unique, never 0


def reference(ncols, CW, koff):
    """Straight render + black margin, then shift right by koff words."""
    line = [BLACK] * NW
    for c in range(ncols):
        for w in range(CW):
            i = c * CW + w
            if i < NW:
                line[i] = tag(c, w)
    out = [BLACK] * NW
    for i in range(NW):
        s = i - koff
        out[i] = line[s] if 0 <= s < NW else BLACK
    return out


def optimized(ncols, CW, koff):
    """Mirror of the emitted scan: [c0,c1) full cells, <=1 partial each end,
    then black outside [lo, hi)."""
    line = [None] * NW                     # None = never written (a bug)

    c0, c1 = 0, ncols
    if koff > 0:
        c1 = (NW - koff) // CW
        if c1 > ncols:
            c1 = ncols
    elif koff < 0:
        c0 = (-koff + CW - 1) // CW
        if c0 > ncols:
            c0 = ncols

    for c in range(c0, c1):                # fully visible cells
        for w in range(CW):
            line[koff + c * CW + w] = tag(c, w)

    if koff > 0 and c1 < ncols:            # clipped cell, right edge
        base = koff + c1 * CW
        for w in range(NW - base):
            line[base + w] = tag(c1, w)
    elif koff < 0 and c0 > 0:              # clipped cell, left edge
        cp = c0 - 1
        base = koff + cp * CW
        for w in range(-base, CW):
            line[base + w] = tag(cp, w)

    lo = koff if koff > 0 else 0
    hi = min(koff + ncols * CW, NW)
    for i in range(0, lo):
        line[i] = BLACK
    for i in range(hi, NW):
        line[i] = BLACK
    return line


GEOM = [('8x16', 100, 4), ('10x20', 80, 5), ('12x24', 66, 6)]
bad = 0
for name, ncols, CW in GEOM:
    lim = CW + 2                           # beyond anything wobble produces
    for koff in range(-lim, lim + 1):
        exp = reference(ncols, CW, koff)
        got = optimized(ncols, CW, koff)
        unwritten = [i for i, v in enumerate(got) if v is None]
        diffs = [i for i in range(NW) if got[i] != exp[i]]
        if unwritten or diffs:
            bad += 1
            print('%-6s koff=%+d  %d unwritten, %d wrong  first=%s'
                  % (name, koff, len(unwritten), len(diffs),
                     (unwritten + diffs)[:6]))
    print('%-6s ncols=%-4d CW=%d  koff %+d..%+d  ok' % (name, ncols, CW, -lim, lim))

print()
print('FAILED' if bad else 'every geometry and displacement matches the reference')
