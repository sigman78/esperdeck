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
> **Always state the terminal content mode when quoting a render-ISR
> (interrupt service routine) number** — the `t:*` / `fx:*` phase tags used
> throughout are defined in [`bench-methodology.md`](bench-methodology.md).
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
   (16 MHz pixel clock (pclk) / 820×500 total — the "60 fps" comments in `display_render.c`
   are stale). The PR #23 cycle bench (deleted again within that PR) measured
   ~69.7k cycles per bounce chunk ⇒ the ISR consumes **34–54% of core 0
   permanently** (the spread is whether the bench chunk was 16 or 10 lines —
   re-adding the bench settles it). The ISR shares core 0 with `ssh_read_task`,
   NimBLE, and WiFi; core 1 runs only the near-idle shell task.
   *(2026-08-20: the stale "60 fps" comments were fixed in `render_scan.c:116`
   and `render_fx_pass.c:27`. The ISR moved to core 1 in item #1, and its cost is
   now **65.5% of core 1** — see the re-measurement section.)*
2. **SGR-dense output (btop) is parser-bound.** (SGR: Select Graphic
   Rendition, the color/attribute escape codes.) Host microbench of the untouched
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
  per-byte parser state, 256 B print buffer, and OSC (Operating System Command escape) buffer
  (`termstate.c:683-699`, `termstate.h:94`). Cells, alt cells, dirty array:
  PSRAM. Per-wake PSRAM working set far exceeds the 32 KB dcache; one scroll
  memmove touches 46 KB.
- `vtparse_feed` is 1217 B of IRAM (always-mapped instruction RAM), but at `-Os` its helpers (`append_print`,
  `flush_print`, `do_clear`, emitters) are out-of-lined to **flash** — every
  printable byte calls flash from IRAM. The old "whole parser inlines into
  vtparse_feed" claim is dead. No ISR callers exist.
- `on_print` per printed cell: out-of-line `charset_xlat` call + out-of-line
  `mark_dirty` + ~9 reloads of `tsm_t` fields + a `mull` + 5 narrow PSRAM
  stores.
- The vterm bridge buffer the ISR reads is internal DRAM (data RAM; `vterm.c:113-116`);
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
| 5b | CSI (control-sequence-introducer) param fast path (digits/`;`/`:` = 0x30–0x3B run scan) | folded into pass-3 parse −25%/B | small | **DONE** (pass 3) |
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

## Render ISR headroom study (2026-08-21)

Goal: find the cycles to run the panel at 20 MHz / 48.8 Hz. Found something more
urgent on the way.

### 1. The shipped wobble default overruns the band deadline TODAY

`bench_stress` returns from `app_main()` before `cyberdeck_app_init()`, so
`display_fx_set()` is never called, `g_fx_wobble_lut` stays all-zero, `dx == 0`
and `render_fx_wobble()` exits on its guard. **Every render-ISR figure this
project ever recorded was measured with wobble off** — while the shipped default
is `.wobble = 2`. The `fx:` bench phases (added 2026-08-21) close that hole.

Worst chunk, dense terminal, overlay off, all sources at `-Os`:

| phase | 8x16 (820 us budget) | 10x20 (512.5 us budget) |
|---|---|---|
| `off` | 573 us — 70% | 396 us — 77% |
| **`fx:app`** (shipped) | **896 us — 109% OVER** | **623 us — 122% OVER** |
| `fx:app+sp` (shipped, realistic content) | **841 us — 103% OVER** | **565 us — 110% OVER** |
| `fx:noscan` | 568 us — 69% | 392 us — 76% |
| `fx:none` | 567 us — 69% | 391 us — 76% |

Per-effect peak cost: **wobble +323 us (8x16) / +227 us (10x20)**; scanlines +5/+4 us
(0.9%); bold_pop +1 us (0.13%); glow already `0` by default. So there is no effects
budget to reclaim anywhere except wobble — and wobble is not unused, it ships on.

### 2. The wobble cost is induced by codegen, not fundamental

At `-Os` the in-place shift compiles to **eight instructions per pixel**, moving one
16-bit pixel at a time with both addresses recomputed every iteration:

```
loop   a13, ...
  extui  a8, a14, 0, 16    ; re-narrow i to 16 bits -> blocks strength reduction
  add.n  a8, a8, a8
  add.n  a9, a8, a12
  add.n  a9, a2, a9        ; full address recompute, every pixel
  l16ui  a9, a9, 0         ; ONE pixel
  add.n  a8, a2, a8
  s16i   a9, a8, 0         ; ONE pixel
  addi.n a14, a14, 1
```

That is **6.06 cycles/px** (77,605 cyc / 16 lines / 800 px). For scale the entire
glyph pipeline — bit extraction, XOR mask, colour resolve, margin fill — runs at
**8.15 cycles/px** while writing *two* pixels per 32-bit store. A pure memmove costs
75% of what the whole renderer costs. The edge zero-fill also becomes a `callx8` to
ROM `memset` (cache-safe, but a windowed call per shifted line in ISR context).

### 3. `-Os` vs `-O2`: the project default is CORRECT, and `-O2` is much worse

The render core had no optimisation override — the whole of `components/display/`
built at the project's `-Os`, while `font_renderer.c` (the decoder it calls) and
tsm's parser were already promoted to `-O2`. The obvious move is to promote the
renderer too. **Measured, that is a 35% regression.**

8x16, dense, overlay off:

| config | scan.c instrs | `off` max | `t:blank` avg | `fx:app` max | wobble delta |
|---|---|---|---|---|---|
| all `-Os` (shipped) | 973 | 573 us | 104,323 | 896 us | 77,605 cyc |
| all `-O2` | **1350 (+38.7%)** | **766 us (+34%)** | **153,311 (+47%)** | 971 us | 49,255 cyc |
| **`-O2` on `render_fx_pass.c` only** | 973 | **573 us** | 104,321 | **776 us** | **48,800 cyc** |

`t:blank` is the purest scan measurement (no decode) and it degrades **+47%** at
`-O2`. The static instruction count of the hot loop grows **+38.7%**, tracking the
runtime almost 1:1. Cause: `-O2` inlines all the per-size `scan_band_*` variants into
one 3 KB `render_scan_band`, defeating the hand-tuning the code documents
("two load-bearing choices at -Os: columns go in PAIRS with hoisted loads, and the
pixel loops need RENDER_UNROLL").

But `-O2` *does* fix the wobble shift — **−37%** — because that loop is exactly the
naive scalar code `-Os` refuses to unroll or strength-reduce. Hence the split.

**Result of `-O2` on `render_fx_pass.c` alone** (+48 bytes of image, one line of CMake):

| | 8x16 | 10x20 |
|---|---|---|
| `fx:app` before | 896 us — 109% OVER | 623 us — 122% OVER |
| `fx:app` after | **776 us — 94.6% OK** | **530 us — 103.4%, still over** |
| `fx:app+sp` after | 734 us — 90% OK | 472 us — 92% OK |

8x16 is back inside its deadline with no behaviour change at all. 10x20 shrinks its
overrun from 111 us to 18 us, so it still needs a real wobble fix — but a far
smaller one than before, and amplitude reduction alone may cover it.

*Amendment 2026-08-23:* the `-O2` override on `render_fx_pass.c` was **dropped
later in the same PR** (commit `fa27db0`). Once the wobble was folded into the
scan as a destination offset (step 2 below), the fx pass no longer contained
the naive shift loop that needed it, and `components/display/` went back to
the project-default `-Os`. The flag-hygiene finding below stays valid as
method.

**Flag hygiene** (this idiom appends, producing `-Os ... -O2`): verified by
disassembly that the trailing `-O2` is a clean override. Compiling
`render_scan.c` / `render_fx_pass.c` / `render_cache.c` as-configured vs `-O2` alone
gives **identical instruction counts and identical disassembly md5**. The objects
differ by exactly 4 bytes, which is the recorded command-line string (`-Os ` = 4
chars), not code. `set_source_files_properties(... COMPILE_OPTIONS "-O2")` is safe.

### 4. SIMD: real, but only legal in task context

ESP32-S3 has a 128-bit SIMD (single-instruction, multiple-data) unit —
Xtensa **PIE** (Processor Instruction Extensions), the one `esp-dsp` and
`esp_lvgl_port` use. Confirmed against this toolchain: `ee.zero.q`, `ee.movi.32.q`,
`ee.vld.128.ip`, `ee.vst.128.ip`, `ee.andq`, `ee.orq`, `ee.xorq`, `ee.notq`,
`ee.src.q`, `ee.vadds.s16`, `ee.vmul.s16` all assemble with no extra flags.

Our scan is an ideal fit. `px = bg ^ (xf & mask)` becomes four instructions per
**eight** pixels:

```
ee.movi.32.q  q_bg, a_bg, 0..3   ; broadcast, once per cell
ee.movi.32.q  q_xf, a_xf, 0..3
ee.vld.128.ip q_m, a_lut, 0      ; 128-bit mask from a 256-entry LUT[glyph byte]
ee.andq       q_t, q_xf, q_m
ee.xorq       q_t, q_bg, q_t
ee.vst.128.ip q_t, a_dst, 16
```

At **8x16 this is Espressif's ideal alignment case**: 8 px x 2 B = 16 B = exactly one
vector, and the 1600 B line stride keeps every cell 16-byte aligned, so no
misalignment path is needed. 10x20 (20 B/cell) and 12x24 (24 B/cell) would need cell
grouping (4 cells = 5 vectors; 2 cells = 3 vectors) or `wur.sar_byte` + `ee.src.q`.

**The blocker is the ISR, not the ISA.** From `tie.h`: `XCHAL_CP_MASK 0x09` — the S3
has CP0 `"FPU"` (72 B) and **CP3 `"cop_ai"` (208 B save area)**. PIE is CP3, gated by
`CPENABLE`, and FreeRTOS assigns coprocessors lazily per task. `esp_lvgl_port`'s
assembly has **no `CPENABLE` handling and no register save/restore** — it "assumes
the coprocessor is already enabled by the caller", which is true for LVGL because
LVGL renders in a *task*. With `CONFIG_FREERTOS_FPU_IN_ISR` off (the default) the
interrupt entry does not touch `CPENABLE` at all; with it on, entry forces
`CPENABLE = 0` for the ISR's duration. Either way a vector instruction in the bounce
ISR traps or clobbers whichever task owns CP3.

Consequence for our plan: **the prebuild task is the SIMD enabler.** It already moves
work out of ISR context into a core-1 task, which is exactly the context where PIE is
supported with no coprocessor hacks. Taken further — task pre-renders *pixels*, ISR
copies the band (25.6 KB, ~6,400 cycles / 27 us vs 559 us today) — it is the same
shape as ESP-IDF's own `num_fbs>=1` + bounce mode, but with a small band ring in
internal SRAM instead of a 768 KB PSRAM framebuffer, avoiding the PSRAM contention
that pushed this project to `no_fb` in the first place.

Reference (worth reading before attempting it):
`esp-bsp/components/esp_lvgl_port/src/lvgl9/simd/` — 11 hand-written `.S` files
dispatched through `LV_DRAW_SW_ASM_CUSTOM_INCLUDE`, each with three alignment paths
(16 B / 4 B / 1 B), validated bit-exact against a retained ANSI reference and
benchmarked in cycles-per-sample. Reported S3 gains: fill ARGB8888 ~4.9x, image copy
RGB565 ~9.8x — but note both are pure `memset`/`memcpy`, so those ratios do not
transfer to our per-pixel compute directly.

### Ranked plan to 20 MHz

Deadlines fall to 656 / 410 / 492 us at 8x16 / 10x20 / 12x24.

| # | Step | Effect | Status |
|---|---|---|---|
| 1 | `-O2` on `render_fx_pass.c` | 8x16 back under deadline; 10x20 overrun 111 -> 18 us | **done, then retired** — step 2 removed the loop that needed it; the override was dropped again in `fa27db0` |
| 2 | Real wobble fix — fold the displacement into the scan as a destination word offset (needs even-pixel displacement), or cut amplitude | closes the remaining 10x20 overrun | **done — see below** |
| 3 | Per-cell pair LUT in the scan — four `uint32` per cell for glyph bits `00 01 10 11`, built once per cell per row, read *fh* times. Target 7 -> 2-3 cyc/px, ~1.6 KB DRAM | est. −130 us at 10x20 -> ~64% of the 20 MHz deadline | the main lever |
| 4 | ASCII glyph cache (decode is 31% of the worst band) | up to −122 us at 10x20 | insurance |
| 5 | Prebuild task | levels the 23–28% spike at the half-row sizes; **precondition for 6** | promoted |
| 6 | PIE SIMD inside the prebuild task | task context, no coprocessor hacks, 8x16 naturally aligned | after 5 |

Do NOT do: promoting `render_scan.c` / `render_cache.c` / `display_render.c` to `-O2`
(measured 35% worse — see §3), or stripping effects other than wobble (under 1%
combined — see §1).

Note on item #7 in the ranked plan above: it was re-promoted in PR #48 on *duty*
grounds and that still holds, but it does **not** help this problem. A blank-cell
fast path only pays on content that has blanks, and the deadline is set by worst-case
dense content where there are none.

### Wobble fix — render at offset (item 2, done)

Rather than rendering straight and shifting afterwards, the displacement is
handed to the scan as a per-scanline destination WORD offset (`cx->xoff`), so
the pixels land wobbled the first time they are written and the shift pass
disappears. That converts a ~37,000-cycle spike on one band per frame into a
few cycles on every band.

The catch, and why displacement is quantised to even pixels: RGB565 packs two
pixels per 32-bit word, so an ODD displacement is a half-word offset that no
pointer arithmetic can express — it would force the scan to straddle pixel
pairs across cell boundaries. Even displacement is a plain word offset. The
cost is that the wiggle steps in 2 px increments instead of 1, which on an
800 px panel is below the noise floor of a deliberately glitchy effect.

`xoff` is NULL whenever no line in the band moves — the overwhelming
majority: the wiggle spans 16 scanlines, so at 8x16 it touches one band in
thirty. That case keeps its own untouched copy of the scan loop in
`render_scan.inc` rather than a per-scanline `koff` in a shared loop —
folding the offset in measured **+5.9% on the undisplaced path**, because the
extra live values disturb register allocation in a loop tuned down to the
cycle. `tools/scancheck.py` proves offset + clipping bit-identical to
render-then-shift across all geometries.

## Pixel-pair LUT — the scan rewrite (measured 2026-08-21)

The scan's inner loop recomputed `bg ^ (xf & mask)` per pixel per scanline. A
cell has only **two** colours, so a pixel pair has only **four** possible
outcomes — precompute them per cell once per ROW and index with the glyph's two
bits. Per output word: ~10 ALU (arithmetic) ops become one `extui` plus one indexed load.

```c
d[p >> 1] = t0[(b0 >> (W - 2 - p)) & 3];   /* bit 1 = left px, bit 0 = right */
```

| dense, worst chunk | 8x16 [820 us] | 10x20 [512.5 us] |
|---|---|---|
| `off` | 567 → **360 us** (−36.5%) | 391 → **260 us** (−33.4%) |
| `bold` | 581 → 373 us (−35.7%) | 421 → 269 us (−36.1%) |
| `fx:app` | 602 → **404 us** (−32.8%) | 401 → **278 us** (−30.7%) |
| `t:blank` | 441 → 233 us (−47.2%) | 296 → 171 us (−42.3%) |

`t:blank` falls hardest — it is pure scan with no decode, so it shows the win
undiluted. That is the control.

Derived: **10.49 → 6.56 cyc/px**, core-1 duty **65.5% → 40.9%**, fps ceiling
**59.6 → 95.4**. Costs 3.2 KB internal DRAM (`pr[2][100][4]`); D/IRAM 50.8%,
168 KB free. `bg[]`/`xf[]` stay for the underline pass.

### Why it is faster — the assembly (tools/asmdiff.py)

Both revisions compiled with identical flags, `render_scan_band` compared
(`scan_band_8x16` inlines into it when one size is linked):

| | instrs | regs | frame | spill st | spill ld | **pixel st** | useful ld |
|---|---|---|---|---|---|---|---|
| before (`scan_gpair`) | 2142 | 18 | 112 B | 108 | 217 | **105** | 43 |
| after (pair LUT) | **1226** | 17 | **80 B** | 56 | 112 | **105** | 133 |

**Pixel stores are identical (105 -> 105)** — same output, so this is pure
efficiency, not less work.

Per pixel pair (one 32-bit store), the emitted code goes 11 instructions -> 4:

```
BEFORE                              AFTER
  extui a13, a12, 4, 1                extui  a3, a11, 4, 2   ; BOTH bits, one index
  extui a15, a12, 5, 1                addx4  a3, a3, a14     ; LUT base + idx*4
  neg   a13, a13                      l32i.n a3, a3, 0       ; precomputed pair
  neg   a15, a15                      s32i.n a3, a8, 4
  and   a13, a14, a13
  and   a15, a14, a15
  xor   a13, a11, a13
  xor   a15, a11, a15
  slli  a13, a13, 16
  or    a13, a13, a15
  s32i.n a13, a10, 4
```

Two causes, and they compound:

1. **Instruction count in the emit: 11 -> 4 per pair (2.75x).** Extracting the
   two glyph bits as a single 2-bit index instead of two 1-bit masks removes
   the whole `neg`/`and`/`xor`/`slli`/`or` chain. Opcode deltas across the
   function: `and` -192, `xor` -192, `neg` -186, `or` -96, `slli` -82, against
   `addx4` +96 for the indexing.
2. **Register pressure — the live set per cell HALVES.** The paired-column
   loop had to keep `bg0, xf0, bg1, xf1` live at once; now it keeps two LUT
   base pointers `t0, t1`. Two fewer live values per cell pair is exactly what
   was spilling: frame 112 -> 80 B, stack traffic 325 -> 168 accesses (-48%).

The second effect is why the LUT's loads are free: **+90 useful loads replaced
-157 spill accesses**, a net -67 memory operations. The LUT reads land in the
slots the spill reloads vacated.

Instruction count -42.8% predicts the measurement well: `t:blank` (pure scan,
no decode) came in at -47.2%, slightly better than the count alone, because a
disproportionate share of what was removed was stack traffic rather than ALU.

### Glyph cache — decode removed (2026-08-21)

Same rule as the LUT applied to the decoder: 3000 decodes per frame over a
distinct set far smaller than the font. Terminus ships ~1470 glyphs (70 KB at
12x24 if cached outright), so the cache is **bounded and associative**:
256 entries, 2-way, round-robin replacement, keyed on `(bold, cp)`.

| dense, worst chunk | 8x16 | 10x20 |
|---|---|---|
| `off` (ASCII) | 360 → **246 us** (−31.7%) | 260 → **171 us** (−34.2%) |
| `t:mix160` (text-UI repertoire) | 370 → **250 us** (−32.5%) | — → 172 us |
| `t:blank` | 233 → 233 us (0.0%) | 171 → 171 us (0.0%) |

An ASCII-only cache was tried first and rejected: it left every box-drawing and
accented cell decoding, costing **+51%** on a TUI-shaped screen (370 vs 245 us).
The associative version brings non-ASCII to **within 4 us of pure ASCII** while
the ASCII path pays only **+0.6%** for the hash and tag compare.

**Sizing** (`t:mixNNN` phases — distinct codepoints vs worst band):

| phase | 8x16 (100x30) | 10x20 (80x24) |
|---|---|---|
| `t:mix160` — 160 distinct, 0.62x cap | 250 us (+1.6%) | 172 us (+0.6%) |
| `t:mix320` — 320 distinct, 1.25x cap | 273 us (+11%) | 197 us (+15%) |
| `t:mix510` — see note | 404 us (+64%) | 266 us (+55%) |

> **`span` is a cap, not the distinct count.** The generator walks
> `r*13 + c`, so the working set is `rows*13 + cols` bounded by span. At 160
> and 320 both grids see the full span and are comparable. At 510 they are
> **not**: 8x16 reaches **477 distinct (1.86x capacity)** while 10x20 reaches
> only **379 (1.48x)**, which is why 8x16 looks so much worse in that row. It
> is a benchmark artifact, not a property of the cache — at 320, where both
> grids see the same 320 glyphs, **10x20 is the worse of the two**.

`tools/cachesim.py` replays the exact access pattern against the cache model
and predicts max misses/row of 97 (8x16) vs 70 (10x20) at `t:mix510`, ratio
1.39 against a measured cycle-delta ratio of 1.65 — the residual being
per-glyph decode variance.

No cliff: an overflowing set costs one decode for the loser, not a cascade,
amortised over 3000 cells. Even the deepest case measured (477 distinct — a
font chart, not a terminal) sits at 65% of the 20 MHz deadline. 256 entries is
the right size.

Why not true LRU: it needs a recency write on every hit, 3000 ISR-context
stores per frame, to improve a case already under 1%. Round-robin advances a
per-set bit only on fill, so hits stay read-only.

Costs 5.1 / 11.3 / 13.3 KB internal DRAM by size; allocation failure degrades
to decoding per frame.

### Where that leaves pclk

Band utilisation with the shipped fx config, after the whole 2026-08-21 arc:

| pclk | refresh | 8x16 band | 10x20 band | verdict |
|---|---|---|---|---|
| 16 MHz | 39.0 Hz | 36% | 36% | current |
| **20 MHz** | **48.8 Hz** | **45%** | **46%** | **verified on hardware** |
| 26.67 MHz | 65.0 Hz | 60% | 61% | plausible, untested |
| 32 MHz | 78.0 Hz | 72% | 73% | plausible, untested |
| 40 MHz | 97.6 Hz | 90% | 91% | at the peripheral limit |

(worst phase, `fx:app`, after the LUT and the glyph cache)

At the start of this branch those first two rows read 109%/122% and were both
over the deadline. **20 MHz is now a config change, not a project.**

Remaining levers, in order: ASCII glyph cache (decode is still ~31% of the
worst band), prebuild task (levels the 23–28% avg/max spread at the half-row
sizes), then PIE SIMD inside that task.

## ESP32-S3 memory-access rules (measured 2026-08-21)

> The practical distillation of everything below — how to write and validate
> a tight data loop on this part — is **`docs/tight-loops.md`**. This section
> keeps the raw measurements behind it.


On-device microbenchmark (`membench` in `bench_stress.c`), internal SRAM,
1600 B, min of 8 runs. **Uses `volatile`, so it measures raw issue cost, not
what optimised C achieves** — a real `-O2` byte loop beats 5 cyc/byte. Treat
the table as RELATIVE guidance, not an absolute model.

| fill | cyc/byte | | copy | cyc/byte |
|---|---|---|---|---|
| `u8` | 5.00 | | `u8` | 9.00 |
| `u16` | 2.50 | | `u16` | 4.50 |
| `u32` @16B | 1.25 | | `u32` @16B | 2.00 |
| `u32` @4B | **1.25** | | `u32` ×4 unrolled | 1.81 |
| `u32` ×4 unrolled | 1.25 | | `u32` src+2 (misaligned) | **2.31** |
| ROM `memset` | **0.33** | | `u32` funnel (shift+or) | **3.24** |
| | | | ROM `memcpy` | **0.64** |

Hardware facts behind it (`core-isa.h`, `tie.h` for esp32s3):

- `XCHAL_DATA_WIDTH 16` — the load/store unit is 16 bytes wide.
- `XCHAL_UNALIGNED_LOAD_HW 1`, `..._STORE_HW 1`, and both
  `..._EXCEPTION 0` — misaligned access works in hardware, no trap.
- `XCHAL_DCACHE_LINESIZE 16`; internal SRAM is **not** cached at all (the
  32 KB data cache is for flash/PSRAM only).
- `XCHAL_HAVE_LOOPS 1` — zero-overhead `loop`, which GCC does emit.

### The rules

1. **Cost is per INSTRUCTION, not per byte.** Every width lands at ~5 cyc per
   access. So widening pays only where it REDUCES the instruction count — bulk
   moves and fills. Where the halves are needed separately it loses: two `u16`
   loads beat one `u32` load plus an extract. This is why `bg[]`/`xf[]` stay
   as separate arrays rather than an interleaved `u32`.
2. **16-byte alignment buys nothing for scalar code** — `u32` @16B and @4B are
   identical. It matters only for the PIE vector ops (`ee.vld.128` requires
   it) and for the ROM routines' fast path. 4-byte alignment is sufficient
   everywhere the current renderer touches.
3. **Misaligned 32-bit loads are cheap (+15%), funnel shifts are not (+62%).**
   For a half-word-offset copy, a misaligned `u32` load beats
   `(a >> 16) | (b << 16)` by ~29%. Counter-intuitive, and the opposite of the
   usual embedded folklore.
4. **ROM `memset`/`memcpy` are 3–4x any hand-rolled word loop, and they are
   ISR-SAFE.** `nm` resolves them to `0x400011e8` / `0x400011f4` — absolute
   symbols in ROM, never behind the flash cache. **The "memset is off-limits
   with the flash cache disabled" comment in `render_cache.c` is wrong.**
   The break-even is the `callx8` overhead: not worth it for a 16–48 B glyph,
   clearly worth it for the whole-band fills (`fill_black`,
   `render_fx_fill_hidden`, `render_fx_clip_apply` — each 25.6 KB).

### Applied

`zero_fill()` byte loop → word loop, and `smear_glyph()` → SWAR (4 rows per
word at rb==1, 2 at rb==2, masking the bit that would cross a lane). Both in
the decoder, which is ~31% of the worst band.

| dense, worst chunk | 8x16 | 12x24 |
|---|---|---|
| `off` | 567 us (−2.9%) | 400 us (−1.9%) |
| `bold` | 581 us (−3.6%) | 421 us (−5.3%) |
| `t:blank` | unchanged | unchanged |

`t:blank` not moving is the control: blank cells take the zero-fill fast path
in `build_row_cache` and never reach the decoder.

### Rejected: `restrict` on the render path

Adding `__restrict` to the scan's `rows`/`bgv`/`xfv`/`d` and to the decoder's
`pool`/`out` — all genuinely non-aliasing — measured **+2.6% WORSE** at 8x16
(`off` 133,681 → 137,121; `bold` 138,313 → 141,794).

The regression is entirely in the decoder: `t:blank` came out **bit-identical**
(104,427 / 105,853 both runs), and blank cells run the full scan but skip
`font_decode_glyph`. So the scan's `restrict` produced identical code — GCC had
already proven it or could not use it — and the decoder's made things worse,
the same way `-O2` does: more freedom to reorder, worse scheduling and register
pressure on this core.

Same lesson as the `-Os`/`-O2` result above: **on this hot path, giving the
compiler more latitude reliably loses.** The code is tuned around what GCC
actually emits at `-Os`, and aliasing hints are not free wins. Reverted.

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
