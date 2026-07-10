/*
 * termstate — internal terminal model
 *
 * Not part of the public API; included only by termstate.c.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "tsm.h"
#include "vtparse.h"
#include "charsets.h"
#include "color.h"

#include "esp_heap_caps.h"

/* ── Scrollback ──────────────────────────────────────────────────────────── */

#define TSM_SCROLLBACK_ROWS  0   /* disabled by default; stub only */

/* ── Cursor save/restore slot ────────────────────────────────────────────── */

typedef struct {
    int          col;
    int          row;
    uint8_t      attrs;
    uint8_t      attrs2;
    uint16_t     fg;
    uint16_t     bg;
    charset_id_t g0;
    charset_id_t g1;
    int          gl;   /* active GL index: 0=G0, 1=G1 */
} tsm_cursor_save_t;

/* ── Mode flags ──────────────────────────────────────────────────────────── */

typedef struct {
    bool lnm;        /* LNM — line feed / new line mode          */
    bool irm;        /* IRM — insert / replace mode              */
    bool decom;      /* DECOM — origin mode (scroll region)      */
    bool decawm;     /* DECAWM — auto-wrap mode (default on)     */
    bool dectcem;    /* DECTCEM — cursor visible (default on)    */
    bool decalt;     /* DECALT — alt screen active               */
    bool decckm;     /* DECCKM — application cursor key mode     */
    bool bracketed;  /* bracketed paste mode (stub)              */
    bool mouse_x10;  /* mouse X10 reporting (stub)               */
    bool mouse_btn;  /* mouse button reporting (stub)            */
    bool sync_update;  /* ?2026 — synchronized output: freeze renderer */
} tsm_mode_t;

/* ── Terminal struct ─────────────────────────────────────────────────────── */

struct tsm_s {
    int cols;
    int rows;

    /* Primary and alternate screen cell grids (heap-allocated) */
    tsm_cell_t *cells;      /* active screen — cols*rows */
    tsm_cell_t *alt_cells;  /* alt screen   — cols*rows */

    /* Dirty tracking */
    tsm_row_dirty_t *dirty;  /* rows entries */

    /* Cursor */
    int  cx;            /* cursor column (0-based) */
    int  cy;            /* cursor row    (0-based) */
    bool pending_wrap;  /* next print triggers newline (auto-wrap pending) */

    /* Current SGR state */
    uint8_t  attrs;
    uint8_t  attrs2;
    uint16_t fg;
    uint16_t bg;

    /* Character sets */
    charset_id_t g[2];   /* G0, G1 */
    int          gl;     /* active: 0=G0, 1=G1 */

    /* Scroll region (row indices, inclusive, 0-based) */
    int scroll_top;
    int scroll_bot;

    /* Saved cursor (DECSC/DECRC and DEC alt-screen auto-save) */
    tsm_cursor_save_t saved;
    tsm_cursor_save_t alt_saved;

    /* Mode flags */
    tsm_mode_t mode;

    /* OSC title buffer (last received OSC 0/2) */
    char title[64];

    /* VT parser */
    vtparse_t vtp;

    /* Response callback (DA1, DSR, CPR) */
    tsm_response_fn_t response_cb;
    void             *response_user;
};

/* ── Internal helpers (used only within termstate.c) ─────────────────────── */

/* Clamp integer value to [lo, hi]. */
static inline int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
