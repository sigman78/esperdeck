# VT output speedups — consolidated plan (pass 2)

Status of pass 1 (ingest pacing): items 1–3 of `docs/speedup-render.md` shipped in
PR #14 (drain-until-EAGAIN loop, WIFI_PS_NONE during session, TCP window bump),
verified on hardware. This doc supersedes the remaining backlog of that file:
every item below was re-verified against the current tree on 2026-08-09 by a
three-angle review (parser disassembly + map, render path, host microbench).

> **Reading this document.** Every measurement block is dated and describes the
> tree **as of that date**. Sections are kept as a historical record and are not
> retro-edited — later measurements supersede earlier ones in place-marked notes.
>
> **Always state the terminal content mode when quoting a render-ISR number.**
> ISR cost varies **+28.6%** between a blank screen and a 100%-painted one,
> because blank cells skip the glyph decode. The 2026-08-09/08-10 blocks measured
> **real session content**; the 2026-08-20 block measures both that class and the
> dense synthetic worst case. Comparing across the two looks like a regression and
> is not one — see
> [Render ISR re-measured (2026-08-20)](#render-isr-re-measured-2026-08-20).
> The tsm/parser figures are unaffected and still current.

## Where the cycles actually go

Three regimes, each with a different dominant cost:

1. **Constant tax — the render ISR.** `no_fb` bounce mode redraws every pixel of
   every frame; dirty state never reaches the ISR. The panel runs at **39.0 Hz**
   (16 MHz pclk / 820×500 total — the "60 fps" comments in `display_render.c`
   are stale). The PR #23 cycle bench (deleted again within that PR) measured
   ~69.7k cycles per bounce chunk ⇒ the ISR consumes **34–54% of core 0
   permanently** (the spread is whether the bench chunk was 16 or 10 lines —
   re-adding the bench settles it). The ISR shares core 0 with `ssh_read_task`,
   NimBLE, and WiFi; core 1 runs only the near-idle shell task.
   *(2026-08-20: the stale "60 fps" comments were fixed in `render_scan.c:116`
   and `render_fx_pass.c:27`. The ISR moved to core 1 in item #1, and its cost is
   now **65.5% of core 1** — see the re-measurement section.)*
2. **SGR-dense output (btop) is parser-bound.** Host microbench of the untouched
   tsm sources: `vtparse` is **~81% of `tsm_feed`** on truecolor streams;
   `do_sgr` costs ~6 ns/sequence (micro-opting it is pointless). At `-Os`
   (the build's flag) the parser is ~2.4× slower than `-O2`.
3. **Scroll output (`ls`, logs) is memmove-bound.** `scroll_up()` does one flat
   memmove of 23,200 B PSRAM→PSRAM per scrolled line @100×30
   (`termstate.c:102-114`), then marks the whole region dirty. ~83% of
   `tsm_feed` on scroll streams; an 8 KB drain batch of `ls` output is
   ~100 ms of memmove.

The July "14–15 ms of tsm+copy per 60 KB btop frame" was measured as a core-0
cycle delta and is inflated ~1.5× by render-ISR preemption; pure CPU ≈ 9–10 ms.

Placement facts (map + code):

- The **whole `tsm_t` is PSRAM-first** — including the embedded `vtparse_t`
  per-byte parser state, 256 B print buffer, and OSC buffer
  (`termstate.c:683-699`, `termstate.h:94`). Cells, alt cells, dirty array:
  PSRAM. Per-wake PSRAM working set far exceeds the 32 KB dcache; one scroll
  memmove touches 46 KB.
- `vtparse_feed` is 1217 B of IRAM, but at `-Os` its helpers (`append_print`,
  `flush_print`, `do_clear`, emitters) are out-of-lined to **flash** — every
  printable byte calls flash from IRAM. The old "whole parser inlines into
  vtparse_feed" claim is dead. No ISR callers exist.
- `on_print` per printed cell: out-of-line `charset_xlat` call + out-of-line
  `mark_dirty` + ~9 reloads of `tsm_t` fields + a `mull` + 5 narrow PSRAM
  stores.
- The vterm bridge buffer the ISR reads is internal DRAM (`vterm.c:113-116`);
  fonts are copied to DRAM at boot with flash fallback (`font_renderer.c:109-143`).
- ~~Config drift: `sdkconfig.defaults` sets `CONFIG_LCD_RGB_ISR_IRAM_SAFE=y`,
  current sdkconfig has it off.~~ **Resolved** — both are `=y` as of PR #36; the
  bounce ISR keeps rendering through flash writes, guarded post-build by
  `tools/check_iram.py`.

## Ranked plan

| # | Item | Est. gain | Effort | Status |
|---|------|-----------|--------|--------|
| 0 | Instrumentation (see below) | enables the rest | tiny | this branch |
| 1 | LCD bounce ISR → core 1 | frees 34–54% of core 0 | small | this branch |
| 2 | GROUND fast-path run scanner in `vtparse_feed` | ~5× plain text, −15–25% mixed | small | this branch |
| 3 | Batch `do_print_span` | measured: print −51%/B btop, −82%/B ls | medium | **DONE** (pass 3) |
| 4 | `tsm_t`+dirty → internal DRAM; drop `IRAM_ATTR` from `vtparse_feed` | net +1.3 KB internal free | small | **DONE** (pass 3) |
| 5 | `-O2` on tsm (after #4) | folded into pass-3 parse −25%/B | trivial | **DONE** (pass 3) |
| 5b | CSI-param fast path (digits/`;`/`:` = 0x30–0x3B run scan) | folded into pass-3 parse −25%/B | small | **DONE** (pass 3) |
| 6 | Row-pointer ring for scroll | measured: 40× cheaper per scrolled line | high | **DONE** (feature/scroll-ring) |
| 7 | Blank-cell fast path in ISR band loop | ISR duty −~40% | small | **open — re-promoted 2026-08-20** |
| 8 | memcpy per dirty span; flush rate-limit to ~39 Hz; BEL memchr | few % each | tiny | open |

### 0. Instrumentation

- `vterm_bench_report()` (`vterm.c`) has **zero callers** — counters accumulate
  and never print. Call it (then reset) from the 30 s stat block in
  `ssh_read_task` (`ssh_client.c:333-337`).
- Add a parse-vs-state split under `CONFIG_VTERM_BENCH`: cycle accumulators in
  the termstate vtable shims (`on_print`/`on_csi`/`on_c0`, `termstate.c:653-661`);
  `tsm_cycles − sum(shims)` = pure vtparse.
- Re-add the PR #23 render-ISR cycle bench (deleted in that PR's second commit,
  recoverable from git) behind a Kconfig, reported through the same stat block.
  ~20 cycles/chunk overhead.
- Count scroll_up calls + rows moved to confirm memmove volume in real sessions.

### 1. LCD bounce ISR → core 1

`esp_lcd_new_rgb_panel` allocates the LCD interrupt on the calling core;
`display_init()` currently runs from `app_main` (core 0, `main.c:179`). Run the
panel bring-up inside a short-lived task pinned to core 1 so the ISR lands
there. Core 1 hosts only the shell task (prio 5, mostly idle in-session).
Internal SRAM is uncached on the S3, so all ISR-read state (cell buffer,
overlay double buffer, FX statics) is cross-core coherent as-is. Risks: any
future esp_lcd call that reallocates the interrupt must come from core 1;
ISR now genuinely concurrent with `vterm_flush` writes (same benign tearing
class as today's preemption).

Rejected alternative: pinning `ssh_read_task` to core 1 instead — the ssh
send path's locking was designed around the current core split.

### 2. GROUND fast-path run scanner

In `vtparse_feed`: when the machine is in GROUND with no pending UTF-8
continuation, scan ahead over `0x20..0x7E` and bulk-append the run to the print
buffer, flushing on capacity as usual. Strictly exclude DEL (0x7F) and anything
≥0x80 (UTF-8 leads/C1). Replaces ~25–40 cycles/byte (switch dispatch +
IRAM→flash `append_print` call + PSRAM `print_len` load/store) with ~3–6.
Verified by the tsm host test suite (47 vtparse + 78 termstate) plus new
run-boundary tests.

### 3. Batch `do_print_span` (worse than the old writeup: `charset_xlat` is a
second per-cell out-of-line call). The active charset cannot change mid-span
(SO/SI and ESC designations flush the print buffer first), so hoist the
`CHARSET_DEC_GFX` test and all `tsm_t` fields once per span; write in row
segments `min(count, cols-cx)` with one `mark_dirty` per segment; compose each
cell as two u32 stores. Zero-DRAM variant exists (task stack is PSRAM — no
stack buffers). Keep the slow path for `mode.irm`.

### 4. Split the `tsm_new` allocation: struct + dirty array internal-first,
grids stay PSRAM. Drop `IRAM_ATTR` from `vtparse_feed` (no ISR callers; buys
nothing today since the hot loop calls flash anyway) — returns 1217 B to the
shared SRAM pool and unblocks #5.

### 5. Per-file `-O2` for `vtparse.c`/`termstate.c` via
`set_source_files_properties` after `cyberdeck_component_register()`. Only
after #4, else inlining grows the IRAM function out of the DRAM heap.

### 6. Row-pointer ring: replace the scroll memmove with base-index rotation.
Constraints (all re-verified): `base`+`alt_base` must swap together with the
`cells`/`alt_cells` pointer swap; DECSTBM/IL/DL need a per-row copy fallback
once base≠0; flat `&cells[row*cols]` indexing also lives in ICH/DCH/IRM insert
and `tsm_screen()` returns a flat pointer, so the vterm copy loop and tests
are touched. The 125 passing tsm tests de-risk it.

### 7. In `SCAN_BAND`, test for the NULL glyph (blank cell) before the mask
math and store `bg|bg<<16` words directly. Screens are well over half blank;
bit-identical output; ~150 B IRAM.

### 8. Small: one `memcpy` per dirty span in `refresh_display` (`vterm.c:86-91`;
ISR never reads byte 7; add `offsetof` static asserts; no `esp_async_memcpy`).
Leading-edge rate-limit on `vterm_flush` to the 39 Hz panel — but always let
the ?2026 end-of-sync flush through. Replace the byte-wise BEL scan
(`vterm.c:155`) with `memchr` or a bell callback.

## Measured on hardware (2026-08-09, items 0-2 flashed, BLE kbd connected)

30 s bench windows from a real session (btop → `ls -lR` → idle), ISR on core 1:

- **btop steady state**: 84 KB/s sustained ingest (2.52 MB/30 s); tsm = 1.41 s
  per 30 s (≈4.7% of core 0); split **parse 74.6% / print 15.5% / csi 9.9%**;
  draw (copy layer) = 2.9%. Parser-bound on hw, as the host bench predicted.
  Absolute ns/B (~560) is inflated by WiFi/BLE preemption charged to the
  task-context cycle counters — trust the split, not the absolute.
- **ls scroll flood**: tsm = 7.95 s per 30 s (**26.5% of core 0**), of which
  the c0 bucket (LF → scroll_up) = **87-97%**. 5,325 scrolls, 154,425 rows
  moved = 123.5 MB of PSRAM memmove in 30 s (~17.8 MB/s effective); exactly
  29 rows (23.2 KB) per scroll at 100x30, confirming the model.
- **render ISR** *(still valid — REAL SESSION content)*: avg 112,820 cycles
  per 16-line chunk, 1,170 chunks/s (= 30 × 39.0 fps), **duty 54.9% — now of
  core 1**; max 115-140k vs the 196.8k band budget (57% avg / ~71% peak
  utilization). Per-line cost 7.05k cycles matches PR #23's 6.97k (which was
  10-line chunks) — the 34-vs-54% question is settled: **it was always ~55% of a
  core**. *2026-08-20 note: this is a real-session figure — the dense stress
  screen did not exist yet. It reproduces on HEAD (`t:sparse` = 111,730 / 54.5%),
  so it has NOT regressed. The dense synthetic worst case is 134,245 / 65.5%.*

Measured ranking adjustments:
- **#6 (scroll ring) is the top remaining code win** — scroll memmove is
  ~90% of tsm CPU during scroll workloads, which is where the deck feels slow.
- For btop the residual is the **parse bucket (75%)**: #4 + #5 first, and a
  new candidate — a **CSI-parameter fast path** (btop bytes are ~90% escape
  sequences; the GROUND fast path never touches those). #3 (do_print batch)
  addresses only the ~15% print bucket for btop; bigger for plain text.
- #7 (blank-cell ISR fast path) now buys FX headroom on core 1, not pipeline
  throughput — deprioritized. *(Reversed 2026-08-20: at 65.5% duty this is the
  cheapest lever on the deck's largest fixed cost — see the re-measurement.)*
- Copy layer at 2.9% confirms #8-memcpy as a minor cleanup, as ranked.

Bench instrument notes: cycle accumulator must be u64 (a 30 s idle window is
within 8% of u32 wrap — the first-ever report window overflowed and printed
garbage avg/chunks-per-sec); counters reset at session start so window 1 is
clean. Both fixed.

### Scroll ring measured (2026-08-10, item #6 flashed)

Same ls flood, 30 s windows, before (flat memmove) → after (ring):

| metric | memmove | ring |
|---|---|---|
| tsm CPU | 12.23 s (41% of core 0) | 1.80 s (6%) |
| scroll path (c0 bucket) | 11.81 s | 0.85 s |
| scroll_up calls | 9,114 | 26,381 |
| CPU per scroll | ~1,295 µs | **~32 µs (40×)** |
| rows memmoved | 264,306 (211 MB PSRAM) | **0** |
| bytes ingested | 664 KB | 1.43 MB (2.2×) |

btop regression check: parse 72% / 586 ns/B — unchanged within variance.
ISR duty stable at 54.6% (core 1). During ls the top bucket is now print
(714 ms) → item #3 (do_print batch) is the next scroll-workload lever;
for btop it remains #4/#5 + the CSI-parameter fast path.

### Pass 3 measured (2026-08-10: #3 + #4 + #5 + CSI fast path flashed)

- **btop**: 104 KB/s sustained ingest (highest recorded; 51 pre-drain, 84
  pre-ring). CPU/byte 586 → **426 ns/B (−27%)**: parse −25%/B (CSI fast
  path + -O2 + DRAM parser state), print −51%/B (batching), do_sgr
  unchanged (expected — its cost was always noise).
- **ls**: 34,479 lines/30 s (1,150 lines/s) at **883 ms tsm CPU = 2.9% of
  core 0** (pre-ring: 41%). Print 500 → 91 ns/B (−82%); scroll c0 32 →
  14.7 µs/line. Cumulative across passes 2+3: **a byte of ls output costs
  ~38× less CPU** (18.4 → 0.48 µs/B).
- rows_moved = 0 in every window (ring holding); ISR duty 55–57% on
  core 1; boot heap +1.3 KB internal (IRAM reclaim > tsm_t move).
- The "-O2 did little on hw" prior finding is obsolete: it predates the
  ring + fast paths; with helpers inlined and parser state in DRAM it
  contributes to the −25%/B parse win.

Remaining: #7 (blank-cell ISR fast path — core-1 FX headroom only) and #8
small cleanups. The pipeline is no longer meaningfully tsm-bound in either
regime; next bottlenecks are the render ISR's constant cost and the
network/drain pacing.

## Render ISR re-measured (2026-08-20)

The pass-2/pass-3 blocks above measured the ISR **before** #34 (compressed glyph
tables), #35 (−O2 decoder, committed one-step glyph pipeline, simpler row cache)
and #36 (modular render core). Re-run on current HEAD with the extended
`CYBERDECK_BENCH_STRESS` harness, **all three font sizes**, dense 100%-painted
stress, 240 MHz. Repeatable to **±1–3 cycles** across three cycles per size.

**The three sizes are not interchangeable** — they differ in band geometry, not
just cell count. At 8×16 the band *is* a character row, so every band pays a
row-cache rebuild. At 10×20 and 12×24 the band is **half** a row, so only the
first band of each row pays it, and the cost concentrates into a spike:

| | grid | band | bands/frame | band deadline | avg | max | spread |
|---|---|---|---|---|---|---|---|
| 8×16 | 100×30 | 16 lines (full row) | 30 | 820 µs | 559 µs | 573 µs | **+2.5%** |
| 10×20 | 80×24 | 10 lines (½ row) | 48 | 512.5 µs | 309 µs | 396 µs | **+28.0%** |
| 12×24 | 66×20 | 12 lines (½ row) | 40 | 615 µs | 332 µs | 408 µs | **+23.1%** |

(Band deadline = band lines × 51.25 µs, the conservative scan-out figure. The
render-spike research quotes 534/640 µs for the same bands — that is
frame_time ÷ bands, which amortizes vertical blanking. Both are valid; the
deadline figure is the one that must not be exceeded.)

Cross-validation: the render-spike research (2026-08-15, post-#34) recorded sync
worst cases of **391 µs @10×20** and **410 µs @12×24**; this run measures **396**
and **408**. Those numbers were always current — it is specifically the 8×16
figure in the 2026-08-09/08-10 blocks that went stale.

### There is no baseline drift — ISR cost is content-dependent

An earlier draft of this section claimed a **+19% regression** from 112,820 to
134,245 cycles and attributed it to #34 (compressed glyph tables). **That was
wrong, and it is retracted.** The two figures were measured on *different
workloads*:

- `main/bench_stress.c` — the dense 100%-painted screen — **did not exist** until
  2026-08-15 (added in #35, commit 84e6640). The `CONFIG_DISPLAY_ISR_BENCH`
  per-chunk counter existed from 2026-08-09 (#29), but the only thing to point it
  at was **real session content** (btop → `ls -lR` → idle), which is what the
  2026-08-09/08-10 blocks above measured.
- Blank cells take the zero-fill fast path in `build_row_cache()` instead of a
  glyph decode, so ISR cost varies strongly with how much ink is on screen.

The bench now measures that directly (8×16, overlay off):

| terminal content | avg cycles | µs | max | µs | core-1 duty | fps ceiling |
|---|---|---|---|---|---|---|
| `t:blank` — cleared screen | 104,349 | 434 | 105,643 | 440 | 50.9% | 76.7 |
| `t:sparse` — ~1 glyph in 6 | 111,730 | 465 | 128,116 | 533 | 54.5% | **71.6** |
| `off` — 100% painted, per-cell SGR | 134,245 | 559 | 137,630 | 573 | 65.5% | 59.6 |
| *historic 2026-08-10, real session* | *112,820* | *470* | *115–140k* | — | *54.9%* | *70.9* |

**The historic figure lands within 1% of today's `t:sparse`** — 112,820 vs
111,730, duty 54.9% vs 54.5%, ceiling 70.9 vs 71.6 fps. Same workload class, same
cost. There is **no measurable ISR regression from the font-compression work**;
the entire apparent gap was dense-synthetic vs real content.

Content alone spans **104,349 → 134,245 (+28.6%)**, which is larger than the
"regression" that was claimed. Any future comparison must state its content mode.

### Per-size CPU cost and ceiling

Only the 384,000 **active** pixels are rendered — vertical blanking costs no ISR
time — so the divisor is 800×480, not 820×500.

All rows below are the **dense** worst case — the bound the ISR must survive, not
what a real session costs (see the content table above: a realistic screen is
~54% duty at 8×16, not 65.5%).

| | chunks/s | core-1 duty | peak band util | cycles/px | fps @100% of core 1 |
|---|---|---|---|---|---|
| 8×16 | 1,170 | **65.5%** | 69.9% | 10.49 | **59.6** |
| 10×20 | 1,873 | **58.0%** | **77.3%** | 7.75 | 67.3 |
| 12×24 | 1,561 | **51.8%** | 66.3% | 8.39 | 75.3 |

Two different constraints, and they rank the sizes **oppositely**:

- **Total CPU** — 8×16 is worst (65.5% of core 1), simply because it has the most
  cells (3,000 vs 1,920 / 1,320). It sets the fps ceiling: **59.6 fps**.
- **Band deadline** — 10×20 is worst (77.3%), because its half-row band
  concentrates the rebuild spike into a period only 512.5 µs long.

**Raising pclk to 20 MHz / 48.8 Hz is not viable at any size.** Band periods fall
to 656 / 410 / 492 µs; measured peaks are 573 / 396 / 408, i.e. 87% / **97%** /
83% before any margin — and with bold chrome 10×20 lands at **100.5%**, over the
deadline outright. The prebuild-task prototype (`research/prebuild-task` @
a956418) fixes the *deadline* — it is aimed precisely at the 10×20/12×24 spike —
but not the *budget*: the work it moves still lands on core 1. Raising the
refresh rate now requires cutting cycles/pixel.

### Overlay compositing — first ever measurement

The overlay (`display_set_overlay_buffer`, the shell's chrome layer) had **never
been benchmarked**: `bench_stress.c` fed vterm only and ran with no shell, so
`s_overlay.buf` was NULL for every figure above. The harness now cycles seven
overlay phases, tagged `ov=` in the log line, with a dense terminal underneath in
all of them. Measured at all three sizes.

Average cycles per chunk, and Δ against `off`:

| phase | 8×16 | Δ | 10×20 | Δ | 12×24 | Δ |
|---|---|---|---|---|---|---|
| `off` not registered | 134,245 | — | 74,346 | — | 79,710 | — |
| `clear` all transparent | 135,452 | **+0.90%** | 74,833 | **+0.65%** | 80,111 | **+0.50%** |
| `scrim` transparent + DIM | 135,752 | **+1.12%** | 74,949 | **+0.81%** | 80,209 | **+0.63%** |
| `spaces` opaque U+0020 | 122,622 | **−8.66%** | 68,119 | **−8.38%** | 73,155 | **−8.23%** |
| `dense` opaque glyph soup | 132,147 | −1.56% | 73,679 | −0.90% | 78,409 | −1.63% |
| `bars` opaque + INVERSE ×8 | 122,621 | −8.66% | 68,119 | −8.38% | 73,154 | −8.23% |
| `bold` dense + BOLD | **143,501** | **+6.90%** | **77,562** | **+4.33%** | **84,610** | **+6.15%** |

Worst-case chunk time and band utilization — this is where size matters:

| phase | 8×16 (820 µs) | 10×20 (512.5 µs) | 12×24 (615 µs) |
|---|---|---|---|
| `off` | 573 µs — 69.9% | 396 µs — 77.3% | 408 µs — 66.3% |
| `spaces` / `bars` | 515 µs — 62.8% | 333 µs — 65.0% | 348 µs — 56.6% |
| `dense` | 555 µs — 67.7% | 379 µs — 74.0% | 392 µs — 63.7% |
| `bold` | 601 µs — 73.3% | **412 µs — 80.4%** | 444 µs — 72.2% |

**The deltas are consistent across all three sizes** — every qualitative finding
below holds regardless of font. What changes is the absolute headroom: bold at
10×20 (80.4%) is the tightest configuration measured anywhere on the deck, and
bold at 12×24 has the largest absolute peak penalty (+36 µs over `off`).

1. **A registered overlay is nearly free when transparent** — +0.9%/+1.1%, just
   the per-column `ov_row[]` load stream. A session with a toast or the scrollback
   indicator up pays ~1%.
2. **Opaque overlay cells are *cheaper* than the terminal cells they cover.**
   `resolve_overlay_cell()` has no glow tier and no scrim, and a space decodes
   faster than a dense glyph. A full-screen modal runs 8.7% *below* baseline.
   The prior expectation — that the overlay's missing blank fast path would make
   space-padded chrome expensive — is **wrong**: the comparison that matters is
   against the terminal cell being replaced, not against a terminal space.
3. **INVERSE bar-tint math is free** — `bars` and `spaces` land 1 cycle apart.
4. **BOLD is the only path that exceeds the plain-terminal worst case**, at every
   size. `bold` − `dense` per cell: **~114 cyc @8×16**, ~81 @10×20, ~94 @12×24 —
   the bold range lookup plus synthesize-and-smear. The tightest result on the
   deck is bold @10×20: **412 µs against a 512.5 µs deadline (80.4%)**. Still
   inside budget, but it is the number to watch.
5. The missing blank fast path at `render_cache.c:238` (overlay decodes U+0020
   where the terminal branch word-zero-fills) is real but minor: `dense − spaces`
   = 9,525 cycles/row shows decode *content* dominates the fixed overhead a fast
   path would remove.

**Upshot:** shell chrome is not a render-cost concern. Bold-heavy full-screen
chrome is the one case worth watching.

### Ranking impact

- **#7 (blank-cell fast path in `SCAN_BAND`) should be re-promoted.** It was
  deprioritized at 54.9% duty as "core-1 FX headroom only". At **65.5%** (8×16) it
  is the cheapest lever on the deck's largest fixed cost, and the fps-ceiling
  analysis shows pclk headroom now requires cycles/pixel cuts specifically. Still
  open — `render_scan.inc` has no blank-glyph test.
- **Prebuild task is now the 10×20/12×24 story, not the 8×16 one.** At 8×16 avg
  and max are 2.5% apart — there is no spike left to level, because every band
  rebuilds. At 10×20/12×24 the spread is 23–28%, which is exactly what prebuild
  flattens. Anyone evaluating it should benchmark at 10×20, not the default.
- The overlay branch could take the same treatment (a `cp == 0x20` zero-fill
  mirror of `render_cache.c:245`), but measure first: item 5 above suggests the
  win is small, and an INVERSE space still renders correctly from a zero-filled
  glyph because every pixel takes `bg`.
- Nothing here changes the tsm/parser rankings.

## Rejected ideas (don't revisit without new data)

- **ISR reads tsm's grid directly / pointer swap**: grids are PSRAM (ISR reads
  would fight WiFi + parser for the 32 KB dcache), no snapshot semantics
  mid-scroll, and it breaks ?2026 synchronized update (the withheld copy *is*
  the mechanism). The DRAM double-buffer variant costs 2×24 KB the heap
  doesn't have.
- **Per-cell diffing in the copy layer**: dirty spans already bound the copy
  and the ISR ignores dirtiness anyway.
- **`do_sgr` micro-opts**: ~6 ns/sequence on host — noise.
- **Scroll-offset register in the renderer**: only removes the copy
  amplification, not the tsm memmove that dominates; complicates
  cursor/underline row mapping.
  *(2026-08-20: the first half of this rationale is obsolete — item #6 landed and
  the tsm memmove is gone (rows_moved = 0). What survives is the row-mapping
  complexity. Note this was rejected as a **throughput** idea; a sub-row scanline
  offset for **smooth scrolling** is a separate, UX-motivated proposal and is not
  covered by this rejection.)*

## Measurement recipe

Host microbench: compile untouched `components/tsm/src/{vtparse,termstate,color,charsets}.c`
with `-I components/tsm/include -I components/tsm/src -I idfsim` (same recipe
as `tests/tsm/CMakeLists.txt`), feed 2048 B chunks, flush every 8192 B to
mirror the drain loop. Key host numbers (-O2, 100×30): plain-text redraw
5.7 ns/B through `tsm_feed`; btop-like truecolor 3.2 ns/B (81% in the parser);
scroll lines 10.9 ns/B (83% termstate, half of it the memmove); dirty-row copy
~1.1 ns/cell. On-hw anchor: 60 KB btop frame ≈ 9–10 ms pure CPU (July's
14–15 ms included ISR preemption).
