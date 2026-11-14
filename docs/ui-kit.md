# UI kit: one surface, cut layouts, one interaction model

**Status: draft, 2026-11-14.** The kit and its lab exist. The look is
decided: plain slabs on a slate screen (see *Parts*). The parts are
built and gated against their design sketches. HOME, the host page,
the question page, the setup hub, its sections, the session sheet, the
host form, the code pad, the connect screen, the host-key page, keyboard
pairing, Wi-Fi onboarding and host import draw with them. Only the old
helpers themselves, and the status row, are still to go.
The whole shell uses the slate colour pair.
Companion to
[`ui-spec.md`](ui-spec.md) (what each part must do) and
[`overlay-style.md`](overlay-style.md) (how a look is named).

## Why rebuild

The shell grew three header styles, two field styles and per-screen
selection code. Each screen kept its own `sel` index, its own grid
arithmetic for arrow keys and its own pixel-to-tile hit test. Three
effects followed.

- A touch target below three rows (the editor's one-row bracket fields)
  was easy to write and hard to notice.
- A layout tuned at 100x30 clipped at 66x20, because sizes were literals.
- Touch and keyboard were two code paths per screen, so they drifted:
  HOME needed a second tap to connect because the first tap was the
  keyboard's "select".

The kit moves all three concerns out of the screens.

## Layout: cut rects, never literal coordinates

`ui_layout.h` is integer geometry with no display dependency.

- **Rect-cut.** A screen starts from the full grid and slices it:
  `ui_cut_top(&r, n)` returns the top `n` rows and shrinks `r`. A cut
  larger than the rect takes what is there. Cuts cannot overlap or leak
  by construction.
- **Grids.** `ui_grid(area, ncols, ch, gx, gy)` lays equal cells
  row-major. The cell width absorbs the area. `ui_grid_capacity()`
  tells a screen how many items fit, so "fits one screen" is a test,
  not a hope.
- **Metrics.** `ui_metrics_for(cols, rows)` gives the size class
  (`UI_COMPACT` 66x20, `UI_REGULAR` 80x24, `UI_WIDE` 100x30) and the
  spacing tokens: side margin, column gutter, the three-row touch unit
  and the header height. A layout branches on the class, never on a
  column count.

`ui_page()` is the standard split: header rows on top, one status row
at the bottom, the body between them inset by the margin.

## Interaction: regions registered while drawing

`ui_hot.h` holds the one interaction model. While a frame draws, each
interactive widget registers its rect under an id (`ui_hot(rect, id)`).
Input resolves against the regions of the last finished frame.

- **Touch.** `ui_hot_touch(kind, col, row)` hits a region. `DOWN` sets
  the pressed look, `TAP` returns `UI_ACT_PRESS`, `HOLD` returns
  `UI_ACT_HOLD`. A tap holds the pressed look for `UI_FLASH_FRAMES` so
  a quick finger still sees feedback. See *Press and release* below for
  where `DOWN` and `UP` come from.
- **Keys.** Arrows move focus to the nearest region in that direction.
  Rows count double in the distance, because a cell is twice as tall as
  it is wide. Enter presses the focused region.
- **Focus ring.** Touch hides the ring. The first key after a touch only
  reveals it. A touch screen with one lit card reads as "selected" and
  invites a wrong second tap.
- **Back.** The header registers its whole bar as `UI_ID_BACK`. A
  screen maps that id to its own back step: the menu steps one page up,
  most screens pop. It never takes the default focus.
- **Disabled.** A disabled card stays a region, so activating it can
  say why it is unavailable. A card drawn with id 0 is inert.

A widget reads its own state with `ui_focused(id)` and `ui_pressed(id)`.
A screen never passes "selected" and keeps no selection index.

Input and render run on the one shell task and never interleave, so one
region list serves both. `ui_clear()` starts the list for a frame. The
navigation stack calls `ui_hot_reset()` on every screen change. A screen
that swaps pages inside one stack entry (the menu) resets for itself and
drops taps until the new page has drawn once.

## Widgets: the card and what is built on it

`ui_widgets.h`. Every widget takes a rect and an id.

| Widget | Job |
|--------|-----|
| `ui_card` | The one tappable surface. Three rows hold one line, four rows hold a title and a detail line. Optional right-aligned value, check mark, chevron. |
| `ui_btn` | A centered card that commits something. |
| `ui_header` | Page title, optional muted path, optional note. With `back`, the back target. |
| `ui_input` | A labeled value. Masked, editing (caret) and cycle (fixed set) variants. |
| `ui_meter` | One-row gauge in eighths of a cell. |
| `ui_dialog` | A window centered over a dimmed backdrop. Returns its content rect. |
| `ui_pager` | A `< 1/2 >` stepper at the right end of a header. Draws nothing for one page. |

**Half-height edges.** A card's top row draws U+2584 (lower half block)
and its bottom row U+2580 (upper half block) in the card colour. Two
stacked cards then show a full row of gap without spending a gutter
row. This is what lets eight two-line items fit the 16 body rows of
the 66x20 grid. The font lacks U+2594 and U+2595 (one-eighth blocks),
so thinner edges are not available.

**Heights.** The one-line card is three rows, edge rows included, so
its label sits on the true centre row. That keeps the odd-height rule
from `ui-spec.md`. The two-line card is four rows: two edge rows and
two text rows. It is even, and it bends that rule. The menu therefore
uses one-line cards only, which also keeps the 2026-08-27 call that hub
tiles carry the section name alone. Two-line cards are a proposal for
profile cards on HOME and in lists, pending review.

**Focus mark.** The focused card carries a white rail in the left half
of its first column, over the full visible height of the card. Quadrant
glyphs (U+2596, U+2598) end the rail on the half-height edge rows. The
dark half of that column is the seam between rail and card.

**Value well.** A card with a value keeps its title at full length. The
value sits in a darker well that takes the rest of the row.

**Colour.** All colour decisions sit in one function, `card_look()`. A
widget asks for a look and paints cells. New palette rows for panels
and edges are baked by `ui_theme.c` with the rest of the style table;
`tests/theme` proves no entry has text equal to background on either
theme.

## Parts: slabs on a slate screen

`ui_part.h`. This is the look chosen on 2026-11-14; it replaces the
widgets above screen by screen. The design was drawn first on the
JavaScript model in `tools/uimock` (`sketches-mo.js`), and the C parts
port those sketches line by line.

| Part | Job |
|------|-----|
| `ui_part` | The one tappable part. Title, optional detail line, optional right-hand text, optional colour stripe. |
| `ui_push` | A button: one centered label. |
| `ui_pill` | A header button, cut from the right end of the header. |
| `ui_switch` | A two-state setting. |
| `ui_chips` | One choice of several, every option visible. |
| `ui_entry`, `ui_entry_at` | A form field: label, value, caret while editing, invalid mark. `ui_entry_at` puts the caret anywhere in the value and scrolls to keep it in view. |
| `ui_title` | Page header. With `back`, the whole bar is the back target. |
| `ui_pages`, `ui_rail` | Page stepper in the header; position rail at the side of a paged list. |
| `ui_info`, `ui_panel` | Text panels. They hold text only and are never touch targets. |
| `ui_slate_page` | Paints the background and returns header, body, status and the whole area. |
| `ui_qr` | A QR code on a white panel: half blocks, a quiet zone and a caption row. `ui_qr_w` and `ui_qr_h` give its size. |
| `ui_onboard` | The page of a phone or PC flow: numbered steps on a text panel (`ui_steps_wrap` wraps them), a `ui_qr` beside them when it fits, a state line (`ui_state`) or a striped state panel under them. With no steps it is a centered result card. |

**Tone, not frames.** A part is a plain rectangle with half-height top
and bottom edges. Three tones give depth: `CARD` is a neutral panel,
`TINT` a dark accent (a caution, the joined network), `SOLID` a bright
accent (the main action). Fake rounding with quarter-cell corners was
tried and rejected.

**Colour marks a state.** Key focus is one sand fill on every part, and
no other state uses it. A finger down shows the same light fill in the
part's own accent. Green marks done or on, amber the step in progress,
red an invalid value or a destructive action. A label beside its value
takes a teal hue. In a value with a unit ("2 h 14 min", "-55 dBm") the
unit draws dim. A number is digits with at most one `.` or `:` part, so
an address does not dim the word after it.

**A switch says its state three ways:** the word On or Off, the side of
the diamond knob on its line, and green for On.

**An edged part sits on the screen background.** The edge styles carry
the theme background, so a part drawn on a panel would show slate
notches. Panels therefore hold text only. A question is a panel with
its two answers below it, on its own page.

**Own widths.** A button, switch or action is as wide as its text needs
(`ui_push_w`, `ui_act_w`, `ui_switch_w`), never as wide as its row. Rows
pack from the left with `ui_flow`; a part with no room starts the next
row. Only tiles in a grid and text panels take a width from the layout,
and a grid gains columns on a wider screen (`ui_cols_for`). The chips of
one page share a caption column (`ui_caption_w`, the `cap_w` argument of
`ui_chips`). Arrow keys prefer the part straight ahead over a nearer one
off to the side.

**Heights.** A one-line part is three rows; a two-line part is four.
`ui_centre` places the text. A label that cannot sit on the centre row
counts as a layout fault, and so does a part with no room. `ui_faults`
reports the count. The lab fails its run when a screen adds one, so a
layout that breaks at one font size cannot pass in silence.

**Text is UTF-8.** `ui_text` and every width in the parts count code
points. C sources stay ASCII: a glyph in a string literal comes from a
`UI_S_*` macro, such as `UI_S_CHECK " joined"`.

**State sheets.** `ui_debug_pose` forces the focus or pressed look of
an id. It exists in host builds only, for the lab's parts sheet and for
tests. The production parts have no "draw as focused" option.

## The lab

`cyberdeck_uilab` (target in `sim/CMakeLists.txt`) links only `ui`,
`display` and `font`. It draws example screens through the real display
path with example data.

- `--out DIR` renders every screen to a BMP in a fixed pose (keyboard
  focus on one card, a finger down on another) with animated effects
  off, so two runs give the same image.
- Without `--out` it opens a window. The mouse is the finger,
  right-click is a long press, arrows and Enter drive focus, Tab steps
  through the screens.
- `--font 8x16|10x20|12x24` picks the grid. `--screen NAME` limits the
  run to one screen.
- `--dump DIR` writes the font's glyphs, the baked style tables and
  every posed frame's cells as JSON. `tools/uimock` reads them: a
  JavaScript model of the display for trying designs without a C build.
  Its two checks compare the model with the lab's screenshots and
  cells; see `tools/uimock/README.md`.

The lab status bar is a stand-in. The shell's bar is specified in
[`status-bar.md`](status-bar.md).

**The parity gate.** `tools/uimock/sketches-mo.js` lists the frames the
parts must match (`MO_GATE`): each screen in its states, with a focus
or finger pose. The lab draws a screen of the same name for each one
(`sim/uilab_mo.c`). `kitcheck.mjs` then compares every cell and every
touch region at all three fonts. A frame the lab does not draw fails; it
does not skip. Change a part and its sketch together, then rerun
`export.mjs`, `check.mjs` and `kitcheck.mjs`.

## Press and release

The touch driver posts `INPUT_EVENT_PRESS` on finger-down. Exactly one
of TAP, LONG_PRESS or RELEASE follows. RELEASE means the press ended as
no gesture: a slow lift, or a drag that became a scroll. The simulator's
mouse path posts the same sequence.

- A screen acts on TAP only. PRESS and RELEASE change how a card looks.
- The screen saver ignores both. A wake on PRESS would hand the TAP of
  the same gesture to the screen under the saver.
- The input queue drops an event when it is full. A dropped TAP leaves
  the card lit until the next touch.

The device path is built but not yet judged on hardware. Touch is polled
every 50 ms and the shell draws at about 10 frames a second.

## Paging

A menu fits one screen and never pages. Data can outgrow a grid: the one
case today is HOME at 66x20, which holds six two-line profile cards
above its action row while storage holds eight.

- `ui_page_count(count, per)` and `ui_page_clamp()` give whole pages.
  `per` comes from `ui_grid_capacity()`.
- `ui_pager()` puts the stepper in the header. No drag, no scroll
  track, no partial page.
- A two-column picker of two-line cards holds all eight profiles at
  every grid, so pickers do not page. The shell has no profile picker
  now; edit, delete and move live on the host page.
- The stepper takes focus like any card. An arrow key does not turn the
  page at the edge of a grid yet.

## Decisions, 2026-11-12

| Question | Verdict |
|----------|---------|
| Rendition | Slab. Wire was second. The first focus mark was rejected and redrawn as the rail above. |
| On-screen keyboard | Wanted, not now. The lab keeps its letters-only screen as a sketch. `ui-spec.md` tier 2 stays keyboard-assumed. |
| HOME taps | One tap connects. A long press on a profile opens its editor. A long press elsewhere still opens pairing. |
| Focus ring | Shown only after a key. |
| Press events | Adopted on trial; see *Press and release*. |
| Long lists | Page, do not scroll; see *Paging*. |

## Decisions, 2026-11-14

| Question | Verdict |
|----------|---------|
| Look | The slate parts above. The Panel and Wire renditions are deleted. The Slab widgets stay until the last shell screen moves. |
| HOME | Two-line host tiles, two columns. New, Setup and Lock are header buttons, so no action row and no Settings tile. Eight hosts fit at 66x20. |
| Tap and hold | A tap connects. A hold opens the host page (connect, edit, delete, reorder). |
| Keyboard | Every screen works from the keys alone. The kit has a hold key (`K_HOLD`) for the alternate action. |
| Values | A switch for two states, chips for a choice. No value hides behind a cycling tile. |
| Questions | A page of their own; the safe answer holds the first focus. |
| Lists | Paged, with the page in the header and a rail at the side. |
| Status row | The adopted chips on a panel-tone bar. |
| Widths | Stretched buttons look odd. A button, switch or action keeps its own width and rows pack from the left; tile grids gain columns instead. |

## Still open

1. **The key for hold.** Space is the proposal, with Shift+Enter as a
   second binding. The shell port sets it; the kit only knows `K_HOLD`.
2. **Label hue.** Teal comes from the accent table. A softer hue needs
   its own style row.
3. **The right margin at 12x24.** 66 cells leave 8 px that stay black
   beside a slate screen. The display side must fill or centre it.

## Migration order

One screen per change, old and new widgets side by side until the last
one moves.

1. Shell plumbing (done): `app_hot_input()` maps input events to
   `ui_hot_touch` / `ui_hot_key`; the navigation stack resets regions on
   every screen change. Focus restore per stack entry is still open.
2. Menu hub and sections (done). The profile pickers are gone; the host
   page covers edit, delete and move.
3. The parts and their gate (done): `ui_part.c`, the lab's slate
   screens, the parity gate.
4. The slate colour pair, HOME, the host page and the question page
   (done). The margin fill at 12x24 is still open: the scanline code
   zeroes those pixels, so it is a display change with its own timing
   check.
5. Setup hub, sections and the session sheet (done): `app_menu.c` draws
   the lab's `scr_setup`, `draw_section` and `scr_menu`. A section packs
   its rows once; a long one pages with `ui_pages` and `ui_rail`. A
   destructive action asks on the question page.
   The host form (done): `app_profile.c` draws the lab's `draw_edit`.
   An entry types in place, with a movable caret (`ui_entry_at`). Tab
   walks the controls. A changed form asks before it discards. The
   paged lists are still open.
   The unlock pad (done): `app_unlock.c` draws the lab's `draw_unlock`.
   Touch goes through the hot regions; keys never do, and the screen
   parks focus on an unused id so no key lights while typing
   (`ui_code_plan` lays out the entry line).
6. Connect progress and the host-key prompt (done). `app_connect.c`
   draws the lab's `draw_connect`: a text panel with a Cancel button.
   A failed connect stays on that screen as the failed page (Back, Try
   again, Edit host), and `ui_wrap2` wraps its reason. `app_hostkey.c`
   draws `draw_hostkey` with `ui_group4` for the fingerprint. A changed
   key asks on the question page. Status row still open.
   Keyboard pairing, Wi-Fi onboarding and host import (done).
   `app_pairing.c` shows the found keyboards as tiles, with Forget bonds
   at the bottom. It asks on the question page, which stops the scan
   when it opens, so the pairing page starts the scan again on return.
   `app_wifiprov.c` and `app_sshimport.c` draw `ui_onboard`. The QR code
   drops out where it does not fit beside the steps (the Wi-Fi code at
   66x20). The old radar sweep, comet and RAM readout are gone. A touch
   away from the header bar no longer cancels either page.
7. Delete the Slab widgets and the old tile, field and header helpers.
