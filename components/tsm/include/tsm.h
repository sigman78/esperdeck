/*
 * tsm -- lightweight VT terminal for ESP32-S3
 *
 * Public header.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Cell -- 8 bytes.  Binary-compatible with terminal_cell_t in display.h so a
 * direct cast (or memcpy-free pointer pass) to display_set_text_buffer() is safe.
 *
 * Layout matches terminal_cell_t exactly:
 *   offset 0  cp       uint16_t  Unicode codepoint (BMP, U+0000-U+FFFF)
 *   offset 2  fg       uint16_t  foreground RGB565
 *   offset 4  bg       uint16_t  background RGB565
 *   offset 6  attrs    uint8_t   primary attribute flags (BOLD/UNDERLINE/...)
 *   offset 7  attrs2   uint8_t   extended attribute flags + cell flags
 */
typedef struct {
    uint16_t cp;      /* Unicode codepoint (BMP)                              */
    uint16_t fg;      /* foreground RGB565, pre-converted at SGR parse time   */
    uint16_t bg;      /* background RGB565, pre-converted at SGR parse time   */
    uint8_t  attrs;   /* primary SGR attributes (see CELL_ATTR_* below)       */
    uint8_t  attrs2;  /* extended attrs + cell flags (see CELL_ATTR2_* below) */
} tsm_cell_t;

/* Primary attributes -- attrs byte (offset 6).
 * Lower nibble matches display.h ATTR_* bits for ISR compatibility. */
#define CELL_ATTR_BOLD        (1u << 0)   /* SGR 1  */
#define CELL_ATTR_UNDERLINE   (1u << 1)   /* SGR 4  */
#define CELL_ATTR_INVERSE     (1u << 2)   /* SGR 7  */
#define CELL_ATTR_BLINK       (1u << 3)   /* SGR 5  */
#define CELL_ATTR_DIM         (1u << 4)   /* SGR 2  */
#define CELL_ATTR_ITALIC      (1u << 5)   /* SGR 3  */
#define CELL_ATTR_INVISIBLE   (1u << 6)   /* SGR 8  */
#define CELL_ATTR_STRIKE      (1u << 7)   /* SGR 9  */

/* Extended attributes + cell flags -- attrs2 byte (offset 7) */
#define CELL_ATTR2_OVERLINE   (1u << 0)   /* SGR 53 */
#define CELL_ATTR2_PROTECTED  (1u << 1)   /* DECSCA -- protected from erase  */
#define CELL_ATTR2_WIDE_RIGHT (1u << 2)   /* right-half placeholder of wide glyph */

/*
 * Dirty row segment.
 * Tracks the leftmost and rightmost columns written since the last
 * tsm_clear_dirty() call.  l > r means the row is clean.
 *
 * Kept as uint8_t -- compact array-of-structs; sentinel encoding uses the
 * full byte range (0xFF / 0x00).
 */
typedef struct {
    uint8_t l;   /* leftmost dirty column (inclusive) */
    uint8_t r;   /* rightmost dirty column (inclusive); l > r means clean */
} tsm_row_dirty_t;

/* Clean sentinel values */
#define TSM_DIRTY_L_CLEAN  0xFFu
#define TSM_DIRTY_R_CLEAN  0x00u

/* Terminal dimensions */

#define TSM_COLS_MAX  220   /* practical max for 800px / 4px font */
#define TSM_ROWS_MAX  60    /* practical max for 480px / 8px font */

typedef struct tsm_s tsm_t;

/* Terminal API */

/* Allocate and initialise a new terminal of cols x rows.
 * Returns NULL on allocation failure. */
tsm_t *tsm_new(int cols, int rows);

/* Free all resources. */
void tsm_free(tsm_t *tsm);

/* Feed raw bytes from the host (VT sequences + UTF-8 text). */
void tsm_feed(tsm_t *tsm, const uint8_t *data, size_t len);

/* Direct pointer to the cell grid (cols*rows tsm_cell_t, row-major).
 * Binary-compatible with terminal_cell_t; pass directly to
 * display_set_text_buffer(). */
const tsm_cell_t *tsm_screen(const tsm_t *tsm);

/* Current cursor position and visibility. */
void tsm_cursor(const tsm_t *tsm, int *col, int *row, bool *visible);

/* Dirty row segments since last tsm_clear_dirty().
 * Returns pointer to array of rows tsm_row_dirty_t entries. */
const tsm_row_dirty_t *tsm_dirty(const tsm_t *tsm);

/* Mark all rows clean. */
void tsm_clear_dirty(tsm_t *tsm);

/* Dimensions. */
int tsm_cols(const tsm_t *tsm);
int tsm_rows(const tsm_t *tsm);

/* Full terminal reset: reinitialises the VT parser state machine and resets
 * all terminal display state to power-on defaults. */
void tsm_reset(tsm_t *tsm);

/* Returns true when DECCKM (application cursor key mode) is active. */
bool tsm_app_cursor_keys(const tsm_t *tsm);

/* Returns true when synchronized output mode (?2026) is active.
 * While true, vterm must not flush dirty rows to the display buffer. */
bool tsm_sync_update(const tsm_t *tsm);

/* Response callback */

/* Called by tsm when a terminal response must be sent to the host
 * (DA1 reply, DSR reply, CPR).  data/len are NOT NUL-terminated. */
typedef void (*tsm_response_fn_t)(const char *data, size_t len, void *user);

/* Register (or clear, if cb==NULL) the response callback. */
void tsm_set_response_cb(tsm_t *tsm, tsm_response_fn_t cb, void *user);
