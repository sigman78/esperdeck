/*
 * cyberdeck_ui.h — the public UI kit (extensibility item 4 / ui-spec).
 *
 * This is the drawing surface a plugin screen needs, and nothing
 * more. It has overlay primitives, the verified glyph palette, and
 * the slate parts (ui_part.h) with region-owned hit-testing. Part
 * state lives in the CALLER's struct; frame composition (clear →
 * render → chrome → present) belongs to the shell — screens draw,
 * never present.
 * Kit rule: this header must stay buildable in all three contexts
 * (device, simulator, bare Unity tests over idfsim) — no SDL, ESP-IDF
 * headers only from the idfsim-stubbed set.
 */

#pragma once

#include "display.h"     /* color_t, display_overlay_style_t */
#include "ui_hot.h"      /* ui_key_t, ids, touch + focus model */
#include "ui_layout.h"   /* rects, cuts, grids, metrics */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Overlay styling (docs/overlay-style.md): a draw call names ONE style
 * plus optionally UI_BOLD; the pen picks the accent within it. The kit
 * maps (style, pen) to a baked palette entry — screens never compose
 * color effects.
 *
 * Styles are consecutive integers, NOT flags: OR two together and the
 * result is a different, wrong style (docs/overlay-style.md). UI_BOLD
 * is the only OR-able bit; ui_putch asserts the vocabulary on both
 * builds.
 */
#define UI_TEXT    0   /* accent text on the screen background      */
#define UI_MUTED   1   /* receding text: the accent at half brightness */
#define UI_BAR     2   /* solid bar / chip / selection / QR         */
#define UI_WELL    3   /* value well: darker companion of the bar   */
#define UI_FOCUS   4   /* focused / lit bar: pastel wash, dark text */
#define UI_TRACK   5   /* gauge track: medium white on dark tint    */
/* Edge styles carry a surface colour as INK on the screen background,
 * for the half-block cells (UI_HALF_*) that end a surface mid-cell. */
#define UI_BAR_EDGE    6
#define UI_WELL_EDGE   7
#define UI_FOCUS_EDGE  8
#define UI_PANEL       9   /* accent text on the dark neutral panel    */
#define UI_PANEL_MUTED 10  /* receding text on the panel               */
#define UI_PANEL_EDGE  11
#define UI_FOCUS_MUTED 12  /* receding text on a UI_FOCUS wash         */
#define UI_STYLE_COUNT 13
#define UI_STYLE_MASK  0x0F
#define UI_BOLD    (1 << 7)   /* bold glyph face; composes with any style */

/* Pen accents — index within a style's palette bank. */
#define OVERLAY_COL_DEFAULT   0
#define OVERLAY_COL_GREEN     1
#define OVERLAY_COL_CYAN      2
#define OVERLAY_COL_MAGENTA   3
#define OVERLAY_COL_AMBER     4
#define OVERLAY_COL_RED       5
#define OVERLAY_COL_BLUE      6
#define OVERLAY_COL_WHITE     7
#define OVERLAY_ACCENTS       8

#define UI_PAL_COUNT (UI_STYLE_COUNT * OVERLAY_ACCENTS)

/* Every codepoint below is present in Terminus at all three sizes
 * (verified against the font's range tables). Missing glyphs render as
 * '?', so stick to these — or verify before adding. */
#define UI_BLOCK  0x2588u  /* █ full block   */
#define UI_SHADE3  0x2593u  /* ▓ dark shade   */
#define UI_SHADE2  0x2592u  /* ▒ medium shade */
#define UI_SHADE1  0x2591u  /* ░ light shade  */
#define UI_LHALF   0x258Cu  /* ▌ left half  */
#define UI_RHALF   0x2590u  /* ▐ right half */
#define UI_QUAD_LL 0x2596u  /* ▖ lower-left quadrant */
#define UI_QUAD_UL 0x2598u  /* ▘ upper-left quadrant */
#define UI_QUAD_LR 0x2597u  /* ▗ lower-right quadrant */
#define UI_QUAD_UR 0x259Du  /* ▝ upper-right quadrant */
#define UI_HALF_UP 0x2580u  /* ▀ upper half: a surface's bottom edge */
#define UI_HALF_DN 0x2584u  /* ▄ lower half: a surface's top edge    */
#define UI_CHECK   0x2713u  /* check mark */
#define UI_CROSS   0x2718u  /* heavy ballot x */
#define UI_HEAVY_H 0x2501u  /* ━ the switch line */
#define UI_VLINE   0x2502u  /* │ light vertical line */
#define UI_LOW_1   0x2581u  /* ▁ lower eighth block */
#define UI_CHEVRON 0x203Au  /* › */
#define UI_ELLIPSIS 0x2026u /* … */
#define UI_LED_ON  0x25CFu  /* ● */
#define UI_LED_OFF 0x25CBu  /* ○ */
#define UI_DIAMOND 0x25C6u  /* ◆ */
#define UI_PLAY    0x25B6u  /* ▶ */
#define UI_ARROW   0x25BAu  /* ► */
#define UI_POINT_L 0x25C4u  /* ◄ */
#define UI_VBAR    0x25AEu  /* ▮ */
#define UI_BULLET  0x2022u  /* • */

int  ui_cols(void);
int  ui_rows(void);

/** Set the accent color (OVERLAY_COL_*) for subsequent draws; reset to
 *  default by ui_clear()/ui_dim(). */
void ui_pen(uint8_t color);

/** Declare this frame's backdrop a scrim: the render dims the terminal
 *  under every transparent cell. For modals over a session. Writes no
 *  cells, so draw order around it does not matter. ui_clear() resets it. */
void ui_dim(void);

/** Put one codepoint; style = one UI_* style, optionally | UI_BOLD. */
void ui_putch(int col, int row, uint16_t cp, uint8_t style);

/** Put an ASCII/Latin-1 string (byte = codepoint, no UTF-8 decode). */
void ui_puts(int col, int row, const char *s, uint8_t style);

/** UTF-8 variant: decodes multi-byte sequences to BMP codepoints
 *  (beyond-BMP and malformed input render '?'). Chrome glyphs should
 *  still prefer the verified UI_* palette above. */
void ui_puts_u8(int col, int row, const char *s, uint8_t style);

/** printf into a row (ASCII), truncated to the overlay width. */
void ui_printf(int col, int row, uint8_t style, const char *fmt, ...);

/** Fill a rectangle with spaces (opaque background). */
void ui_fill(int col, int row, int w, int h, uint8_t style);

/** Chip: optional caps around a " text " bar in the current pen.
 *  @p style adds to the bar (UI_BOLD, or UI_FOCUS to replace it).
 *  Returns the column after the chip. */
int ui_chip(int col, int row, uint16_t left_cp, const char *text,
            uint16_t right_cp, uint8_t style);

/* Accumulate-then-floor drag converter (ui-spec touch rule 4). The
 * touch task polls at 50 ms, so converting each small dy alone floors
 * to zero. A slow drag would otherwise never scroll. The converter
 * accumulates travel in pixel-percent (scaled by
 * CONFIG_INPUT_TOUCH_SCROLL_SPEED_PCT), spends whole rows, and keeps
 * the remainder. */
typedef struct { int accum; } ui_drag_t;

/** Feed @p dy pixels of travel; returns whole rows (@p row_px each). */
int  ui_drag_rows(ui_drag_t *d, int dy, int row_px);

/** Returns one of 8 braille glyphs, indexed by @p frame mod 8. */
uint16_t spinner_glyph(uint32_t frame);

/** Braille "noise" glyph from a hash (never blank). */
uint16_t braille_noise(uint32_t h);

/** Stable per-profile accent color from a djb2 hash of the name —
 *  identity accents draw from {green, cyan, magenta, amber, blue};
 *  red/white/default stay reserved for alert/focus/body (ui-spec). */
uint8_t prof_accent(const char *name);

/** Scrollback fill down the right edge, line count on gray above it.
 *  @p offset is rows back from live, @p total the history available. */
void draw_scrollbar(int offset, int total);

#include "ui_widgets.h"
#include "ui_part.h"
