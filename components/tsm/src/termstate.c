/*
 * termstate.c — VT terminal model
 *
 * Handles the full state machine for a VT100/VT220/xterm-compatible terminal:
 *   - SGR attributes (bold, dim, italic, underline, blink, inverse, …)
 *   - 16/256/truecolor foreground and background
 *   - Cursor movement and scrolling
 *   - Primary and alternate screen buffers
 *   - DECSC/DECRC save/restore
 *   - Scroll region (DECSTBM)
 *   - Character set designation (G0/G1: ASCII, DEC Special Graphics)
 *   - Insert / line / character erase operations
 *   - Auto-wrap mode (DECAWM)
 *   - Origin mode (DECOM)
 *   - OSC 0/2 title (stored locally; no OS hook)
 *
 * Mouse reporting: stubs only; see TODO: MOUSE below.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "termstate.h"

/* Bench instrumentation (parse-vs-state split + scroll accounting) — see
 * tsm.h tsm_bench_t. Host test build never defines CONFIG_VTERM_BENCH, so
 * this (and esp_cpu.h) compiles away entirely there. */
#ifdef CONFIG_VTERM_BENCH
#include "esp_cpu.h"
static tsm_bench_t s_tsm_bench;
#endif

/* ── Response helper ─────────────────────────────────────────────────────── */

static void send_response(tsm_t *t, const char *s, int len)
{
    if (t->response_cb)
        t->response_cb(s, (size_t)len, t->response_user);
}

void tsm_set_response_cb(tsm_t *t, tsm_response_fn_t cb, void *user)
{
    t->response_cb   = cb;
    t->response_user = user;
}

/* ── Utilities ────────────────────────────────────────────────────────────── */

/* Resolve a CSI parameter; return def if the slot is -1 (omitted). */
static inline int32_t param(const int32_t *params, int nparams, int i, int32_t def)
{
    if (i >= nparams || params[i] < 0) return def;
    return params[i];
}

/* Physical row of logical row r under the ring. base and r are both in
 * [0, rows), so one conditional subtract replaces the modulo. */
static inline int phys_row(const tsm_t *t, int r)
{
    int p = r + t->base;
    if (p >= t->rows) p -= t->rows;
    return p;
}

/* Pointer to the first cell of logical row on the active screen. */
static inline tsm_cell_t *row_ptr(tsm_t *t, int row)
{
    return &t->cells[phys_row(t, row) * t->cols];
}

/* Pointer to cell (col, row) on the active screen. */
static inline tsm_cell_t *cell_at(tsm_t *t, int col, int row)
{
    return row_ptr(t, row) + col;
}

static inline void mark_dirty(tsm_t *t, int row, int l, int r)
{
    if (t->dirty[row].l > (uint8_t)l) t->dirty[row].l = (uint8_t)l;
    if (t->dirty[row].r < (uint8_t)r) t->dirty[row].r = (uint8_t)r;
}

static inline void mark_row_dirty(tsm_t *t, int row)
{
    mark_dirty(t, row, 0, t->cols - 1);
}

/* ── Blank cell using current SGR ────────────────────────────────────────── */

static inline tsm_cell_t blank_cell(tsm_t *t)
{
    return (tsm_cell_t){ .cp = ' ', .fg = t->fg, .bg = t->bg,
                         .attrs = 0, .attrs2 = 0 };
}

/* ── Screen fill / erase ─────────────────────────────────────────────────── */

/* Erase columns [l, r] on row with current bg color. */
static inline void erase_range(tsm_t *t, int row, int l, int r)
{
    tsm_cell_t b = blank_cell(t);
    for (int c = l; c <= r; c++)
        *cell_at(t, c, row) = b;
    mark_dirty(t, row, l, r);
}

static inline void erase_row(tsm_t *t, int row)
{
    erase_range(t, row, 0, t->cols - 1);
}

static void erase_screen(tsm_t *t)
{
    for (int r = 0; r < t->rows; r++) erase_row(t, r);
}

/* ── Scrolling ───────────────────────────────────────────────────────────── */

/* sb_clear() does not touch cell data; sb_len = 0 makes it unreachable. */
static void sb_clear(tsm_t *t)
{
    t->sb_len  = 0;
    t->sb_head = 0;
    t->sb_off  = 0;
}

/* Copy one logical row into the scrollback ring, oldest overwritten first. */
static void sb_push_row(tsm_t *t, int row)
{
    memcpy(&t->sb_cells[(size_t)t->sb_head * (size_t)t->cols],
           row_ptr(t, row), (size_t)t->cols * sizeof(tsm_cell_t));
    if (++t->sb_head >= t->sb_max) t->sb_head = 0;
    if (t->sb_len < t->sb_max) t->sb_len++;

    /* Follow the ring so a held view keeps showing the same content as new
     * output arrives. At sb_off == sb_len history is sliding out from under
     * it and there is nowhere further back to hold. */
    if (t->sb_off > 0 && t->sb_off < t->sb_len) t->sb_off++;
}

/* Scroll [top, bot] up by n lines; new lines at bottom are blank.
 *
 * Full-region scroll rotates the row ring — O(1) bookkeeping plus n erased
 * rows instead of a (span-n)*cols*8 B memmove. A partial region (DECSTBM,
 * IL/DL) copies per logical row. Once base != 0, the physical rows are not
 * contiguous, so the old flat memmove would be wrong. Distinct logical rows
 * never alias physically, so memcpy per row is safe. */
static void scroll_up(tsm_t *t, int n)
{
    if (n <= 0) return;
#ifdef CONFIG_VTERM_BENCH
    s_tsm_bench.scroll_up_calls++;
#endif
    int top  = t->scroll_top;
    int bot  = t->scroll_bot;
    int span = bot - top + 1;

    /* Only the full primary screen makes history: a DECSTBM region is an app
     * managing a pane, not the session scrolling. Must run before the work
     * below, while the rows are still at their old logical positions. */
    if (t->sb_max > 0 && !t->mode.decalt && top == 0 && bot == t->rows - 1) {
        int push = n < span ? n : span;
        for (int r = 0; r < push; r++) sb_push_row(t, r);
    }

    if (n >= span) { for (int r = top; r <= bot; r++) erase_row(t, r); return; }
    if (top == 0 && bot == t->rows - 1) {
        t->base += n;
        if (t->base >= t->rows) t->base -= t->rows;
    } else {
        for (int r = top; r <= bot - n; r++)
            memcpy(row_ptr(t, r), row_ptr(t, r + n),
                   (size_t)t->cols * sizeof(tsm_cell_t));
#ifdef CONFIG_VTERM_BENCH
        s_tsm_bench.scroll_rows += (uint32_t)(span - n);
#endif
    }
    for (int r = bot - n + 1; r <= bot; r++) erase_row(t, r);
    for (int r = top; r <= bot; r++) mark_row_dirty(t, r);
}

/* Scroll [top, bot] down by n lines; new lines at top are blank. */
static void scroll_down(tsm_t *t, int n)
{
    if (n <= 0) return;
#ifdef CONFIG_VTERM_BENCH
    s_tsm_bench.scroll_down_calls++;
#endif
    int top  = t->scroll_top;
    int bot  = t->scroll_bot;
    int span = bot - top + 1;
    if (n >= span) { for (int r = top; r <= bot; r++) erase_row(t, r); return; }
    if (top == 0 && bot == t->rows - 1) {
        t->base -= n;
        if (t->base < 0) t->base += t->rows;
    } else {
        for (int r = bot; r >= top + n; r--)
            memcpy(row_ptr(t, r), row_ptr(t, r - n),
                   (size_t)t->cols * sizeof(tsm_cell_t));
#ifdef CONFIG_VTERM_BENCH
        s_tsm_bench.scroll_rows += (uint32_t)(span - n);
#endif
    }
    for (int r = top; r < top + n; r++) erase_row(t, r);
    for (int r = top; r <= bot; r++) mark_row_dirty(t, r);
}

/* ── Cursor movement ─────────────────────────────────────────────────────── */

/* Move cursor to absolute (col, row) — clamped to screen. */
static inline void cursor_goto(tsm_t *t, int col, int row)
{
    int top = t->mode.decom ? t->scroll_top : 0;
    int bot = t->mode.decom ? t->scroll_bot : t->rows - 1;
    t->cx = clampi(col, 0, t->cols - 1);
    t->cy = clampi(row, top, bot);
    t->pending_wrap = false;
}

/* Advance cursor by one cell; handle wrap/scroll. */
static inline void cursor_advance(tsm_t *t)
{
    if (t->cx + 1 < t->cols) {
        t->cx++;
    } else if (t->mode.decawm) {
        t->pending_wrap = true;
    }
}

/* Perform pending wrap (newline + scroll if needed). */
static inline void do_wrap(tsm_t *t)
{
    t->pending_wrap = false;
    t->cx = 0;
    if (t->cy == t->scroll_bot) {
        scroll_up(t, 1);
    } else if (t->cy + 1 < t->rows) {
        t->cy++;
    }
}

/* ── Save / restore cursor ───────────────────────────────────────────────── */

static void save_cursor(tsm_t *t, tsm_cursor_save_t *s)
{
    s->col = t->cx; s->row = t->cy;
    s->attrs = t->attrs; s->attrs2 = t->attrs2;
    s->fg = t->fg; s->bg = t->bg;
    s->g0 = t->g[0]; s->g1 = t->g[1]; s->gl = t->gl;
}

static void restore_cursor(tsm_t *t, const tsm_cursor_save_t *s)
{
    t->cx = clampi(s->col, 0, t->cols - 1);
    t->cy = clampi(s->row, 0, t->rows - 1);
    t->attrs = s->attrs; t->attrs2 = s->attrs2;
    t->fg = s->fg; t->bg = s->bg;
    t->g[0] = s->g0; t->g[1] = s->g1; t->gl = s->gl;
    t->pending_wrap = false;
}

/* ── Alt screen switch ───────────────────────────────────────────────────── */

/* The ring base is per-grid state. It must travel with the cells pointer in
 * every swap. Otherwise a rotated primary screen comes back scrambled on
 * alt exit. */
static void swap_grids(tsm_t *t)
{
    tsm_cell_t *tmp = t->cells;
    t->cells     = t->alt_cells;
    t->alt_cells = tmp;
    int tb       = t->base;
    t->base      = t->alt_base;
    t->alt_base  = tb;
}

static void switch_to_alt(tsm_t *t)
{
    if (t->mode.decalt) return;
    /* An app taking the alt screen owns the whole viewport from here on;
     * a stale scrollback view would leave history stranded above it. */
    t->sb_off = 0;
    save_cursor(t, &t->saved);
    swap_grids(t);
    t->mode.decalt = true;
    erase_screen(t);
    t->base = 0;   /* freshly erased — mapping is free to reset */
    restore_cursor(t, &t->alt_saved);
}

static void switch_to_primary(tsm_t *t)
{
    if (!t->mode.decalt) return;
    save_cursor(t, &t->alt_saved);
    swap_grids(t);
    t->mode.decalt = false;
    restore_cursor(t, &t->saved);
    for (int r = 0; r < t->rows; r++) mark_row_dirty(t, r);
}

/* ── SGR (Select Graphic Rendition) ──────────────────────────────────────── */

static void do_sgr(tsm_t *t, const int32_t *params, int nparams)
{
    int np = nparams;
    if (np == 0) {
        t->attrs = 0; t->attrs2 = 0;
        t->fg = COLOR_DEFAULT_FG; t->bg = COLOR_DEFAULT_BG;
        return;
    }

    for (int i = 0; i < np; ) {
        int32_t p0 = params[i] < 0 ? 0 : params[i];
        switch (p0) {
        case  0: t->attrs = 0; t->attrs2 = 0;
                 t->fg = COLOR_DEFAULT_FG; t->bg = COLOR_DEFAULT_BG; i++; break;
        case  1: t->attrs |=  CELL_ATTR_BOLD;      i++; break;
        case  2: t->attrs |=  CELL_ATTR_DIM;       i++; break;
        case  3: t->attrs |=  CELL_ATTR_ITALIC;    i++; break;
        case  4: t->attrs |=  CELL_ATTR_UNDERLINE; i++; break;
        case  5: t->attrs |=  CELL_ATTR_BLINK;     i++; break;
        case  7: t->attrs |=  CELL_ATTR_INVERSE;   i++; break;
        case  8: t->attrs |=  CELL_ATTR_INVISIBLE; i++; break;
        case  9: t->attrs |=  CELL_ATTR_STRIKE;    i++; break;
        case 22: t->attrs &= ~(CELL_ATTR_BOLD | CELL_ATTR_DIM); i++; break;
        case 23: t->attrs &= ~CELL_ATTR_ITALIC;    i++; break;
        case 24: t->attrs &= ~CELL_ATTR_UNDERLINE; i++; break;
        case 25: t->attrs &= ~CELL_ATTR_BLINK;     i++; break;
        case 27: t->attrs &= ~CELL_ATTR_INVERSE;   i++; break;
        case 28: t->attrs &= ~CELL_ATTR_INVISIBLE; i++; break;
        case 29: t->attrs &= ~CELL_ATTR_STRIKE;    i++; break;
        case 39: t->fg = COLOR_DEFAULT_FG; i++; break;
        case 49: t->bg = COLOR_DEFAULT_BG; i++; break;
        case 53: t->attrs2 |=  CELL_ATTR2_OVERLINE; i++; break;
        case 55: t->attrs2 &= ~CELL_ATTR2_OVERLINE; i++; break;
        default:
            if (p0 >= 30 && p0 <= 37) { t->fg = color_ansi((uint8_t)(p0-30)); i++; break; }
            if (p0 >= 40 && p0 <= 47) { t->bg = color_ansi((uint8_t)(p0-40)); i++; break; }
            if (p0 >= 90 && p0 <= 97) { t->fg = color_ansi((uint8_t)(p0-90+8)); i++; break; }
            if (p0 >= 100 && p0 <= 107) { t->bg = color_ansi((uint8_t)(p0-100+8)); i++; break; }
            /* 38 / 48: extended color */
            if ((p0 == 38 || p0 == 48) && i + 1 < np) {
                int32_t mode = params[i+1] < 0 ? 0 : params[i+1];
                bool is_fg = (p0 == 38);
                if (mode == 5 && i + 2 < np) {
                    /* 256-color: 38;5;n or 48;5;n */
                    int32_t idx = params[i+2];
                    uint16_t c = color_ansi(idx < 0 ? 0 : (uint8_t)idx);
                    if (is_fg) t->fg = c; else t->bg = c;
                    i += 3;
                } else if (mode == 2 && i + 4 < np) {
                    /* truecolor: 38;2;r;g;b or 48;2;r;g;b */
                    int32_t r = params[i+2];
                    int32_t g = params[i+3];
                    int32_t b = params[i+4];
                    uint16_t c = color_rgb(r<0?0:(uint8_t)r, g<0?0:(uint8_t)g, b<0?0:(uint8_t)b);
                    if (is_fg) t->fg = c; else t->bg = c;
                    i += 5;
                } else { i++; }
                break;
            }
            i++; /* unknown — skip */
            break;
        }
    }
}

/* ── CSI dispatch ─────────────────────────────────────────────────────────── */

static void do_csi(tsm_t *t, uint8_t prefix, uint8_t intermediate, uint8_t final,
                   const int32_t *params, int nparams)
{
    int32_t p1 = param(params, nparams, 0, -1);
    int32_t p2 = param(params, nparams, 1, -1);

    /* Private sequences (prefix '?') */
    if (prefix == '?') {
        int32_t mode_n = p1 < 0 ? 0 : p1;
        bool set   = (final == 'h');
        bool reset = (final == 'l');
        /* DECRQM — CSI ? 2026 $ p: query mode state */
        if (intermediate == '$' && final == 'p' && mode_n == 2026) {
            char buf[16];
            int n = snprintf(buf, sizeof(buf), "\x1b[?2026;%d$y",
                             t->mode.sync_update ? 1 : 2);
            if (n > 0 && n < (int)sizeof(buf))
                send_response(t, buf, (size_t)n);
            return;
        }
        if (!set && !reset) return;
        switch (mode_n) {
        case    1: t->mode.decckm   = set;                        break; /* DECCKM */
        case    3: /* DECCOLM 80/132 — ignored */                 break;
        case    5: /* DECSCNM reverse screen — ignored */         break;
        case    6: t->mode.decom    = set;                        break;
        case    7: t->mode.decawm   = set;                        break;
        case   12: /* cursor blink — ignored */                   break;
        case   25: t->mode.dectcem  = set;                        break;
        case 1000: t->mode.mouse_btn = set; /* TODO: MOUSE */     break;
        case 1006: /* SGR mouse encoding — stub */                break;
        case   47:  /* alt screen — no cursor save (original xterm) */
        case 1047:  /* alt screen — no cursor save (xterm variant)  */
            if (set)   switch_to_alt(t);
            else       switch_to_primary(t);
            break;
        case 1048:  /* cursor save/restore only — no screen switch  */
            if (set)   save_cursor(t, &t->saved);
            else       restore_cursor(t, &t->saved);
            break;
        case 1049:
            if (set)   switch_to_alt(t);
            else       switch_to_primary(t);
            break;
        case 2004: t->mode.bracketed = set; /* TODO: BRACKETED */ break;
        case 2026: t->mode.sync_update = set;                    break; /* BSU/ESU */
        default: break;
        }
        return;
    }

    /* Standard CSI sequences */
    switch (final) {

    /* ── Cursor movement ─────────────────────────────────────────────────── */
    case 'A': /* CUU — cursor up */
        cursor_goto(t, t->cx, t->cy - (int)(p1 < 1 ? 1 : p1));
        break;
    case 'B': /* CUD — cursor down */
        cursor_goto(t, t->cx, t->cy + (int)(p1 < 1 ? 1 : p1));
        break;
    case 'C': /* CUF — cursor forward */
        cursor_goto(t, t->cx + (int)(p1 < 1 ? 1 : p1), t->cy);
        break;
    case 'D': /* CUB — cursor backward */
        cursor_goto(t, t->cx - (int)(p1 < 1 ? 1 : p1), t->cy);
        break;
    case 'E': /* CNL — cursor next line */
        cursor_goto(t, 0, t->cy + (int)(p1 < 1 ? 1 : p1));
        break;
    case 'F': /* CPL — cursor preceding line */
        cursor_goto(t, 0, t->cy - (int)(p1 < 1 ? 1 : p1));
        break;
    case 'G': /* CHA — cursor horizontal absolute */
        cursor_goto(t, (int)(p1 < 1 ? 1 : p1) - 1, t->cy);
        break;
    case 'H': /* CUP — cursor position */
    case 'f': /* HVP — horizontal vertical position */
    {
        int row = (int)(p1 < 1 ? 1 : p1) - 1;
        int col = (int)(p2 < 1 ? 1 : p2) - 1;
        if (t->mode.decom) { row += t->scroll_top; }
        cursor_goto(t, col, row);
        break;
    }
    case 'd': /* VPA — vertical position absolute */
        cursor_goto(t, t->cx, (int)(p1 < 1 ? 1 : p1) - 1);
        break;

    case 'J': /* ED — erase display */
        switch (p1 < 0 ? 0 : p1) {
        case 0: /* from cursor to end */
            erase_range(t, t->cy, t->cx, t->cols - 1);
            for (int r = t->cy + 1; r < t->rows; r++) erase_row(t, r);
            break;
        case 1: /* from start to cursor */
            for (int r = 0; r < t->cy; r++) erase_row(t, r);
            erase_range(t, t->cy, 0, t->cx);
            break;
        case 2: /* whole screen — history is NOT touched */
            erase_screen(t);
            break;
        case 3: /* whole screen + scrollback (xterm ED 3) */
            /* Must NOT share a body with case 2. ED 2 is what every
             * full-screen app sends on exit. Clearing history there
             * throws the session away the moment you quit mc. */
            sb_clear(t);
            erase_screen(t);
            break;
        }
        break;
    case 'K': /* EL — erase line */
        switch (p1 < 0 ? 0 : p1) {
        case 0: erase_range(t, t->cy, t->cx, t->cols - 1); break;
        case 1: erase_range(t, t->cy, 0, t->cx); break;
        case 2: erase_row(t, t->cy); break;
        }
        break;

    /* ── Insert / delete ─────────────────────────────────────────────────── */
    case 'L': /* IL — insert lines */
    {
        int n = (int)(p1 < 1 ? 1 : p1);
        if (t->cy >= t->scroll_top && t->cy <= t->scroll_bot) {
            int saved_top = t->scroll_top;
            t->scroll_top = t->cy;
            scroll_down(t, n);
            t->scroll_top = saved_top;
        }
        break;
    }
    case 'M': /* DL — delete lines */
    {
        int n = (int)(p1 < 1 ? 1 : p1);
        if (t->cy >= t->scroll_top && t->cy <= t->scroll_bot) {
            int saved_top = t->scroll_top;
            t->scroll_top = t->cy;
            scroll_up(t, n);
            t->scroll_top = saved_top;
        }
        break;
    }
    case '@': /* ICH — insert characters */
    {
        int n = (int)(p1 < 1 ? 1 : p1);
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        memmove(cell_at(t, t->cx + n, t->cy),
                cell_at(t, t->cx,     t->cy),
                (size_t)(t->cols - t->cx - n) * sizeof(tsm_cell_t));
        erase_range(t, t->cy, t->cx, t->cx + n - 1);
        mark_dirty(t, t->cy, t->cx, t->cols - 1);
        break;
    }
    case 'P': /* DCH — delete characters */
    {
        int n = (int)(p1 < 1 ? 1 : p1);
        if (n > t->cols - t->cx) n = t->cols - t->cx;
        memmove(cell_at(t, t->cx,     t->cy),
                cell_at(t, t->cx + n, t->cy),
                (size_t)(t->cols - t->cx - n) * sizeof(tsm_cell_t));
        erase_range(t, t->cy, t->cols - n, t->cols - 1);
        mark_dirty(t, t->cy, t->cx, t->cols - 1);
        break;
    }
    case 'X': /* ECH — erase characters */
    {
        int n = (int)(p1 < 1 ? 1 : p1);
        int end = t->cx + n - 1;
        if (end >= t->cols) end = t->cols - 1;
        erase_range(t, t->cy, t->cx, end);
        break;
    }

    case 'S': /* SU — scroll up */
        scroll_up(t, (int)(p1 < 1 ? 1 : p1));
        break;
    case 'T': /* SD — scroll down */
        scroll_down(t, (int)(p1 < 1 ? 1 : p1));
        break;

    /* ── Misc ────────────────────────────────────────────────────────────── */
    case 'm': /* SGR */
        do_sgr(t, params, nparams);
        break;
    case 'r': /* DECSTBM — set scroll region */
    {
        int top = (int)(p1 < 1 ? 1 : p1) - 1;
        int bot = (int)(p2 < 1 ? (int32_t)t->rows : p2) - 1;
        if (top < bot && bot < t->rows) {
            t->scroll_top = top;
            t->scroll_bot = bot;
        }
        cursor_goto(t, 0, t->mode.decom ? t->scroll_top : 0);
        break;
    }
    case 's': /* DECSC: save cursor. CSI s does the same thing. */
        if (intermediate == 0 && prefix == 0)
            save_cursor(t, &t->saved);
        break;
    case 'u': /* DECRC: restore cursor. CSI u does the same thing. */
        if (intermediate == 0 && prefix == 0)
            restore_cursor(t, &t->saved);
        break;
    case 'h': /* SM — set mode */
        if (p1 == 4)  t->mode.irm = true;   /* IRM */
        if (p1 == 20) t->mode.lnm = true;   /* LNM */
        break;
    case 'l': /* RM — reset mode */
        if (p1 == 4)  t->mode.irm = false;
        if (p1 == 20) t->mode.lnm = false;
        break;
    case 'n': /* DSR — device status report */
        if (p1 == 5) {
            send_response(t, "\x1b[0n", 4);
        } else if (p1 == 6) {
            char buf[16];
            int  n = snprintf(buf, sizeof(buf), "\x1b[%d;%dR",
                              t->cy + 1, t->cx + 1);
            if (n > 0 && n < (int)sizeof(buf))
                send_response(t, buf, n);
        }
        break;
    case 'c': /* DA1 — device attributes */
        if (prefix == 0 && p1 <= 0)
            send_response(t, "\x1b[?1;2c", 7);
        break;
    default:  break;
    }
}

/* ── Hard reset ───────────────────────────────────────────────────────────── */

static void do_hard_reset(tsm_t *t)
{
    if (t->mode.decalt)            /* return to primary first */
        swap_grids(t);
    sb_clear(t);                   /* RIS drops history, as xterm does */
    /* SGR resets to defaults before the erase. blank_cell() clears with the
     * CURRENT colors (BCE, correct for ED), but RIS must not keep them.
     * Erasing first would leave the whole grid in the old session's colors. */
    t->attrs = 0; t->attrs2 = 0;
    t->fg = COLOR_DEFAULT_FG; t->bg = COLOR_DEFAULT_BG;
    erase_screen(t);
    t->base = 0;                   /* freshly erased — mapping is free to reset */
    t->cx = 0; t->cy = 0;
    t->g[0] = CHARSET_ASCII; t->g[1] = CHARSET_ASCII; t->gl = 0;
    t->scroll_top = 0; t->scroll_bot = t->rows - 1;
    memset(&t->mode, 0, sizeof(t->mode));
    t->mode.decawm = true; t->mode.dectcem = true;
    t->pending_wrap = false;
}

/* ── ESC dispatch ─────────────────────────────────────────────────────────── */

static void do_esc(tsm_t *t, uint8_t intermediate, uint8_t final)
{
    if (intermediate == '(') {
        t->g[0] = (final == '0') ? CHARSET_DEC_GFX : CHARSET_ASCII;
    } else if (intermediate == ')') {
        t->g[1] = (final == '0') ? CHARSET_DEC_GFX : CHARSET_ASCII;
    } else if (intermediate == 0) {
        switch (final) {
        case '7': save_cursor(t, &t->saved);    break; /* DECSC */
        case '8': restore_cursor(t, &t->saved); break; /* DECRC */
        case 'D': /* IND — index (like LF) */
            if (t->cy == t->scroll_bot) scroll_up(t, 1);
            else if (t->cy + 1 < t->rows) t->cy++;
            break;
        case 'E': /* NEL — next line */
            t->cx = 0;
            if (t->cy == t->scroll_bot) scroll_up(t, 1);
            else if (t->cy + 1 < t->rows) t->cy++;
            break;
        case 'M': /* RI — reverse index */
            if (t->cy == t->scroll_top) scroll_down(t, 1);
            else if (t->cy > 0) t->cy--;
            break;
        case 'c': /* RIS — full reset */
            do_hard_reset(t);
            break;
        default: break;
        }
    }
}

/* ── OSC dispatch ────────────────────────────────────────────────────────── */

static void do_osc(tsm_t *t, const uint8_t *data, int len)
{
    if (len < 2) return;
    const uint8_t *d = data;
    int ps = 0;
    int i  = 0;
    while (i < len && d[i] >= '0' && d[i] <= '9')
        ps = ps * 10 + (d[i++] - '0');
    if (i < len && d[i] == ';') i++;
    if (ps == 0 || ps == 2) {
        int tlen = len - i;
        if (tlen >= (int)sizeof(t->title)) tlen = (int)sizeof(t->title) - 1;
        memcpy(t->title, &d[i], (size_t)tlen);
        t->title[tlen] = '\0';
    }
}

/* ── C0 dispatch ──────────────────────────────────────────────────────────── */

static void do_c0(tsm_t *t, uint8_t byte)
{
    switch (byte) {
    case 0x08: /* BS */
        if (t->cx > 0) { t->cx--; t->pending_wrap = false; }
        break;
    case 0x09: /* HT — horizontal tab (advance to next tab stop, 8-col) */
    {
        int next = (t->cx + 8) & ~7;
        if (next >= t->cols) next = t->cols - 1;
        t->cx = next;
        t->pending_wrap = false;
        break;
    }
    case 0x0A: /* LF */
    case 0x0B: /* VT */
    case 0x0C: /* FF */
        if (t->cy == t->scroll_bot) scroll_up(t, 1);
        else if (t->cy + 1 < t->rows) t->cy++;
        if (t->mode.lnm) t->cx = 0;
        t->pending_wrap = false;
        break;
    case 0x0D: /* CR */
        t->cx = 0;
        t->pending_wrap = false;
        break;
    case 0x0E: /* SO — shift out: activate G1 */
        t->gl = 1;
        break;
    case 0x0F: /* SI — shift in: activate G0 */
        t->gl = 0;
        break;
    default: break;
    }
}

/* ── Print (GROUND printable) ────────────────────────────────────────────── */

static void do_print_span_irm(tsm_t *t, const uint32_t *cps, int count)
{
    for (int i = 0; i < count; i++) {
        uint32_t cp = cps[i];
        uint16_t glyph;
        if (cp < 0x80u) {
            glyph = charset_xlat(t->g[t->gl], (uint8_t)cp);
        } else {
            glyph = (cp <= 0xFFFFu) ? (uint16_t)cp : '?';
        }

        if (t->pending_wrap) do_wrap(t);

        if (t->cx + 1 < t->cols) {
            memmove(cell_at(t, t->cx + 1, t->cy),
                    cell_at(t, t->cx,     t->cy),
                    (size_t)(t->cols - t->cx - 1) * sizeof(tsm_cell_t));
            mark_dirty(t, t->cy, t->cx, t->cols - 1);
        }

        tsm_cell_t *c = cell_at(t, t->cx, t->cy);
        c->cp     = glyph;
        c->fg     = t->fg;
        c->bg     = t->bg;
        c->attrs  = t->attrs;
        c->attrs2 = t->attrs2;
        mark_dirty(t, t->cy, t->cx, t->cx);
        cursor_advance(t);
    }
}

/* Batched print. SGR state and the active charset cannot change mid-span:
 * every ESC and SO/SI flushes the parser's print buffer first. This function
 * hoists them once and writes row segments at a cost of one template struct
 * copy per cell. Insert mode keeps the per-cell slow path above. */
static inline void do_print_span(tsm_t *t, const uint32_t *cps, int count)
{
    if (t->mode.irm) { do_print_span_irm(t, cps, count); return; }

    const int  cols = t->cols;
    const bool gfx  = (t->g[t->gl] == CHARSET_DEC_GFX);
    tsm_cell_t tmpl = { .cp = 0, .fg = t->fg, .bg = t->bg,
                        .attrs = t->attrs, .attrs2 = t->attrs2 };

    int i = 0;
    while (i < count) {
        if (t->pending_wrap) do_wrap(t);
        const int cx = t->cx;
        const int cy = t->cy;
        int run = count - i;
        if (run > cols - cx) run = cols - cx;

        tsm_cell_t *c = cell_at(t, cx, cy);
        for (int k = 0; k < run; k++) {
            uint32_t cp = cps[i + k];
            if (cp < 0x80u)
                tmpl.cp = gfx ? charset_xlat(CHARSET_DEC_GFX, (uint8_t)cp)
                              : (uint16_t)cp;
            else
                tmpl.cp = (cp <= 0xFFFFu) ? (uint16_t)cp : '?';
            c[k] = tmpl;
        }
        mark_dirty(t, cy, cx, cx + run - 1);
        i += run;

        /* cursor_advance, batched: past the last column the cursor parks at
         * cols-1; with DECAWM the wrap stays pending until the next cell. */
        if (cx + run < cols) {
            t->cx = cx + run;
        } else {
            t->cx = cols - 1;
            if (t->mode.decawm) t->pending_wrap = true;
        }
    }
}

/* ── Per-type vtable callbacks ────────────────────────────────────────────── */

#ifdef CONFIG_VTERM_BENCH
#define TSM_BENCH_T0()      uint32_t __bt0 = esp_cpu_get_cycle_count()
#define TSM_BENCH_ADD(field) (s_tsm_bench.field += (esp_cpu_get_cycle_count() - __bt0))
#else
#define TSM_BENCH_T0()      ((void)0)
#define TSM_BENCH_ADD(field) ((void)0)
#endif

static void on_print(const uint32_t *cps, int ncp, void *user)
{
    TSM_BENCH_T0();
    do_print_span((tsm_t *)user, cps, ncp);
    TSM_BENCH_ADD(print_cycles);
}
static void on_c0(uint8_t byte, void *user)
{
    TSM_BENCH_T0();
    do_c0((tsm_t *)user, byte);
    TSM_BENCH_ADD(c0_cycles);
}
static void on_esc(uint8_t intermediate, uint8_t final, void *user)
{
    TSM_BENCH_T0();
    do_esc((tsm_t *)user, intermediate, final);
    TSM_BENCH_ADD(other_cycles);
}
static void on_csi(uint8_t prefix, uint8_t intermediate, uint8_t final,
                   const int32_t *params, int nparams, void *user)
{
    TSM_BENCH_T0();
    do_csi((tsm_t *)user, prefix, intermediate, final, params, nparams);
    TSM_BENCH_ADD(csi_cycles);
}
static void on_osc(const uint8_t *data, int len, void *user)
{
    TSM_BENCH_T0();
    do_osc((tsm_t *)user, data, len);
    TSM_BENCH_ADD(other_cycles);
}
static void on_dcs(uint8_t prefix, uint8_t intermediate, uint8_t final,
                   const int32_t *params, int nparams, void *user)
{
    (void)prefix; (void)intermediate; (void)final;
    (void)params; (void)nparams; (void)user;
}

#undef TSM_BENCH_T0
#undef TSM_BENCH_ADD

static const vt_callbacks_t s_tsm_cb = {
    .print = on_print, .c0 = on_c0, .esc = on_esc,
    .csi   = on_csi,   .osc = on_osc, .dcs = on_dcs,
};

/* ── Public API ───────────────────────────────────────────────────────────── */

/* Only task context touches tsm's own grids: tsm_feed on the SSH read task,
 * and the vterm dirty-row copy. The display ISR reads vterm's separate
 * internal bridge buffer instead, never these. So the two 24 KB grids live
 * in PSRAM (SPIRAM-first, internal fallback). The tsm_t struct and the
 * dirty array go the OTHER way. They hold the per-byte-hot parser state
 * (embedded vtparse_t, print buffer, cursor). In PSRAM every parsed byte
 * paid cache misses against the grids' traffic; ~1.3 KB of internal DRAM
 * buys that back. */
static void *tsm_calloc(size_t n, size_t sz)
{
    void *p = heap_caps_calloc(n, sz, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_calloc(n, sz, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    return p;
}

static void *tsm_calloc_hot(size_t n, size_t sz)
{
    void *p = heap_caps_calloc(n, sz, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!p) p = heap_caps_calloc(n, sz, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    return p;
}

tsm_t *tsm_new(int cols, int rows, int sb_lines)
{
    if (cols <= 0 || rows <= 0) return NULL;

    tsm_t *t = (tsm_t *)tsm_calloc_hot(1, sizeof(tsm_t));
    if (!t) return NULL;

    t->cells     = (tsm_cell_t *)tsm_calloc((size_t)cols * (size_t)rows, sizeof(tsm_cell_t));
    t->alt_cells = (tsm_cell_t *)tsm_calloc((size_t)cols * (size_t)rows, sizeof(tsm_cell_t));
    t->dirty     = (tsm_row_dirty_t *)tsm_calloc_hot((size_t)rows, sizeof(tsm_row_dirty_t));

    if (!t->cells || !t->alt_cells || !t->dirty) { tsm_free(t); return NULL; }

    /* Scrollback is a comfort, not a requirement: halve the request until it
     * fits rather than failing the terminal outright. A deck that comes up
     * with a shorter history beats one that will not come up. */
    for (int want = sb_lines; want > 0; want /= 2) {
        t->sb_cells = (tsm_cell_t *)tsm_calloc((size_t)want * (size_t)cols,
                                               sizeof(tsm_cell_t));
        if (t->sb_cells) { t->sb_max = want; break; }
    }

    t->cols = cols;
    t->rows = rows;

    t->fg = COLOR_DEFAULT_FG;
    t->bg = COLOR_DEFAULT_BG;

    t->mode.decawm  = true;
    t->mode.dectcem = true;

    t->scroll_top = 0;
    t->scroll_bot = rows - 1;

    t->g[0] = CHARSET_ASCII;
    t->g[1] = CHARSET_ASCII;
    t->gl   = 0;

    erase_screen(t);
    tsm_clear_dirty(t);

    vtparse_init(&t->vtp, &s_tsm_cb, t);

    return t;
}

void tsm_free(tsm_t *t)
{
    if (!t) return;
    heap_caps_free(t->cells);
    heap_caps_free(t->alt_cells);
    heap_caps_free(t->sb_cells);
    heap_caps_free(t->dirty);
    heap_caps_free(t);
}

void tsm_feed(tsm_t *t, const uint8_t *data, size_t len)
{
    vtparse_feed(&t->vtp, data, len);
}

const tsm_cell_t *tsm_row(const tsm_t *t, int row)
{
    if (t->sb_off <= 0)
        return &t->cells[phys_row(t, row) * t->cols];

    /* Scrolled back: the top sb_off rows come from history. The rest is the
     * live grid, shifted down by the same amount. */
    if (row < t->sb_off) {
        int i = t->sb_len - t->sb_off + row;
        int p = t->sb_head - t->sb_len + i;
        if (p < 0) p += t->sb_max;
        return &t->sb_cells[(size_t)p * (size_t)t->cols];
    }
    return &t->cells[phys_row(t, row - t->sb_off) * t->cols];
}

int tsm_sb_capacity(const tsm_t *t) { return t->sb_max; }
int tsm_sb_len(const tsm_t *t)    { return t->sb_len; }
int tsm_sb_offset(const tsm_t *t) { return t->sb_off; }

int tsm_sb_scroll(tsm_t *t, int delta)
{
    /* Alt-screen apps own the whole viewport; paging the primary screen's
     * history in behind them would show a mix of the two. */
    if (t->sb_max <= 0 || t->mode.decalt) return t->sb_off;

    int off = t->sb_off + delta;
    t->sb_off = clampi(off, 0, t->sb_len);
    return t->sb_off;
}

void tsm_sb_reset(tsm_t *t)
{
    t->sb_off = 0;
}

void tsm_cursor(const tsm_t *t, int *col, int *row, bool *visible)
{
    if (col)     *col     = t->cx;
    if (row)     *row     = t->cy;
    if (visible) *visible = t->mode.dectcem;
}

const tsm_row_dirty_t *tsm_dirty(const tsm_t *t)
{
    return t->dirty;
}

void tsm_clear_dirty(tsm_t *t)
{
    for (int r = 0; r < t->rows; r++) {
        t->dirty[r].l = TSM_DIRTY_L_CLEAN;
        t->dirty[r].r = TSM_DIRTY_R_CLEAN;
    }
}

int tsm_cols(const tsm_t *t) { return t->cols; }
int tsm_rows(const tsm_t *t) { return t->rows; }

void tsm_reset(tsm_t *t)
{
    vtparse_init(&t->vtp, &s_tsm_cb, t);
    do_hard_reset(t);
}

bool tsm_app_cursor_keys(const tsm_t *t) { return t->mode.decckm; }

bool tsm_sync_update(const tsm_t *t) { return t->mode.sync_update; }

void tsm_bench_get(tsm_bench_t *out)
{
    if (!out) return;
#ifdef CONFIG_VTERM_BENCH
    *out = s_tsm_bench;
#else
    memset(out, 0, sizeof(*out));
#endif
}

void tsm_bench_reset(void)
{
#ifdef CONFIG_VTERM_BENCH
    memset(&s_tsm_bench, 0, sizeof(s_tsm_bench));
#endif
}
