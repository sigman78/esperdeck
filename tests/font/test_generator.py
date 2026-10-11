"""Offline checks for the symbol source and the unchanged v1 encoder."""

import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import gen_terminus as gen

CODEPOINTS = {
    0x2297, 0x2298, 0x2299, 0x231B, 0x23F1, 0x23F2,
    0x25D0, 0x25D1, 0x25D2, 0x25D3, 0x25D4, 0x25D5, 0x26A0, 0x276E, 0x276F,
}


class GeneratorTests(unittest.TestCase):
    def setUp(self):
        self.source = json.loads(gen.SYMBOLS_PATH.read_text(encoding="utf-8"))

    def test_all_sizes_have_exact_symbol_coverage(self):
        for width, height in gen.SIZES:
            with self.subTest(size=(width, height)):
                original = {0x3F: [1] * height}
                merged = gen.add_tui_symbols(original, width, height)
                self.assertEqual(set(merged) - set(original), CODEPOINTS)
                self.assertEqual(original, {0x3F: [1] * height})
                self.assertEqual(merged[0x3F], original[0x3F])
                for cp in CODEPOINTS:
                    self.assertEqual(len(merged[cp]), height)
                    self.assertTrue(any(merged[cp]))
                    self.assertTrue(all(0 <= row < 1 << width for row in merged[cp]))

    def test_symbols_roundtrip_through_v1_for_all_sizes(self):
        for width, height in gen.SIZES:
            with self.subTest(size=(width, height)):
                normal = {cp: [0] * height for lo, hi in gen.BOLD_SUBSET
                          for cp in range(lo, hi + 1)}
                normal[0x3F] = [1] * height
                normal = gen.add_tui_symbols(normal, width, height)
                data = gen.build_font_data(width, height, normal, {})
                offsets = {cp: idx[cp - lo] for lo, hi, idx in data["reg_range_data"]
                           for cp in range(lo, hi + 1)}
                for cp in CODEPOINTS:
                    rows = gen.decode_record(data["reg_pool"], offsets[cp], height,
                                             data["rb"], data["palette"])
                    self.assertEqual(rows, normal[cp])
                bold_cps = {cp for lo, hi, _ in data["bold_range_data"]
                            for cp in range(lo, hi + 1)}
                self.assertFalse(CODEPOINTS & bold_cps)

    def test_rejects_upstream_collisions(self):
        for width, height in gen.SIZES:
            with self.assertRaisesRegex(ValueError, "duplicate codepoint"):
                gen.add_tui_symbols({0x2298: [0] * height}, width, height)

    def test_rejects_invalid_or_duplicate_codepoints(self):
        for key in ("10000", "-1", "02298"):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as tmp:
                source = copy.deepcopy(self.source)
                source["glyphs"][key] = source["glyphs"]["2298"]
                path = Path(tmp) / "symbols.json"
                path.write_text(json.dumps(source), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, "invalid or duplicate codepoint"):
                    gen.add_tui_symbols({}, 8, 16, path=path)

    def test_rejects_incomplete_or_invalid_drawings(self):
        for size in ("8x16", "10x20", "12x24"):
            for fault in ("missing-size", "short-cell", "wide-row", "bad-pixel"):
                with self.subTest(size=size, fault=fault), tempfile.TemporaryDirectory() as tmp:
                    source = copy.deepcopy(self.source)
                    glyph = source["glyphs"]["2298"]
                    if fault == "missing-size":
                        del glyph[size]
                    elif fault == "short-cell":
                        glyph[size].pop()
                    elif fault == "wide-row":
                        glyph[size][0] += "."
                    else:
                        glyph[size][0] = "x" + glyph[size][0][1:]
                    path = Path(tmp) / "symbols.json"
                    path.write_text(json.dumps(source), encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, "needs .* pixels"):
                        gen.add_tui_symbols({}, *map(int, size.split("x")), path=path)

    def test_source_order_does_not_change_compression(self):
        for width, height in gen.SIZES:
            normal = {cp: [0] * height for lo, hi in gen.BOLD_SUBSET
                      for cp in range(lo, hi + 1)}
            normal = gen.add_tui_symbols(normal, width, height)
            reversed_normal = dict(reversed(list(normal.items())))
            self.assertEqual(gen.build_font_data(width, height, normal, {}),
                             gen.build_font_data(width, height, reversed_normal, {}))


if __name__ == "__main__":
    unittest.main()
