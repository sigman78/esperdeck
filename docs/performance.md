# Performance — measurements, decisions, plan

This file is the performance log for the `SSH → vtparse → tsm → vterm →
display` pipeline: dated measurements, decisions, and the ranked plan.
Pass 1 (ingest pacing) shipped in PR #14; its record is the appendix. On
2026-08-09 a three-angle review (parser disassembly + map, render path,
host microbench) re-verified every item against the tree of that date.

> **Reading this log.** Each measurement block is dated and describes
> the tree **as of that date**. Later measurements supersede earlier
> ones only through place-marked notes. (Condensed 2026-08-25: prose
> tightened; every measured number, table, and listing kept verbatim.)
>
> **Always state the terminal content mode when quoting a render-ISR
> (interrupt service routine) number.** The `t:*` / `fx:*` phase tags
> are defined in [`bench-methodology.md`](bench-methodology.md). ISR
> cost varies **+28.6%** between a blank screen and a 100%-painted one,
> because blank cells skip the glyph decode. The 2026-08-09/08-10 blocks
> measured **real session content**; the 2026-08-20 block measures both
> that class and the dense synthetic worst case. Comparing the two looks
> like a regression, but it is not — see
> [Render ISR re-measured (2026-08-20)](#render-isr-re-measured-2026-08-20).

Contents:

- [Where the cycles go — three regimes (as of 2026-08-09)](#where-the-cycles-go--three-regimes-as-of-2026-08-09)
- [Ranked plan](#ranked-plan)
- [Measured 2026-08-09 — items 0–2 flashed](#measured-2026-08-09--items-02-flashed)
- [Render ISR re-measured (2026-08-20)](#render-isr-re-measured-2026-08-20)
- [Render ISR headroom study (2026-08-21)](#render-isr-headroom-study-2026-08-21)
- [Pixel-pair LUT — the scan rewrite (2026-08-21)](#pixel-pair-lut--the-scan-rewrite-2026-08-21)
- [ESP32-S3 memory-access rules (2026-08-21)](#esp32-s3-memory-access-rules-2026-08-21)
- [Rejected ideas (don't revisit without new data)](#rejected-ideas-dont-revisit-without-new-data)
- [Measurement recipe](#measurement-recipe)
- [Appendix: pass 1 — ingest pacing (2026-07-12)](#appendix-pass-1--ingest-pacing-2026-07-12)

## Where the cycles go — three regimes (as of 2026-08-09)

This is the baseline picture the plan was ranked against; the dated
blocks below record how each cost was then removed. Each regime has a
different dominant cost:

1. **Constant tax — the render ISR.** `no_fb` bounce mode redraws every
   pixel of every frame; dirty state never reaches the ISR. The panel
   runs at **39.0 Hz** (16 MHz pixel clock (pclk) / 820×500 total — the
   "60 fps" comments in `display_render.c` were stale). The PR #23 cycle
   bench (deleted again within that PR) measured ~69.7k cycles per
   bounce chunk, so the ISR consumed **34–54% of core 0 permanently**;
   the spread came from whether the bench chunk was 16 or 10 lines.
   *(2026-08-20: the stale "60 fps" comments were fixed in
   `render_scan.c:116` and `render_fx_pass.c:27`. The ISR moved to
   core 1 in item #1, and its dense-content cost is **65.5% of
   core 1** — see the re-measurement.)*
2. **SGR-dense output (btop) is parser-bound.** (SGR: Select Graphic
   Rendition, the color/attribute escape codes.) A host microbench of
   the untouched tsm sources shows `vtparse` at **~81% of `tsm_feed`**
   on truecolor streams. `do_sgr` costs ~6 ns/sequence, so
   micro-optimizing it is pointless. At `-Os` (the build's flag), the
   parser runs ~2.4× slower than at `-O2`.
3. **Scroll output (`ls`, logs) is memmove-bound.** `scroll_up()` does
   one flat memmove of 23,200 B PSRAM→PSRAM per scrolled line @100×30
   (`termstate.c:102-114`), then marks the whole region dirty. This
   memmove is ~83% of `tsm_feed` on scroll streams. An 8 KB drain batch
   of `ls` output costs ~100 ms of memmove.

The July figure of "14–15 ms of tsm+copy per 60 KB btop frame" was
measured as a core-0 cycle delta. Render-ISR preemption inflates it
~1.5×; pure CPU is ≈ 9–10 ms.

Placement facts (map + code):

- **The whole `tsm_t` is PSRAM-first** — including the embedded
  `vtparse_t` per-byte parser state, the 256 B print buffer, and the OSC
  (Operating System Command escape) buffer (`termstate.c:683-699`,
  `termstate.h:94`). Cells, alt cells, and the dirty array all live in
  PSRAM. The per-wake PSRAM working set far exceeds the 32 KB dcache;
  one scroll memmove touches 46 KB.
- **`vtparse_feed` is 1217 B of IRAM** (always-mapped instruction RAM),
  but at `-Os` its helpers (`append_print`, `flush_print`, `do_clear`,
  emitters) are out-of-lined to **flash** — every printable byte calls
  flash from IRAM. The old claim that "the whole parser inlines into
  vtparse_feed" no longer holds. No ISR callers exist.
- **`on_print` per printed cell:** out-of-line `charset_xlat` call +
  out-of-line `mark_dirty` + ~9 reloads of `tsm_t` fields + a `mull` +
  5 narrow PSRAM stores.
- **The vterm bridge buffer the ISR reads is internal DRAM** (data RAM;
  `vterm.c:113-116`). Fonts are copied to DRAM at boot, with flash as a
  fallback (`font_renderer.c:109-143`).
- ~~Config drift: `sdkconfig.defaults` sets
  `CONFIG_LCD_RGB_ISR_IRAM_SAFE=y`, but the current sdkconfig has it
  off.~~ **Resolved** — both are `=y` as of PR #36. The bounce ISR keeps
  rendering through flash writes, guarded post-build by
  `tools/check_iram.py`.

## Ranked plan

| # | Item | Est. gain | Effort | Status |
|---|------|-----------|--------|--------|
| 0 | Instrumentation (see below) | enables the rest | tiny | **DONE** (pass 2) |
| 1 | LCD bounce ISR → core 1 | frees 34–54% of core 0 | small | **DONE** (pass 2) |
| 2 | GROUND fast-path run scanner in `vtparse_feed` | ~5× plain text, −15–25% mixed | small | **DONE** (pass 2) |
| 3 | Batch `do_print_span` | measured: print −51%/B btop, −82%/B ls | medium | **DONE** (pass 3) |
| 4 | `tsm_t`+dirty → internal DRAM; drop `IRAM_ATTR` from `vtparse_feed` | net +1.3 KB internal free | small | **DONE** (pass 3) |
| 5 | `-O2` on tsm (after #4) | folded into pass-3 parse −25%/B | trivial | **DONE** (pass 3) |
| 5b | CSI (control-sequence-introducer) param fast path (digits/`;`/`:` = 0x30–0x3B run scan) | folded into pass-3 parse −25%/B | small | **DONE** (pass 3) |
| 6 | Row-pointer ring for scroll | measured: 40× cheaper per scrolled line | high | **DONE** (feature/scroll-ring) |
| 7 | Blank-cell fast path in ISR band loop | ISR duty −~40% | small | **open — re-promoted 2026-08-20** |
| 8 | memcpy per dirty span; flush rate-limit to ~39 Hz; BEL memchr | few % each | tiny | open |

### 0–6: shipped — what each was, in one breath

The DONE items' design detail now lives in the code; what follows is
the one-line record plus any constraint that outlives the change.

- **0. Instrumentation** — `vterm_bench_report()` wired into the 30 s
  stat block; a parse-vs-state split via cycle accumulators in the
  termstate vtable shims (`tsm_cycles − sum(shims)` = pure vtparse);
  the PR #23 render-ISR cycle bench re-added behind a Kconfig
  (~20 cycles/chunk overhead); scroll_up call/row counters.
- **1. LCD bounce ISR → core 1** — panel bring-up runs in a short-lived
  task pinned to core 1, so `esp_lcd_new_rgb_panel` allocates the
  interrupt there. **Live constraint:** any future esp_lcd call that
  reallocates the interrupt must also come from core 1. The ISR is
  genuinely concurrent with `vterm_flush` writes — the same benign
  tearing class as the old preemption. Rejected alternative: pinning
  `ssh_read_task` to core 1 instead; the ssh send path's locking was
  designed around the current split.
- **2. GROUND fast-path run scanner** — in GROUND with no pending UTF-8
  continuation, bulk-append runs of `0x20..0x7E` (DEL and ≥0x80
  strictly excluded), replacing ~25–40 cycles/byte with ~3–6. Verified
  by the tsm host suite (47 vtparse + 78 termstate) plus run-boundary
  tests.
- **3. Batch `do_print_span`** — hoist `charset_xlat`/`CHARSET_DEC_GFX`
  and the `tsm_t` fields once per span (the charset cannot change
  mid-span; SO/SI and ESC designations flush first); write row
  segments with one `mark_dirty` each; slow path kept for `mode.irm`.
- **4. `tsm_t` + dirty array → internal DRAM** — grids stay PSRAM;
  `IRAM_ATTR` dropped from `vtparse_feed` (no ISR callers), returning
  1217 B and unblocking #5.
- **5. `-O2` on the tsm parser** — per-file override via
  `set_source_files_properties`, only possible after #4.
- **6. Row-pointer scroll ring** — base-index rotation instead of the
  memmove. Constraints that shaped it: `base`+`alt_base` swap together
  with the `cells`/`alt_cells` pointer swap; DECSTBM/IL/DL fall back to
  per-row copy once base≠0; flat `&cells[row*cols]` indexing in
  ICH/DCH/IRM and `tsm_screen()` meant the vterm copy loop and tests
  were touched too.

### 7. Blank-cell fast path in the ISR band loop

In `SCAN_BAND`, test for the NULL glyph (blank cell) before the mask and
store `bg|bg<<16` words directly. Screens run well over half blank, so
the fast path triggers often. Output stays bit-identical; the cost is
~150 B IRAM.

### 8. Small cleanups

One `memcpy` per dirty span in `refresh_display` (`vterm.c:86-91`) —
the ISR never reads byte 7; add `offsetof` static asserts, and skip
`esp_async_memcpy`. Rate-limit the leading edge of `vterm_flush` to the
39 Hz panel, but always let the ?2026 end-of-sync flush through.
Replace the byte-wise BEL scan (`vterm.c:155`) with `memchr` or a bell
callback.

## Measured 2026-08-09 — items 0–2 flashed

30 s bench windows from a real session (btop → `ls -lR` → idle), BLE
keyboard connected, ISR on core 1:

- **btop steady state:** 84 KB/s sustained ingest (2.52 MB/30 s). tsm
  CPU is 1.41 s per 30 s (≈4.7% of core 0), split **parse 74.6% / print
  15.5% / csi 9.9%**; draw (copy layer) is 2.9%. Parser-bound on
  hardware, as the host bench predicted. The absolute ns/B figure
  (~560) is inflated by WiFi/BLE preemption charged to the task-context
  cycle counters — trust the split, not the absolute value.
- **ls scroll flood:** tsm CPU is 7.95 s per 30 s (**26.5% of
  core 0**), of which the c0 bucket (LF → scroll_up) is **87-97%**.
  5,325 scrolls moved 154,425 rows, or 123.5 MB of PSRAM memmove in
  30 s (~17.8 MB/s effective). That is exactly 29 rows (23.2 KB) per
  scroll at 100x30, confirming the model.
- **render ISR** *(still valid — REAL SESSION content)*: avg 112,820
  cycles per 16-line chunk, at 1,170 chunks/s (= 30 × 39.0 fps), for a
  **duty of 54.9% — now of core 1**. Max is 115-140k against the 196.8k
  band budget (57% avg / ~71% peak utilization). The per-line cost of
  7.05k cycles matches PR #23's 6.97k (which used 10-line chunks) —
  settling the 34-vs-54% question: **it was always ~55% of a core**.
  *2026-08-20 note: this is a real-session figure — the dense stress
  screen did not exist yet. It reproduces on HEAD (`t:sparse` = 111,730
  / 54.5%), so it has NOT regressed. The dense synthetic worst case is
  134,245 / 65.5%.*

Measured ranking adjustments:

- **#6 (scroll ring) is the top remaining code win.** Scroll memmove is
  ~90% of tsm CPU during scroll workloads — where the deck feels slow.
- **btop's residual is the parse bucket (75%).** Do #4 and #5 first,
  plus a new candidate: a **CSI-parameter fast path** (btop bytes are
  ~90% escape sequences; the GROUND fast path never touches those). #3
  addresses only the ~15% print bucket for btop; it matters more for
  plain text.
- **#7 deprioritized** — at 54.9% duty it buys FX headroom on core 1,
  not pipeline throughput. *(Reversed 2026-08-20: at 65.5% duty it is
  the cheapest lever on the deck's largest fixed cost.)*
- **Copy layer at 2.9%** confirms #8 (memcpy) as a minor cleanup, as
  ranked.

Bench instrument notes: the cycle accumulator must be u64 — a 30 s idle
window comes within 8% of u32 wrap, and the first-ever report window
overflowed and printed garbage avg/chunks-per-sec. Counters now reset at
session start, so window 1 is clean. Both issues are fixed.

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

btop regression check: parse at 72% / 586 ns/B — unchanged within
variance. ISR duty stays stable at 54.6% (core 1). During ls the top
bucket is now print (714 ms), so item #3 (do_print batch) is the next
scroll-workload lever. For btop it remains #4/#5 plus the CSI-parameter
fast path.

### Pass 3 measured (2026-08-10: #3 + #4 + #5 + CSI fast path flashed)

- **btop:** 104 KB/s sustained ingest, the highest recorded (51
  pre-drain, 84 pre-ring). CPU/byte drops 586 → **426 ns/B (−27%)**:
  parse −25%/B (CSI fast path, `-O2`, DRAM parser state), print −51%/B
  (batching), do_sgr unchanged — expected, its cost was always noise.
- **ls:** 34,479 lines/30 s (1,150 lines/s), at **883 ms tsm CPU, or
  2.9% of core 0** (pre-ring: 41%). Print drops 500 → 91 ns/B (−82%);
  scroll c0 drops 32 → 14.7 µs/line. Across passes 2+3 combined, **a
  byte of ls output now costs ~38× less CPU** (18.4 → 0.48 µs/B).
- rows_moved stays 0 in every window (the ring holds). ISR duty runs
  55–57% on core 1. Boot heap gains +1.3 KB internal — the IRAM reclaim
  outweighs the tsm_t move.
- The earlier finding that "-O2 did little on hw" is obsolete: it
  predates the ring and the fast paths. With helpers inlined and parser
  state in DRAM, `-O2` now contributes to the −25%/B parse win.

Remaining after pass 3: #7 and #8. The pipeline is no longer
meaningfully tsm-bound in either regime; the next bottlenecks are the
render ISR's constant cost and network/drain pacing.

## Render ISR re-measured (2026-08-20)

The blocks above measured the ISR **before** #34 (compressed glyph
tables), #35 (−O2 decoder, one-step glyph pipeline, simpler row cache),
and #36 (modular render core). This section re-runs the measurement on
the then-current HEAD with the extended `CYBERDECK_BENCH_STRESS`
harness: **all three font sizes**, dense 100%-painted stress, 240 MHz.
Results repeat to **±1–3 cycles** across three runs per size.

**The three sizes are not interchangeable** — they differ in band
geometry, not just cell count. At 8×16 the band *is* a character row, so
every band pays a row-cache rebuild. At 10×20 and 12×24 the band is
**half** a row, so only the first band of each row pays it, and the cost
concentrates into a spike:

| | grid | band | bands/frame | band deadline | avg | max | spread |
|---|---|---|---|---|---|---|---|
| 8×16 | 100×30 | 16 lines (full row) | 30 | 820 µs | 559 µs | 573 µs | **+2.5%** |
| 10×20 | 80×24 | 10 lines (½ row) | 48 | 512.5 µs | 309 µs | 396 µs | **+28.0%** |
| 12×24 | 66×20 | 12 lines (½ row) | 40 | 615 µs | 332 µs | 408 µs | **+23.1%** |

(Band deadline = band lines × 51.25 µs, the conservative scan-out
figure. The render-spike research quotes 534/640 µs for the same
bands — that is frame_time ÷ bands, which amortizes vertical blanking.
Both are valid; the deadline figure is the one that must not be
exceeded.)

Cross-validation: the render-spike research (2026-08-15, post-#34)
recorded sync worst cases of **391 µs @10×20** and **410 µs @12×24**;
this run measures **396** and **408**. Those numbers were always
current — it is specifically the 8×16 figure in the 2026-08-09/08-10
blocks that went stale.

### No baseline drift — ISR cost is content-dependent

An earlier draft of this section claimed a **+19% regression** from
112,820 to 134,245 cycles and attributed it to #34 (compressed glyph
tables). **That was wrong, and it is retracted.** The two figures were
measured on *different workloads*:

- `main/bench_stress.c` — the dense 100%-painted screen — **did not
  exist** until 2026-08-15 (added in #35, commit 84e6640). The
  `CONFIG_DISPLAY_ISR_BENCH` per-chunk counter existed from 2026-08-09
  (#29), but the only thing to point it at was **real session content**
  (btop → `ls -lR` → idle) — which is what the 2026-08-09/08-10 blocks
  measured.
- Blank cells take the zero-fill fast path in `build_row_cache()`
  instead of a glyph decode, so ISR cost varies strongly with how much
  ink is on screen.

The bench now measures that directly (8×16, overlay off):

| terminal content | avg cycles | µs | max | µs | core-1 duty | fps ceiling |
|---|---|---|---|---|---|---|
| `t:blank` — cleared screen | 104,349 | 434 | 105,643 | 440 | 50.9% | 76.7 |
| `t:sparse` — ~1 glyph in 6 | 111,730 | 465 | 128,116 | 533 | 54.5% | **71.6** |
| `off` — 100% painted, per-cell SGR | 134,245 | 559 | 137,630 | 573 | 65.5% | 59.6 |
| *historic 2026-08-10, real session* | *112,820* | *470* | *115–140k* | — | *54.9%* | *70.9* |

**The historic figure lands within 1% of today's `t:sparse`** — 112,820
vs 111,730, duty 54.9% vs 54.5%, ceiling 70.9 vs 71.6 fps. Same
workload class, same cost. There is **no measurable ISR regression from
the font-compression work**; the entire apparent gap was dense-synthetic
vs real content.

Content alone spans **104,349 → 134,245 (+28.6%)** — larger than the
"regression" that was claimed. Any future comparison must state its
content mode.

### Per-size CPU cost and ceiling

Only the 384,000 **active** pixels are rendered — vertical blanking
costs no ISR time — so the divisor is 800×480, not 820×500. All rows
below are the **dense** worst case, the bound the ISR must survive, not
what a real session costs (a realistic screen is ~54% duty at 8×16, not
65.5%).

| | chunks/s | core-1 duty | peak band util | cycles/px | fps @100% of core 1 |
|---|---|---|---|---|---|
| 8×16 | 1,170 | **65.5%** | 69.9% | 10.49 | **59.6** |
| 10×20 | 1,873 | **58.0%** | **77.3%** | 7.75 | 67.3 |
| 12×24 | 1,561 | **51.8%** | 66.3% | 8.39 | 75.3 |

Two constraints, and they rank the sizes **oppositely**:

- **Total CPU** — 8×16 is worst (65.5% of core 1), simply because it
  has the most cells (3,000 vs 1,920 / 1,320). It sets the fps ceiling:
  **59.6 fps**.
- **Band deadline** — 10×20 is worst (77.3%), because its half-row band
  concentrates the rebuild spike into a period only 512.5 µs long.

**Raising pclk to 20 MHz / 48.8 Hz was not viable at any size** (at
this point in the log — see the 2026-08-21 arc for how that changed).
Band periods fall to 656 / 410 / 492 µs; measured peaks are 573 / 396 /
408, i.e. 87% / **97%** / 83% before any margin. With bold chrome,
10×20 lands at **100.5%**, over the deadline outright. The
prebuild-task prototype (`research/prebuild-task` @ a956418) fixes the
*deadline* — precisely the 10×20/12×24 spike — but not the *budget*:
the work it moves still lands on core 1. Raising the refresh rate
requires cutting cycles/pixel.

### Overlay compositing — first measurement

The overlay (`display_set_overlay_buffer`, the shell's chrome layer)
had never been benchmarked: `bench_stress.c` fed vterm only, so
`s_overlay.buf` was NULL for every figure above. The harness now cycles
seven overlay phases (tagged `ov=`), dense terminal underneath, at all
three sizes.

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

Worst-case chunk time and band utilization — where size matters:

| phase | 8×16 (820 µs) | 10×20 (512.5 µs) | 12×24 (615 µs) |
|---|---|---|---|
| `off` | 573 µs — 69.9% | 396 µs — 77.3% | 408 µs — 66.3% |
| `spaces` / `bars` | 515 µs — 62.8% | 333 µs — 65.0% | 348 µs — 56.6% |
| `dense` | 555 µs — 67.7% | 379 µs — 74.0% | 392 µs — 63.7% |
| `bold` | 601 µs — 73.3% | **412 µs — 80.4%** | 444 µs — 72.2% |

The deltas are consistent across all three sizes — every qualitative
finding below holds regardless of font. What changes is the absolute
headroom: bold at 10×20 (80.4%) is the tightest configuration measured
anywhere on the deck, and bold at 12×24 has the largest absolute peak
penalty (+36 µs over `off`).

1. **A registered overlay is nearly free when transparent** —
   +0.9%/+1.1%, just the per-column `ov_row[]` load stream. A session
   with a toast or the scrollback indicator up pays ~1%.
2. **Opaque overlay cells are *cheaper* than the terminal cells they
   cover.** `resolve_overlay_cell()` has no glow tier and no scrim, and
   a space decodes faster than a dense glyph. A full-screen modal runs
   8.7% *below* baseline. The prior expectation — that the overlay's
   missing blank fast path would make space-padded chrome expensive —
   was **wrong**. The comparison that matters is against the terminal
   cell being replaced, not against a terminal space.
3. **INVERSE bar-tint math is free** — `bars` and `spaces` land 1 cycle
   apart.
4. **BOLD is the only path that exceeds the plain-terminal worst case**,
   at every size. `bold` − `dense` per cell: **~114 cyc @8×16**, ~81
   @10×20, ~94 @12×24 — the bold range lookup plus synthesize-and-smear.
   The tightest result on the deck is bold @10×20: **412 µs against a
   512.5 µs deadline (80.4%)**. Inside budget, but the number to watch.
5. **The missing blank fast path** at `render_cache.c:238` (the overlay
   decodes U+0020 where the terminal branch word-zero-fills) is real but
   minor. `dense − spaces` = 9,525 cycles/row shows that decode
   *content* dominates the fixed overhead a fast path would remove.

**Upshot:** shell chrome is not a render-cost concern. Bold-heavy
full-screen chrome is the one case worth watching.

### Ranking impact

- **#7 (blank-cell fast path in `SCAN_BAND`) re-promoted.** It was
  deprioritized at 54.9% duty as "core-1 FX headroom only". At **65.5%**
  (8×16) it is the cheapest lever on the deck's largest fixed cost, and
  the fps-ceiling analysis shows pclk headroom now requires
  cycles/pixel cuts specifically. Still open — `render_scan.inc` has no
  blank-glyph test.
- **Prebuild task is the 10×20/12×24 story, not the 8×16 one.** At
  8×16, avg and max are 2.5% apart — there is no spike left to level,
  because every band rebuilds. At 10×20/12×24 the spread is 23–28%,
  which is exactly what prebuild flattens. Benchmark it at 10×20, not
  the default.
- **The overlay branch** could take the same treatment (a `cp == 0x20`
  zero-fill mirror of `render_cache.c:245`), but measure first: finding
  5 suggests the win is small, and an INVERSE space still renders
  correctly from a zero-filled glyph, because every pixel takes `bg`.
- Nothing here changes the tsm/parser rankings.

## Render ISR headroom study (2026-08-21)

Goal: find the cycles to run the panel at 20 MHz / 48.8 Hz. Along the
way, something more urgent turned up.

### 1. The shipped wobble default overruns the band deadline

`bench_stress` returns from `app_main()` before `cyberdeck_app_init()`,
so `display_fx_set()` is never called: `g_fx_wobble_lut` stays
all-zero, `dx == 0`, and `render_fx_wobble()` exits on its guard.
**Every render-ISR figure this project ever recorded was measured with
wobble off**, while the shipped default is `.wobble = 2`. The `fx:`
bench phases (added 2026-08-21) close that hole.

Worst chunk, dense terminal, overlay off, all sources at `-Os`:

| phase | 8x16 (820 us budget) | 10x20 (512.5 us budget) |
|---|---|---|
| `off` | 573 us — 70% | 396 us — 77% |
| **`fx:app`** (shipped) | **896 us — 109% OVER** | **623 us — 122% OVER** |
| `fx:app+sp` (shipped, realistic content) | **841 us — 103% OVER** | **565 us — 110% OVER** |
| `fx:noscan` | 568 us — 69% | 392 us — 76% |
| `fx:none` | 567 us — 69% | 391 us — 76% |

Per-effect peak cost: **wobble +323 us (8x16) / +227 us (10x20)**;
scanlines +5/+4 us (0.9%); bold_pop +1 us (0.13%); glow is already `0`
by default. There is no effects budget to reclaim anywhere except
wobble — and wobble is not unused, since it ships on.

### 2. The wobble cost is codegen, not fundamental

At `-Os`, the in-place shift compiles to **eight instructions per
pixel**, moving one 16-bit pixel at a time and recomputing both
addresses every iteration:

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

That is **6.06 cycles/px** (77,605 cyc / 16 lines / 800 px). For scale,
the entire glyph pipeline — bit extraction, XOR mask, colour resolve,
margin fill — runs at **8.15 cycles/px**, while writing *two* pixels
per 32-bit store. A pure memmove costs 75% of what the whole renderer
costs. The edge zero-fill also becomes a `callx8` to ROM `memset` —
cache-safe, but a windowed call per shifted line in ISR context.

### 3. `-Os` vs `-O2` — the project default is correct

The render core had no optimisation override: all of
`components/display/` built at the project's `-Os`, while
`font_renderer.c` (the decoder it calls) and tsm's parser were already
promoted to `-O2`. The obvious move is to promote the renderer too.
**Measured, that is a 35% regression.**

8x16, dense, overlay off:

| config | scan.c instrs | `off` max | `t:blank` avg | `fx:app` max | wobble delta |
|---|---|---|---|---|---|
| all `-Os` (shipped) | 973 | 573 us | 104,323 | 896 us | 77,605 cyc |
| all `-O2` | **1350 (+38.7%)** | **766 us (+34%)** | **153,311 (+47%)** | 971 us | 49,255 cyc |
| **`-O2` on `render_fx_pass.c` only** | 973 | **573 us** | 104,321 | **776 us** | **48,800 cyc** |

`t:blank` is the purest scan measurement (no decode), and it degrades
**+47%** at `-O2`. The hot loop's static instruction count grows
**+38.7%**, tracking the runtime almost 1:1. The cause: `-O2` inlines
all the per-size `scan_band_*` variants into one 3 KB
`render_scan_band`, defeating the hand-tuning the code documents ("two
load-bearing choices at -Os: columns go in PAIRS with hoisted loads,
and the pixel loops need RENDER_UNROLL").

But `-O2` *does* fix the wobble shift — **−37%** — because that loop is
exactly the naive scalar code `-Os` refuses to unroll or
strength-reduce. Hence the split.

**Result of `-O2` on `render_fx_pass.c` alone** (+48 bytes of image,
one line of CMake):

| | 8x16 | 10x20 |
|---|---|---|
| `fx:app` before | 896 us — 109% OVER | 623 us — 122% OVER |
| `fx:app` after | **776 us — 94.6% OK** | **530 us — 103.4%, still over** |
| `fx:app+sp` after | 734 us — 90% OK | 472 us — 92% OK |

8x16 is back inside its deadline with no behaviour change. 10x20
shrinks its overrun from 111 us to 18 us — it still needs a real wobble
fix, but a far smaller one; amplitude reduction alone may cover it.

**Amendment 2026-08-23:** the `-O2` override on `render_fx_pass.c` was
**dropped later in the same PR** (commit `fa27db0`). Once the wobble
was folded into the scan as a destination offset (step 2 below), the fx
pass no longer contained the naive shift loop that needed it, so
`components/display/` went back to the project-default `-Os`. The
flag-hygiene finding below still stands as method.

**Flag hygiene** (this idiom appends, producing `-Os ... -O2`): a
disassembly check confirms the trailing `-O2` is a clean override.
Compiling `render_scan.c` / `render_fx_pass.c` / `render_cache.c`
as-configured vs `-O2` alone gives **identical instruction counts and
identical disassembly md5**; the objects differ by exactly 4 bytes,
which is the recorded command-line string (`-Os ` = 4 chars), not code.
`set_source_files_properties(... COMPILE_OPTIONS "-O2")` is safe.

### 4. SIMD — real, but only legal in task context

ESP32-S3 has a 128-bit SIMD (single-instruction, multiple-data) unit —
Xtensa **PIE** (Processor Instruction Extensions), the one `esp-dsp`
and `esp_lvgl_port` use. Against this toolchain, `ee.zero.q`,
`ee.movi.32.q`, `ee.vld.128.ip`, `ee.vst.128.ip`, `ee.andq`, `ee.orq`,
`ee.xorq`, `ee.notq`, `ee.src.q`, `ee.vadds.s16`, and `ee.vmul.s16`
all assemble with no extra flags.

Our scan is an ideal fit: `px = bg ^ (xf & mask)` becomes four
instructions per **eight** pixels:

```
ee.movi.32.q  q_bg, a_bg, 0..3   ; broadcast, once per cell
ee.movi.32.q  q_xf, a_xf, 0..3
ee.vld.128.ip q_m, a_lut, 0      ; 128-bit mask from a 256-entry LUT[glyph byte]
ee.andq       q_t, q_xf, q_m
ee.xorq       q_t, q_bg, q_t
ee.vst.128.ip q_t, a_dst, 16
```

At **8x16 this is Espressif's ideal alignment case**: 8 px x 2 B =
16 B, exactly one vector, and the 1600 B line stride keeps every cell
16-byte aligned — no misalignment path needed. 10x20 (20 B/cell) and
12x24 (24 B/cell) would need cell grouping (4 cells = 5 vectors;
2 cells = 3 vectors), or `wur.sar_byte` + `ee.src.q`.

**The blocker is the ISR, not the ISA.** From `tie.h`: `XCHAL_CP_MASK
0x09` — the S3 has CP0 `"FPU"` (72 B) and **CP3 `"cop_ai"` (208 B save
area)**. PIE is CP3, gated by `CPENABLE`, and FreeRTOS assigns
coprocessors lazily per task. `esp_lvgl_port`'s assembly has no
`CPENABLE` handling and no register save/restore — it assumes the
caller already enabled the coprocessor, which is true for LVGL because
LVGL renders in a *task*. With `CONFIG_FREERTOS_FPU_IN_ISR` off (the
default), interrupt entry does not touch `CPENABLE` at all; with it on,
entry forces `CPENABLE = 0` for the ISR's duration. Either way, a
vector instruction in the bounce ISR traps or clobbers whichever task
owns CP3.

Consequence: **the prebuild task is the SIMD enabler.** It already
moves work out of ISR context into a core-1 task — exactly the context
where PIE is supported with no coprocessor hacks. Taken further, the
task could pre-render *pixels* while the ISR just copies the band
(25.6 KB, ~6,400 cycles / 27 us vs 559 us today). That is the same
shape as ESP-IDF's own `num_fbs>=1` plus bounce mode, but with a small
band ring in internal SRAM instead of a 768 KB PSRAM framebuffer —
avoiding the PSRAM contention that pushed this project to `no_fb` in
the first place.

Reference (read before attempting it):
`esp-bsp/components/esp_lvgl_port/src/lvgl9/simd/` — 11 hand-written
`.S` files dispatched through `LV_DRAW_SW_ASM_CUSTOM_INCLUDE`, each
with three alignment paths (16 B / 4 B / 1 B), validated bit-exact
against a retained ANSI reference and benchmarked in
cycles-per-sample. Reported S3 gains: fill ARGB8888 ~4.9x, image copy
RGB565 ~9.8x. Both are pure `memset`/`memcpy`, so those ratios do not
transfer directly to per-pixel compute.

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

Do not promote `render_scan.c` / `render_cache.c` / `display_render.c`
to `-O2` — measured 35% worse, see §3. Do not strip effects other than
wobble either — they cost under 1% combined, see §1. Note on ranked-plan
item #7: PR #48 re-promoted it on *duty* grounds, and that holds, but it
does **not** help the deadline problem — a blank-cell fast path only
pays off on content that has blanks, and the deadline is set by
worst-case dense content, where there are none.

### Wobble fix — render at offset (item 2, done)

Rather than rendering straight and shifting afterwards, the fix hands
the displacement to the scan as a per-scanline destination WORD offset
(`cx->xoff`). Pixels land wobbled the first time they are written, so
the shift pass disappears — converting a ~37,000-cycle spike on one
band per frame into a few cycles on every band.

The catch, and why displacement is quantised to even pixels: RGB565
packs two pixels per 32-bit word, so an ODD displacement would be a
half-word offset no pointer arithmetic can express — it would force the
scan to straddle pixel pairs across cell boundaries. An even
displacement is a plain word offset. The cost: the wiggle steps in 2 px
increments instead of 1, which on an 800 px panel is below the noise
floor of a deliberately glitchy effect.

`xoff` is NULL whenever no line in the band moves — the overwhelming
majority of cases. The wiggle spans 16 scanlines, so at 8x16 it touches
one band in thirty. That case keeps its own untouched copy of the scan
loop in `render_scan.inc`, rather than a per-scanline `koff` in a
shared loop: folding the offset in measured **+5.9% on the undisplaced
path**, because the extra live values disturb register allocation in a
loop tuned down to the cycle. `tools/scancheck.py` proves offset plus
clipping is bit-identical to render-then-shift, across all geometries.

## Pixel-pair LUT — the scan rewrite (2026-08-21)

The scan's inner loop recomputed `bg ^ (xf & mask)` per pixel per
scanline. A cell has only **two** colours, so a pixel pair has only
**four** possible outcomes. Precompute them per cell once per ROW, and
index with the glyph's two bits. Per output word, ~10 ALU (arithmetic)
ops become one `extui` plus one indexed load.

```c
d[p >> 1] = t0[(b0 >> (W - 2 - p)) & 3];   /* bit 1 = left px, bit 0 = right */
```

| dense, worst chunk | 8x16 [820 us] | 10x20 [512.5 us] |
|---|---|---|
| `off` | 567 → **360 us** (−36.5%) | 391 → **260 us** (−33.4%) |
| `bold` | 581 → 373 us (−35.7%) | 421 → 269 us (−36.1%) |
| `fx:app` | 602 → **404 us** (−32.8%) | 401 → **278 us** (−30.7%) |
| `t:blank` | 441 → 233 us (−47.2%) | 296 → 171 us (−42.3%) |

`t:blank` falls hardest — pure scan with no decode, showing the win
undiluted. That is the control.

Derived: **10.49 → 6.56 cyc/px**, core-1 duty **65.5% → 40.9%**, fps
ceiling **59.6 → 95.4**. Costs 3.2 KB internal DRAM (`pr[2][100][4]`);
D/IRAM stands at 50.8%, with 168 KB free. `bg[]`/`xf[]` stay for the
underline pass.

### Why it is faster — the assembly (tools/asmdiff.py)

Both revisions compiled with identical flags, `render_scan_band`
compared (`scan_band_8x16` inlines into it when one size is linked):

| | instrs | regs | frame | spill st | spill ld | **pixel st** | useful ld |
|---|---|---|---|---|---|---|---|
| before (`scan_gpair`) | 2142 | 18 | 112 B | 108 | 217 | **105** | 43 |
| after (pair LUT) | **1226** | 17 | **80 B** | 56 | 112 | **105** | 133 |

**Pixel stores are identical (105 -> 105)** — same output, so this is
pure efficiency, not less work. Per pixel pair (one 32-bit store), the
emitted code goes 11 instructions -> 4:

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

1. **Instruction count in the emit: 11 -> 4 per pair (2.75x).**
   Extracting the two glyph bits as a single 2-bit index, instead of
   two 1-bit masks, removes the whole `neg`/`and`/`xor`/`slli`/`or`
   chain. Opcode deltas across the function: `and` -192, `xor` -192,
   `neg` -186, `or` -96, `slli` -82, against `addx4` +96 for the
   indexing.
2. **Register pressure — the live set per cell HALVES.** The
   paired-column loop had to keep `bg0, xf0, bg1, xf1` live at once;
   now it keeps only two LUT base pointers, `t0, t1`. Two fewer live
   values per cell pair is exactly what was spilling: frame drops
   112 -> 80 B, stack traffic drops 325 -> 168 accesses (-48%).

The second effect explains why the LUT's loads are free: **+90 useful
loads replaced -157 spill accesses**, a net -67 memory operations. The
LUT reads land in the slots the spill reloads vacated. The -42.8%
instruction-count drop predicts the measurement well; `t:blank` (pure
scan) came in at -47.2%, slightly better, because a disproportionate
share of what was removed was stack traffic rather than ALU work.

### Glyph cache — decode removed (2026-08-21)

The same rule applies to the decoder as to the LUT: 3000 decodes happen
per frame, over a distinct set far smaller than the font. Terminus
ships ~1470 glyphs (70 KB at 12x24 if cached outright), so the cache
is **bounded and associative**: 256 entries, 2-way, round-robin
replacement, keyed on `(bold, cp)`.

| dense, worst chunk | 8x16 | 10x20 |
|---|---|---|
| `off` (ASCII) | 360 → **246 us** (−31.7%) | 260 → **171 us** (−34.2%) |
| `t:mix160` (text-UI repertoire) | 370 → **250 us** (−32.5%) | — → 172 us |
| `t:blank` | 233 → 233 us (0.0%) | 171 → 171 us (0.0%) |

An ASCII-only cache was tried first and rejected: it left every
box-drawing and accented cell decoding, costing **+51%** on a
TUI-shaped screen (370 vs 245 us). The associative version brings
non-ASCII to **within 4 us of pure ASCII**, while the ASCII path pays
only **+0.6%** for the hash and tag compare.

**Sizing** (`t:mixNNN` phases — distinct codepoints vs worst band):

| phase | 8x16 (100x30) | 10x20 (80x24) |
|---|---|---|
| `t:mix160` — 160 distinct, 0.62x cap | 250 us (+1.6%) | 172 us (+0.6%) |
| `t:mix320` — 320 distinct, 1.25x cap | 273 us (+11%) | 197 us (+15%) |
| `t:mix510` — see note | 404 us (+64%) | 266 us (+55%) |

> **`span` is a cap, not the distinct count.** The generator walks
> `r*13 + c`, so the working set is `rows*13 + cols` bounded by span.
> At 160 and 320, both grids see the full span and are comparable. At
> 510 they are **not**: 8x16 reaches **477 distinct (1.86x capacity)**,
> while 10x20 reaches only **379 (1.48x)** — a benchmark artifact, not
> a property of the cache. At 320, where both grids see the same 320
> glyphs, **10x20 is the worse of the two**.

`tools/cachesim.py` replays the exact access pattern against the cache
model. It predicts max misses/row of 97 (8x16) vs 70 (10x20) at
`t:mix510`, a ratio of 1.39, against a measured cycle-delta ratio of
1.65; the residual is per-glyph decode variance.

There is no cliff: an overflowing set costs one decode for the loser,
not a cascade, amortised over 3000 cells. Even the deepest case
measured (477 distinct — a font chart, not a terminal) sits at 65% of
the 20 MHz deadline. 256 entries is the right size. Why not true LRU:
it needs a recency write on every hit — 3000 ISR-context stores per
frame — to improve a case already under 1%; round-robin advances a
per-set bit only on fill, so hits stay read-only.

Costs 5.1 / 11.3 / 13.3 KB internal DRAM by size. If allocation fails,
it degrades to decoding per frame.

### Where that leaves pclk

Band utilisation with the shipped fx config, after the whole 2026-08-21
arc (worst phase, `fx:app`, after the LUT and the glyph cache):

| pclk | refresh | 8x16 band | 10x20 band | verdict |
|---|---|---|---|---|
| 16 MHz | 39.0 Hz | 36% | 36% | current |
| **20 MHz** | **48.8 Hz** | **45%** | **46%** | **verified on hardware** |
| 26.67 MHz | 65.0 Hz | 60% | 61% | plausible, untested |
| 32 MHz | 78.0 Hz | 72% | 73% | plausible, untested |
| 40 MHz | 97.6 Hz | 90% | 91% | at the peripheral limit |

At the start of this branch those first two rows read 109%/122% and
were both over the deadline. **20 MHz is now a config change, not a
project.** Remaining levers, in order: ASCII glyph cache (decode is
still ~31% of the worst band), prebuild task (levels the 23–28%
avg/max spread at the half-row sizes), then PIE SIMD inside that task.

## ESP32-S3 memory-access rules (2026-08-21)

> The practical distillation — how to write and validate a tight data
> loop on this part — lives in **`docs/tight-loops.md`**. This section
> keeps the raw measurements behind it.

On-device microbenchmark (`membench` in `bench_stress.c`), internal
SRAM, 1600 B, minimum of 8 runs. **It uses `volatile`, so it measures
raw issue cost, not what optimised C achieves** — a real `-O2` byte
loop beats 5 cyc/byte. Treat the table as RELATIVE guidance, not an
absolute model.

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
- `XCHAL_DCACHE_LINESIZE 16`. Internal SRAM is **not** cached at all —
  the 32 KB data cache is for flash/PSRAM only.
- `XCHAL_HAVE_LOOPS 1` — zero-overhead `loop`, which GCC does emit.

### The rules

1. **Cost is per INSTRUCTION, not per byte.** Every width lands at ~5
   cyc per access, so widening pays off only where it REDUCES the
   instruction count — bulk moves and fills. Where the halves are
   needed separately, it loses: two `u16` loads beat one `u32` load
   plus an extract. This is why `bg[]`/`xf[]` stay as separate arrays
   rather than an interleaved `u32`.
2. **16-byte alignment buys nothing for scalar code** — `u32` @16B and
   @4B are identical. It matters only for the PIE vector ops
   (`ee.vld.128` requires it) and the ROM routines' fast path. 4-byte
   alignment is sufficient everywhere the current renderer touches.
3. **Misaligned 32-bit loads are cheap (+15%); funnel shifts are not
   (+62%).** For a half-word-offset copy, a misaligned `u32` load beats
   `(a >> 16) | (b << 16)` by ~29%. Counter-intuitive, and the opposite
   of the usual embedded folklore.
4. **ROM `memset`/`memcpy` run 3–4x faster than any hand-rolled word
   loop, and they are ISR-SAFE.** `nm` resolves them to `0x400011e8` /
   `0x400011f4` — absolute symbols in ROM, never behind the flash
   cache. **The "memset is off-limits with the flash cache disabled"
   comment in `render_cache.c` is wrong.** The break-even is the
   `callx8` overhead: not worth it for a 16–48 B glyph, clearly worth
   it for the whole-band fills (`fill_black`,
   `render_fx_fill_hidden`, `render_fx_clip_apply` — each 25.6 KB).

### Applied

`zero_fill()` moved from a byte loop to a word loop, and
`smear_glyph()` moved to SWAR (4 rows per word at rb==1, 2 at rb==2,
masking the bit that would cross a lane). Both live in the decoder,
which is ~31% of the worst band.

| dense, worst chunk | 8x16 | 12x24 |
|---|---|---|
| `off` | 567 us (−2.9%) | 400 us (−1.9%) |
| `bold` | 581 us (−3.6%) | 421 us (−5.3%) |
| `t:blank` | unchanged | unchanged |

`t:blank` not moving is the control: blank cells take the zero-fill
fast path in `build_row_cache` and never reach the decoder.

### Rejected: `restrict` on the render path

Adding `__restrict` to the scan's `rows`/`bgv`/`xfv`/`d`, and to the
decoder's `pool`/`out` — all genuinely non-aliasing — measured **+2.6%
WORSE** at 8x16 (`off` 133,681 → 137,121; `bold` 138,313 → 141,794).

The regression is entirely in the decoder. `t:blank` came out
**bit-identical** (104,427 / 105,853 both runs), and blank cells run
the full scan but skip `font_decode_glyph`. So the scan's `restrict`
produced identical code — GCC had already proven it, or could not use
it — while the decoder's `restrict` made things worse, the same way
`-O2` does: more freedom to reorder means worse scheduling and register
pressure on this core.

Same lesson as the `-Os`/`-O2` result above: **on this hot path, giving
the compiler more latitude reliably loses.** The code is tuned around
what GCC actually emits at `-Os`, and aliasing hints are not free wins.
Reverted.

## Rejected ideas (don't revisit without new data)

- **ISR reads tsm's grid directly / pointer swap**: grids are PSRAM.
  ISR reads would fight WiFi and the parser for the 32 KB dcache, there
  is no snapshot semantics mid-scroll, and it breaks ?2026 synchronized
  update (the withheld copy *is* the mechanism). The DRAM double-buffer
  variant costs 2×24 KB the heap doesn't have.
- **Per-cell diffing in the copy layer**: dirty spans already bound the
  copy, and the ISR ignores dirtiness anyway.
- **`do_sgr` micro-opts**: ~6 ns/sequence on host — noise.
- **Scroll-offset register in the renderer**: only removes the copy
  amplification, not the tsm memmove that dominates, and it complicates
  cursor/underline row mapping.
  *(2026-08-20: the first half of this rationale is obsolete — item #6
  landed and the tsm memmove is gone (rows_moved = 0). What survives is
  the row-mapping complexity. This was rejected as a **throughput**
  idea; a sub-row scanline offset for **smooth scrolling** is a
  separate, UX-motivated proposal, not covered by this rejection.)*

## Measurement recipe

Host microbench: compile untouched
`components/tsm/src/{vtparse,termstate,color,charsets}.c` with
`-I components/tsm/include -I components/tsm/src -I idfsim` (the same
recipe as `tests/tsm/CMakeLists.txt`). Feed 2048 B chunks, and flush
every 8192 B to mirror the drain loop. Key host numbers (-O2, 100×30):
plain-text redraw runs 5.7 ns/B through `tsm_feed`; btop-like truecolor
runs 3.2 ns/B (81% in the parser); scroll lines run 10.9 ns/B (83%
termstate, half of it the memmove); dirty-row copy runs ~1.1 ns/cell.
On-hw anchor: a 60 KB btop frame takes ≈ 9–10 ms pure CPU (July's
14–15 ms figure included ISR preemption).

---

## Appendix: pass 1 — ingest pacing (2026-07-12)

*Merged from the retired `docs/speedup-render.md` on 2026-08-23. A
multi-agent research pass covered the whole pipeline; every
recommendation was checked adversarially against the actual code,
`sdkconfig`, the linker map, disassembly of the `-Os` build, and the
vendored libssh2 sources. Items 1–3 below shipped in PR #14. The old
pass-1 backlog (items 4–8) is not reproduced here: it was re-verified
and re-ranked into the plan at the top of this file, where most of it
has since shipped (the memcpy-per-dirty-span idea lives on as plan
item 8).*

### Headline: the bottleneck was never tsm

A 60 KB btop frame costs **~14–15 ms of tsm + copy CPU** but **~1.2 s
of wall time**. The gap is pacing, not parsing:

- `ssh_read_task` read at most 512 B per iteration, then always slept
  `vTaskDelay(1)` = 10 ms (`CONFIG_FREERTOS_HZ=100`) after each read.
  This capped ingest at a hard **51.2 KB/s**: 30 / 60 / 100 KB frames
  took 0.6 / 1.2 / 2.0 s, no matter how fast the terminal engine ran.
- Every chunk of at most 512 B also triggered a full
  `refresh_display()` pass — about 118 dirty-copy passes per 60 KB
  frame.
- After the pacing fix, the next limits appear in this order: TCP
  window divided by RTT (round-trip time) (5760 B window ÷ RTT), then
  RTT inflation from WiFi modem power-save, then — far behind —
  libssh2 decrypt (~1–4 MB/s) and tsm parse (~3–8 MB/s).

`CONFIG_VTERM_BENCH=y` was already set; `vterm_bench_report()` shows
`tsm_us + draw_us` as a small fraction of wall time, confirming this on
hardware. The display side is *not* a factor: the render ISR re-renders
every band every frame from the DRAM cell buffer at constant cost.

### Implemented (pass 1, PR #14)

#### 1. Drain-until-EAGAIN read loop, present once per batch

In `ssh_client.c ssh_read_task`, the task used to do one 512 B read
plus a 10 ms sleep per iteration. Now it drains the channel until
EAGAIN or a per-wake budget (8 KB or 5 ms, whichever comes first), then
presents the batch once and yields one tick. Reads go into a static
2 KB PSRAM (external RAM) buffer. (The 8 KB PSRAM task stack also
carries libssh2's transport path; internal DRAM is too scarce for a
bigger stack buffer.)

- **Task-side ceiling:** 51.2 KB/s → **~800 KB/s** (8 KB per 10 ms
  tick).
- **The session mutex is given up and retaken per chunk**, so
  `ssh_client_send` (shell task, core 1) can interleave between chunks.
  The worst-case key-echo lock wait is one chunk (~1–2 ms), not a whole
  batch.
- **The unconditional end-of-wake `vTaskDelay(1)` stays.** IDLE0 must
  run on every wake or the task WDT fires — the old comment was right
  about that; only its claim that "51 KB/s is far above any practical
  limit" was wrong.
- **The byte budget is paired with a time budget** because the read
  task (prio 6, core 0) outright preempts touch (prio 4) and uart
  (prio 5) on the same core. The mutex does not protect them.
- **Verified against the vendored libssh2:** when the outbound
  WINDOW_ADJUST would block, `channel_read` returns EAGAIN with data
  still queued. The adjust packet is staged in stable state and flushes
  on the next wake, so the loop cannot livelock (worst case: one lost
  tick).
- **The keepalive call site is unchanged.** `keepalive.c` sends from a
  stack-local buffer and ignores EAGAIN — a pre-existing wart. Do not
  move it onto a different stack frame.

This required a new vterm API: `vterm_feed()` (parse only) plus
`vterm_flush()` (present dirty rows; a no-op while a DEC ?2026
synchronized update is open). `vterm_write()` keeps its old
feed-then-present behavior.

Presentation coalescing comes free where it matters most: **btop wraps
every frame in `?2026`** unconditionally, and the DECRQM reply that
nvim / vim / tmux probe for is already implemented (`termstate.c`
`CSI ? 2026 $ p`), so those apps present atomically once per frame.
mc/htop (ncurses) never emit ?2026; they still get one present per
8 KB batch instead of one per 512 B.

#### 2. WiFi power-save off during SSH sessions (config-gated)

No `esp_wifi_set_ps()` call existed anywhere, so the IDF default
`WIFI_PS_MIN_MODEM` was active for every session. Receive wakeups are
gated on the AP's DTIM beacon, commonly adding 20–100 ms RTT. After
fix 1, throughput is bound by `TCP_WND / RTT`, so RTT is the multiplier
that matters — and every keystroke echo pays it too.

Gated behind **`CONFIG_SSH_WIFI_PS_NONE`** (default y): `WIFI_PS_NONE`
is set when a session opens, and `WIFI_PS_MIN_MODEM` restored on
`ssh_client_disconnect()`.

One verified caveat explains why this is a config option: with the BLE
keyboard connected, **WiFi/BT coexistence still time-slices the
radio** — per the IDF 5.5.2 docs, WiFi sleeps during BT slices even
under `WIFI_PS_NONE`. The win is real (DTIM-gated sleep removed) but
smaller than the naive 3–10× estimate; it also costs radio power and
may pressure the BLE link. Measure with the keyboard *connected*: ping
the deck during an idle session, before and after.

#### 3. TCP receive window 5760 → 11520, recvmbox 6 → 12

In `sdkconfig.defaults` (plus the local `sdkconfig`):
`LWIP_TCP_WND_DEFAULT` and `LWIP_TCP_SND_BUF_DEFAULT` go from 5760 to
11520 (4×MSS to 8×MSS; MSS = TCP maximum segment size, 1440 B), and
`LWIP_TCP_RECVMBOX_SIZE` goes from 6 to 12 (rule: WND/MSS + 2). Max
in-flight data was 5760 B, capping throughput at `5760/RTT` (~576 KB/s
at 10 ms RTT); 8×MSS doubles that ceiling.

Verified safe for the razor-thin internal heap: with
`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`, queued unread segments live
in SPIRAM-first WiFi RX buffers / lwip pbufs (zero-copy, `L2_TO_L3_COPY`
off) — **not** internal RAM. Going larger (with `LWIP_WND_SCALE`) stays
structurally available if measurements call for it; scale
`DYNAMIC_RX_BUFFER_NUM` and the mbox together with it. Note that
`ESP_WIFI_RX_BA_WIN=3` may cap WiFi-layer aggregation independently.

**Expected end state for items 1–3: a 60 KB btop frame drops from
~1.2 s to ~100–150 ms.** To validate on hardware: run
`vterm_bench_report()` before and after, and wall-clock a btop refresh
and `mc` startup.

### Reference notes

- **Standard fast-terminal architecture** (st, alacritty, foot): drain
  input in large chunks, parse continuously, present at most once per
  refresh interval (st: 8/33 ms min/max latency; alacritty: 64 KB
  batched PTY reads; foot: row-damage plus frame pacing). Item 1
  follows this pattern; an optional 16–33 ms present-timer for
  non-?2026 apps is a natural extension.
- **DEC ?2026 adoption:** btop always; nvim 0.10+/vim/tmux after a
  successful DECRQM probe (already answered by our `termstate.c`);
  mc/htop never, because ncurses has no support for it — no TERM change
  would let them advertise support they don't have.
- **ESP32-S3 octal PSRAM @80 MHz, real-world:** ~57 MB/s sequential
  memcpy PSRAM→internal, ~21 MB/s PSRAM→PSRAM memmove; 32 B cache
  lines, 32 KB 8-way data cache shared by grid traffic and parser
  state.
