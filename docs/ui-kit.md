# UI kit: one surface, cut layouts, one interaction model

**Status: draft, 2026-11-14.** The kit and its lab exist. The look is
decided: plain slabs on a black screen (see *Parts*). The parts are
gated against their design sketches. Every shell screen draws with
them: HOME, the host page, the question page, the setup hub, its
sections, the session sheet, the host form, the code pad, the connect
screen, the host-key page, keyboard pairing, Wi-Fi onboarding and host
import. The status row and the in-session toast draw with `ui_status`
and `ui_toast`. The whole shell uses one colour pair: light grey on black
(the `slate` theme).
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

`ui_slate_page()` is the standard split: header rows on top, one status
row at the bottom, the body between them inset by the margin.

## Interaction: regions registered while drawing

`ui_hot.h` holds the one interaction model. While a frame draws, each
interactive part registers its rect under an id (`ui_hot(rect, id)`).
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
  reveals it. A touch screen with one lit part reads as "selected" and
  invites a wrong second tap.
- **Back.** The header registers its whole bar as `UI_ID_BACK`. A
  screen maps that id to its own back step: the menu steps one page up,
  most screens pop. It never takes the default focus.
- **Disabled.** A part drawn `UI_PART_OFF`, or with id 0, registers no
  region and is inert.

A part reads its own state with `ui_focused(id)` and `ui_pressed(id)`.
A screen never passes "selected" and keeps no selection index.

Input and render run on the one shell task and never interleave, so one
region list serves both. `ui_clear()` starts the list for a frame. The
navigation stack calls `ui_hot_reset()` on every screen change. A screen
that swaps pages inside one stack entry (the menu) resets for itself and
drops taps until the new page has drawn once.

## Files

| File | Job |
|------|-----|
| `ui.c` | Overlay primitives: pen, cells, strings, the double-buffered frame. |
| `ui_theme.c` | The baked style table. `tests/theme` proves no entry has text equal to background. |
| `ui_layout.c` | Rect-cut, grids, metrics, page counts. |
| `ui_hot.c` | Regions, focus, touch and key resolution. |
| `ui_pure.c` | Integer logic with no display: text fit, entry window, code plan, note spans, announcements. Unit-tested in `tests/ui`. |
| `ui_part.c` | The parts below. |
| `ui_widgets.c` | Helpers every part shares: `ui_text`, `ui_metrics`, `ui_touch_px`. |
| `ui_parts.c` | The drag converter, the spinner and noise glyphs, the host accent and the session scrollbar. |

`cyberdeck_ui.h` pulls them together. A screen includes it alone.

## Parts: slabs on a black screen

`ui_part.h`. This is the look chosen on 2026-11-14. The design was drawn
first on the JavaScript model in `tools/uimock` (`sketches-mo.js`), and
the C parts port those sketches line by line.

| Part | Job |
|------|-----|
| `ui_part` | The one tappable part. Title, optional detail line, optional right-hand text, optional colour stripe. |
| `ui_push` | A button: one centered label. |
| `ui_pill` | A header button, cut from the right end of the header. |
| `ui_switch` | A two-state setting. |
| `ui_chips` | One choice of several, every option visible; the chosen chip sticks out half a cell into the gap on each side (the first chip only on its right). |
| `ui_entry`, `ui_entry_at` | A form field: label, value, caret while editing, invalid mark. `ui_entry_at` puts the caret anywhere in the value and scrolls to keep it in view. |
| `ui_title` | Page header. With `back`, the whole bar is the back target. |
| `ui_pages`, `ui_rail` | Page stepper in the header; position rail at the side of a paged list. |
| `ui_info`, `ui_panel` | Text panels. They hold text only and are never touch targets. |
| `ui_slate_page` | Paints the background and returns header, body, status and the whole area. |
| `ui_status` | The status row: a panel-tone bar, the chips (`ui_sb_chip_t`, one state per cell), the clock, then a note (plain, good or alert) or a hint pair in the gap. `ui_note_span` places the text. One chip may stretch with a detail text (`xchip`, `xtext`, `xw`); `ui_sb_room` clips it. `ui_ann_*` is the queue and clock of those announcements. |
| `ui_toast` | The in-session toast: a panel-tone box as wide as its text. The good tone adds a check mark. |
| `ui_qr` | A QR code on a white panel: half blocks, a quiet zone and a caption row. `ui_qr_w` and `ui_qr_h` give its size. |
| `ui_onboard` | The page of a phone or PC flow: numbered steps on a text panel (`ui_steps_wrap` wraps them), a `ui_qr` beside them when it fits, a state line (`ui_state`) or a striped state panel under them. With no steps it is a centered result card. |

**Half-height edges.** A part's top row draws U+2584 (lower half block)
and its bottom row U+2580 (upper half block). Two stacked parts show a
full row of gap without spending a gutter row. This lets eight two-line
items fit the 16 body rows of the 66x20 grid. The font lacks U+2594 and
U+2595 (one-eighth blocks), so thinner edges are not available.

**Tone, not frames.** A part is a plain rectangle with half-height top
and bottom edges. Three tones give depth: `CARD` is a neutral panel,
`TINT` a dark accent (a caution, the joined network), `SOLID` a bright
accent (the main action). Fake rounding with quarter-cell corners was
tried and rejected.

**Colour marks a state.** Key focus is one sand fill on every part, and
no other state uses it. A finger down shows the same light fill in the
part's own accent. Key focus also redraws the part's first and last
column as a half-cell sand bar and a dark slit (`ui_focus_bars`, `▌ label ▐`);
a striped part keeps its stripe as the left bar. Green marks done or on, amber the step in progress,
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
  focus on one part, a finger down on another) with animated effects
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

The lab draws the status row with the same `ui_status` as the shell, so
the gate covers the bar (`mo-status-*` frames). The bar's chips and
states are specified in [`status-bar.md`](status-bar.md).

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

- A screen acts on TAP only. PRESS and RELEASE change how a part looks.
- The screen saver ignores both. A wake on PRESS would hand the TAP of
  the same gesture to the screen under the saver.
- The input queue drops an event when it is full. A dropped TAP leaves
  the part lit until the next touch.

The device path is built but not yet judged on hardware. Touch is polled
every 50 ms and the shell draws at about 10 frames a second.

## Paging

A menu fits one screen and never pages. Data can outgrow a grid: the one
case today is HOME at 66x20, which holds six two-line profile cards
above its action row while storage holds eight.

- `ui_page_count(count, per)` and `ui_page_clamp()` give whole pages.
  `per` comes from `ui_grid_capacity()`.
- `ui_pages()` puts the stepper in the header. No drag, no scroll
  track, no partial page.
- A two-column picker of two-line cards holds all eight profiles at
  every grid, so pickers do not page. The shell has no profile picker
  now; edit, delete and move live on the host page.
- The stepper takes focus like any part. An arrow key at the edge of a
  grid turns the page: `ui_page_step()` maps Down from the last row, Up
  from the first row of page 2+, Right from a page's last tile and Left
  from its first tile to a tile on the neighbour page. A screen calls it
  before `ui_hot_key`. HOME does, with 32 hosts in mind (frames
  `mo-home-32*`).
- A title's note (`ui_title`) is dropped whole when it does not fit.

## Decisions, 2026-11-12

| Question | Verdict |
|----------|---------|
| Rendition | Slab. Wire was second. The first focus mark was rejected and redrawn as the rail above. |
| On-screen keyboard | Wanted, not now. The lab's letters-only screen is gone. `ui-spec.md` tier 2 stays keyboard-assumed. |
| HOME taps | One tap connects. A long press on a profile opens its editor. A long press elsewhere still opens pairing. |
| Focus ring | Shown only after a key. |
| Press events | Adopted on trial; see *Press and release*. |
| Long lists | Page, do not scroll; see *Paging*. |

## Decisions, 2026-11-14

| Question | Verdict |
|----------|---------|
| Look | The slate parts above. The Panel and Wire renditions and the first Slab widgets are deleted. |
| HOME | Two-line host tiles, two columns. New, Setup and Lock are header buttons, so no action row and no Settings tile. Eight hosts fit at 66x20. |
| Tap and hold | A tap connects. A hold opens the host page (connect, edit, delete, reorder). |
| Keyboard | Every screen works from the keys alone. The kit has a hold key (`K_HOLD`) for the alternate action. |
| Values | A switch for two states, chips for a choice. No value hides behind a cycling tile. |
| Questions | A page of their own; the safe answer holds the first focus. |
| Lists | Paged, with the page in the header and a rail at the side. |
| Status row | The adopted chips on a panel-tone bar, drawn by `ui_status`. |
| Widths | Stretched buttons look odd. A button, switch or action keeps its own width and rows pack from the left; tile grids gain columns instead. |

## Still open

1. **The key for hold.** Space is the proposal, with Shift+Enter as a
   second binding. The shell port sets it; the kit only knows `K_HOLD`.
2. **Label hue.** Teal comes from the accent table. A softer hue needs
   its own style row.
3. **The right margin at 12x24.** 66 cells leave 8 px that stay black.
   On the black screen they do not show. A coloured background would
   need the display side to fill or centre them: the scanline code
   zeroes those pixels, so that is a display change with its own timing
   check.
4. **Focus restore.** The navigation stack keeps no focus per entry.
5. **Key hints.** Whether the shipped screens show them.

## Screens

| Screen | Source | Lab frames |
|--------|--------|------------|
| HOME, host page | `app_home.c`, `app_host.c` | `mo-home*`, `mo-host*` |
| Question page | `app_ask.c` | `mo-confirm*` |
| Setup hub, sections, session sheet | `app_menu.c` | `mo-setup*`, `mo-section*`, `mo-menu*` |
| Host form | `app_profile.c` | `mo-edit*` |
| Code pad | `app_unlock.c` | `mo-unlock*` |
| Connecting, failed | `app_connect.c` | `mo-connect*`, `mo-fail*` |
| Host key | `app_hostkey.c` | `mo-hostkey*` |
| Keyboard pairing | `app_pairing.c` | `mo-pair*` |
| Wi-Fi onboarding, host import | `app_wifiprov.c`, `app_sshimport.c` | `mo-wifiprov*`, `mo-import*` |
| Status row, toast | `app_statusbar.c` | `mo-status-*` |

Notes that cost time to learn:

- A long section pages with `ui_pages` and `ui_rail`. A destructive
  action asks on the question page.
- A section may carry a list (`menu_list_t`, `app_menu_defs.h`): rows of
  data as tiles below the items, drawn like HOME's (`host_tile`). The
  items repeat on every page of tiles, and an arrow at a page edge turns
  the page (`ui_page_step`). `off` greys the whole list, `empty` is the
  text for no rows. After a row's page pops, focus returns to its tile.
  Danger items hang below the tiles on the last page. Setup > Hosts
  (frames `mo-section-hosts*`) and Setup > Wi-Fi (`mo-section-wifi*`)
  use it. A section may also carry a text panel (`menu_panel_t`): a
  `ui_info` above the items, on every page. A tile's `detail_pen` colours
  a state mark in its detail, on a plain card only. The saved-network
  page is `SCR_WIFINET` (frames `mo-net*`). Setup > Keyboard uses the
  panel, the list and a danger row together (`mo-section-kbd*`); the
  paired-keyboard page is `SCR_KBDDEV` (`mo-kbd*`).
- The code pad takes touch through the hot regions. Keys never do, and
  the screen parks focus on an unused id so no key lights while typing.
- Pairing asks on the question page, which stops the scan. The pairing
  page starts the scan again on return.
- The QR code drops out where it does not fit beside the steps (the
  Wi-Fi code at 66x20).
- The shell's status tick runs the chip announcements and expires the
  toast for every screen.

## Adding a screen

1. Sketch it in `tools/uimock/sketches-mo.js`: a draw function, an entry
   in `MO_SKETCHES`, and gate frames in `MO_GATE` for each state.
2. Port it into `sim/uilab_mo.c` with the parts. Run the gate
   (`export.mjs`, `check.mjs`, `kitcheck.mjs`) until the lab matches.
3. Write the pure helpers it needs in `ui_pure.c` with unit tests in
   `tests/ui` first.
4. Add `app_<name>.c`: a `nav_screen_t` with `input` and `render`, an
   id in `app_screens.h` and a slot in `SCREENS[]` (`cyberdeck_app.c`).
   Copy `app_ask.c`: it draws on `ui_slate_page()`, resolves input with
   `app_hot_input()` and counts `ui_faults()`.
5. Add a scenario to `tools/sim_regress.py`. Check all three fonts.
