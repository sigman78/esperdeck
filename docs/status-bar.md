# StatusBar 2 — the chip grammar

> **Status: ADOPTED 2026-08-29** (rev 3: lettered chips → icons →
> expansion, per same-day user rounds). Amends the StatusBar section
> of [`ui-spec.md`](ui-spec.md); the contract rows there get updated
> as implementation lands and this file carries the rationale. Builds
> on the shipped `ui_statusbar()` (extensibility item 4, 2026-08-27)
> — an evolution of that bar, not a replacement. Remaining open
> questions below are rendition-level, settled by sim mockup shots
> during implementation.

**Purpose:** one bar that carries every resident status on the deck,
with a fixed grammar for what a "chip" is, which of its states look
how, what earns residency versus a toast, and how a plugin adds one.

---

## Triage: what earns a resident cell

Every fact currently shown in the HOME header, the shell corners, or
the bar gets sorted along the three axes that matter on a 1-row bar:
**importance** (do I act on it?), **size** (cells it eats), and
**actuality** (does it change while I look at it?).

| Class | Test | Surface |
|---|---|---|
| **Identity state** | boolean-ish, always relevant, cheap to read at a glance | resident chip (`NET`, `KBD`, `PHN`) |
| **Quality scalar** | meaningful only while its identity state is up | 1-cell glyph *fused into* the parent chip (RSSI stair) |
| **Detail fact** | important but long and near-static | event toast on change + read-only STATUS page; never resident |
| **Debug scalar** | developer-facing, actionable under pressure only | opt-in chip that force-shows in ALERT (`MEM`) |
| **Momentary event** | a transition, not a state | the owning chip *expands in place* to show it; chip-less events ride a toast |

Applied to today's inventory:

| Info | Today | Proposed home |
|---|---|---|
| WiFi link state | HOME LED line + bar patch | `NET` chip (gains BUSY/ALERT states) |
| RSSI (received signal strength, dBm) | HOME text `-52dBm` + 4-step glyph | the stair glyph **is** the NET icon — height carries quality; the number moves to STATUS page |
| IP address / SSID | HOME LED line | NET chip expansion on connect + STATUS page |
| Keyboard link (BLE HID — Bluetooth Low Energy Human Interface Device) | HOME LED line + bar patch | `KBD` chip (gains BUSY state for pairing/reconnect) |
| Keyboard name | HOME LED line | KBD chip expansion on pair + STATUS page |
| Caps lock | amber chip fused to KBD | unchanged (the fused-segment pattern generalizes it) |
| Num / scroll lock | unrendered (no referent) | unchanged; segments make them a one-liner the day a keymap honors them |
| Phone presence | HOME-only `PHN` chip | `PHN` bar chip — its distance colors map 1:1 onto the state grammar |
| Free RAM (internal DRAM / PSRAM) | `int 43K  psram 7213K` on HOME + 2 screen corners | `MEM` chip: two 1-cell free-heap gauges, opt-in, force-shown on pressure; numbers → STATUS page; corners deleted |
| Clock | bar, right | unchanged |
| Wordmark / version | HOME right | header rework (next pass); version also on STATUS page |
| ☺ all-systems-go | HOME margin | stays a HOME flourish; not bar material |

The rule that falls out: **the bar shows states, a chip expands to
show its own transitions, toasts carry chip-less messages, the STATUS
page holds the numbers.** Nothing long-form is ever resident.

---

## The chip grammar

A **chip** is 1–2 segments drawn adjacent on the bar, no gaps between
chips (the shipped patch look). **A chip's body is an icon, not a
word** (user call, 2026-08-29): cells are the scarce resource — at
12×24 the grid is 66 cells wide and the bar must still carry
everything — and a 1-cell glyph plus a colored wash says what a 5-cell
`" NET "` patch said. Each segment has a state, and state — carried by
wash luminance and icon shape together, never hue alone (red/green
washes at equal luminance are indistinguishable to protan/deutan
vision) — is what the eye reads:

| State | Meaning | Rendition (style palette — [`overlay-style.md`](overlay-style.md)) |
|---|---|---|
| **ABSENT** | no referent (not enrolled, feature off, lock not set) | not drawn — ink is spent only on facts |
| **OFF** | present but down | own icon, `UI_WELL` + blue pen, bold (the shipped receding look) |
| **ON** | up / healthy | own icon, `UI_FOCUS` + the chip's accent, bold, dark text |
| **BUSY** | transitioning: connecting, reconnecting, pairing, syncing | **braille spinner replaces the icon** (`spinner_glyph`, already in the kit and session-safe), amber, blinking `UI_FOCUS` ↔ `UI_WELL` at ~1 Hz (blink = alternate styles per animation frame, per the style-palette rule) |
| **ALERT** | failed / needs attention | **`✘` (U+2718) replaces the icon**, `UI_FOCUS` + red — fixed chip order carries identity, so the eye still knows *which* thing failed |

BUSY is the state the shipped bar is missing: today "connecting" and
"radio off" render identically. ALERT covers `WIFI_MGR_FAILED` and
memory pressure. Accent semantics stay the locked chrome roles (green
healthy, amber pending, red alert, cyan neutral).

### The icon vocabulary — what the font already gives us

The committed Terminus tables (`components/font/terminus*.c` range
lists) cover far more symbol estate than the current UI uses; icons
cost **zero sprite work** wherever a font glyph fits:

- **Stairs / gauges** — full block elements U+2580–259F: the `▁..█`
  eighth-ramp. An indicator built from it is always **one cell wide**
  — the ramp is vertical resolution, never width. NET uses four of
  the eight heights (`▂▄▆█`, today's buckets; eight levels would be
  false precision for RSSI), the MEM gauges use the full ramp.
- **Verdicts** — `✓` U+2713 / `✘` U+2718 for ok / failed — the
  *endpoints* of the covered range, verified as real glyphs in all
  three tables (interior codepoints of a merged range may be `?`
  fillers).
- **Motion** — braille U+2800–28FF complete (the existing spinner);
  `↻` U+21BB; arrows U+2190–2195.
- **CP437 heritage** — `☺ ☻ ☼ ♠ ♥ ♪ ♫ ◘ ◙ ⌂` — on-brand for the
  DOS-solid look where a flourish is wanted.
- **Powerline glyphs** — U+E0A0–E0A2 (branch, line-number, and a
  **padlock**) and U+E0B0–E0B3 (the angled segment separators). The separators are a free *rendition* option: chips
  with powerline-angled ends instead of rectangular washes, at one
  cell per boundary. Noted for the mockup round, not mandated here.

What the font does **not** have: a keyboard or a phone. Those two come
from the sprite icon set (U+E000 local namespace, per ui-spec's
iconography rule). Sprites are blanked in-session
(`font_sprite_clear_all`), and the bar does appear there during the
transient summon — so **every sprite-iconed chip declares a 1-char
fallback** (`K`, `P`) that the summon pass renders instead. Font-glyph
icons need no fallback.

Legibility is trained, not assumed: with at most ~6 chips on a
personal device the vocabulary is small, and two surfaces teach it —
the STATUS page shows each icon beside its labeled row, and an
expanding chip (below) pairs symbol to meaning exactly when the user
is looking: the detail stretches out of the icon itself.

**Segments** generalize the caps-chip fusion: a chip may emit a second
segment with its own state and accent, drawn flush against the first.
Caps is `KBD` + amber ` C `; a future num/scroll lock is another
segment, not another mechanism. Segment text is UTF-8 (`ui_puts_u8`),
so block glyphs ride inside a segment.

### Per-chip mappings

Each chip is its icon in a 3-cell wash (` ▆ ` — one padding cell each
side so the wash reads as a chip); fused segments add 2 cells each.

- **NET** — the signal stair **is** the icon: `▂ ▄ ▆ █` from the
  existing RSSI buckets (≤−78 / ≤−67 / ≤−55 / above), reusing HOME's
  thresholds and its 1-s snapshot cadence (never poll the WiFi driver
  at render rate). OFF = dim `▂` in the well (shape says "signal",
  luminance says "down"); BUSY = spinner (connecting, lost); ALERT =
  `✘` (failed). The dBm number lives on the STATUS page.
- **KBD** — sprite keyboard icon, fallback `K`. OFF (idle) / BUSY
  (pairing scan, reconnect search) / ON (cyan) + a fused amber `C`
  segment while caps lock is on (num/scroll join as segments the day
  they gain a referent).
- **PHN** — sprite phone icon, fallback `P`. ABSENT until enrolled;
  the existing distance colors are already this grammar: BUSY amber
  while enroll-advertising, ON green near, ON blue in-range-but-far,
  ALERT red enrolled-but-gone.
- **MEM** — two 1-cell **free-heap gauges** (internal, then PSRAM)
  from the eighth-block ramp `▁..█`, fill = fraction free, 4 cells
  with padding vs 18 today. Renders in `UI_WELL` (a readout, not a
  state) while the debug toggle is on; **force-shows in ALERT** when
  a pool crosses its low-water mark (internal < 16 K or
  PSRAM < 512 K, tunable constants), expanding with the actual
  numbers (`✘ int 11K`). Exact figures live on the STATUS page;
  leak-hunting sessions read the gauge trend, not digits. When the debug era ends the toggle default
  flips off and the chip costs nothing.
- **Clock** — unchanged, right edge, numeric (`HH:MM` is the one
  resident where digits beat any icon), hidden until SNTP (Simple
  Network Time Protocol) or host time exists.

### Expansion — a chip announces its own change

(User direction, 2026-08-29.) On a state edge the chip stretches in
place: the wash extends rightward to carry a one-line detail, holds
~3 s, then decays — collapsing one cell per frame back to the compact
chip (at the 10 fps tick that is a quick sweep; on a cell grid this
truncation walk IS the animation, and it is nearly free). Neighbor
chips shift right during the hold and slide home with the collapse —
deliberate: motion at the change site is the notification, and the
detail is physically attached to the symbol it explains.

```
on the edge:  │ ▆ MySSID 192.168.1.7 ⌨ C ☏             ▇▆  12:47 │
…decays to:   │ ▆ ⌨ C ☏                                 ▇▆  12:47 │
```

Rules:

- **The engine owns edge detection.** It polls every chip per frame
  anyway; it diffs against the previous state and drives the
  hold/decay state machine centrally. A chip — plugin included —
  only supplies a `detail()` callback; announce behavior comes free
  with registration, and no chip ever animates itself.
- **Announced edges**: into ON (the arrival fact — SSID + IP,
  keyboard name), into ALERT (the reason — `✘ int 11K`), and ON→OFF
  (the loss). Never into BUSY (the spinner already is the announce),
  and never for fused-segment changes (a caps chip appearing is its
  own signal).
- **Wash inheritance**: the expansion renders in the chip's current
  state style — arrivals stretch green/cyan, failures stretch red.
- **One at a time, first-in-first-out.** Simultaneous edges (boot
  brings everything up together) queue and play in turn with a
  shortened hold; detail text clips so cluster + expansion never
  reaches the right cluster.
- **Boot is the payoff**: the first NET arrival expands with the IP —
  the one moment you actually want it on screen — then gets out of
  the way.
- **In session** the bar is hidden, so edges there play out unseen —
  fine: the persistent state carries the news to the next summon,
  and session-critical failures already have their own signaling.

Expansions replace rev-1's transition toasts. The toast survives for
messages that belong to no chip (menu notes, hints, errors), and its
span is now rarely contended.

### Layout

Left cluster = identity chips in priority order; right cluster = MEM +
clock; the toast lives in the gap **without hiding the chips**
(`⌨`/`☏` stand in for the two sprite icons below):

```
66 cols, everything on, caps lit, toast live:
│ ▆ ⌨ C ☏   ⌨ paired — Logi K380                        ▇▆  12:47 │

66 cols, nothing enrolled, radio down (all wells, receding):
│ ▂ ⌨                                                       12:47 │
```

Fully lit, the left cluster is ~11 cells (was ~19 lettered) and the
right ~11 (was ~17), leaving a ≥ 40-cell toast span at 66 cols — the
icon turn is what makes "display all" and "one row" compatible at
12×24.

Amendment to the shipped behavior (toast takes the indicator span
over): now that chips carry live state — BUSY blink, ALERT — hiding
them during a toast loses exactly the state the toast is announcing.
The toast clips to the free gap (`%.*s`, ≥ ~40 cells even at 66 cols
with every chip lit); if a pathological chip set leaves under ~16
cells, the toast takes the span as today (fallback, not the norm).

### What chips are not

Not touch targets. The locked touch rule stands: 1-row chips are never
tappable. Detail lives on the STATUS page and in transition toasts; a
whole-bar tap summon is an open question below, not part of this
proposal.

---

## Detail relocation

Two pieces make "IP moved off the header" true rather than "IP is now
hidden":

1. **Chip expansion** (above) — the fact appears at its chip, on the
   edge, then decays: `▆ MySSID 192.168.1.7` on connect,
   `⌨ Logi K380` on pair. Rate-limited by nature (edges, not
   levels).
2. **STATUS page** — a read-only section in the CONFIG hub, standard
   two-column value tiles with `UI_WELL` wells: SSID, IP, RSSI dBm,
   gateway later, keyboard name + battery later, firmware version,
   heap numbers. Fits the fit-one-screen contract (≤ 8 items) and
   costs zero new widgetry — it is a menu page whose items have no
   action.

The HOME header rows this frees (NET/KBD/RAM HUD lines) are the raw
material for the header rework, which is deliberately **out of scope
here** — this bar must land first so the header has somewhere to shed
weight to.

---

## Extensibility: the chip table

Same shape as every other seam in this codebase — a descriptor row,
polled by the shell, no event bus:

```c
typedef enum { CHIP_ABSENT, CHIP_OFF, CHIP_ON, CHIP_BUSY, CHIP_ALERT }
    ui_chip_state_t;

typedef struct {
    ui_chip_state_t state;
    uint16_t        icon;          /* codepoint for the single icon cell */
    uint16_t        alt;           /* stand-in for the session summon when
                                      icon is a U+E000 sprite; 0 = icon is
                                      already session-safe */
    uint8_t         accent;        /* ON accent; 0 = descriptor default */
} ui_chip_seg_t;
```

(As built in step 1: every rendition is exactly one cell, so a segment
carries a codepoint, not a UTF-8 buffer — no decode per frame.)

```c

typedef struct {
    const char *id;                /* stable key: "net", "kbd", "mem" */
    uint8_t     anchor;            /* CHIP_LEFT | CHIP_RIGHT           */
    uint8_t     priority;          /* draw order within anchor         */
    uint8_t     accent;            /* default ON accent                */
    int       (*poll)(ui_chip_seg_t seg[2]);  /* fill, return count   */
    int       (*detail)(char *buf, size_t n); /* expansion text on a
                                                 state edge; NULL =
                                                 never expands       */
} ui_chip_desc_t;
```

- Core chips are one static table (NET 10, KBD 20, PHN 30, MEM 10-R);
  spaced priorities leave room for plugins to slot between.
- `cyberdeck_plugin_t` gains a `status_chips` array, mirroring
  `home_tiles` — registration is a table row, per the plugin seam.
- The shell polls at the render cadence (~10 fps); anything expensive
  snapshots internally on its own clock (the RSSI pattern).
- Overflow: hard cap (6 left-anchored chips), lowest priority dropped
  first, logged once — same posture as HOME tile overflow.
- Implementation moves `ui_statusbar` + the chip engine into its own
  `app_statusbar.c`; `app_widgets.c` keeps the generic kit.

---

## Landing order (each step ships alone)

1. **Chip engine** — state grammar + segments + table walk; convert
   NET (stair icon) / KBD (letter `K` until its sprite lands) / caps.
   First visible win: connecting spins, failed shows `✘`, and the bar
   shrinks by half.
2. **Sprite icons + PHN migration** — draw the keyboard/phone sprites
   (one U+E000 set per ui-spec), wire the `alt` fallback through the
   session summon; HOME HUD keeps duplicating for now.
3. **MEM gauges** — settings toggle + pressure thresholds + alert
   toast with numbers; delete the `ram_stats` corners (SSH-import,
   WiFi-provisioning) and HOME's RAM line.
4. **Expansion + STATUS page** — the engine's edge-diff + hold/decay
   machine, `detail()` on NET/KBD/MEM; after this, IP/SSID/name leave
   the HOME header with no information loss.
5. **Plugin seam** — `status_chips` on the descriptor, dogfooded by
   whichever chip moves out of core last.

Then the header rework starts, on the estate this vacates. Visual
tuning rides the sim `--drive shot:` loop as usual.

---

## Open questions

- [x] ~~RSSI stair: 1 cell vs 3-cell staircase~~ — resolved by the
      icon turn (2026-08-29): the 1-cell stair *is* the NET chip; a
      3-cell form would spend the exact cells the turn reclaims.
- [ ] Powerline-angled chip ends (U+E0B0–E0B3, free glyphs) vs flat
      rectangular washes — pure rendition, decide from mockup shots.
- [ ] MEM default while the debug era lasts: toggle-on (proposed) or
      alert-only from day one?
- [ ] MEM gauge direction: fill = free (proposed; draining bar =
      trouble approaching) or fill = used (dashboard convention)?
- [ ] Whole-bar tap = summon a status-summary toast? Needs a touch-rule
      amendment (bar-as-one-target); deferred, the STATUS page covers
      the need.
- [ ] Clock before time sync: hidden (today) vs a muted `--:--`?
- [ ] Scroll lock has no source bit and no referent — drop forever, or
      adopt the feat-idea of mapping it to scrollback freeze (which
      would give both the key and a chip segment a real meaning)?
