# Render-bench methodology

This explains how to take a render-ISR (interrupt service routine)
measurement that is comparable to the ones already on record, and what
invalidates one. The numbers themselves live in
[`performance.md`](performance.md) (the results log) and in the decision
tables of [`ARCHITECTURE.md`](ARCHITECTURE.md). The validation discipline
— proving bit-exactness *before* benchmarking — is in
[`tight-loops.md`](tight-loops.md) §5.

## The stress mode

`CONFIG_CYBERDECK_BENCH_STRESS` (default n) is a development tool, not a
user feature. Instead of the app shell, the device boots into a task
that continuously repaints the whole grid through vterm and logs the
`CONFIG_DISPLAY_ISR_BENCH` per-chunk cycle counters every ~5 s — one
measured window per phase, tagged `ph=` in the log line:

```
bench_stress: font=8x16 ph=off       avg=... cyc (... us) max=... cyc (... us) chunks=...
```

Recipe: enable `CYBERDECK_BENCH_STRESS` plus `DISPLAY_ISR_BENCH` (and
`CYBERDECK_BENCH_STRESS_FONT_*` for the size under test), flash, and
capture one full phase cycle from the serial log. A bench build returns
from `app_main()` before `cyberdeck_app_init()`, so none of the shell
runs. **Reflash normal firmware when done** — a deck left in bench mode
boots into the stress screen.

The worst `max` chunk is compared against the chunk period (one
bounce-buffer band): 820 µs at 8x16, 512.5 µs at 10x20, 640 µs at 12x24
with the 16 MHz pixel clock (deadlines shrink at 20 MHz — see the
headroom study in `performance.md`).

## Three independent axes

The phase schedule (currently 16 phases — the table in `bench_stress.c`
is authoritative) sweeps three axes one at a time. Everything except the
`fx:` phases runs with wobble forced off, which reproduces the historic
runs.

### 1. Overlay (`off` … `bold`)

`build_row_cache()` composites the shell's overlay chrome ABOVE the
terminal cell buffer. Either way that's one glyph decode per column, but
the two layers do not cost the same per cell, and only the overlay-off
path had ever been measured before these phases existed. Each phase
isolates one branch of the overlay resolve in `render_cache.c`:

| phase | overlay contents | isolates |
|---|---|---|
| `off` | not registered | the published baseline: two predicted null checks per column |
| `clear` | every cell transparent (what `ui_clear()` produces) | the per-column `ov_row[]` load stream, nothing more |
| `scrim` | every cell transparent, registered with the frame scrim flag set — the modal backdrop (`ui_dim()`) | `clear`, plus the scrim branch of `resolve_terminal_cell()` |
| `spaces` | fully opaque U+0020 | the overlay branch has NO blank fast path, so every cell pays a full `font_decode_glyph()` where the identical space in the terminal layer is a word zero-fill — what a space-padded `ui_printf()` actually costs |
| `dense` | fully opaque glyph soup | worst-case chrome, max decode rate |
| `bars` | opaque spaces cycling all 8 palette entries | style variety. Since the style palette (docs/overlay-style.md) every entry costs the same single table load, so this phase guards that: any delta vs `spaces` beyond noise is a resolve regression, not a "bar premium" |
| `bold` | dense + BOLD | the bold lookup / synthesize-and-smear path |

The terminal underneath is kept dense in every overlay phase, so the
transparent phases measure real compositing rather than a blank screen.

### 2. Terminal content (`t:*`)

Blank cells take the zero-fill fast path in `build_row_cache()` instead
of a glyph decode, which makes ISR cost strongly content-dependent (~29%
swing). **Quoting a render-ISR number without its content mode is
meaningless.**

- **dense** (the default under the other axes) — every cell is a
  non-blank glyph with a per-cell SGR (Select Graphic Rendition, i.e.
  color/attribute) change and 25% bold. This is strictly denser than any
  real terminal screen; its max is a worst-case chunk time.
- **`t:sparse`** — about 1 glyph in 6, no SGR churn: a shell screen, not
  a painted one.
- **`t:mixNNN`** — a TUI-shaped repertoire of up to NNN distinct
  codepoints (ASCII, box drawing, blocks, Latin-1, Greek, Cyrillic — what
  mc/btop/tmux put on screen), used to size the glyph cache. NNN is a
  *cap*, not the distinct count — see the sizing note in
  `performance.md` before comparing `t:mix510` across grid sizes.
- **`t:blank`** — cleared screen: pure scan, no decode. This is the
  control that must not move when only the decoder changes.

`t:sparse` and `t:blank` bracket the content-dependence range.

**Historical caveat.** The pre-2026-08-15 figures in `performance.md`
were taken on REAL SESSION content (btop / `ls -lR` / idle), because the
stress screen did not exist yet. Comparing them against dense-phase
numbers is a cross-workload comparison, not a regression.

### 3. Effect config (`fx:*`)

A bench build never calls `display_fx_set()`, so before the `fx:` phases
existed, the wobble LUT (lookup table) stayed flat and wobble was absent
from every measurement — despite shipping enabled (`.wobble = 2`).
**Every render-ISR figure recorded before 2026-08-21 measured the deck
with WOBBLE OFF.** The `fx:` phases close that hole; each phase names its
own complete fx config, so none inherits another's:

| phase | config |
|---|---|
| `fx:app` | `display_fx_defaults()` — exactly what the shell loads |
| `fx:app+sp` | shipped config over sparse (realistic) content |
| `fx:noscan` | wobble and scanlines off |
| `fx:none` | everything off |

## Memory-access microbenchmark

`CONFIG_DISPLAY_ISR_BENCH` also runs a one-shot load/store microbench at
startup. Rationale: the ESP32-S3 load/store unit is 16 bytes wide
(`XCHAL_DATA_WIDTH`), and the core reports
`XCHAL_UNALIGNED_{LOAD,STORE}_HW = 1` — misaligned access works without
an exception, but that says nothing about what it *costs*. Internal SRAM
is not cached, so these numbers are the raw access cost the render ISR
pays. They are reported as cycles per byte over one 1600-byte scanline,
min of 8 runs to shed preemption noise. The distilled rules live in
`performance.md` § "ESP32-S3 memory-access rules".
