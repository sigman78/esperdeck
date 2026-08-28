# UI spec — formal parts, menus, status, touch, and the public UI API

> **Status: rendition LOCKED (2026-08-26, five sim mockup rounds).**
> The design half of [`extensibility.md`](extensibility.md) phase-1
> items 2–4: this file names the parts and the API contract, the kit
> implements them. Backlog context: [`feat-ideas.md`](feat-ideas.md)
> §4, absorbed here.

**Purpose:** one vocabulary and one API for the shell's UI, generalized far
enough that the next extensibility step (plugins that bring their own UI)
is a registration, not a surgery.

**Ground rule: the current rendition is reference material, not a
constraint.** What this spec fixes is *contracts* — row semantics, part
responsibilities, hit behavior, API shapes. Visuals, glyphs, and layout
inside a part's rect may be redesigned freely, at any time, without
touching this spec. Nothing below means "keep the pixels."

---

## The problem, concretely

- **Status is scattered**: HOME draws its own WiFi/BLE/RAM/clock lines;
  menu, SSH-import, and WiFi-provisioning each re-draw RAM/clock corners.
  The toast sits on a different row per screen.
- **Menus are the biggest usability debt**: a crammed flat list, single-line
  large-font items that make poor touch targets, values cycled blind with
  no slider/stepper, and behavior living in a ~240-line positional
  `(page, index)` switch — array order is the API.
- **The UI API is private and screen-shaped**: only `cyberdeck_app.h` is
  exported; chrome helpers are copy-adapted per screen; grids silently drop
  overflow because there is no list widget. A plugin cannot draw a tile.

---

## Part vocabulary

Grid is 100×30 / 80×24 / 66×20 by font. Locked composition
(2026-08-26 mockup rounds): a breadcrumb bar on top, the body, a
status bar on the bottom — no hint row, no tab row.

```
 rows 0..2    ┌ BreadcrumbBar — place name + THE back target ┐
 rows 3..n-2  │ Body: section grid / value tiles /           │
              │  ListView (pickers) / Fields                 │
 row n-1      └ StatusBar ─ indicators ─ (Toast) ─── clock ──┘
```

| Part | Contract (what is fixed) | Rendition |
|---|---|---|
| **BreadcrumbBar** | rows 0–2, full width; names the place (`< CONFIGURATION / DISPLAY`) and IS the back button — one big target | locked |
| **StatusBar** | row n-1, own background; lettered indicators + lock + clock; a live Toast takes the indicator span over | locked |
| **Toast** | lives on the StatusBar while alive; one impl, all screens | free |
| **Menu** | hub + section panels; own section below | locked |
| **TileGrid** | slot rects + hit + arrow-nav | free |
| **ListView** | scrolls, never drops overflow — pickers and other variable-length lists, NOT menus | shipped (item 4 pt 1) |
| **Value well** | right span of a value tile, dimmed accent, tap = step; replaces the Slider/Stepper concept | locked |
| **Field** | edit + caret + focus contract | free |
| **Modal/Confirm** | one helper, title/body/actions; `ui_button_bar` is its action row | action row shipped |
| **Scrim** | dims terminal behind modals | free |
| **ScrollIndicator** | right edge, offset/total | free |
| **StepRow / QRPanel** | onboarding widgets | free |
| **SpriteWidget** | showcase; plugin later (phase 3) | free |

Retired: the HintBar (no permanently displayed hints — guidance is
transient toasts or self-evident layout) and the tab strip (the hub
replaced it).

Existing `draw_*` helpers are the *starting* implementations of these
parts, nothing more; they migrate to `ui_<part>_*` names as the kit forms,
and any of them may be rewritten in the visual pass.

---

## Menu — the redesign

Two halves, deliberately separable: the *model* (extensibility item 3) and
the *rendition* (this spec). Either can land first; both are required.

### Model: items carry their behavior

Per [`extensibility.md`](extensibility.md), an item is data:

```c
typedef struct {
    const char *label;
    uint8_t     accent;                    /* semantic accent, table below */
    void      (*action)(uint64_t now);     /* NULL = submenu/value-only  */
    bool      (*dim)(void);                /* grayed-out predicate        */
    const char *(*value)(char *buf, size_t n);  /* current value, NULL = none */
    uint8_t     flags;                     /* CONFIRM | SLIDER | ...      */
} menu_item_t;
```

Kills `menu_activate()`'s positional switch; pages register via
`menu_page_register()` / `menu_items_extend()` — the plugin seam for menus.

### Rendition: LOCKED 2026-08-26 (five sim-screenshot mockup rounds)

The two-column control panel — chosen over tab strips, scrolling lists
and pagination widgets, all of which were mocked and rejected (tabs:
not touch-tall; scrolling: too coarse on a cell grid with no smooth
scroll; pagination: ugly, eats estate):

- **Two-level tree**: a HUB of section tiles → section pages of value
  tiles. Both are **two-column grids of 3-row solid tiles**. Hub tiles
  carry a body line naming their contents (`fx - font`).
- **Fit-one-screen contract**: a section holds at most what the
  smallest grid shows — 2×4 = **8 items at 66×20** (10 at 80×24, 12 at
  100×30). A larger section is split at design time (e.g. glow-build
  EFFECTS → CRT + MOTION). Menus never scroll, never paginate, never
  need a swipe.
- **Tiles are 3 rows** — odd heights only, so text centers on a real
  row; even-height tiles are banned (misaligned text).
- **Value well**: the right span of a value tile in the *dimmed* accent
  (`UI_WELL`, the darker companion tone in the style palette —
  docs/overlay-style.md), value right-aligned. Tap steps the preset; the fx preset API
  (`count/index/set/label`, app_settings.h) is the data contract. No
  slider widget.
- **BreadcrumbBar** is the title AND the back path; there is no tab
  row and no Back tile inside pages.
- **Typing leaves** (profile editor, import) keep the vocabulary —
  breadcrumb bar, status bar, touch Save/Cancel via `ui_button_bar` —
  around a dense 1-row-field form (accepted as "a little crammed" at
  66×20 for now; see touch tiers below).

---

## StatusBar — the indicators

A full-width bar on its **own background color** (row n-1), everywhere
the shell owns the screen. Indicators are **lettered, 2–4 characters**,
drawn as accent patches on the bar: lit = the state's accent as the
patch background, off = the bar's base. Fixed order, left to right; the
clock right-aligned; a live Toast takes the indicator span over until
it expires.

| Indicator | Form | Source (polled) |
|---|---|---|
| WiFi | `NET` (green lit; RSSI buckets may modulate later) | `wifi_manager_get_rssi()` (dBm, 0 = disconnected), re-queried ≤ every 2 s — the wifi component API, never `esp_wifi_*` directly: the shell is platform-neutral and the sim stubs the component |
| Keyboard | `KBD` (cyan lit) | BLE (Bluetooth Low Energy) HID state |
| Caps lock | ` C ` amber chip **fused to the KBD patch**, present only while ON (design round 2026-08-27: caps is a keyboard sub-state, not a peer indicator; an off-state chip is ink spent on nothing) | `get_locks` on the BLE service ops — the deck is the host, so it owns the toggles (item 6) |
| Num lock | ~~`NUM`~~ unrendered 2026-08-27: the keymap ignores num lock, so an indicator would have no referent; `get_locks` still reports the bit for a future numpad-aware keymap | — |
| Keystore | ~~`●` amber while locked~~ dropped 2026-08-27: a locked deck shows the PIN pad — the state is self-evident | — |
| Clock | `HH:MM`, hidden until time known | SNTP (Simple Network Time Protocol) or host time |

Excluded: free-RAM (debug stat, dev screens only), scroll position
(ScrollIndicator's job), battery (no fuel gauge), session state (the
session never shows the bar — see placement).

Polled from the shell tick at the existing ~10 fps cadence; no event bus
(consistent with the extensibility architecture decision).

**Placement:** shell screens always (replaces their hand-rolled corners).
**Session: never persistent** — the terminal owns every cell. Transient
summon (top-edge tap or F-key, ~1.5 s linger) on the existing session
chrome pass. A reserved status row is rejected for now: it would shrink
the PTY (pseudo-terminal) to 29/23/19 rows behind a preference toggle.

**Constraint:** session entry blanks all sprite slots
(`font_sprite_clear_all`), so the in-session cluster uses plain BMP glyphs
only; shell screens may use sprite icons.

---

## One hint grammar, one accent meaning

- **Hints are transient, never resident** (locked 2026-08-26: the
  HintBar is retired). When guidance is needed it rides a Toast on the
  StatusBar, in `verb target` pairs, two spaces apart, ≤ 3 pairs,
  lowercase, no punctuation: `tap or Esc cancel  F12 menu`.
- **Accent semantics** — two orthogonal roles, split by region:
  - *State meanings* (chrome rows + indicators): green = healthy/confirm ·
    amber = warning/pending · red = destructive/alert · blue =
    informational · cyan = neutral chrome · white = focus · default =
    body text.
  - *Identity accents* (body only): profile/tile identity hashes across
    the pool {green, cyan, magenta, amber, blue} (`prof_accent` today).
    **Red, white, and default are reserved** — never in the identity pool,
    so destructive/alert and focus stay unambiguous everywhere.
- **Iconography:** one sprite icon set (U+E000 local namespace) on shell
  screens; BMP fallbacks in session.

---

## Touch rules

1. **Interactive tiles and buttons are 3 cell-rows tall** (48 px at
   8×16) and ≥ 4 cells wide — odd heights only, so text centers on a
   real row. 2 rows is the absolute floor for anything tappable;
   1-row chips are never touch targets (locked 2026-08-26).
2. **Hit-testing is widget-owned** — screens ask the part
   (`ui_<part>_hit`), never compare raw pixels.
3. **Edge gestures are globally reserved:** right edge = scrollback drag;
   top edge = status summon in session (proposed); long-press = menu.
   Parts must not claim edges.
4. **One shared drag converter** (accumulate-then-floor, from the
   `app_connect.c` remnant) in the kit; kinetic/momentum scrolling builds
   on it once, all scrolling parts inherit it.
5. **One drag axis per part** (vertical = scroll) — applies to the
   parts that still scroll (pickers, scrollback). Menus never scroll
   by design (locked); horizontal gestures are no longer required by
   anything in this spec.
6. **No touch-as-mouse** (feat-ideas rejection stands).

## Touch tiers (locked 2026-08-26)

**Every navigation path is touch-operable; leaf screens whose purpose
is typing may assume a keyboard.** The test: what must work when there
is no keyboard (unpaired, bond lost, dead batteries)? Unlock, WiFi,
reaching the pairing screen, and connecting a profile — by definition.

- *Touch-first (tier 1):* HOME + connect, the unlock pad, pairing,
  WiFi-provisioning confirms, the whole menu spine (hub, sections,
  value toggles — you traverse it to reach pairing).
- *Keyboard-assumed (tier 2):* the profile editor, SSH-import type-in
  and similar typing forms. They keep the shared vocabulary and their
  touch Save/Cancel bar; the typing itself owes touch nothing.

Consistency holds at the vocabulary level (one look everywhere), not
as a uniform interaction guarantee.

---

## The generalized UI API

The kit (`cyberdeck_ui.h`, public) is what extensibility items 2/4 need.
Contracts, not implementation:

**Three layers.**
1. *Primitives* — cells, pens, chips, boxes (today's `app_ui.h`, exported).
2. *Parts* — each is a plain struct + free functions:
   `ui_<part>_draw(p, frame)`, `ui_<part>_hit(p, px, py)`,
   `ui_<part>_nav(p, key)` where applicable. No hidden globals: the
   animation clock and config come in as arguments; part state lives in
   the caller's struct (plugin state → PSRAM).
3. *Screens* — a screen's `render(ctx, now)` (from the extensibility
   item-2 registry) draws its body parts; the **shell composites shared
   chrome** (TitleBar/Toast/HintBar/StatusCluster) around it. Screens
   stop drawing chrome entirely — that is what makes chrome redesign a
   one-file change forever after.

   Two consequences the current code makes explicit:
   - **Present ownership inverts.** Today 13 files call
     `ui_clear`/`ui_present` themselves; under the registry the shell
     owns the clear → screen render → chrome → present cycle, one
     present per frame. This lands with item 2, not as a separate step.
   - **Chrome is per-screen opt-in.** Saver, boot, and session
     legitimately draw no standard chrome (and HOME/menu use bespoke
     headers today). The screen descriptor carries a chrome flag
     (`NONE` / `FULL`, maybe `STATUS_ONLY`); session stays the special
     case — overlay hidden except the transient chrome pass.

**Compliance checklist** — the kit is done when all four hold:

- [ ] An out-of-component plugin can register a screen and draw it using
      `cyberdeck_ui.h` alone — no `app_internal.h`, no shell statics.
      *(The drawing half holds as of 2026-08-27; registration lands
      with extensibility item 5's plugin table.)*
- [ ] A plugin can contribute menu items/pages purely as `menu_item_t`
      data + callbacks — no positional contracts anywhere.
      *(The item model holds since item 3; the contribution point is
      item 5's shared page table.)*
- [x] Every touchable part exposes `_hit`; no screen does pixel math.
- [x] The three build contexts stay green: device, simulator, and the
      standalone Unity tests. Kit headers include **no SDL ever**, and
      ESP-IDF headers only from the idfsim-stubbed set — the standalone
      tests already build against `idfsim/` (see `tests/tsm/CMakeLists.txt`),
      which is the house compatibility pattern, not an exception to it.

---

## Sequencing

1. **Item 2** (screen registry, `render` hooks) — gives chrome its
   composition point.
2. **Item 4 kit core**: ListView + Slider + Modal + drag converter +
   `cyberdeck_ui.h`. StatusCluster and the **menu redesign are the first
   consumers** and the acceptance test of the API's shape.
3. **Item 3** (menu model) can land before or in parallel — it is data
   plumbing, independent of rendition.
4. Visual pass (item chrome, icon set, selection styling) rides on the
   sim with `--drive` screenshot scripts; iterate freely — the contracts
   above don't move.

## Open questions

- [ ] Session summon gesture: top-edge tap vs. F-key only — top-row TUI
      hotzones may collide with the tap.
- [ ] Editor density at 66×20: accepted "a little crammed" for now —
      revisit if it grates in use.
- [x] ~~Clock placement at 66 cols~~ — measured in the mocks; fits on
      the StatusBar at every grid.
- [x] ~~Menu pages: tabs vs. rail~~ — neither: hub + breadcrumb
      (2026-08-26 mockup rounds; tabs and rail both mocked, rejected).
- [x] ~~Toast row 0 vs HOME bottom~~ — Toasts live on the StatusBar.
- [x] ~~Visual direction~~ — locked: two-column DOS-solid control
      panel (see the Menu section).

## Status log

- **2026-08-25** — draft 1 from the widget/chrome inventory.
- **2026-08-25 (review)** — round 2: dropped the preservationist framing
  (contracts fixed, rendition free), promoted the menu redesign to its own
  section, added the generalized-API layer model + extensibility
  compliance checklist.
- **2026-08-26 (design lock)** — five mockup rounds in the sim
  (`--drive shot:` + throwaway env-gated painters, three font grids)
  ended in a LOCKED rendition: two-column control panel (hub +
  sections), fit-one-screen contract, breadcrumb bar as title+back,
  dimmed-accent value wells, StatusBar with lettered indicators on its
  own background, HintBar and tab strip retired, touch tiers. Rejected
  on the way, with screenshots: chip tabs (not touch-tall), embedded
  sliders ("looks off"), scrolled menus (coarse on a cell grid),
  pagination widgets (ugly, eats estate), master-detail rail (breaks
  past 4 sections), even-height tiles (misaligned text). New display
  requirement: a dimmed-accent variant in the overlay render path
  (`DIM|INVERSE` dimmed the glyph, not the background; met 2026-08-28
  by `UI_WELL` in the style palette — docs/overlay-style.md).
- **2026-08-26** — kit part 1 landed (extensibility item 4): ListView +
  the shared drag converter (touch rule 4) as `ui_list_t`/`ui_drag_t`
  in app_widgets; the profile pickers converted to two-line scrolling
  rows. Parts model honored: caller-owned state, widget-owned
  hit-testing, no hidden globals.
- **2026-08-25 (compliance audit)** — round 3, checked against the code:
  RSSI source corrected to the wifi component API (shell stays
  platform-neutral); accent semantics split into state vs. identity roles
  (`prof_accent` hashes five accents — magenta was never exclusive);
  swipe demoted to an enhancement behind the input-pipeline dependency
  (no horizontal gestures exist today); chrome composition spelled out
  (present-ownership inversion + per-screen chrome flag — 13 screens
  self-present today, 4 use bespoke/no headers); kit header rule aligned
  with the idfsim pattern. Verified sound as written: grid math, touch
  target sizes, all five indicator sources, the sprite-blank session
  constraint, the PTY-shrink argument, and the `menu_item_t` shape vs.
  extensibility item 3.
