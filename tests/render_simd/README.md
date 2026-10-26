# 8x16 SIMD render prototype

Production uses the generalized two-pass kernels in `components/display/`.
The original C scan remains the fallback and reference. Panel dimensions
are driver constants; the new kernels receive stride and counts explicitly.
See [integration notes](../../docs/performance.md#two-pass-simd-integration-2026-10-26).

The [fused-expansion follow-up](FUSED_RESULTS.md) tests whether eliminating
the shift pass pays for changing traversal. This first fused implementation
is slower than the two-pass version; it remains an opt-in experiment.

The latest [all-font and fixed-offset wobble experiment](VARIANTS_RESULTS.md)
adds 10x20/12x24 kernels and nine specialized SIMD scanline shifts. It measures
the full live renderer and checks output against the production scalar scan.

The follow-up [live LCD pipeline benchmark](PIPELINE_RESULTS.md) measures
30-35% average ISR render savings with explicit vector-state preservation
and aligned bounce buffers. The original task-only microbenchmark follows.

The original kernels below are standalone experiments, not linked into the
cyberdeck firmware. No LCD, WiFi,
BLE, or serial-port access is needed to build or run the host tests.

`scan8_s3.S` expands a full 16-line band of up to 100 characters to RGB565.
It consumes the existing decoded glyph layout and pixel-pair colour table.
It visits all rows of a cell together to reuse broadcast background and
foreground-XOR-background vectors. Each eight-pixel row loads one mask,
ANDs, XORs, and stores one vector. Mask table: 4096 bytes, initialized once.

This first version excludes scanline colour variants, clipping, wobble,
underline, cursor and other post-passes. It leaves uncovered columns alone.
See `scan8.h` for alignment and buffer contracts. The assembly is task-only;
do not call it from the LCD bounce interrupt.

## Host correctness

From the repository root, with GCC and Ninja available:

```sh
cmake -S tests/render_simd -B build/render-simd-gcc -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_BUILD_TYPE=Release
cmake --build build/render-simd-gcc
ctest --test-dir build/render-simd-gcc --output-on-failure
```

Tests compare both the scalar pixel-pair baseline and the portable vector
model against direct foreground/background selection. They cover glyph byte
patterns, every RGB565 bit, equal/mixed colours, column counts 0..100,
row/column addressing, untouched margins and guards around the output.
Host tests do not execute or validate the S3 assembly itself.

The scalar baseline preserves paired-column LUT emission and explicit loop
unrolling from `render_scan.inc`. It excludes the same effects and margins
as the SIMD kernel. It is a kernel comparison, not the full production ISR.

## Build the standalone ESP-IDF app

In an activated ESP-IDF 5.5.2 environment, from the repository root:

```sh
idf.py -C tests/render_simd/idf -B build/render-simd-idf -D IDF_TARGET=esp32s3 build
```

This command only builds. Device access requires separate user approval.
The app uses a pinned core-1 task, internal RAM buffers and 240 MHz CPU.
Both kernels reside in IRAM; scalar code uses `-Os`. The app first runs the
same correctness checks against the actual SIMD kernel, then times full
bands in both orders, with warm-up iterations excluded. Interrupts remain
enabled. Min is a warm-kernel floor; mean/max include interference. These
numbers do not establish an LCD interrupt deadline or end-to-end speedup.

## Local validation, 2026-10-26

- GCC host correctness tests pass.
- S3 assembly compiles with Espressif GCC 14.2.0; standalone object is 66
  bytes of code. Disassembly confirms vector loads, AND/XOR and stores,
  a hardware inner loop, and no helper calls or register spills.
- Standalone ESP-IDF 5.5.2 application builds and links successfully.
- On-device correctness tests pass for both the scalar LUT and S3 assembly.

## Hardware result, 2026-10-26

Waveshare ESP32-S3, revision v0.2, 240 MHz, ESP-IDF 5.5.2, GCC 14.2.0,
`-Os`, internal RAM input/output, IRAM kernels, pinned core-1 task.
Each band contains 100 cells x 8 pixels x 16 lines = 12,800 pixels.
Mixed glyph rows, alternating 0xABCD/0x1234 foreground/background colours.
240 timed iterations after 16 warm-ups per measurement:

| Kernel | Min cycles | Mean cycles | Max cycles | Min time |
|---|---:|---:|---:|---:|
| Scalar LUT | 38,666 | 38,670 | 38,790 | 161.11 us |
| S3 SIMD | 19,117 | 19,117 | 19,117 | 79.65 us |
| S3 SIMD repeat | 19,117 | 19,117 | 19,117 | 79.65 us |
| Scalar LUT repeat | 38,666 | 38,669 | 38,774 | 161.11 us |

The isolated kernel is **2.02x faster**, saving **50.6%** of kernel cycles.
This includes the change from scanline-major to cell-major traversal.
It excludes cache construction, LCD DMA/ISR load, effects, render-ahead
copying and synchronization. No display-refresh or full-renderer speedup
has been measured. Both kernels passed the same pixel/guard checks on-device
before timing. Host tests also pass independently.

Installation used `idf.py app-flash` at 0x10000. Storage, NVS and partition
table checksums matched before testing and after app restoration. The original
3 MB app partition was restored byte-for-byte (checksum verified), then the
device was reset to boot it. Local capture and backup artifacts live
under `build/render-simd-idf/`; they are intentionally not committed.

Before integration, measure against the current LUT kernel on hardware,
then account for render-ahead buffers, copying, scheduling, effects, and
the 10x20/12x24 alignment cases. A fast isolated kernel alone is insufficient.

References: [project investigation](../../docs/performance.md),
[Espressif vector assembly examples](https://github.com/espressif/esp-dsp/blob/master/modules/math/add/fixed/dsps_add_s16_aes3.S),
[FAST SIMD article](https://shraiwi.com/blog/simd-fast-esp32s3/).
