/*
 * app_ui.h — overlay TUI primitives (internal to cyberdeck_app). All shell
 * UI draws into the display overlay layer; the vterm cell buffer belongs to
 * the SSH session alone.
 */

#pragma once

#include "display.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* Unicode box-drawing codepoints (present in terminus8x16).
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

/* Cyberpunk glyph palette — every codepoint below is present in Terminus
 * 8x16 (verified against the font's range table). Missing glyphs render as
 * '?', so stick to these. */
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
#define UI_VBAR    0x25AEu  /* ▮ */
#define UI_BULLET  0x2022u  /* • */
#define UI_PL_R    0xE0B0u  /* powerline right-facing separator */
#define UI_PL_L    0xE0B2u  /* powerline left-facing separator  */

/** Allocate the overlay buffer for the current display size. */
esp_err_t ui_init(void);

int  ui_cols(void);
int  ui_rows(void);

/** Publish the drawn frame (double-buffered, atomic) and swap. */
void ui_present(void);
/** Unregister the overlay so the terminal buffer shows through. */
void ui_hide(void);
bool ui_visible(void);

/** Park the terminal cursor off-screen (call in full-screen modals). */
void ui_no_cursor(void);

/** Set the two overlay colors (all cells share them; INVERSE swaps). */
void ui_colors(color_t fg, color_t bg);

/** Set the accent color (OVERLAY_COL_*) for subsequent draws; reset to
 *  default by ui_clear()/ui_dim(). */
void ui_pen(uint8_t color);

/** Clear the whole overlay to transparent. */
void ui_clear(void);

/** Transparent DIM scrim fading the session behind it; draw opaque chrome
 *  on top afterwards. For modals over a session. */
void ui_dim(void);

/** Put one codepoint; attrs = 0 or OVERLAY_ATTR_INVERSE. */
void ui_putch(int col, int row, uint16_t cp, uint8_t attrs);

/** Put an ASCII/Latin-1 string. */
void ui_puts(int col, int row, const char *s, uint8_t attrs);

/** printf into a row (ASCII), truncated to the overlay width. */
void ui_printf(int col, int row, uint8_t attrs, const char *fmt, ...);

/** Horizontal rule: left_cp fill_cp... right_cp */
void ui_hline(int col, int row, int width,
              uint16_t left_cp, uint16_t fill_cp, uint16_t right_cp);

/** Fill a rectangle with spaces (opaque background). */
void ui_fill(int col, int row, int w, int h, uint8_t attrs);

/** Box with border and title centered in the top rule. */
void ui_box(int col, int row, int w, int h, const char *title);

/** Chip: optional caps around " text " INVERSE, in the current pen.
 *  Returns the column after the chip. */
int ui_chip(int col, int row, uint16_t left_cp, const char *text,
            uint16_t right_cp, uint8_t attrs);

/** Finger-sized tile: a solid bar in the pen color with a bold title and an
 *  optional body line. Selection washes the bar pastel; an overlong body
 *  bounce-scrolls while selected. */
void ui_tile(int col, int row, int w, int h,
             const char *title, const char *body, bool selected);

/** Feed the ~10 fps animation frame counter (drives the tile marquee). */
void ui_frame(uint32_t frame);

/** Single-line text-entry field with a block cursor at @p cursor when
 *  @p focused; scrolls to keep the cursor visible. @p mask renders '*'. */
void ui_field(int col, int row, int width, const char *text,
              int cursor, bool focused, bool mask);
