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

/* Pointer to cell (col, row) on the active screen. */
static inline tsm_cell_t *cell_at(tsm_t *t, int col, int row)
{
    return &t->cells[row * t->cols + col];
}

/* Mark column range [l, r] on row as dirty. */
static inline void mark_dirty(tsm_t *t, int row, int l, int r)
{
    if (t->dirty[row].l > (uint8_t)l) t->dirty[row].l = (uint8_t)l;
    if (t->dirty[row].r < (uint8_t)r) t->dirty[row].r = (uint8_t)r;
}

/* Mark entire row dirty. */
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

/* Erase entire row. */
static inline void erase_row(tsm_t *t, int row)
{
    erase_range(t, row, 0, t->cols - 1);
}

/* Erase entire screen. */
static void erase_screen(tsm_t *t)
{
    for (int r = 0; r < t->rows; r++) erase_row(t, r);
}

/* ── Scrolling ───────────────────────────────────────────────────────────── */

/* Scroll [top, bot] up by n lines; new lines at bottom are blank. */
static void scroll_up(tsm_t *t, int n)
{
    if (n <= 0) return;
    int top  = t->scroll_top;
    int bot  = t->scroll_bot;
    int span = bot - top + 1;
    if (n >= span) { for (int r = top; r <= bot; r++) erase_row(t, r); return; }
    memmove(&t->cells[top * t->cols],
            &t->cells[(top + n) * t->cols],
            (size_t)(span - n) * (size_t)t->cols * sizeof(tsm_cell_t));
    for (int r = bot - n + 1; r <= bot; r++) erase_row(t, r);
    for (int r = top; r <= bot; r++) mark_row_dirty(t, r);
}

/* Scroll [top, bot] down by n lines; new lines at top are blank. */
static void scroll_down(tsm_t *t, int n)
{
    if (n <= 0) return;
    int top  = t->scroll_top;
    int bot  = t->scroll_bot;
    int span = bot - top + 1;
    if (n >= span) { for (int r = top; r <= bot; r++) erase_row(t, r); return; }
    memmove(&t->cells[(top + n) * t->cols],
            &t->cells[top * t->cols],
            (size_t)(span - n) * (size_t)t->cols * sizeof(tsm_cell_t));
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

static void switch_to_alt(tsm_t *t)
{
    if (t->mode.decalt) return;
    save_cursor(t, &t->saved);
    tsm_cell_t *tmp = t->cells;
    t->cells = t->alt_cells;
    t->alt_cells = tmp;
    t->mode.decalt = true;
    erase_screen(t);
    restore_cursor(t, &t->alt_saved);
}

static void switch_to_primary(tsm_t *t)
{
    if (!t->mode.decalt) return;
    save_cursor(t, &t->alt_saved);
    tsm_cell_t *tmp = t->cells;
    t->cells = t->alt_cells;
    t->alt_cells = tmp;
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

    /* ── Erase ───────────────────────────────────────────────────────────── */
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
        case 2: /* whole screen */
        case 3: /* whole screen + scrollback (not implemented) */
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

    /* ── Scroll ──────────────────────────────────────────────────────────── */
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
    case 's': /* DECSC (also CSI s — save cursor) */
        if (intermediate == 0 && prefix == 0)
            save_cursor(t, &t->saved);
        break;
    case 'u': /* DECRC (also CSI u — restore cursor) */
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
    if (t->mode.decalt) {          /* return to primary first */
        tsm_cell_t *tmp = t->cells;
        t->cells        = t->alt_cells;
        t->alt_cells    = tmp;
    }
    erase_screen(t);
    t->cx = 0; t->cy = 0;
    t->attrs = 0; t->attrs2 = 0;
    t->fg = COLOR_DEFAULT_FG; t->bg = COLOR_DEFAULT_BG;
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

static inline void do_print_span(tsm_t *t, const uint32_t *cps, int count)
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

        if (t->mode.irm) {
            if (t->cx + 1 < t->cols) {
                memmove(cell_at(t, t->cx + 1, t->cy),
                        cell_at(t, t->cx,     t->cy),
                        (size_t)(t->cols - t->cx - 1) * sizeof(tsm_cell_t));
                mark_dirty(t, t->cy, t->cx, t->cols - 1);
            }
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

/* ── Per-type vtable callbacks ────────────────────────────────────────────── */

static void on_print(const uint32_t *cps, int ncp, void *user)
    { do_print_span((tsm_t *)user, cps, ncp); }
static void on_c0(uint8_t byte, void *user)
    { do_c0((tsm_t *)user, byte); }
static void on_esc(uint8_t intermediate, uint8_t final, void *user)
    { do_esc((tsm_t *)user, intermediate, final); }
static void on_csi(uint8_t prefix, uint8_t intermediate, uint8_t final,
                   const int32_t *params, int nparams, void *user)
    { do_csi((tsm_t *)user, prefix, intermediate, final, params, nparams); }
static void on_osc(const uint8_t *data, int len, void *user)
    { do_osc((tsm_t *)user, data, len); }
static void on_dcs(uint8_t prefix, uint8_t intermediate, uint8_t final,
                   const int32_t *params, int nparams, void *user)
{
    (void)prefix; (void)intermediate; (void)final;
    (void)params; (void)nparams; (void)user;
}

static const vt_callbacks_t s_tsm_cb = {
    .print = on_print, .c0 = on_c0, .esc = on_esc,
    .csi   = on_csi,   .osc = on_osc, .dcs = on_dcs,
};

/* ── Public API ───────────────────────────────────────────────────────────── */

tsm_t *tsm_new(int cols, int rows)
{
    if (cols <= 0 || rows <= 0) return NULL;

    tsm_t *t = (tsm_t *)heap_caps_calloc(1, sizeof(tsm_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!t) return NULL;

    t->cells     = (tsm_cell_t *)heap_caps_calloc((size_t)cols * (size_t)rows, sizeof(tsm_cell_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    t->alt_cells = (tsm_cell_t *)heap_caps_calloc((size_t)cols * (size_t)rows, sizeof(tsm_cell_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    t->dirty     = (tsm_row_dirty_t *)heap_caps_malloc((size_t)rows * sizeof(tsm_row_dirty_t), MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);

    if (!t->cells || !t->alt_cells || !t->dirty) { tsm_free(t); return NULL; }

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
    heap_caps_free(t->dirty);
    heap_caps_free(t);
}

void tsm_feed(tsm_t *t, const uint8_t *data, size_t len)
{
    vtparse_feed(&t->vtp, data, len);
}

const tsm_cell_t *tsm_screen(const tsm_t *t)
{
    return t->cells;
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
