/*
 * cyberdeck_ui.h — the public UI kit (extensibility item 4 / ui-spec).
 *
 * This is the drawing surface a plugin screen needs, and nothing
 * more. It has overlay primitives, the verified glyph palette, and
 * the touch-first parts (tile grid, list, buttons) with widget-owned
 * hit-testing. Part state lives in the CALLER's struct; frame
 * composition (clear → render →
 * chrome → present) belongs to the shell — screens draw, never present.
 * Kit rule: this header must stay buildable in all three contexts
 * (device, simulator, bare Unity tests over idfsim) — no SDL, ESP-IDF
 * headers only from the idfsim-stubbed set.
 */

#pragma once

#include "display.h"     /* color_t, display_overlay_style_t */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Overlay styling (docs/overlay-style.md): a draw call names ONE style
 * plus optionally UI_BOLD; the pen picks the accent within it. The kit
 * maps (style, pen) to a baked palette entry — screens never compose
 * color effects.
 */
#define UI_TEXT    0   /* accent text on the screen background      */
#define UI_MUTED   1   /* receding text: the accent at half brightness */
#define UI_BAR     2   /* solid bar / chip / selection / QR         */
#define UI_WELL    3   /* value well: darker companion of the bar   */
#define UI_FOCUS   4   /* focused / lit bar: pastel wash, dark text */
#define UI_TRACK   5   /* gauge track: medium white on dark tint    */
#define UI_STYLE_COUNT 6
#define UI_STYLE_MASK  0x07
#define UI_BOLD    (1 << 3)   /* bold glyph face; composes with any style */

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
 * '?', so stick to these — or verify before adding.
 * Frame language: ROUNDED single-line corners for passive info panels
 * (ui_box), double-line squares (UI_D*) for tappable tiles (ui_tile). */
#define UI_BOX_H   0x2500u  /* ─ */
#define UI_BOX_V   0x2502u  /* │ */
#define UI_BOX_TL  0x256Du  /* ╭ */
#define UI_BOX_TR  0x256Eu  /* ╮ */
#define UI_BOX_BL  0x2570u  /* ╰ */
#define UI_BOX_BR  0x256Fu  /* ╯ */
#define UI_BOX_ML  0x251Cu  /* ├ */
#define UI_BOX_MR  0x2524u  /* ┤ */
#define UI_DH      0x2550u  /* ═ double horizontal */
#define UI_DV      0x2551u  /* ║ double vertical   */
#define UI_DTL     0x2554u  /* ╔ */
#define UI_DTR     0x2557u  /* ╗ */
#define UI_DBL     0x255Au  /* ╚ */
#define UI_DBR     0x255Du  /* ╝ */
#define UI_DML     0x2560u  /* ╠ */
#define UI_DMR     0x2563u  /* ╣ */
#define UI_BLOCK   0x2588u  /* █ full block   */
#define UI_SHADE3  0x2593u  /* ▓ dark shade   */
#define UI_SHADE2  0x2592u  /* ▒ medium shade */
#define UI_SHADE1  0x2591u  /* ░ light shade  */
#define UI_LHALF   0x258Cu  /* ▌ left half  */
#define UI_RHALF   0x2590u  /* ▐ right half */
#define UI_LED_ON  0x25CFu  /* ● */
#define UI_LED_OFF 0x25CBu  /* ○ */
#define UI_DIAMOND 0x25C6u  /* ◆ */
#define UI_PLAY    0x25B6u  /* ▶ */
#define UI_ARROW   0x25BAu  /* ► */
#define UI_POINT_L 0x25C4u  /* ◄ */
#define UI_VBAR    0x25AEu  /* ▮ */
#define UI_BULLET  0x2022u  /* • */
#define UI_PL_R    0xE0B0u  /* powerline right-facing separator */
#define UI_PL_L    0xE0B2u  /* powerline left-facing separator  */

/* Decoded UI keys — decoded once by the core, screens get the result. */
typedef enum {
    K_NONE = 0, K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_ENTER, K_ESC, K_F12, K_CHAR, K_BACKSPACE, K_TAB,
    K_SCROLL_UP, K_SCROLL_DOWN,
} ui_key_t;

int  ui_cols(void);
int  ui_rows(void);

/** Set the accent color (OVERLAY_COL_*) for subsequent draws; reset to
 *  default by ui_clear()/ui_dim(). */
void ui_pen(uint8_t color);

/** Transparent scrim fading the session behind it; draw opaque chrome
 *  on top afterwards. For modals over a session. */
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

/** Horizontal rule: left_cp fill_cp... right_cp */
void ui_hline(int col, int row, int width,
              uint16_t left_cp, uint16_t fill_cp, uint16_t right_cp);

/** Fill a rectangle with spaces (opaque background). */
void ui_fill(int col, int row, int w, int h, uint8_t style);

/** Box with border and title centered in the top rule. */
void ui_box(int col, int row, int w, int h, const char *title);

/** Chip: optional caps around a " text " bar in the current pen.
 *  @p style adds to the bar (UI_BOLD, or UI_FOCUS to replace it).
 *  Returns the column after the chip. */
int ui_chip(int col, int row, uint16_t left_cp, const char *text,
            uint16_t right_cp, uint8_t style);

/** Finger-sized tile: a solid bar in the pen color with a bold title and an
 *  optional body line. Selection washes the bar pastel; an overlong body
 *  bounce-scrolls while selected. */
void ui_tile(int col, int row, int w, int h,
             const char *title, const char *body, bool selected);

/** Single-line text-entry field with a block cursor at @p cursor when
 *  @p focused; scrolls to keep the cursor visible. @p mask renders '*'. */
void ui_field(int col, int row, int width, const char *text,
              int cursor, bool focused, bool mask);

/* A page of finger-sized tiles laid out in a grid, with two-axis touch
 * hit-testing. Recomputed by each render and saved for the tap handler.
 * All dimensions are in character cells. */
typedef struct {
    int x0, y0;        /* top-left cell of the grid            */
    int tw, th;        /* tile size in cells                   */
    int gx, gy;        /* gutter between tiles, in cells       */
    int ncols, nrows;  /* tiles per page                       */
    int count;         /* live tiles on this page (<= ncols*nrows) */
} tilegrid_t;

/** Cell coordinates of tile @p slot's top-left corner. */
int tile_x(const tilegrid_t *g, int slot);
int tile_y(const tilegrid_t *g, int slot);

/** Map a touch pixel to a tile slot, or -1 for a gutter / margin / empty
 *  cell (two-axis hit-test). */
int tile_hit(const tilegrid_t *g, int px, int py);

/** Keyboard navigation within the grid (arrow keys). */
int tile_nav(const tilegrid_t *g, int sel, ui_key_t k);

/** Shared full-screen picker grid (HOME + PAIRING). */
tilegrid_t picker_grid(int count);

/* Accumulate-then-floor drag converter (ui-spec touch rule 4). The
 * touch task polls at 50 ms, so converting each small dy alone floors
 * to zero. A slow drag would otherwise never scroll. The converter
 * accumulates travel in pixel-percent (scaled by
 * CONFIG_INPUT_TOUCH_SCROLL_SPEED_PCT), spends whole rows, and keeps
 * the remainder. */
typedef struct { int accum; } ui_drag_t;

/** Feed @p dy pixels of travel; returns whole rows (@p row_px each). */
int  ui_drag_rows(ui_drag_t *d, int dy, int row_px);
void ui_drag_reset(ui_drag_t *d);

/* Scrolling list of fixed-height rows — never drops overflow. The part
 * owns geometry, scroll, hit and arrow-nav; the CALLER owns the struct
 * and draws the rows (ui_tile per row keeps the house look) at
 * ui_list_row_y(). */
typedef struct {
    int x, y, w, h;        /* body rect, in cells                       */
    int row_h;             /* cell-rows per row, incl. a 1-row gutter   */
    int count;             /* model rows                                */
    int sel;               /* selected row                              */
    int top;               /* first visible row                         */
    ui_drag_t drag;        /* vertical drag accumulator                 */
} ui_list_t;

/** Rows that fit the rect. */
int  ui_list_visible(const ui_list_t *l);
/** Clamp sel/top into range and scroll sel into view. */
void ui_list_clamp(ui_list_t *l);
/** Top cell row of @p idx, or -1 while it stays off-screen. */
int  ui_list_row_y(const ui_list_t *l, int idx);
/** Row index at a touch pixel, or -1 (widget-owned hit-testing). */
int  ui_list_hit(const ui_list_t *l, int px, int py);
/** Arrow/page navigation; true = selection moved. */
bool ui_list_nav(ui_list_t *l, ui_key_t k);
/** Drag travel in pixels; returns rows scrolled (sel follows the view). */
int  ui_list_scroll(ui_list_t *l, int dy_px);
/** Right-edge overflow cue inside the rect; no-op when all rows fit. */
void ui_list_draw_scroll(const ui_list_t *l);

/** Centered @p count-button action bar (Save/Cancel, Trust/Cancel...).
 *  One row of @p tw x @p th tiles with 4-cell gutters, top row @p y0.
 *  Hit-test and arrow-nav via the tile_* helpers as usual. */
tilegrid_t ui_button_bar(int y0, int count, int tw, int th);

/** One bar button, drawn in the current pen. */
void ui_button(const tilegrid_t *g, int slot, const char *label,
               const char *body, bool sel);

/** Title chip framed by a shade gradient, drawn on row 0. */
void draw_titlebar(int x0, const char *text);

/** Standard modal header: titlebar chip + right-aligned "// tag" + rule. */
void draw_screen_header(const char *title, const char *tag);

/** Animated cyan ░▒▓█ comet sweeping an otherwise empty row. */
void draw_rule(int row);

/** Returns one of 8 braille glyphs, indexed by @p frame mod 8. */
uint16_t spinner_glyph(uint32_t frame);

/** Braille "noise" glyph from a hash (never blank). */
uint16_t braille_noise(uint32_t h);

/** Wall-clock "HH:MM" once SNTP/host time exists; false until then. */
bool clock_str(char *buf, size_t sz);

/** Stable per-profile accent color from a djb2 hash of the name —
 *  identity accents draw from {green, cyan, magenta, amber, blue};
 *  red/white/default stay reserved for alert/focus/body (ui-spec). */
uint8_t prof_accent(const char *name);

/** QR module sampler: true = dark module at (x,y). */
typedef bool (*qr_module_fn)(int x, int y);

/** Draw a QR top-right as half-block cells; returns its first column (the
 *  caller's text limit), or ui_cols() when it did not fit. */
int draw_qr_panel(int qsz, qr_module_fn mod, const char *caption);

/** Numbered onboarding step row; returns the next step row. */
int draw_step(int y, char num, const char *label,
              const char *value, uint8_t value_pen, int xlimit);

/** Scrollback fill down the right edge, line count on gray above it.
 *  @p offset is rows back from live, @p total the history available. */
void draw_scrollbar(int offset, int total);
