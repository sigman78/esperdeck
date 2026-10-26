# Fused glyph expansion: 8x16 proof of concept

Measured on the Waveshare ESP32-S3 at 240 MHz, ESP-IDF 5.5.2,
Espressif GCC 14.2.0, 2026-10-26. Compared directly with the
[two-pass SIMD prototype](VARIANTS_RESULTS.md) in the same binary.

**Keep the existing two-pass kernel.** This scanline-major fused prototype
is pixel-correct but slower: displaced-band kernel time increases by
6.1-6.8%, and live application-effects peak render time rises from 200.00
to 213.15 us (+6.6%). Average application-effects time rises from 166.01
to 167.39 us (+0.8%); only about 5.8% of callbacks contain displaced bands.

The three-way comparison explains the result. At a +2-word shift, changing
from character-major to scanline-major two-pass rendering costs 32.36 us
per band (117.72 -> 150.08 us). Fusion recovers 24.73 us (150.08 ->
125.35 us), leaving it 7.63 us slower than the existing implementation.
Fusion itself saves 16-19% versus the matching scanline two-pass loop for
nonzero offsets. It cannot repay the palette reloads, per-line guards/calls
and traversal overhead introduced here.

This does not rule out a fused kernel that retains multi-scanline palette
reuse. That would require carrying neighboring glyph state for multiple
scanlines or processing them in small groups, with different register and
scratch-buffer costs. It is a deeper rewrite, not a demonstrated win.

## What was tested

`scan8_fused.S` expands glyph bits to RGB565, retains the previous glyph
vector, and combines adjacent glyphs with `EE.SRC.Q` before the single
aligned output store. It never reads the destination. Ten specialized
functions cover offsets -5..4 words, including unshifted scanlines and
clipped black edges. Each call preserves q0..q5, CPENABLE and SAR_BYTE.
The functions total 1,561 instruction bytes, excluding alignment/dispatch.

The fused kernel traverses scanlines, which reloads palettes for each
glyph on each line. The existing character-major band kernel reuses
two palettes over all 16 lines. To separate these effects, the standalone
kernel experiment compares three implementations:

1. Existing character-major expansion followed by SIMD shifts.
2. The new scanline expansion at zero offset followed by SIMD shifts.
3. The new fused scanline expansion at the requested offset.

Each offset is measured in forward then reverse implementation order,
with 16 warmups and 100 measured bands per sample. Input/output are internal
RAM, kernels are IRAM, and the LCD has not started. Interrupts remain
enabled; CSV includes mean, minimum and maximum cycles. Both runs' means
are averaged for the table.

In the live test, only bands containing a nonzero wobble offset use the
fused scanline implementation. Undisplaced bands retain the existing fast
character-major kernel. Any zero-offset lines within a displaced band
still use the scanline traversal. This is intentional and included in
the measured cost, along with per-line register guards and dispatch.

The live comparison uses the production renderer and all 16 existing
font/text stress phases, with the same ABBA measurement schedule and
16-byte bounce alignment as the previous experiment. The two implementations
are current two-pass SIMD and fused SIMD; scalar is not remeasured here.
Times cover the render body inside the actual LCD callback, excluding
interrupt entry/exit and driver bookkeeping. Network and BLE are inactive.
Reported peaks are observed maxima, not worst-case bounds.

Raw data: [kernel cycles](fused-micro-results.csv),
[live renderer cycles](fused-pipeline-results.csv).

## Correctness and scope

All three host suites pass. The fused portable model and actual S3 assembly
are checked independently against per-pixel expectations over 256 pattern
seeds, all 16 glyph rows, all ten offsets, colors, black clipping and buffer
guards. The existing 8/10/12-wide device tests also pass.

Before LCD startup, fused-enabled output matches the production scalar
renderer for 3,840 bands across the stress phases, including 30 fused bands.
Live task canaries check q0..q5, SAR_BYTE and CPENABLE preservation across
actual ISR preemption and passed in all 64 measurement windows. All measured
callbacks stayed on SIMD with zero scalar fallbacks. The existing IRAM audit
passed, and the new kernels/dispatcher and dispatch table were separately
confirmed in IRAM/DRAM. This is an isolated benchmark, not production integration.

The fused kernel is implemented for **8x16 only**. This tests the simplest
one-glyph-per-vector case; it does not establish performance for a future
10x20/12x24 fused packing kernel or an implementation retaining character-major
palette reuse. Those remain separate experiments.

## Reproduction

```sh
idf.py -C tests/render_simd/pipeline-idf -B build/render-pipeline-idf -D IDF_TARGET=esp32s3 -D BENCH_FONT=0 -D BENCH_FUSED=ON build
```

Set `BENCH_FUSED=OFF` for the earlier scalar/two-pass SIMD comparison.
Device installation must use `app-flash`, after preserving the original app.
Local capture and protection records are under `build/render-pipeline-idf/`.

This run used app-only installation. Afterward the original 3 MB application
partition was restored byte-for-byte and rebooted. Application, storage,
NVS and partition-table MD5s all matched this run's pre-test values.
The statusbar/UI work remains unchanged.

## Kernel measurements

Times are mean cycles divided by 240, for a full 800x16 band with the
same offset on every line. The offset is in 32-bit words (two pixels).

| Offset | Current band two-pass us | Line two-pass us | Fused lines us | Fused vs current |
|---:|---:|---:|---:|---:|
| -5 | 117.31 | 149.68 | 124.41 | +6.1% time |
| -4 | 104.05 | 136.41 | 111.01 | +6.7% time |
| -3 | 117.51 | 149.88 | 125.48 | +6.8% time |
| -2 | 117.51 | 149.88 | 125.48 | +6.8% time |
| -1 | 117.51 | 149.88 | 125.48 | +6.8% time |
| 0 | 80.74 | 113.15 | 111.95 | +38.7% time |
| 1 | 117.72 | 150.08 | 125.35 | +6.5% time |
| 2 | 117.72 | 150.08 | 125.35 | +6.5% time |
| 3 | 117.72 | 150.08 | 125.35 | +6.5% time |
| 4 | 104.25 | 136.61 | 110.88 | +6.4% time |

## Live renderer measurements

| Phase | Current avg us | Fused avg us | Current max us | Fused max us | Fused bands / callbacks |
|---|---:|---:|---:|---:|---:|
| off | 164.28 | 164.29 | 169.30 | 169.32 | 0/4214 |
| clear | 169.32 | 169.34 | 174.35 | 174.38 | 0/4215 |
| scrim | 172.24 | 172.25 | 177.27 | 177.29 | 0/4214 |
| spaces | 159.70 | 159.72 | 164.69 | 164.71 | 0/4215 |
| dense | 158.92 | 158.94 | 163.92 | 163.94 | 0/4215 |
| bars | 159.70 | 159.72 | 164.69 | 164.71 | 0/4214 |
| bold | 159.66 | 159.68 | 164.69 | 164.71 | 0/4214 |
| t:sparse | 152.93 | 152.95 | 162.07 | 162.09 | 0/3091 |
| t:mix160 | 163.85 | 163.87 | 169.59 | 169.61 | 0/2810 |
| t:mix320 | 174.24 | 174.28 | 186.91 | 185.65 | 0/2809 |
| t:mix510 | 184.56 | 184.63 | 300.41 | 300.15 | 0/2810 |
| t:blank | 150.90 | 150.92 | 156.88 | 156.89 | 0/2810 |
| fx:app | 166.01 | 167.39 | 200.00 | 213.15 | 244/4215 |
| fx:app+sp | 154.53 | 155.86 | 190.12 | 204.74 | 174/3091 |
| fx:noscan | 150.66 | 150.68 | 155.77 | 155.78 | 0/4215 |
| fx:none | 149.93 | 149.95 | 155.03 | 155.05 | 0/4216 |
