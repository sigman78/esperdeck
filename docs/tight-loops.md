# Tight data loops on ESP32-S3 — what actually works

Distilled from the render-ISR work (2026-08-20/21). Everything here was measured
on this board, not inferred from documentation — Espressif's performance guide
covers none of it. Numbers are 240 MHz, internal SRAM, `xtensa-esp32s3-elf-gcc`
14.2, project default `-Os`.

The render ISR is the worked example throughout: it regenerates every pixel of
every frame with no framebuffer, so it is the most cost-sensitive code in the
tree. `docs/speedupsall.md` has the full measurement history.

---

## 1. The hardware, verified

| fact | value | where |
|---|---|---|
| Load/store unit width | **16 bytes** | `XCHAL_DATA_WIDTH` |
| Misaligned load/store | **works in HW, no trap** | `XCHAL_UNALIGNED_{LOAD,STORE}_HW = 1`, `..._EXCEPTION = 0` |
| Internal SRAM cache | **none** — direct access | `XCHAL_DCACHE_SIZE 0`; the 32 KB D-cache is flash/PSRAM only |
| Zero-overhead loops | yes, GCC emits them | `XCHAL_HAVE_LOOPS 1` |
| Register window | 16 visible, windowed ABI | `entry` / `retw` |
| SIMD | 128-bit PIE, **coprocessor 3** `"cop_ai"`, 208 B state | `tie.h`, `XCHAL_CP_MASK 0x09` |
| `memset`/`memcpy` | **ROM**, `0x400011e8` / `0x400011f4` | `nm` — type `A`, absolute |

Header is at
`$IDF_PATH/components/xtensa/esp32s3/include/xtensa/config/{core-isa,tie}.h`.

---

## 2. The cost model

Measured by `membench` in `main/bench_stress.c` (1600 B, min of 8):

| fill | cyc/byte | copy | cyc/byte |
|---|---|---|---|
| `u8` | 5.00 | `u8` | 9.00 |
| `u16` | 2.50 | `u16` | 4.50 |
| `u32` @16B | 1.25 | `u32` @16B | 2.00 |
| `u32` @4B | 1.25 | `u32` misaligned (src+2) | 2.31 |
| ROM `memset` | 0.33 | `u32` funnel `(a>>16)|(b<<16)` | 3.24 |
| | | ROM `memcpy` | 0.64 |

> These use `volatile`, so they measure **raw issue cost**, not what optimised C
> achieves — a real `-O2` byte loop beats 5 cyc/byte. Use the table for
> **relative** guidance only.

### The one rule

**Cost is per INSTRUCTION, not per byte.** Every width lands at ~5 cyc per
access. Four consequences:

1. **Widen only where it reduces instruction count** — bulk moves and fills.
   Where you need the halves separately it *loses*: two `u16` loads beat one
   `u32` load plus an extract. (This is why the row cache keeps `bg[]` and
   `xf[]` as separate arrays instead of interleaving them.)
2. **16-byte alignment buys nothing for scalar code.** @16B and @4B measure
   identical. It matters only for PIE vector ops and the ROM routines.
   4-byte alignment is enough for everything the renderer does.
3. **Misaligned 32-bit loads are cheap (+15%); funnel shifts are not (+62%).**
   For a half-word-offset copy, a misaligned load beats shift-and-or by ~29%.
   This is the opposite of the usual embedded folklore.
4. **ROM `memset`/`memcpy` are 3–4× any hand-rolled loop and are ISR-safe** —
   they are absolute ROM symbols, never behind the flash cache. Break-even is
   the `callx8` overhead: not worth it under ~64 B, clearly worth it for
   whole-band fills.

---

## 3. Writing the loop

**Precompute per-N-uses, not per-use.** The single biggest win in this codebase
(−36% band time) was noticing that a cell has only two colours, so a pixel pair
has only four possible values. Build them once per *row*, index them *fh* times.
Before reaching for cleverness, ask: how small is the set of distinct outputs,
and how often is each reused?

**Count live values, not just instructions.** The register window is 16. The old
scan kept `bg0, xf0, bg1, xf1` live per cell pair and spilled; the LUT collapsed
those four into two base pointers and the spills vanished — frame 112 → 80 B,
stack traffic −48%. **A change that halves the live set can pay for the loads it
adds**: the LUT's +90 loads displaced −157 spill accesses, a net −67 memory ops.

**Delete passes, don't optimise them.** The wobble shift went 77,605 → 36,978
cycles by being rewritten well, then → 2,283 by being folded into the scan as a
destination offset so the pass no longer existed. Rewriting bought −52%;
deleting bought −96%.

**Give the common case its own copy of the loop.** Folding a rarely-taken offset
into the shared scan cost **+5.9% on the path that does not use it**, purely from
the extra live values disturbing register allocation. Splitting into two loop
bodies restored the fast path byte-for-byte. Duplication is cheap (~4 KB); a
regression on the 97%-case is not.

**SWAR where the operation has no carry between lanes.** The bold smear is a
per-row shift-or; it became 4 rows per word with a lane mask
(`w |= (w << 1) & 0xFEFEFEFE`). Check the boundary bit direction carefully and
prove it (§5).

---

## 4. What the compiler will and will not do for you

**`-Os` is correct for this hot path, and `-O2` is a 35% regression.**

| `render_scan.c` | instrs | band time |
|---|---|---|
| `-Os` | 973 | 573 µs |
| `-O2` | 1350 (+38.7%) | 766 µs (+34%) |

`-O2` inlined every per-size scan variant into one 3 KB function and blew the
register budget. **But it is per-file, not per-project**: `-O2` on the *naive*
wobble shift loop was −37%, because that loop was exactly the unrolling and
strength reduction `-Os` refuses to do. Decide per file, measure both.

**`restrict` measured −2.6% (worse).** Applied to genuinely non-aliasing
pointers in both the scan and the decoder. The scan's produced *byte-identical
code* (GCC already knew); the decoder's made it slower.

**The pattern:** on code already tuned around what `-Os` emits, giving the
compiler more latitude reliably loses. Treat `-O2`, `restrict` and
`#pragma unroll` as experiments with a measurement attached, never as free wins.

**`-Os` unrolls nothing** — hence `RENDER_UNROLL` (`#pragma GCC unroll 6`) on
constant-trip pixel loops. That pragma is load-bearing; removing it is a
regression.

**Watch for accidental `callx8`.** GCC turns small zero-fill loops into calls to
ROM `memset`. Harmless (ROM is always mapped) but it is a windowed call in ISR
context — check the disassembly if a loop looks unaccountably slow.

---

## 5. Validating — do this before benchmarking

A logic bug does not show up in a cycle count. Every optimisation in this arc was
proved bit-exact against the code it replaced *before* being measured.

**Model it in Python and compare against the reference implementation,
exhaustively where the input space allows.** The scan's glyph rows are 8/10/12
bits — fully exhaustive. Existing models, all runnable standalone:

| tool | proves |
|---|---|
| `tools/lutcheck.py` | pair LUT == `scan_gpair`, 2.1M combinations |
| `tools/scancheck.py` | wobble offset + clipping == render-then-shift, all geometries |
| `tools/smearcheck.py` | SWAR smear == per-row loop, both directions |

**Keep the reference implementation in the tree** even once it is off the hot
path — `scan_gpair()` still lives in `render_scan.c` as the definition of the
pixel packing. Espressif do the same for their SIMD blends.

**Have a control that must NOT change.** `t:blank` (blank screen) exercises the
scan but skips the decoder. When the decoder changed it stayed bit-identical;
when the scan changed it moved hardest. A control that behaves as predicted is
what turns "it got faster" into "it got faster *for the reason I claimed*".

**Diff the assembly.** `tools/asmdiff.py` compiles any two revisions with
identical flags and reports instruction mix, register count, frame size and
spill traffic. Use it to explain a result, and to catch the case where a win came
from accidentally doing less work — pixel-store count should be unchanged.

**Re-run `check_iram.py`.** Any new ISR-path function or table must be in IRAM /
DRAM; the post-build guard enforces it, but new names must match its patterns.

---

## 6. Benchmarking pitfalls that actually bit us

### Two numbers are comparable only if the workload was identical — verify it

This is the mistake that has cost the most time here, twice, in different
disguises. Both times the measurements were correct and the **label** was wrong.

> **1. Different content, same name.** ISR cost varies **+28.6%** between a
> blank screen and a fully-painted one. A dense-synthetic figure was compared
> against a real-session one and read as a **19% regression that did not
> exist**.
>
> **2. Same name, different actual workload.** A cache-sizing sweep labelled
> its rows by the bench's `span` knob. But `span` was a *cap*: the real working
> set was `rows*13 + cols` bounded by it, so at `span=510` the 100x30 grid
> touched **477** distinct glyphs and the 80x24 grid only **379**. The two rows
> being compared were not the same test, which made one font size look
> disproportionately bad.

The practices that would have caught both:

- **Label a run by what the workload WAS, not by the knob you turned.** If a
  parameter is a cap, a seed, a ratio or a target, derive the actual figure and
  put that in the table. `span=510` was honest about the input and silent about
  the experiment.
- **Before comparing two runs, state what is held constant** — content, config,
  geometry, build flags — and check each one. When comparing across font sizes
  or grid geometries, the working set usually is *not* constant.
- **Model the workload offline when you can.** `tools/cachesim.py` replays the
  exact access pattern and prints distinct count and capacity ratio; it exposed
  the 477-vs-379 split in seconds, with no device run. A twenty-line model beats
  a re-measurement.
- **Prefer a control that must not move** (§5) over a cross-run comparison. A
  control is internal to one run, so it cannot be invalidated by a workload
  difference between runs.
- **Distrust a result that is surprisingly lopsided.** Both incidents announced
  themselves as an implausible asymmetry. That is the cue to re-examine the
  setup before the code.

### The rest

**Benchmark the configuration you ship.** `bench_stress` returned before
`cyberdeck_app_init()`, so `display_fx_set()` was never called and the wobble LUT
stayed flat. **Every ISR number ever recorded had wobble silently disabled** —
while the shipped default enables it, and it was overrunning the band deadline.
Check that your harness reaches the real init path.

**Peak and average answer different questions.** The band deadline is a per-band
hard limit; total duty sets the fps ceiling. An optimisation can improve one and
worsen the other — folding the wobble in cut peak 16% while *raising* average
until the fast path was split back out.

**Watch for cross-build measurement traps.** `Copy-Item` preserves mtime, so a
restored `sdkconfig` looked older than the generated config, ninja skipped
regeneration, and a bench image got flashed while reporting success. Verify the
generated `sdkconfig.h`, not just the file you wrote.

**Take min-of-N for microbenchmarks**, and sanity-check repeatability — the band
bench reproduces to ±1–3 cycles across runs, so anything above ~0.1% is real.

---

## 7. If you reach for SIMD

The 128-bit PIE unit exists and the toolchain assembles it with no extra flags
(`ee.vld.128.ip`, `ee.andq`, `ee.xorq`, `ee.movi.32.q`, …). The scan's
`px = bg ^ (xf & mask)` is four instructions per **eight** pixels, and 8×16 is
naturally 16-byte aligned.

**But PIE is coprocessor 3 and the ISR does not own it.** FreeRTOS assigns
coprocessors lazily per task through `CPENABLE`; with `CONFIG_FREERTOS_FPU_IN_ISR`
off (default) interrupt entry does not touch it, so a vector instruction in an
ISR either traps or clobbers whichever task owns CP3. `esp_lvgl_port`'s SIMD
assembly has no save/restore at all — it "assumes the coprocessor is already
enabled by the caller", which is true because **LVGL renders in a task**.

So: SIMD is legal in task context, not in the bounce ISR. The prebuild-task
design is therefore the enabler, not merely a latency measure. Reference
implementation worth reading first:
`esp-bsp/components/esp_lvgl_port/src/lvgl9/simd/` — 11 hand-written `.S` files,
three alignment paths each, validated bit-exact against a retained ANSI
reference.

---

## Checklist

- [ ] Is there a small set of distinct outputs I can precompute and reuse?
- [ ] Does this reduce **instruction count**, or just byte width?
- [ ] How many values are live in the innermost loop? Can I collapse any?
- [ ] Can the pass be **deleted** rather than sped up?
- [ ] Does the common case still get untouched code?
- [ ] Bit-exact model written and run before benchmarking?
- [ ] Is there a control phase that should *not* move?
- [ ] `asmdiff` run — is the useful-work count unchanged?
- [ ] Measured at every font size, with the shipped fx config?
- [ ] For any comparison: was the workload genuinely identical, derived
      rather than assumed?
- [ ] `check_iram.py` still passing?
