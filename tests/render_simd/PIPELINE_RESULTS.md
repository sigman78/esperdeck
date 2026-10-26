# Live LCD render benchmark: 8x16 SIMD

Historical first run. The [follow-up measurements](VARIANTS_RESULTS.md)
cover SIMD wobble and all three font sizes; the fallback limitations below
describe this initial version.

Measured 2026-10-26 on the Waveshare ESP32-S3, 240 MHz, IDF 5.5.2,
Espressif GCC 14.2.0. These are actual LCD bounce callback timings, including
row-cache resolution, glyph lookup/decoding, pixel scan, effects and the
benchmark wrapper. This is not a microkernel extrapolation.

The separate `pipeline-idf` app compiles the production display, font and
vterm sources directly and includes `main/bench_stress.c` for its unchanged
workload definitions. No production source or SDK file was modified for the
experiment. Font decoding and tsm use their usual `-O2`; rendering uses `-Os`.
Octal PSRAM is enabled, while render buffers and font copies use internal RAM.
The LCD uses the existing 16 MHz timing and 16-line bands. No shell, network,
storage initialization or BLE workload is started.

## Results

Both bounce buffers are explicitly 16-byte aligned in this run. Averages
below are means of the two matching runs; maxima are the worse of those
runs. Raw samples, including the initial native-alignment experiment, are
in [pipeline-results.csv](pipeline-results.csv).

| Workload | Scalar avg us | SIMD avg us | Reduction | Scalar max us | SIMD max us |
|---|---:|---:|---:|---:|---:|
| Dense terminal (`off`) | 244.54 | 164.17 | 32.9% | 249.57 | 169.20 |
| Transparent overlay | 249.59 | 169.22 | 32.2% | 254.61 | 174.25 |
| Scrim | 252.51 | 172.13 | 31.8% | 257.54 | 177.16 |
| Opaque spaces | 239.97 | 159.59 | 33.5% | 244.95 | 164.58 |
| Dense overlay | 239.20 | 158.82 | 33.6% | 244.20 | 163.81 |
| Bars | 239.97 | 159.59 | 33.5% | 244.95 | 164.58 |
| Bold overlay | 239.94 | 159.57 | 33.5% | 244.95 | 164.58 |
| Sparse terminal | 233.21 | 152.85 | 34.5% | 242.34 | 161.97 |
| TUI mix160 | 244.12 | 163.75 | 32.9% | 249.94 | 169.57 |
| Cache pressure mix320 | 254.59 | 174.22 | 31.6% | 267.16 | 185.96 |
| Cache pressure mix510 | 264.96 | 184.56 | 30.3% | 380.40 | 300.35 |
| Blank terminal | 231.16 | 150.79 | 34.8% | 237.50 | 156.77 |
| Application effects | 247.56 | 171.83 | 30.6% | 284.84 | 284.87 |
| Effects + sparse | 235.91 | 160.09 | 32.1% | 277.10 | 277.46 |
| No scanline effect | 231.10 | 150.72 | 34.8% | 236.23 | 155.86 |
| No effects | 230.37 | 150.00 | 34.9% | 235.50 | 155.12 |

The dense case saves about 80.4 us per band. At the unchanged ~39 Hz panel
rate (30 bands/frame), that reduces nominal core-1 render duty from about
28.6% to 19.2%, recovering 9.4 percentage points of a core. It does not
change refresh rate or establish SSH/terminal throughput improvements.

Wobbled bands deliberately fall back to the scalar renderer. About 5.8%
of the effects run's bands took that fallback. Thus average time improves,
but the effects worst case does not. Cache misses also remain: mix510's
worst band is still 300 us after removing 80 us of scan cost.

## What integration requires

1. **Guarantee destination alignment.** Native allocations in this test
   aligned only one bounce buffer to 16 bytes. SIMD then handled half the
   bands: dense average 244.5 -> 204.4 us, with no worst-case improvement.
   This is allocation-layout dependent, not a guarantee about every build.
   IDF 5.5.2 derives alignment from GDMA constraints; its deprecated
   `sram_trans_align` field is not used in allocation. The experiment wraps
   the two bounce allocations to request at least 16 bytes. Production
   needs an explicit allocator/driver solution or an unaligned-output path.
2. **Own and preserve vector state explicitly.** `scan8_guarded.S` saves
   CPENABLE, enables CP3, preserves q0..q5, renders, restores the vectors,
   then restores CPENABLE. It uses 144 bytes of stack and a 4 KB mask table.
   It does not modify q6/q7, accumulators or SAR_BYTE. The measured savings
   include preservation and dispatch overhead. This is custom ISR assembly,
   not permission to call ordinary task-oriented SIMD libraries in an ISR.
3. **Expand coverage where needed.** This kernel handles 8x16, including both
   scanline palettes. 10x20 and 12x24 remain scalar. Their 20/24-byte cell
   strides, plus wobble's offsets, require unaligned/grouped output handling.
   Wobble support is necessary to improve the current effects worst case.

The earlier notes that SIMD *requires* moving rendering into a task were
too categorical. Explicit context preservation makes a contained ISR
experiment possible. Before production use, retain the scalar fallback,
audit all reachable code/data for cache-disabled ISR safety, and test
nested interrupts, SIMD-using tasks, all effects/fonts, and sustained load.
The benchmark's task-side validation branches are not a production ISR API.

## Render-ahead alternative

A copy-only callback measured **16,038 avg / 16,058 max cycles**, or
**66.83 / 66.91 us**, copying one 25,600-byte internal-RAM band to the real
DMA bounce buffer. This repeats a fixed band: it measures copy cost only,
not a functioning producer queue.

It suggests ~7.8% core duty in the ISR, but rendering still runs elsewhere.
As a rough estimate, 164.2 us render + 66.8 us copy = 231.0 us of CPU work,
versus 244.5 us scalar today, before queue overhead. That is only about 5.5%
less total work, although the interrupt becomes much shorter. This estimate
has not been validated with a task producer. Two extra band slots cost
51,200 bytes, on top of the existing DMA buffers; ownership, underrun and
frame-state synchronization also need design. This is a larger change for
latency isolation, not the first choice for total CPU savings.

## Validation and reproduction

- Before LCD startup, real scan output was compared with SIMD for all 16
  stress phases: 3,810 SIMD bands and 30 fallback bands, no differences.
- Vector-register/CPENABLE preservation probe passed. In the aligned run,
  task vector canaries survived actual LCD ISR preemption in every SIMD
  measurement window; callback counts confirmed interrupts occurred.
- Measurement order per phase: scalar, SIMD, SIMD, scalar. Each run uses
  three feed+100 ms settle iterations, then twelve measured iterations.
  Feed work makes elapsed windows workload-dependent; averages are cycles
  per callback, not counts divided by an assumed 1.2-second window.
- Effect frame counter is reset at each measured window. No writes to
  storage or configuration are performed by the benchmark app.

In an activated IDF environment, from the repository root:

```sh
idf.py -C tests/render_simd/pipeline-idf -B build/render-pipeline-idf -D IDF_TARGET=esp32s3 build
```

`-D BENCH_ALIGN_LCD=OFF` reproduces native allocation behavior, though exact
addresses depend on heap layout. Device installation is a separate action:
use **app-flash only**, never the full flash target. Preserve and restore the
currently installed app. Local raw logs and protection checks live under
`build/render-pipeline-idf/`.

Both installations used app-flash. Afterward, the original 3 MB app
partition was restored byte-for-byte and rebooted; storage, NVS and
partition-table checksums matched their pre-test values.

## Next experiment: SIMD wobble

Offsets are integer 32-bit words. With aligned scanline bases, they reduce
to four destination alignment classes modulo 16 bytes. A scanline kernel
can stitch neighboring rendered vectors with `EE.SRC.Q` and emit aligned
stores, handling the clipped first/last vectors separately. The offset is
constant across a scanline, so dispatch or shift-register setup belongs
outside the cell loop. The normal undisplaced band kernel can stay intact.

One function per exact offset is possible, but initially a zero-offset path
and one shifted path may suffice: `EE.SRC.Q` takes its byte shift from
SAR_BYTE, set once per scanline. Four specialized variants are worth testing
only if they beat that shared loop. Any such implementation must preserve
SAR_BYTE and any additional vector registers, alongside CPENABLE.

This is a proposed follow-up, not included in the measurements above.
