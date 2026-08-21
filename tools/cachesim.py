"""Simulate the glyph cache against the exact t:mixNNN access pattern, to
explain why 8x16 degrades more than 10x20 at the same `span`."""
M32 = 0xFFFFFFFF


def mixed_cp(k, span):
    k %= span
    if k < 94:  return 0x21 + k
    k -= 94
    if k < 128: return 0x2500 + k
    k -= 128
    if k < 32:  return 0x2580 + k
    k -= 32
    if k < 64:  return 0x00C0 + k
    k -= 64
    if k < 64:  return 0x0390 + k
    k -= 64
    return 0x0410 + (k % 128)


SETS, WAYS, LOG = 128, 2, 7


def simulate(cols, rows, span, frames=6):
    tags = [None] * (SETS * WAYS)
    victim = [0] * SETS
    per_row = []
    for f in range(frames):
        for r in range(rows):
            miss = 0
            for c in range(cols):
                key = mixed_cp(r * 13 + c + f, span)
                s = ((key * 2654435761) & M32) >> (32 - LOG)
                base = s * WAYS
                if tags[base] == key or tags[base + 1] == key:
                    continue
                miss += 1
                if tags[base] is None:      w = 0
                elif tags[base + 1] is None: w = 1
                else:
                    w = victim[s]; victim[s] ^= 1
                tags[base + w] = key
            if f == frames - 1:             # steady state
                per_row.append(miss)
    return per_row


print('%-7s %5s %5s %6s %9s %9s %9s %9s' %
      ('font', 'cols', 'rows', 'span', 'distinct', 'x cap', 'miss/row', 'max/row'))
for name, cols, rows in (('8x16', 100, 30), ('10x20', 80, 24)):
    for span in (160, 320, 510):
        distinct = len({mixed_cp(r * 13 + c, span)
                        for r in range(rows) for c in range(cols)})
        pr = simulate(cols, rows, span)
        print('%-7s %5d %5d %6d %9d %9.2f %9.1f %9d' %
              (name, cols, rows, span, distinct, distinct / 256.0,
               sum(pr) / len(pr), max(pr)))
