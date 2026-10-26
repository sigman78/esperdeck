# Fixed-offset SIMD wobble and all font sizes

Follow-up: [fused glyph expansion measurements](FUSED_RESULTS.md). The tested
scanline-major fused kernel is slower than this two-pass implementation.

Measured 2026-10-26 on the Waveshare ESP32-S3 at 240 MHz, ESP-IDF 5.5.2,
Espressif GCC 14.2.0. This extends the [initial ISR experiment](PIPELINE_RESULTS.md).
The prototype remains isolated under `tests/render_simd`; production rendering
and the statusbar UI work are unchanged.

The prototype reduces average ISR render time by **30-35% at 8x16,
14-15% at 10x20, and 21-23% at 12x24** across the measured workloads.
For application effects, the observed peak improves from **286.23 to
200.25 us**, **191.56 to 175.53 us**, and **196.20 to 173.15 us** respectively.
The original 8x16 experiment left wobble scalar and did not improve that peak.
All measured SIMD bands now stay on the fast path.

For the plain terminal (`off`) workload, savings per band are 80.42, 19.44,
and 32.29 us. At the unchanged ~39 Hz refresh rate and 30/48/40 bands per
frame, these recover approximately **9.4, 3.6, and 5.0 percentage points
of a core** respectively. Refresh rate itself does not increase.

The 10x20 kernel is the least effective: half its cells require four
individual 32-bit stores for the SIMD portion. Larger fonts also retain
scalar tail pixels. Glyph-cache work remains outside this optimization;
the worst cache-pressure bands still reach 300.10/251.95/217.36 us.

## Implementation

- Nine fixed-offset assembly functions cover offsets -5..-1 and +1..+4 in
  32-bit words (-10..-2 and +2..+8 pixels); zero is a no-op. These cover the
  current wobble strengths. Unsupported offsets retain the scalar fallback.
- `shift_line.S` shifts an already expanded, aligned 800-pixel scanline
  in place. Fractional-vector offsets use `EE.SRC.Q`, with SAR_BYTE set once
  per call. Traversal direction preserves unread pixels; clipped edges are
  filled with black. Full-vector offsets use direct vector copies.
- This is a **second pass on displaced scanlines**, not fused shifted glyph
  emission. Reported timings include this pass and its context saves.
- `scan_wide.S` adds 10x20 and 12x24 expansion. The first eight pixels use
  SIMD masks, with the remaining two/four pixels using the existing pair LUT.
  Store selection handles the 20/24-byte cell strides. Cell-major traversal
  reuses both scanline palettes throughout each band. The 12x24 right margin
  is cleared before shifting.
- The new kernels preserve q0..q5, SAR_BYTE and CPENABLE, using 144 bytes
  of stack per call; q6/q7 and accumulators are untouched. The existing
  8x16 expansion does not change SAR_BYTE. All use the same 4 KB mask table.
  The two wide kernels total 910 bytes of instructions; the nine shifts
  add roughly 1.1 KB, excluding alignment and C dispatch.

## Measurement and validation

The standalone app compiles the production renderer, font decoding and vterm
and includes the existing `main/bench_stress.c` workload definitions. It wraps
the scan entry point and explicitly aligns both LCD bounce buffers to 16 bytes.
Measurements surround the real `display_render_chunk` call inside the LCD
callback: cache work, glyph expansion, post-effects and wrapper overhead are
included. These are render-body ISR timings, not the entire interrupt entry,
driver bookkeeping and exit. Wi-Fi, BLE and SSH are not running.

Every phase runs scalar/SIMD/SIMD/scalar, with three settling and twelve
measured feed+100 ms iterations. The effects frame counter resets for each
window. Tables report the mean of the two callback averages and the larger
observed maximum, converting cycles at 240 MHz. Maxima are observations,
not worst-case execution-time bounds. Raw cycles and callback counts are in
[variants-results.csv](variants-results.csv).

The three fonts use different band heights (16, 10, 12 lines) and grids
(100x30, 80x24, 66x20). Compare scalar/SIMD within each font; raw per-band
times and cache-pressure workloads are not equivalent across font sizes.

Validation includes:

- Both standalone host suites pass. Portable models are checked against
  independent per-pixel expectations.
- Actual S3 kernels pass pixel and buffer-guard checks for widths 8/10/12,
  256 glyph-pattern seeds, varying colors, both palettes, both half-row
  origins where applicable, all ten offsets including zero, and margins.
- Before LCD startup, production scalar and SIMD output is compared for
  all 16 benchmark phases over eight frames each.
- During live LCD operation, task-side q0..q5, CPENABLE and SAR_BYTE canaries
  are checked across actual ISR preemption in every SIMD measurement window.

Production comparisons passed for 3,840 / 6,144 / 5,120 bands at 8x16 /
10x20 / 12x24, with no scalar fallbacks. All 96 live SIMD windows also
reported zero fallbacks. The existing IRAM symbol audit passed; the added
assembly kernels and shift dispatcher were separately confirmed in IRAM,
and the mask table in internal DRAM. This checks placement, not every
transitive path's safety during flash-cache suspension.

Copy-only callbacks measured 66.83 / 41.83 / 50.16 us average for the three
band sizes. They repeat a fixed band and do not implement a producer queue;
moving rendering to a task still adds producer work and synchronization.

## Results

### 8x16

| Phase | Scalar avg us | SIMD avg us | Reduction | Scalar max us | SIMD max us |
|---|---:|---:|---:|---:|---:|
| off | 244.66 | 164.24 | 32.9% | 249.68 | 169.27 |
| clear | 249.71 | 169.29 | 32.2% | 254.72 | 174.32 |
| scrim | 252.62 | 172.20 | 31.8% | 257.65 | 177.23 |
| spaces | 240.08 | 159.66 | 33.5% | 245.07 | 164.65 |
| dense | 239.31 | 158.89 | 33.6% | 244.30 | 163.88 |
| bars | 240.08 | 159.66 | 33.5% | 245.07 | 164.65 |
| bold | 240.05 | 159.64 | 33.5% | 245.07 | 164.65 |
| t:sparse | 233.33 | 152.91 | 34.5% | 242.45 | 162.04 |
| t:mix160 | 244.24 | 163.82 | 32.9% | 250.09 | 169.68 |
| t:mix320 | 254.79 | 174.37 | 31.6% | 266.11 | 185.70 |
| t:mix510 | 265.16 | 184.70 | 30.3% | 380.51 | 300.10 |
| t:blank | 231.28 | 150.86 | 34.8% | 237.25 | 156.84 |
| fx:app | 247.64 | 166.06 | 32.9% | 286.23 | 200.25 |
| fx:app+sp | 236.06 | 154.50 | 34.6% | 278.22 | 190.29 |
| fx:noscan | 231.12 | 150.71 | 34.8% | 236.24 | 155.82 |
| fx:none | 230.40 | 149.97 | 34.9% | 235.51 | 155.10 |

### 10x20

| Phase | Scalar avg us | SIMD avg us | Reduction | Scalar max us | SIMD max us |
|---|---:|---:|---:|---:|---:|
| off | 133.30 | 113.86 | 14.6% | 174.35 | 154.97 |
| clear | 135.31 | 115.87 | 14.4% | 178.43 | 158.95 |
| scrim | 136.47 | 117.02 | 14.3% | 180.65 | 161.21 |
| spaces | 131.45 | 112.01 | 14.8% | 170.66 | 151.22 |
| dense | 131.16 | 111.72 | 14.8% | 170.06 | 150.62 |
| bars | 131.46 | 112.03 | 14.8% | 170.66 | 151.22 |
| bold | 131.44 | 112.02 | 14.8% | 170.64 | 151.20 |
| t:sparse | 132.90 | 113.47 | 14.6% | 173.57 | 154.13 |
| t:mix160 | 133.03 | 113.58 | 14.6% | 173.46 | 154.03 |
| t:mix320 | 138.87 | 119.43 | 14.0% | 193.29 | 173.90 |
| t:mix510 | 142.66 | 123.24 | 13.6% | 271.73 | 251.95 |
| t:blank | 132.94 | 113.50 | 14.6% | 174.18 | 154.75 |
| fx:app | 134.72 | 115.06 | 14.6% | 191.56 | 175.53 |
| fx:app+sp | 134.28 | 114.63 | 14.6% | 190.51 | 174.70 |
| fx:noscan | 127.82 | 108.39 | 15.2% | 163.41 | 144.04 |
| fx:none | 127.53 | 108.10 | 15.2% | 162.90 | 143.35 |

### 12x24

| Phase | Scalar avg us | SIMD avg us | Reduction | Scalar max us | SIMD max us |
|---|---:|---:|---:|---:|---:|
| off | 143.97 | 111.68 | 22.4% | 180.63 | 148.35 |
| clear | 145.64 | 113.35 | 22.2% | 183.95 | 151.70 |
| scrim | 146.61 | 114.34 | 22.0% | 185.91 | 153.66 |
| spaces | 142.48 | 110.22 | 22.6% | 177.58 | 145.33 |
| dense | 142.21 | 109.94 | 22.7% | 177.05 | 144.82 |
| bars | 142.48 | 110.19 | 22.7% | 177.58 | 145.33 |
| bold | 142.40 | 110.11 | 22.7% | 177.44 | 145.16 |
| t:sparse | 144.79 | 112.53 | 22.3% | 182.74 | 150.47 |
| t:mix160 | 143.76 | 111.49 | 22.4% | 180.04 | 147.75 |
| t:mix320 | 152.07 | 119.84 | 21.2% | 243.66 | 211.39 |
| t:mix510 | 152.09 | 119.80 | 21.2% | 249.63 | 217.36 |
| t:blank | 145.04 | 112.78 | 22.2% | 183.72 | 151.45 |
| fx:app | 145.41 | 113.03 | 22.3% | 196.20 | 173.15 |
| fx:app+sp | 146.19 | 113.79 | 22.2% | 198.16 | 175.19 |
| fx:noscan | 139.49 | 107.21 | 23.1% | 171.69 | 139.43 |
| fx:none | 139.25 | 106.99 | 23.2% | 171.20 | 138.94 |

## Reproduction and integration

From an activated ESP-IDF environment, select `BENCH_FONT=0`, `1`, or `2`
for 8x16, 10x20, or 12x24:

```sh
idf.py -C tests/render_simd/pipeline-idf -B build/render-pipeline-idf -D IDF_TARGET=esp32s3 -D BENCH_FONT=0 -D BENCH_FUSED=OFF build
```

Device testing uses `app-flash` only, after backing up the installed app.
Never use the full `flash` target for this experiment.

All three test installations used `app-flash`. After the final run, the
original 3 MB application partition was restored byte-for-byte and rebooted.
Its MD5 matched the backup, and storage, NVS and partition-table MD5s matched
their pre-test values. Raw serial logs, installation logs and verification
records remain locally in `build/render-pipeline-idf/`.

Before production integration, guarantee bounce-buffer alignment, retain
fallbacks, remove task-side validation branches from the ISR path, audit all
reachable code/data for cache-disabled operation, and validate under sustained
network/BLE load and nested interrupts. The context canaries provide useful
evidence but are not a complete interrupt-safety proof.

A fused glyph-expansion/shift loop could avoid the extra pass. Grouping cells
into aligned output vectors could also reduce the individual stores in 10x20.
Neither improvement has been measured here. The current prototype establishes
what this smaller change buys before undertaking either rewrite.
