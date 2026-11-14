# uimock: design the shell UI without a C build

A model of the deck's display in JavaScript. A screen is a grid of
cells; the model turns it into the same pixels the device draws. Use it
to try layouts, widgets and flows in seconds, then port the winners to
`components/ui`.

Host-only. Nothing here ships in firmware.

## Why it can be trusted

The model takes its glyphs and style tables from the C code, and two
checks compare it with `cyberdeck_uilab`:

| Check | Proves |
|-------|--------|
| `node check.mjs` | `model.js` renders every lab frame with zero differing pixels. |
| `node kitcheck.mjs` | `kit.js` rebuilds every lab screen with the same cells and touch regions. `theme.js` rebuilds every dumped style table. |

A design verdict taken from the model holds only while both pass.

For the chosen look the proof runs the other way. `MO_GATE` in
`sketches-mo.js` lists the frames the C parts must match: a sketch, its
state and a focus or finger pose. `kitcheck.mjs` fails when the lab
lacks one of them, draws an unlisted `mo-*` frame, or differs in one
cell or touch region. Change a part and its sketch together.

`node navcheck.mjs` is a third check, for the sketches: from the first
focus, the arrow keys must reach every touch region of every screen.

## Files

| File | Job |
|------|-----|
| `export.mjs` | Runs the lab for each font and fills `data/`. |
| `model.js` | Cells to pixels: fonts, style table, scanlines, phosphor modes. |
| `kit.js` | Layout, drawing surface, touch regions, Slab widgets. A port of `components/ui`. |
| `theme.js` | The style table builder. A port of `ui_theme.c`. Also holds colour pairs the lab lacks, such as `slate`. |
| `baseline.js` | Today's lab screens on `kit.js`. The starting point. |
| `sketches.js` | Redesign proposals. Round 1, the DOS-utility look (`nu-*`). It also lists every round for the viewer. |
| `sketches-mo.js` | Round 2, modern tiles (`mo-*`): the chosen look. It also holds `MO_GATE`, the frames the C parts must match. |
| `navcheck.mjs` | Fails when the arrow keys cannot reach a region of a sketch. |
| `shot.mjs` | Renders screens to PNG and prints clipped text, missing glyphs, parts with no room and small touch regions. |
| `viewer.html`, `viewer.js`, `build.mjs` | The review page; `build.mjs` writes `dist/`. |
| `inventory.md` | What the shell shows today: the redesign checklist. |

`data/` and `dist/` are generated and ignored by git.

## Loop

1. Once, and after any change to the C kit, theme or fonts: build
   `cyberdeck_uilab`, then `node export.mjs`, `node check.mjs`,
   `node kitcheck.mjs`.
2. Add or change a sketch. A new round gets its own `sketches-<round>.js`, listed in `sketches.js` and in `MODULES` in `build.mjs`.
3. `node shot.mjs` for PNG files in `dist/shots/`, or `node build.mjs`
   and open `dist/preview.html` to tap through the screens.

## shot.mjs options

| Option | Effect |
|--------|--------|
| `--font 12x24` | Font: `8x16`, `10x20` or `12x24`. |
| `--panel 800x480` | Panel preset. |
| `--theme NAME` | Force a theme. Without it a screen uses its own `theme`, else `default`. |
| `--mono 0` | Phosphor mode: 0 colour, 1 green, 2 amber. |
| `--regions` | Outline the touch regions. |
| `--state JSON` | Seed the screen's state, for example `'{"cat":4}'`. |
| `--focus ID`, `--down ID` | Pose a region as key focus or finger down. |
| `--tag TEXT` | Add TEXT to the file name. |
| `--out DIR` | Output folder. The default is `dist/shots/`. |

## Keys in the viewer

Arrow keys move the focus. Enter taps. Space, or Shift+Enter, is the
hold. Escape goes back. A screen can bind more keys with `ui.key()`:
digits on the hosts page and the code pad, Page Up and Page Down on a
paged list, typed text in a form field.

## Rules the model enforces

- One style and one accent per cell. Styles are the twelve in `kit.js`.
- A glyph the font lacks is reported, and drawn as an error cell.
- A touch region reports its size in millimetres on the chosen panel.
- Text that does not fit its rect is reported as clipped.
- A round 2 part with no room is reported. A label that cannot sit on
  the centre row of its part stops the draw with an error.

## What it cannot judge

Feel. The device polls touch every 50 ms and draws about 10 frames a
second. The lab window and the device stay the final check.
