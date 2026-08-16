/*
 * render_cache.c — the per-row column cache: cell/overlay buffers in,
 * decoded glyph rows + resolved colors out. build_row_cache() runs once
 * per character row per frame; the band scan then reads plain arrays.
 */

#include "render_internal.h"
#include "display_fx_internal.h"
#include "font.h"

/* Terminal cell buffer — registered once by vterm_init. */
static DRAM_ATTR struct {
    const terminal_cell_t *buf;
    int cols, rows;
} s_cells = {};

/* Overlay buffer — optional second compositing layer (shell chrome). */
static DRAM_ATTR struct {
    display_overlay_cell_t *buf;   /* written LAST in the setter — the ISR
                                    * must never see a fresh pointer with
                                    * stale cols/rows */
    int     cols, rows;
    color_t fg, bg;
} s_overlay = { .fg = COLOR_BLACK, .bg = COLOR_CYAN };

/* Overlay accent palettes (index 0 → s_overlay.fg). TEXT accents are VGA
 * bright tones; INVERSE bars use muted companions of the same hues. */
static DRAM_ATTR const color_t s_overlay_pal[OVERLAY_PAL_SIZE] = {
    0,                        /* 0: default → replaced by s_overlay.fg */
    RGB565( 85, 255,  85),    /* 1 green   (VGA bright green)   */
    RGB565( 85, 255, 255),    /* 2 cyan    (VGA bright cyan)    */
    RGB565(255,  85, 255),    /* 3 magenta (VGA bright magenta) */
    RGB565(255, 255,  85),    /* 4 amber   (VGA yellow)         */
    RGB565(255,  85,  85),    /* 5 red     (VGA bright red)     */
    RGB565( 85,  85, 255),    /* 6 blue    (VGA bright blue)    */
    RGB565(255, 255, 255),    /* 7 white   (VGA white)          */
};
static DRAM_ATTR const color_t s_overlay_bar[OVERLAY_PAL_SIZE] = {
    RGB565(148, 148, 148),    /* 0 default → neutral gray       */
    RGB565( 96, 168,  96),    /* 1 sage                         */
    RGB565( 80, 160, 168),    /* 2 teal                         */
    RGB565(168,  96, 160),    /* 3 mauve                        */
    RGB565(200, 152,  72),    /* 4 ochre                        */
    RGB565(184,  88,  80),    /* 5 terracotta                   */
    RGB565(104, 112, 192),    /* 6 periwinkle                   */
    RGB565(255, 255, 255),    /* 7 white (kept pure — QR)       */
};

void display_set_text_buffer(const terminal_cell_t *buf, int cols, int rows)
{
    s_cells.buf  = buf;
    s_cells.cols = cols;
    s_cells.rows = rows;
}

void display_set_overlay_buffer(display_overlay_cell_t *buf, int cols, int rows)
{
    s_overlay.cols = cols;
    s_overlay.rows = rows;
    s_overlay.buf  = buf;   /* written last — atomic 32-bit store */
}

void display_set_overlay_colors(color_t fg, color_t bg)
{
    s_overlay.fg = fg;
    s_overlay.bg = bg;
}

void display_get_text_size(int *cols, int *rows)
{
    if (cols) *cols = s_cells.cols;
    if (rows) *rows = s_cells.rows;
}

IRAM_ATTR int render_cache_text_rows(void) { return s_cells.rows; }
IRAM_ATTR int render_cache_text_cols(void) { return s_cells.cols; }
IRAM_ATTR bool render_cache_has_cells(void)
{
    return s_cells.buf != NULL && s_cells.cols > 0 && s_cells.rows > 0;
}

/* Column cache. bg/xf carry two variants selected per scanline so the hot
 * loop pays no per-pixel effect cost; glyphs are DECODED into rows[] (the
 * compressed font has nothing to point at). One bank, rebuilt on a row's
 * first chunk, reused by its second via the validity tags — the measured
 * history behind this shape is in docs/ARCHITECTURE.md. */
static DRAM_ATTR struct {
    _Alignas(4) uint8_t rows[FONT_MAX_CACHE_BYTES];  /* decoded glyphs  */
    uint16_t bg[2][RENDER_MAX_COLS];                 /* [variant][col]  */
    uint16_t xf[2][RENDER_MAX_COLS];                 /* fg ^ bg         */
    uint8_t  ul[RENDER_MAX_COLS];                    /* underline flags */
#if OVERLAY_DIM_DITHER
    uint8_t  dim[RENDER_MAX_COLS];                   /* scrim flags     */
#endif
    int      row;         /* char row held, -1 = none */
    int      scan;        /* dim variant populated?   */
    uint8_t  frame;       /* g_fx_frame at build      */
    bool     any_ul;      /* pass-skip summaries      */
#if OVERLAY_DIM_DITHER
    bool     any_dim;
#endif
} s_cc = { .row = -1 };

IRAM_ATTR void render_cache_invalidate(void)
{
    s_cc.row = -1;
}

/* Scanline dim: 93.75% brightness via carry-safe per-field shift/mask. */
static inline uint16_t fx_dim565(uint16_t p)
{
    return (uint16_t)(p - ((p >> 4) & 0x0861));
}

/* Luma-map onto a monochrome phosphor ramp: mode 1 = green, 2 = amber.
 * IRAM_ATTR, not `inline`: at -Os GCC outlines this into FLASH while its
 * only caller runs from the bounce ISR with the flash cache disabled. */
static IRAM_ATTR uint16_t fx_mono565(uint16_t p, uint8_t mode)
{
    const uint32_t r6 = ((p >> 11) & 0x1F) << 1;
    const uint32_t g6 = (p >> 5) & 0x3F;
    const uint32_t b6 = (p & 0x1F) << 1;
    const uint32_t y  = (r6 * 5 + g6 * 9 + b6 * 2) >> 4;   /* 0..62 */
    if (mode == 1)
        return (uint16_t)(y << 5);                            /* green  */
    return (uint16_t)(((y >> 1) << 11) | (((y * 11) >> 4) << 5)); /* amber */
}

/* Resolved per-cell values handed back by the cell-type helpers below. */
typedef struct {
    color_t fg, bg;
    uint8_t underline;
    uint8_t bold;
    uint8_t dim;        /* OVERLAY_DIM_DITHER scrim request */
} cell_colors_t;

/* Overlay (chrome) cell. `bold` stays 0 — bold-pop must not recolor
 * chrome; the glyph itself may still use the bold face. */
static IRAM_ATTR void resolve_overlay_cell(uint8_t ov_attrs, uint8_t ov_color,
                                           cell_colors_t *out)
{
    color_t fg, bg;
    if (ov_attrs & OVERLAY_ATTR_INVERSE) {
        /* Solid bar. Colored bars (1-6) get a pale tint of their hue as
         * text; gray/white keep dark text (QR modules stay dark-on-white). */
        bg = s_overlay_bar[ov_color];
        if (ov_color >= 1 && ov_color <= 6 && !(ov_attrs & OVERLAY_ATTR_BRIGHT))
            fg = (color_t)(((bg >> 2) & 0x39E7) + 0x7BEF + 0x39E7);
        else
            fg = s_overlay.bg;
    } else {
        fg = ov_color ? s_overlay_pal[ov_color] : s_overlay.fg;
        bg = s_overlay.bg;
    }
    /* Focus wash: bg 50% toward white (carry-safe half-sum). */
    if (ov_attrs & OVERLAY_ATTR_BRIGHT)
        bg = (color_t)(((bg >> 1) & 0x7BEF) + 0x7BEF);
    out->fg = fg;
    out->bg = bg;
    out->underline = 0;
    out->bold      = 0;
    out->dim       = 0;
}

/* Terminal cell: colors + attrs, row glow, modal scrim. */
static IRAM_ATTR void resolve_terminal_cell(const terminal_cell_t *cell,
                                            int glow_tier, uint8_t ov_attrs,
                                            cell_colors_t *out)
{
    color_t fg = cell->fg_color;
    color_t bg = cell->bg_color;
    if (cell->attrs & ATTR_REVERSE) { color_t t = fg; fg = bg; bg = t; }
    out->underline = cell->attrs & ATTR_UNDERLINE;
    out->bold      = cell->attrs & ATTR_BOLD;
    out->dim       = 0;

#if DISPLAY_FX_ROW_GLOW
    /* Glow before the scrim, so a modal's scrim also dims the glow. */
    if (glow_tier == 1)
        bg = (color_t)((bg - ((bg >> 3) & 0x18E3)) + g_fx_glow_acc[0]);
    else if (glow_tier == 2)
        bg = (color_t)((bg - ((bg >> 2) & 0x39E7)) + g_fx_glow_acc[1]);
#else
    (void)glow_tier;
#endif
    if (ov_attrs & OVERLAY_ATTR_DIM) {
#if OVERLAY_DIM_DITHER
        out->dim = 1;   /* full colour; checkerboarded in the post-pass */
#else
        fg = (color_t)((fg >> 1) & 0x7BEFu);
        bg = (color_t)((bg >> 1) & 0x7BEFu);
#endif
    }
    out->fg = fg;
    out->bg = bg;
}

/* Fill the column cache for character row @p cr. @p scan_on selects
 * whether the scanline-dim variant [1] is also produced. */
static IRAM_ATTR void build_row_cache(int cr, int scan_on)
{
    const terminal_cell_t *row_cells = s_cells.buf + cr * s_cells.cols;
    const display_overlay_cell_t *ov_row =
        (s_overlay.buf && cr < s_overlay.rows)
        ? (s_overlay.buf + cr * s_overlay.cols) : NULL;

    const uint8_t mono     = g_fx_snap.mono;
    const uint8_t bold_pop = g_fx_snap.bold_pop;
    bool any_ul = false;
#if OVERLAY_DIM_DITHER
    bool any_dim = false;
#endif

    int glow_tier = 0;
#if DISPLAY_FX_ROW_GLOW
    /* Row-recency glow tier: 0 none, 1 subtle, 2 strong. */
    if (g_fx_snap.glow && cr < DISPLAY_FX_MAX_ROWS) {
        const uint8_t gf  = g_fx_snap.glow_frames;
        const uint8_t age = (uint8_t)(g_fx_frame - g_fx_row_stamp[cr]);
        if (age < gf)
            glow_tier = (g_fx_snap.glow_strength && age * 2 < gf) ? 2 : 1;
        else if (age > 128)
            /* Re-pin long-expired stamps so the uint8 counter can't wrap
             * back into the glow window (races touch_row benignly). */
            g_fx_row_stamp[cr] = (uint8_t)(g_fx_frame - gf);
    }
#endif

    for (int c = 0; c < s_cells.cols; c++) {
        uint8_t *dst = s_cc.rows + (size_t)c * (size_t)g_rs.gb;
        cell_colors_t cc;

        const uint8_t  ov_attrs = (ov_row && c < s_overlay.cols) ? ov_row[c].attrs : 0;
        const uint16_t ov_cp    = (ov_row && c < s_overlay.cols) ? ov_row[c].cp    : 0;

        if (ov_cp != 0) {
            resolve_overlay_cell(ov_attrs, ov_row[c].color, &cc);
            font_decode_glyph(ov_cp, (ov_attrs & OVERLAY_ATTR_BOLD) != 0, dst);
        } else {
            const terminal_cell_t *cell = &row_cells[c];
            resolve_terminal_cell(cell, glow_tier, ov_attrs, &cc);
            /* Blanks (over half of a real screen) skip the decode: word
             * zero-fill — the cache is 4-aligned, every stride is 4n, and
             * memset is off-limits with the flash cache disabled. */
            if (cell->cp == 0x20 || cell->cp == 0) {
                uint32_t *d32 = (uint32_t *)dst;
                for (int i = 0; i < g_rs.gb >> 2; i++) d32[i] = 0;
            } else {
                font_decode_glyph(cell->cp, cc.bold != 0, dst);
            }
        }

        /* Mono flattens everything onto one ramp; bold pop then brightens
         * within it (carry-safe OR). */
        color_t fg = cc.fg, bg = cc.bg;
        if (mono) {
            fg = fx_mono565(fg, mono);
            bg = fx_mono565(bg, mono);
        }
        if (cc.bold && bold_pop)
            fg |= (uint16_t)((fg >> 1) & 0x7BEF);

        s_cc.bg[0][c] = bg;
        s_cc.xf[0][c] = (uint16_t)(fg ^ bg);
        s_cc.ul[c]    = cc.underline;
        any_ul |= cc.underline != 0;
        if (scan_on) {
            const uint16_t fgd = fx_dim565(fg);
            const uint16_t bgd = fx_dim565(bg);
            s_cc.bg[1][c] = bgd;
            s_cc.xf[1][c] = (uint16_t)(fgd ^ bgd);
        }
#if OVERLAY_DIM_DITHER
        s_cc.dim[c] = cc.dim;
        any_dim |= cc.dim != 0;
#endif
    }

    s_cc.any_ul = any_ul;
#if OVERLAY_DIM_DITHER
    s_cc.any_dim = any_dim;
#endif
}

IRAM_ATTR void render_cache_resolve(int char_row, int scan_on, scan_ctx_t *cx)
{
    if (s_cc.row != char_row || s_cc.scan != scan_on
            || s_cc.frame != g_fx_frame) {
        build_row_cache(char_row, scan_on);
        s_cc.row   = char_row;
        s_cc.scan  = scan_on;
        s_cc.frame = g_fx_frame;
    }
    cx->rows   = s_cc.rows;
    cx->bg[0]  = s_cc.bg[0];
    cx->bg[1]  = s_cc.bg[1];
    cx->xf[0]  = s_cc.xf[0];
    cx->xf[1]  = s_cc.xf[1];
    cx->ul     = s_cc.ul;
    cx->any_ul = s_cc.any_ul;
#if OVERLAY_DIM_DITHER
    cx->dim     = s_cc.dim;
    cx->any_dim = s_cc.any_dim;
#endif
}
