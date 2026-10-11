# Font sources

`terminus8x16.c`, `terminus10x20.c`, and `terminus12x24.c` are committed
generated Esperdeck font tables. They combine pinned Terminus Font 4.49.1
BDFs with 17 original symbols from `tui_symbols.json`, all under the
[SIL Open Font License 1.1](LICENSE).

| Codepoint | Symbol | Meaning |
|---|---|---|
| U+2295 | ⊕ | Circled plus |
| U+2296 | ⊖ | Circled minus |
| U+2297 | ⊗ | Circled times |
| U+2298 | ⊘ | Circled division slash |
| U+2299 | ⊙ | Circled dot operator |
| U+231B | ⌛ | Hourglass |
| U+23F1 | ⏱ | Stopwatch |
| U+23F2 | ⏲ | Timer clock |
| U+25D0 | ◐ | Circle with left half black |
| U+25D1 | ◑ | Circle with right half black |
| U+25D2 | ◒ | Circle with lower half black |
| U+25D3 | ◓ | Circle with upper half black |
| U+25D4 | ◔ | Circle with upper right quadrant black |
| U+25D5 | ◕ | Circle with all but upper left quadrant black |
| U+26A0 | ⚠ | Warning sign |
| U+276E | ❮ | Heavy left-pointing angle quotation mark ornament |
| U+276F | ❯ | Heavy right-pointing angle quotation mark ornament |

The circle-phase names and fill directions follow the
[Unicode Geometric Shapes chart](https://www.unicode.org/charts/PDF/U25A0.pdf).

Each symbol has a separate full-cell drawing for 8×16, 10×20, and 12×24.
In the JSON, `#` means a set pixel and `.` means an empty pixel. Rows run
top to bottom and pixels left to right. Keep all three sizes when editing
a symbol; the generator rejects missing sizes, wrong dimensions, invalid
pixels, and collisions with upstream glyphs. These symbols lie outside
the bold subset, so bold text uses their regular bitmap without smearing.

Regenerate from the repository root:

```sh
python tools/gen_terminus.py
# Offline, using the pinned release's extracted BDF directory:
python tools/gen_terminus.py --src /path/to/terminus-font-4.49.1
```

The existing crop, deduplication, PackBits, palette, and range-merging
steps handle the drawings. Do not edit the generated C arrays. Palette
changes can rewrite existing records even when their decoded pixels stay
identical. There is no new runtime format or decoder path.

The 17 symbols add 262 / 352 / 398 bytes of table data for 8×16 /
10×20 / 12×24, including range directories. Only the selected size's
tables occupy DRAM on the device; the three sizes add 1,012 bytes in total
before linker alignment. Bold exception counts remain 150 / 67 / 56.

Run the offline tests in a host compiler environment:

```sh
cmake -S tests/font -B build/font
cmake --build build/font
ctest --test-dir build/font --output-on-failure
```

The C tests check all 1,356 original glyphs against the existing golden
CRCs, in regular and bold modes. `tui_symbols_golden.h` pins the reviewed
symbol drawings as CRC32 values over decoded rows (one byte per row at
8×16; little-endian 16-bit rows at the other sizes). These references are
independent of the compressor and are not rewritten by regeneration.
Review pixels before updating a symbol's reference.

The Python tests check source coverage, rejected malformed drawings,
compression round trips, and stable output when source order changes.
The C tests also check that the chevrons and left/right circle phases mirror each other.
Generator self-verification checks every merged range, including its
question-mark fallback gaps, against the source cells.
