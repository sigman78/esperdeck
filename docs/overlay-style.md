# Overlay styling: a constrained palette, not composable attributes

**Status: adopted 2026-08-28.** Replaces the `INVERSE`/`DIM`/`BRIGHT`
overlay attribute scheme. Companion to `ui-spec.md` (which locks *what
the parts look like*; this doc locks *how a look is named and resolved*).

## The regression that forced the question

The scrollback indicator vanished after PR #12. Its widget drew the fill
with `BRIGHT|DIM` — a combination no other call site used — relying on
`DIM` being a no-op on opaque text cells. PR #12 gave `DIM` a meaning
there (muted accent text), and the two effects collided: `DIM` halved
white to RGB565 `0x7BEF`, `BRIGHT` washed a black background *to the
same value*. Foreground equaled background; the thumb rendered
invisible.

The bug was not in either change. It was in the model: attribute bits
whose meaning depended on context (transparent cell, solid bar, plain
text), with undefined combinations that rendered *something* instead of
being rejected. Any later change that defined a previously-undefined
combination could break a caller squatting on its accidental behavior —
and did.

## The model

Styling is **choosing a palette entry**, never composing modifiers.

- The display component holds a flat table of `{fg, bg}` RGB565 pairs —
  the **overlay palette** — registered by the app
  (`display_set_overlay_palette`). Display knows indices, not names. Per
  cell it stores `{codepoint, attrs, pal}` (4 bytes, unchanged size) and
  the ISR (interrupt service routine) render resolves a cell with one
  table load. All color math left the ISR.
- The app owns the palette content. `ui_theme_build()` computes every
  entry from the screen's `(fg, bg)` theme colors once, at
  `ui_colors()` time — the companion-bar tones, pale text tints, and
  focus washes that used to be per-cell ISR arithmetic live there now,
  verbatim.
- Exactly one per-cell attribute remains, with one meaning and no
  interaction terms:
  - `OVERLAY_ATTR_BOLD` — use the bold glyph face; colors untouched.
- The modal backdrop dim is per-frame overlay state, not a cell
  attribute. `ui_dim()` sets a flag, and `display_set_overlay_buffer()`
  publishes it with the buffer. The scrim was global in every real
  frame: a modal dims its whole backdrop, session chrome never dims.
  Per-cell encoding bought only a per-cell load in the ISR and a
  full-buffer write per modal frame. Frame state also retires the old
  "honored on transparent cells only" caveat, which can no longer be
  expressed.

`INVERSE`, `DIM`, and `BRIGHT` are gone. There is nothing to combine,
so no combination can be undefined.

## The app-side vocabulary

Screens do not pick raw palette indices. `cyberdeck_ui.h` names six
**styles** — the surfaces the locked rendition actually uses — and keeps
the accent **pen** (`ui_pen`, `OVERLAY_COL_*`) orthogonal to them:

| Style      | Meaning (ui-spec part)                                | Resolution                                     |
|------------|-------------------------------------------------------|------------------------------------------------|
| `UI_TEXT`  | accent text on the screen background                  | bright accent fg, screen bg                    |
| `UI_MUTED` | receding text (inactive indicator, empty slot)        | accent at half brightness, screen bg           |
| `UI_BAR`   | solid bar / chip / selection / QR                     | muted companion tone bg, pale derived text     |
| `UI_WELL`  | value well (darker companion of the bar beside it)    | ~60% companion bg, pale derived text           |
| `UI_FOCUS` | focused / lit bar                                     | pastel wash of the bar tone, dark text         |
| `UI_TRACK` | gauge track (scrollback fill)                         | medium white on dark neutral tint              |

A draw call passes **one** style plus optionally `UI_BOLD`. The kit maps
`(style, pen)` to the palette index (`style * 8 + accent`), and
`ui_putch` asserts the vocabulary on both builds. One hazard survives
the assert: styles are small consecutive integers, so OR-ing two can
land on another in-range style — `UI_MUTED|UI_FOCUS` *is* `UI_TRACK`,
`UI_BAR|UI_MUTED` *is* `UI_WELL` — and renders clean and wrong. No
check can catch that. One style per call is therefore a hard rule,
not a convention. Blinking is not a
render concept: a blinking element is the app choosing a different
style on alternate animation frames (`blink ? UI_BAR : UI_TEXT`).

`UI_TRACK` is the entry this regression was missing. The scrollbar
needed "uniform tinted column, medium-contrast fill" and had no way to
say it, so it synthesized the look from side effects. Now it is a row
in the table like everything else.

## Why constrained beats composable here

- **Illegal states are unrepresentable.** A style is an enum value, not
  a bit soup. The `BRIGHT|DIM` class of bug cannot be expressed. (The
  one residual: OR-ing two styles aliases to a wrong-but-valid one —
  see the vocabulary rule above.)
- **The rendition is finite, so the palette should be too.** ui-spec
  locked a fixed set of looks after five mockup rounds. An open
  combination space promises flexibility nobody may use, and charges
  for it in undefined corners.
- **Retheming is a table edit.** Every look resolves in one place. A
  restyle (like PR #12 intended) changes builder rows and can no longer
  break a distant caller.
- **The invariant became testable.** `tests/theme` asserts `fg != bg`
  over every palette entry on the host. The exact failure that shipped
  is now a red unit test.
- **The ISR got simpler.** Style resolution used to branch and shift
  per cell inside the bounce ISR, which renders with the flash cache
  off. Now it is a DRAM (internal data RAM) table load. Less IRAM
  (instruction RAM) code, fewer cycles per cell.

## What INVERSE actually was, for the record

It never inverted. On this palette-indexed overlay there are no per-cell
colors to swap; the bit selected a different palette *family* (companion
bar tones) plus a text-derivation rule. That is a style choice wearing
an attribute's name — the naming confusion that kept this scheme
looking composable long after it stopped being so. The terminal layer's
`ATTR_REVERSE` is unaffected: terminal cells carry real per-cell colors
and SGR (Select Graphic Rendition) semantics require the swap there.
