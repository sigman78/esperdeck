/*
 * app_widgets.h — shared shell chrome + tile-grid layout (internal).
 *
 * Everything here draws with the app_ui primitives and reads the shared
 * state only for the animation clock (app.anim_frame) and cfg.
 */

#pragma once

#include "app_internal.h"

/* ------------------------------------------------------------ tile grid */

/** Cell coordinates of tile @p slot's top-left corner. */
int tile_x(const tilegrid_t *g, int slot);
int tile_y(const tilegrid_t *g, int slot);

/** Map a touch pixel to a tile slot, or -1 for a gutter / margin / empty
 *  cell (two-axis hit-test). */
int tile_hit(const tilegrid_t *g, int px, int py);

/** Keyboard navigation within the grid (arrow keys). */
int tile_nav(const tilegrid_t *g, int sel, ui_key_t k);

/** Shared full-screen picker grid (HOME + PAIRING + menu pickers + EFFECTS). */
tilegrid_t picker_grid(int count);

/* ------------------------------------------------------------ chrome */

/** Animated cyan ░▒▓█ comet sweeping an otherwise empty row. */
void draw_rule(int row);

/** 8-frame braille spinner glyph. */
uint16_t spinner_glyph(uint32_t frame);

/** Braille "noise" glyph from a hash (never blank). */
uint16_t braille_noise(uint32_t h);

/** Wall-clock "HH:MM" once SNTP/host time exists; false until then. */
bool clock_str(char *buf, size_t sz);

/** Title chip framed by a shade gradient, drawn on row 0. */
void draw_titlebar(int x0, const char *text);

/** Footer hint riding a cyan powerline chip; @p limit = first column to
 *  stay clear of (right-aligned toast), or -1 for full width. */
void draw_footer_lim(const char *hint, int limit);
void draw_footer(const char *hint);

/** Standard modal header: titlebar chip + right-aligned "// tag" + rule. */
void draw_screen_header(const char *title, const char *tag);

/** Free-RAM summary for the header. */
void ram_stats(char *buf, size_t sz);

/** Stable per-profile accent color from a djb2 hash of the name. */
uint8_t prof_accent(const char *name);

/* ------------------------------------------------------- status strings */

const char *wifi_status_str(void);
const char *ble_status_str(void);

/* ------------------------------------------------- QR / onboarding steps */

/** QR module sampler: true = dark module at (x,y). */
typedef bool (*qr_module_fn)(int x, int y);

/** Draw a QR top-right as half-block cells; returns its first column (the
 *  caller's text limit), or ui_cols() when it did not fit. */
int draw_qr_panel(int qsz, qr_module_fn mod, const char *caption);

/** Numbered onboarding step row; returns the next step row. */
int draw_step(int y, char num, const char *label,
              const char *value, uint8_t value_pen, int xlimit);

/* ------------------------------------------------- scrollback indicator */

/** Scrollback fill down the right edge, line count on gray above it.
 *  @p offset is rows back from live, @p total the history available.
 *
 *  A fill rather than a scrollbar: a proportional thumb would be under one
 *  cell tall for most of its travel at 30 rows against 1000 lines. The
 *  boundary lands to an eighth of a cell, so ~4 lines per step. */
void draw_scrollbar(int offset, int total);
