/*
 * display_render.c — shared rendering core, compiled for BOTH the ESP32
 * target (bounce-buffer ISR) and the PC simulator (SDL2 frame loop).
 * No SDL or esp_lcd headers — only display.h and font.h.
 */

#include "display_render.h"
#include "display_fx_internal.h"
#include "font.h"
#include <string.h>

/* -------------------------------------------------------------------------
 * Cell buffer — registered once by vterm_init via display_set_text_buffer.
 * Plain statics (DRAM on ESP32, regular BSS on host) so the ISR can reach them.
 * ---------------------------------------------------------------------- */
static DRAM_ATTR const terminal_cell_t *s_cell_buf  = NULL;
static DRAM_ATTR int                    s_cell_cols = 0;
static DRAM_ATTR int                    s_cell_rows = 0;

/* -------------------------------------------------------------------------
 * Overlay buffer — optional second compositing layer.
 * Written by the main task; read by the ISR.  Pointer written last so the ISR
 * never sees a non-NULL pointer with stale cols/rows.
 * ---------------------------------------------------------------------- */
static DRAM_ATTR display_overlay_cell_t *s_overlay_buf  = NULL;
static DRAM_ATTR int                     s_overlay_cols = 0;
static DRAM_ATTR int                     s_overlay_rows = 0;
static DRAM_ATTR color_t                 s_overlay_fg   = COLOR_BLACK;
static DRAM_ATTR color_t                 s_overlay_bg   = COLOR_CYAN;

/* Overlay accent palettes (index 0 → s_overlay_fg), DRAM for the ISR.
 * Dual palette: TEXT accents use VGA bright tones; INVERSE bars use muted
 * companions of the same hues (full-saturation bars read as motley).
 * WHITE stays pure in both — QR codes need true white modules. */
static DRAM_ATTR const color_t s_overlay_pal[OVERLAY_PAL_SIZE] = {
    0,                        /* 0: default → replaced by s_overlay_fg */
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

/* -------------------------------------------------------------------------
 * Cursor state — updated by terminal via display_set_cursor().
 * Blink is driven internally by a frame counter; ~2 Hz at 60 fps.
 * ---------------------------------------------------------------------- */
#define CURSOR_BLINK_FRAMES  15   /* toggle every 15 frames ≈ 250 ms at 60 fps */

static DRAM_ATTR int           s_cursor_x       = 0;
static DRAM_ATTR int           s_cursor_y       = 0;
static DRAM_ATTR cursor_mode_t s_cursor_mode    = CURSOR_NONE;
static DRAM_ATTR uint32_t      s_blink_count    = 0;
static DRAM_ATTR bool          s_cursor_visible = true;

/* -------------------------------------------------------------------------
 * Active cell geometry. Chosen once at boot (the font size is a stored
 * setting applied on reboot) and then read by the ISR on every band, so it
 * lives in DRAM rather than behind a call into the font component.
 * ---------------------------------------------------------------------- */
static DRAM_ATTR int s_fw   = 8;    /* cell width  */
static DRAM_ATTR int s_fh   = 16;   /* cell height */
static DRAM_ATTR int s_band = 16;   /* bounce band height, in scanlines */
static DRAM_ATTR int s_gb   = 16;   /* active glyph stride in s_cc_rows, bytes
                                      * (font_glyph_bytes() of the active size) */

int display_band_height(void) { return s_band; }
int display_bounce_px(void)   { return DISPLAY_WIDTH * s_band; }
int display_text_cols(void)   { return DISPLAY_WIDTH  / s_fw; }
int display_text_rows(void)   { return DISPLAY_HEIGHT / s_fh; }

/* -------------------------------------------------------------------------
 * Per-column rendering cache.
 * Static (not stack) to avoid blowing the ISR stack on ESP32.
 * DRAM_ATTR keeps it in internal SRAM — reachable from ISR without Flash cache.
 * ---------------------------------------------------------------------- */
/* Sized for the WIDEST selectable grid (8x16 -> 100). One shared set of
 * caches, not one per size: three copies would cost ~2.6 KB of the internal
 * DRAM this feature is trying not to spend, and only one size is ever
 * active. The tail beyond the active grid simply goes unused. */
#define RENDER_MAX_COLS  FONT_MAX_COLS

/* Overlay DIM scrim style: 0 = 50% brightness per-cell (cheapest ISR path,
 * default), 1 = dithered checkerboard (crisper but adds a per-band post-
 * pass, compiled out when 0). */
#ifndef OVERLAY_DIM_DITHER
#define OVERLAY_DIM_DITHER 0
#endif

/* Column cache as parallel arrays. bg/xf carry TWO variants selected per
 * scanline ([0] normal, [1] scanline-dimmed) so the hot loop pays no
 * per-pixel effect cost. Glyphs are no longer cached by pointer — the font
 * tables are compressed (PackBits row-RLE), so there is nothing to point AT.
 * Instead build_row_cache() decodes each column's glyph ONCE per character
 * row into a flat byte cache (s_cc_rows), sized for the worst case across
 * every ENABLED font (FONT_MAX_CACHE_BYTES = max cols * glyph_bytes, see
 * font.h). The band scan loop then reads plain decoded rows out of it — no
 * decode and no NULL check in the hot per-pixel path — by indexing
 * rows + c * glyph_bytes and casting to the active row type.
 *
 * ONE bank, rebuilt synchronously on the first chunk of each character row.
 * The -O2 glyph decoder made the full rebuild cheap enough that the earlier
 * double-banked look-ahead (splitting the next row's rebuild across the
 * current row's two chunks) stopped paying for its complexity and ~4.4 KB
 * of DRAM: measured worst chunks on a dense 100%-painted screen are
 * 10x20 389 us / 12x24 408 us against 534/640 us chunk periods (banking
 * bought 311/341 — headroom we don't need). Reusing the cache for a row's
 * second chunk DOES pay: rebuilding every chunk costs +24%/+19% average
 * ISR time for no complexity win, so the three validity tags stay. */
static DRAM_ATTR _Alignas(4) uint8_t s_cc_rows[FONT_MAX_CACHE_BYTES];
static DRAM_ATTR uint16_t           s_cc_bg[2][RENDER_MAX_COLS];
static DRAM_ATTR uint16_t           s_cc_xf[2][RENDER_MAX_COLS];
static DRAM_ATTR uint8_t            s_cc_ul[RENDER_MAX_COLS];
#if OVERLAY_DIM_DITHER
static DRAM_ATTR uint8_t            s_cc_dim[RENDER_MAX_COLS];
#endif

/* Cache validity: the cache is scanned only when its (row, frame,
 * scanline-variant) tags all match; anything else (new row, new frame, a
 * config flip, an effect-clipped band) rebuilds synchronously first. */
static DRAM_ATTR int     s_cc_row   = -1;  /* char row held, -1 = none */
static DRAM_ATTR int     s_cc_scan  = 0;   /* dim variant populated?   */
static DRAM_ATTR uint8_t s_cc_frame = 0;   /* g_fx_frame at build      */

void display_render_set_font(int width, int height)
{
    /* Bands must be even (scanline-parity effects), divide the cell height
     * (never straddle a character row) and divide the panel height. Checked
     * at runtime because the size is no longer a build constant; a bad
     * value keeps the previous geometry instead of tearing in the ISR. */
    const int band = (height > 16) ? (height / 2) : height;

    if (width <= 0 || height <= 0 ||
        (width & 1) || (band & 1) ||
        (height % band) != 0 ||
        (DISPLAY_HEIGHT % height) != 0 ||
        (DISPLAY_HEIGHT % band) != 0 ||
        (DISPLAY_WIDTH / width) > FONT_MAX_COLS) {
        return;
    }

    s_fw     = width;
    s_fh     = height;
    s_band   = band;
    s_gb     = (int)font_glyph_bytes();
    s_cc_row = -1;   /* stale column cache — rebuild */
}

/* -------------------------------------------------------------------------
 * Carry-safe RGB565 helpers (per-field shift/mask math — a masked
 * right-shift can only shrink a 565 field, never bleed into a neighbour).
 * ---------------------------------------------------------------------- */

/* Scanline dim: 93.75% brightness, the one level that read as CRT texture
 * on hardware — everything stronger (87.5% and below) read as bars. */
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

/* Branchless pixel-pair → 32-bit word (little-endian, 2×RGB565):
 *   bit   = (gb >> (W-1-p)) & 1              (leftmost pixel = top bit)
 *   mask  = (uint16_t)(0u - bit)             (0xFFFF / 0x0000)
 *   pixel = bg ^ (xorfg & mask)              (bg when 0, fg when 1) */
#define GPAIR(W, gb, p0, p1, bg_v, xor_v)                                       \
    (   (uint32_t)((uint16_t)((bg_v) ^ ((xor_v) &                               \
            (uint16_t)(0u - (((unsigned)(gb) >> ((W) - 1u - (p0))) & 1u))))) \
    |  ((uint32_t)((uint16_t)((bg_v) ^ ((xor_v) &                               \
            (uint16_t)(0u - (((unsigned)(gb) >> ((W) - 1u - (p1))) & 1u))))) << 16) )

/* Emit one character column ((W)/2 aligned uint32 stores), unrolled per
 * width: all enabled sizes coexist and the band loop picks one unrolling
 * ONCE per band (see SCAN_BAND). */
#define EMIT_COL_8(d, gb, bg_v, xor_v) do {                                     \
    (d)[0] = GPAIR(8, gb, 0, 1, bg_v, xor_v);                                   \
    (d)[1] = GPAIR(8, gb, 2, 3, bg_v, xor_v);                                   \
    (d)[2] = GPAIR(8, gb, 4, 5, bg_v, xor_v);                                   \
    (d)[3] = GPAIR(8, gb, 6, 7, bg_v, xor_v);                                   \
} while (0)
#define EMIT_COL_10(d, gb, bg_v, xor_v) do {                                    \
    (d)[0] = GPAIR(10, gb, 0, 1, bg_v, xor_v);                                  \
    (d)[1] = GPAIR(10, gb, 2, 3, bg_v, xor_v);                                  \
    (d)[2] = GPAIR(10, gb, 4, 5, bg_v, xor_v);                                  \
    (d)[3] = GPAIR(10, gb, 6, 7, bg_v, xor_v);                                  \
    (d)[4] = GPAIR(10, gb, 8, 9, bg_v, xor_v);                                  \
} while (0)
#define EMIT_COL_12(d, gb, bg_v, xor_v) do {                                    \
    (d)[0] = GPAIR(12, gb, 0,  1,  bg_v, xor_v);                                \
    (d)[1] = GPAIR(12, gb, 2,  3,  bg_v, xor_v);                                \
    (d)[2] = GPAIR(12, gb, 4,  5,  bg_v, xor_v);                                \
    (d)[3] = GPAIR(12, gb, 6,  7,  bg_v, xor_v);                                \
    (d)[4] = GPAIR(12, gb, 8,  9,  bg_v, xor_v);                                \
    (d)[5] = GPAIR(12, gb, 10, 11, bg_v, xor_v);                                \
} while (0)

/* ANSI-256 colour → RGB565. IRAM: callable from the ISR. */
color_t IRAM_ATTR display_ansi_to_rgb565(uint8_t ansi_color)
{
    /* 0-15: standard 16 colours */
    static DRAM_ATTR const color_t ansi_palette[16] = {
        RGB565(0,   0,   0  ),  /*  0 Black             */
        RGB565(128, 0,   0  ),  /*  1 Red               */
        RGB565(0,   128, 0  ),  /*  2 Green             */
        RGB565(128, 128, 0  ),  /*  3 Yellow            */
        RGB565(0,   0,   128),  /*  4 Blue              */
        RGB565(128, 0,   128),  /*  5 Magenta           */
        RGB565(0,   128, 128),  /*  6 Cyan              */
        RGB565(192, 192, 192),  /*  7 White             */
        RGB565(128, 128, 128),  /*  8 Bright Black/Gray */
        RGB565(255, 0,   0  ),  /*  9 Bright Red        */
        RGB565(0,   255, 0  ),  /* 10 Bright Green      */
        RGB565(255, 255, 0  ),  /* 11 Bright Yellow     */
        RGB565(0,   0,   255),  /* 12 Bright Blue       */
        RGB565(255, 0,   255),  /* 13 Bright Magenta    */
        RGB565(0,   255, 255),  /* 14 Bright Cyan       */
        RGB565(255, 255, 255),  /* 15 Bright White      */
    };

    if (ansi_color < 16) {
        return ansi_palette[ansi_color];
    }

    /* 16-231: 6×6×6 RGB cube */
    if (ansi_color <= 231) {
        uint8_t idx = ansi_color - 16;
        uint8_t r = (idx / 36) * 51;
        uint8_t g = ((idx / 6) % 6) * 51;
        uint8_t b = (idx % 6) * 51;
        return RGB565(r, g, b);
    }

    /* 232-255: grayscale ramp */
    uint8_t gray = 8 + (ansi_color - 232) * 10;
    return RGB565(gray, gray, gray);
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void display_set_text_buffer(const terminal_cell_t *buf, int cols, int rows)
{
    s_cell_buf  = buf;
    s_cell_cols = cols;
    s_cell_rows = rows;
}

void display_set_cursor(int x, int y, cursor_mode_t mode)
{
    if (x != s_cursor_x || y != s_cursor_y) {
        s_blink_count    = 0;
        s_cursor_visible = true;
    }
    s_cursor_x    = x;
    s_cursor_y    = y;
    s_cursor_mode = mode;
}

void display_set_overlay_buffer(display_overlay_cell_t *buf, int cols, int rows)
{
    s_overlay_cols = cols;
    s_overlay_rows = rows;
    s_overlay_buf  = buf;   /* written last — atomic 32-bit store; ISR reads after */
}

void display_set_overlay_colors(color_t fg, color_t bg)
{
    s_overlay_fg = fg;
    s_overlay_bg = bg;
}

void display_get_text_size(int *cols, int *rows)
{
    if (cols) *cols = s_cell_cols;
    if (rows) *rows = s_cell_rows;
}

/* Visual bell: a brief red tag flashing in the top-right corner, drawn by
 * the ISR over the top band (works in-session with no overlay). */
static DRAM_ATTR volatile int s_bell_frames = 0;   /* frames the tag stays up */
static DRAM_ATTR bool         s_bell_show   = false; /* latched at frame tick */

#define BELL_TOTAL   40    /* 4 half-periods → exactly two on/off flashes */
#define BELL_HALF    10    /* frames per on (or off) half at ~60 fps      */

void display_bell(void) { s_bell_frames = BELL_TOTAL; }

/* Fill the column cache with the resolved fg/bg/glyph for character row
 * @p cr. @p scan_on (0/1) selects whether the scanline-dim variant [1] is
 * also produced (captured once per band so the fill and the scanline loop
 * always agree, even if the config changes mid-band). */
static IRAM_ATTR void build_row_cache(int cr, int scan_on)
{
    const terminal_cell_t *row_cells = s_cell_buf + cr * s_cell_cols;
    const display_overlay_cell_t *ov_row =
        (s_overlay_buf && cr < s_overlay_rows)
        ? (s_overlay_buf + cr * s_overlay_cols) : NULL;

    const uint8_t mono     = g_fx_cfg.mono;
    const uint8_t bold_pop = g_fx_cfg.bold_pop;

#if DISPLAY_FX_ROW_GLOW
    /* Row-recency back glow: resolve this row's tier once per band.
     * 0 = none, 1 = subtle (12.5% accent), 2 = strong (25% accent). */
    int glow_tier = 0;
    if (g_fx_cfg.glow && cr < DISPLAY_FX_MAX_ROWS) {
        const uint8_t gf  = g_fx_cfg.glow_frames;
        const uint8_t age = (uint8_t)(g_fx_frame - g_fx_row_stamp[cr]);
        if (age < gf)
            glow_tier = (g_fx_cfg.glow_strength && age * 2 < gf) ? 2 : 1;
        else if (age > 128)
            /* Re-pin long-expired stamps so the uint8 frame counter can't
             * wrap back into the glow window and re-tint a stale row. Races
             * display_fx_touch_row benignly (a lost touch self-heals). */
            g_fx_row_stamp[cr] = (uint8_t)(g_fx_frame - gf);
    }
#endif

    for (int c = 0; c < s_cell_cols; c++) {
        color_t fg, bg;
        uint8_t *dst = s_cc_rows + (size_t)c * (size_t)s_gb;
        uint8_t underline = 0;
        uint8_t bold      = 0;
#if OVERLAY_DIM_DITHER
        uint8_t dim = 0;
#endif

        const uint8_t  ov_attrs = (ov_row && c < s_overlay_cols) ? ov_row[c].attrs : 0;
        const uint16_t ov_cp    = (ov_row && c < s_overlay_cols) ? ov_row[c].cp    : 0;
        const uint8_t  ov_color = (ov_row && c < s_overlay_cols) ? ov_row[c].color : 0;

        if (ov_cp != 0) {
            if (ov_attrs & OVERLAY_ATTR_INVERSE) {
                /* Solid bar: muted accent bg. Colored bars (1-6) carry a
                 * pale tint of their own hue as text; gray/white (0,7) keep
                 * dark text — QR modules must stay dark-on-white. */
                bg = s_overlay_bar[ov_color];
                if (ov_color >= 1 && ov_color <= 6 &&
                        !(ov_attrs & OVERLAY_ATTR_BRIGHT))
                    fg = (color_t)(((bg >> 2) & 0x39E7) + 0x7BEF + 0x39E7);
                else
                    fg = s_overlay_bg;
            } else {
                fg = ov_color ? s_overlay_pal[ov_color] : s_overlay_fg;
                bg = s_overlay_bg;
            }
            /* Focus glow: wash the bg 50% toward white (carry-safe
             * half-sum) — the focused bar turns pastel, text stays dark. */
            if (ov_attrs & OVERLAY_ATTR_BRIGHT)
                bg = (color_t)(((bg >> 1) & 0x7BEF) + 0x7BEF);
            /* `bold` stays 0: bold-pop is a terminal effect and must not
             * recolor overlay chrome. */
            font_decode_glyph(ov_cp, (ov_attrs & OVERLAY_ATTR_BOLD) != 0, dst);
        } else {
            const terminal_cell_t *cell = &row_cells[c];
            fg = cell->fg_color;
            bg = cell->bg_color;
            if (cell->attrs & ATTR_REVERSE) { color_t t = fg; fg = bg; bg = t; }
            underline = cell->attrs & ATTR_UNDERLINE;
            bold      = cell->attrs & ATTR_BOLD;
            /* Blanks skip the decode: U+0020 is blank in Terminus and
             * screens run well over half blank, so a plain zero-fill loop
             * (no memset — ISR runs with the flash cache disabled) beats
             * decoding an all-zero glyph. */
            if (cell->cp == 0x20 || cell->cp == 0) {
                for (int i = 0; i < s_gb; i++) dst[i] = 0;
            } else {
                font_decode_glyph(cell->cp, bold != 0, dst);
            }
#if DISPLAY_FX_ROW_GLOW
            /* Back glow tints the terminal bg toward the warm accent,
             * before any scrim dim so a modal's scrim also dims the glow. */
            if (glow_tier == 1)
                bg = (color_t)((bg - ((bg >> 3) & 0x18E3)) + g_fx_glow_acc[0]);
            else if (glow_tier == 2)
                bg = (color_t)((bg - ((bg >> 2) & 0x39E7)) + g_fx_glow_acc[1]);
#endif
            if (ov_attrs & OVERLAY_ATTR_DIM) {
#if OVERLAY_DIM_DITHER
                dim = 1;   /* full colour; checkerboarded in the post-pass */
#else
                fg = (color_t)((fg >> 1) & 0x7BEFu);
                bg = (color_t)((bg >> 1) & 0x7BEFu);
#endif
            }
        }

        /* Mono phosphor mode flattens everything (incl. overlay + glow)
         * onto one ramp; bold pop then brightens within the ramp
         * (carry-safe OR — sets lower field bits, never overflows). */
        if (mono) {
            fg = fx_mono565(fg, mono);
            bg = fx_mono565(bg, mono);
        }
        if (bold && bold_pop)
            fg |= (uint16_t)((fg >> 1) & 0x7BEF);

        s_cc_bg[0][c] = bg;
        s_cc_xf[0][c] = (uint16_t)(fg ^ bg);
        s_cc_ul[c]    = underline;
        if (scan_on) {
            const uint16_t fgd = fx_dim565(fg);
            const uint16_t bgd = fx_dim565(bg);
            s_cc_bg[1][c] = bgd;
            s_cc_xf[1][c] = (uint16_t)(fgd ^ bgd);
        }
#if OVERLAY_DIM_DITHER
        s_cc_dim[c] = dim;
#endif
    }
}

/* Overlay a white bell on a red tag in the top-right of the screen. The bell
 * is a hand-drawn 16x16 silhouette (bit 15 = leftmost column); @p dst points
 * at the band's first pixel, @p y0/@p nrows select the slice of the tag that
 * falls inside this band (sub-row bands render the tag across two calls). */
static IRAM_ATTR void draw_bell_tag(color_t *dst, int y0, int nrows)
{
    static DRAM_ATTR const uint16_t bell[16] = {
        0x0180, 0x0180, 0x03C0, 0x07E0, 0x07E0, 0x0FF0, 0x0FF0, 0x1FF8,
        0x1FF8, 0x3FFC, 0x3FFC, 0x7FFE, 0x7FFE, 0x0000, 0x0180, 0x0180,
    };
    const color_t red   = 0xF800u;
    const color_t white = 0xFFFFu;
    const int tag_w  = 32;                          /* 32 px wide            */
    const int x0     = DISPLAY_WIDTH - tag_w;       /* hard top-right corner */
    const int bell_x = x0 + (tag_w - 16) / 2;       /* centre the 16px bell  */

    if (nrows > 16 - y0) nrows = 16 - y0;           /* tag is 16 px tall */
    for (int n = 0; n < nrows; n++) {
        color_t *p = dst + n * DISPLAY_WIDTH + x0;
        for (int i = 0; i < tag_w; i++) p[i] = red;         /* red background */
        uint16_t bits = bell[y0 + n];
        color_t *b = dst + n * DISPLAY_WIDTH + bell_x;
        for (int col = 0; col < 16; col++)
            if ((bits >> (15 - col)) & 1u) b[col] = white;  /* white bell     */
    }
}

/* Signal-loss static: 16 packed pixel-pairs of grayscale "snow", indexed by
 * a cheap LCG so no RNG state is needed in the ISR. DRAM-resident. */
static DRAM_ATTR const uint32_t s_fx_noise[16] = {
    0x0000FFFFu, 0xFFFF0000u, 0x00000000u, 0xFFFFFFFFu,
    0x84108410u, 0x00008410u, 0x84100000u, 0xC618C618u,
    0x42084208u, 0x0000C618u, 0xFFFF8410u, 0x21042104u,
    0x4208FFFFu, 0x00002104u, 0xC6184208u, 0x8410FFFFu,
};

/**
 * Render one horizontal band (one bounce-buffer height) into dst.
 * A band never straddles a character row: it covers either a whole row
 * (8x16) or half of one (10x20, 12x24). pos_px = start scanline ×
 * DISPLAY_WIDTH; n_bytes = band pixel count × sizeof(color_t).
 */
static IRAM_ATTR void render_chunk_body(color_t *dst, int pos_px, int n_bytes)
{
    if (!dst) return;

    /* Cell buffer not yet registered — fill black. */
    if (!s_cell_buf || s_cell_cols <= 0 || s_cell_rows <= 0) {
        uint32_t *p = (uint32_t *)dst;
        int words = n_bytes >> 2;
        for (int i = 0; i < words; i++) p[i] = 0;
        return;
    }

    /* Active cell geometry, latched once per band (DRAM-resident). */
    const int fw = s_fw, fh = s_fh;
    const int col_words = fw >> 1;

    const int start_scan = pos_px / DISPLAY_WIDTH;
    const int num_scans  = (n_bytes >> 1) / DISPLAY_WIDTH;  /* n_bytes/2 = pixels */
    const int char_row   = start_scan / fh;
    /* First glyph scanline of this band; non-zero only with sub-row bands.
     * Always a multiple of the (even) band height, so scanline-parity
     * effects stay aligned. */
    const int glyph_row0 = start_scan % fh;

    /* Tick per-frame counters once per full frame (scanline 0 = new frame). */
    if (start_scan == 0) {
        if (++s_blink_count >= CURSOR_BLINK_FRAMES) {
            s_blink_count    = 0;
            s_cursor_visible = !s_cursor_visible;
        }
        g_fx_frame++;
        if (g_fx_wipe_left     > 0) g_fx_wipe_left--;
        if (g_fx_collapse_left > 0) g_fx_collapse_left--;
        if (g_fx_static_left   > 0) g_fx_static_left--;
        /* Visual bell: latch this frame's on/off flash phase. */
        if (s_bell_frames > 0) {
            s_bell_show = ((BELL_TOTAL - s_bell_frames) / BELL_HALF) % 2 == 0;
            s_bell_frames--;
        } else {
            s_bell_show = false;
        }
    }

    /* Below the text area — fill black. */
    if (char_row >= s_cell_rows) {
        uint32_t *p = (uint32_t *)dst;
        int words = n_bytes >> 2;
        for (int i = 0; i < words; i++) p[i] = 0;
        return;
    }

    /* ------------------------------------------------------------------
     * Wipe / collapse clip window: while one is active, only scanlines in
     * [wv0, wv1) show content; the rest go black, with bright edge lines
     * at [ea0,ea1) / [eb0,eb1). Bands fully outside skip rendering.
     * ------------------------------------------------------------------ */
    int wv0 = 0, wv1 = DISPLAY_HEIGHT;
    int ea0 = 0, ea1 = 0, eb0 = 0, eb1 = 0;
    color_t ecol = 0;
    bool clipped = false;
    if (g_fx_collapse_left > 0 && g_fx_collapse_total > 2) {
        clipped = true;
        ecol = 0xFFFFu;                      /* hot white collapsing edges */
        if (g_fx_collapse_left <= 2) {       /* last 2 frames: center flash */
            wv0 = wv1 = 0;
            ea0 = DISPLAY_HEIGHT / 2 - 1;
            ea1 = ea0 + 2;
        } else {
            /* Window shrinks to ~0 by the time the flash frames start. */
            const int half = (DISPLAY_HEIGHT / 2) * (g_fx_collapse_left - 2)
                                                  / (g_fx_collapse_total - 2);
            wv0 = DISPLAY_HEIGHT / 2 - half;
            wv1 = DISPLAY_HEIGHT / 2 + half;
            ea0 = wv0; ea1 = wv0 + 2;
            eb0 = wv1 - 2; eb1 = wv1;
        }
    } else if (g_fx_wipe_left > 0 && g_fx_wipe_total > 0) {
        clipped = true;
        ecol = 0x07FFu;                      /* cyan leading edge */
        const int reveal = DISPLAY_HEIGHT * (g_fx_wipe_total - g_fx_wipe_left)
                                          / g_fx_wipe_total;
        wv1 = reveal;
        ea0 = reveal;
        ea1 = reveal + 2 > DISPLAY_HEIGHT ? DISPLAY_HEIGHT : reveal + 2;
    }

    const int band_y0 = start_scan;
    if (clipped && (band_y0 >= wv1 || band_y0 + num_scans <= wv0)) {
        /* Band fully hidden: black + any edge lines. Invalidate the row
         * cache — a later band must never reuse a cache this path skipped
         * rebuilding. */
        s_cc_row = -1;
        uint32_t *p = (uint32_t *)dst;
        int words = n_bytes >> 2;
        for (int i = 0; i < words; i++) p[i] = 0;
        const uint32_t e2 = (uint32_t)ecol | ((uint32_t)ecol << 16);
        for (int n = 0; n < num_scans; n++) {
            const int y = band_y0 + n;
            if ((y >= ea0 && y < ea1) || (y >= eb0 && y < eb1)) {
                uint32_t *e = (uint32_t *)(dst + (unsigned)n * DISPLAY_WIDTH);
                for (int i = 0; i < DISPLAY_WIDTH / 2; i++) e[i] = e2;
            }
        }
        return;
    }

    /* ------------------------------------------------------------------
     * Column-cache resolution: rebuild synchronously on a row's first
     * chunk (or after anything that invalidated the tags). A row's second
     * chunk (10x20/12x24) reuses the cache for free.
     * ------------------------------------------------------------------ */
    const int scan_on = g_fx_cfg.scanlines ? 1 : 0;
    if (s_cc_row != char_row || s_cc_scan != scan_on
            || s_cc_frame != g_fx_frame) {
        build_row_cache(char_row, scan_on);
        s_cc_row   = char_row;
        s_cc_scan  = scan_on;
        s_cc_frame = g_fx_frame;
    }
    const int ncols = s_cell_cols;

    /* Right margin beyond the text area, in uint32 (pixel-pair) words.
     * Zero for 8x16/10x20; 4 words (8 px) for 12x24 (66*12 = 792 < 800). */
    const int margin_words = (DISPLAY_WIDTH - ncols * fw) >> 1;

    color_t *dst_base = dst;

    /* Decoded-glyph rows — hoisted so the macro below indexes a plain
     * pointer. */
    const uint8_t *const cc_rows = s_cc_rows;

    /* One whole band at a FIXED cell width, instantiated per enabled size
     * and selected ONCE per band: width, glyph row type and store count are
     * constants inside, so the per-pixel path matches a compile-time-sized
     * build. Only this loop is duplicated. */
#define SCAN_BAND(W, ROW_T, EMIT)                                              \
    do {                                                                       \
        enum { CW = (W) / 2 };                                                 \
        for (int n = 0; n < num_scans; n++) {                                  \
            const int gl = glyph_row0 + n;                                     \
            /* CRT scanlines: odd scanlines read the pre-dimmed colour        \
             * variant. Bands start on even scanlines, so n's parity is       \
             * global parity. */                                              \
            const int sel = n & scan_on & 1;                                   \
            const uint16_t *bgv = s_cc_bg[sel];                                \
            const uint16_t *xfv = s_cc_xf[sel];                                \
            uint32_t *d = (uint32_t *)(dst_base + (unsigned)n * DISPLAY_WIDTH);\
                                                                               \
            /* glyph_bytes == 2*W*sizeof(ROW_T) for every enabled size, since  \
             * height == 2*width for all three grids — a compile-time-        \
             * constant stride per instantiation, no s_gb load needed here.   \
             * Rows are decoded plain into the bank's cache, so they read     \
             * straight through — no NULL check. */                           \
            int c = 0;                                                         \
            for (; c + 1 < ncols; c += 2) {                                    \
                const ROW_T *g0 = (const ROW_T *)(cc_rows +                    \
                        (size_t)(c    ) * (2u*(W)*sizeof(ROW_T)));             \
                const ROW_T *g1 = (const ROW_T *)(cc_rows +                    \
                        (size_t)(c + 1) * (2u*(W)*sizeof(ROW_T)));             \
                const ROW_T b0 = g0[gl];                                       \
                const ROW_T b1 = g1[gl];                                       \
                const uint16_t bg0 = bgv[c    ], xf0 = xfv[c    ];             \
                const uint16_t bg1 = bgv[c + 1], xf1 = xfv[c + 1];             \
                                                                               \
                EMIT(d,      b0, bg0, xf0);                                    \
                EMIT(d + CW, b1, bg1, xf1);                                    \
                d += 2 * CW;                                                   \
            }                                                                  \
                                                                               \
            /* Trailing odd column (defensive; all three grids are even). */   \
            if (c < ncols) {                                                   \
                const ROW_T *g0 = (const ROW_T *)(cc_rows +                    \
                        (size_t)(c) * (2u*(W)*sizeof(ROW_T)));                 \
                const ROW_T b0 = g0[gl];                                       \
                const uint16_t bg0 = bgv[c], xf0 = xfv[c];                     \
                EMIT(d, b0, bg0, xf0);                                         \
                d += CW;                                                       \
            }                                                                  \
                                                                               \
            /* Blank the margin right of the text area (black). */             \
            for (int i = 0; i < margin_words; i++) d[i] = 0;                   \
        }                                                                      \
    } while (0)

    switch (fw) {
#if FONT_RT_8X16
    case 8:  SCAN_BAND(8,  uint8_t,  EMIT_COL_8);  break;
#endif
#if FONT_RT_10X20
    case 10: SCAN_BAND(10, uint16_t, EMIT_COL_10); break;
#endif
#if FONT_RT_12X24
    case 12: SCAN_BAND(12, uint16_t, EMIT_COL_12); break;
#endif
    default: break;   /* font_init guarantees a linked size */
    }
#undef SCAN_BAND

    /* Underline post-pass — force fg on the last two scanlines of the CELL
     * (they live in the row's last band). Touches only flagged columns. */
    {
        int ul_first = (fh - 2) - glyph_row0;  /* band-relative */
        if (ul_first < 0) ul_first = 0;
        if (ul_first < num_scans) {
            for (int c = 0; c < ncols; c++) {
                if (!s_cc_ul[c]) continue;
                for (int n = ul_first; n < num_scans; n++) {
                    const int sel = n & scan_on & 1;   /* match scanline variant */
                    const uint16_t fg = (uint16_t)(s_cc_bg[sel][c]
                                                   ^ s_cc_xf[sel][c]);
                    const uint32_t fg2 = (uint32_t)fg | ((uint32_t)fg << 16);
                    uint32_t *p = (uint32_t *)(dst_base + (unsigned)n * DISPLAY_WIDTH
                                               + c * fw);
                    for (int w = 0; w < col_words; w++) p[w] = fg2;
                }
            }
        }
    }

#if OVERLAY_DIM_DITHER
    /* Dithered dim post-pass — black out a checkerboard on scrim columns.
     * Cell sizes are even, so the parity reduces to the band scanline:
     * even n blacks the odd pixels (high half of each pair), odd n the
     * even ones. */
    for (int c = 0; c < ncols; c++) {
        if (!s_cc_dim[c]) continue;
        for (int n = 0; n < num_scans; n++) {
            const uint32_t m = (n & 1) ? 0xFFFF0000u : 0x0000FFFFu;
            uint32_t *p = (uint32_t *)(dst_base + (unsigned)n * DISPLAY_WIDTH
                                       + c * fw);
            for (int w = 0; w < col_words; w++) p[w] &= m;
        }
    }
#endif

    /* Cursor overlay — XOR the cursor region, guaranteeing contrast. The
     * underscore lives on the last two scanlines of the CELL; a block
     * cursor spans every band of its row. Word alignment is guaranteed
     * (row stride and cell offsets are multiples of 4 bytes). */
    if (s_cursor_mode != CURSOR_NONE && s_cursor_visible &&
            s_cursor_y == char_row && s_cursor_x >= 0 && s_cursor_x < ncols) {

        const int cx       = s_cursor_x;
        const int px_start = cx * fw;
        int first = 0;

        if (s_cursor_mode == CURSOR_UNDERSCORE) {
            first = (fh - 2) - glyph_row0;   /* band-relative */
            if (first < 0) first = 0;
        }
        for (int n = first; n < num_scans; n++) {
            uint32_t *p = (uint32_t *)(dst_base + n * DISPLAY_WIDTH + px_start);
            for (int w = 0; w < col_words; w++) p[w] ^= 0xFFFFFFFFu;
        }
    }

    /* CRT line wobble — a ~16-scanline S-wiggle sweeps down the screen;
     * the vertical envelope is one full signed sine and bulge depth follows
     * a temporal sine from the LUT. At most 16 shifted lines per FRAME.
     * The shift is in-place WITHIN the line — an offset render origin
     * would spill across band boundaries (the sync-tear lesson). */
    if (g_fx_cfg.wobble) {
        /* Vertical envelope: sin(2*pi * (k+1) / 17) * 127, k = 0..15 —
         * a full period: bulge right, then bulge left (for positive dx). */
        static DRAM_ATTR const int8_t wob_env[16] = {
             46,  86, 114, 126, 122, 101,  67,  23,
            -23, -67, -101, -122, -126, -114, -86, -46,
        };
        const int dx = g_fx_wobble_lut[g_fx_frame];
        /* Analog grit: amplitude drifts ±25% (re-rolled ~10 Hz) so no two
         * sweeps look identical. Same LCG recipe as the static burst. */
        const uint32_t hf = (uint32_t)(g_fx_frame >> 2) * 2246822519u;
        const int dxa = (dx * (12 + (int)((hf >> 7) & 7))) >> 4;
        /* Wiggle top: 0..637 over the 256-frame cycle; >479 = resting. */
        const int yc = (int)(((unsigned)g_fx_frame * 5u) >> 1);
        const int n0 = yc - start_scan;
        for (int k = 0; k < 16 && dx != 0; k++) {
            const int n = n0 + k;
            if (n < 0 || n >= num_scans) continue;     /* other band / off */
            /* Per-row ±1 px jitter (~20 Hz) roughens the clean S-curve.
             * Hashed from absolute y + frame, so the sub-row bands of one
             * frame always agree at their seam. */
            const uint32_t hr = ((uint32_t)(start_scan + n) * 2654435761u)
                              ^ ((uint32_t)(g_fx_frame >> 1) * 2246822519u);
            int d = (dxa * (int)wob_env[k] + 64) >> 7;
            d += (int)((hr >> 9) % 3u) - 1;
            if (d == 0) continue;
            color_t *lp = dst_base + (unsigned)n * DISPLAY_WIDTH;
            if (d > 0) {                               /* shift right */
                for (int i = DISPLAY_WIDTH - 1; i >= d; i--) lp[i] = lp[i - d];
                for (int i = d - 1; i >= 0; i--)             lp[i] = 0;
            } else {                                   /* shift left */
                for (int i = 0; i < DISPLAY_WIDTH + d; i++) lp[i] = lp[i - d];
                for (int i = DISPLAY_WIDTH + d; i < DISPLAY_WIDTH; i++)
                    lp[i] = 0;
            }
        }
    }

    /* Signal-loss static burst — grayscale snow on a few pseudo-random
     * scanlines; transient by design (the one per-pixel-ish pass allowed). */
    if (g_fx_static_left > 0 && num_scans > 0) {
        int nl = g_fx_cfg.static_lines;
        if (nl > 4) nl = 4;
        /* Seed by BAND (not char row): with sub-row bands a per-row seed
         * would draw the same snow lines in both halves of the row, and the
         * pairing reads as structure instead of noise. */
        uint32_t h = ((uint32_t)(start_scan / s_band) * 2654435761u)
                   ^ ((uint32_t)g_fx_frame * 2246822519u);
        for (int k = 0; k < nl; k++) {
            h = h * 1664525u + 1013904223u;
            /* Unbiased 0..num_scans-1 from 4 hash bits (scale, not clamp). */
            int n = (int)((((h >> 27) & 15u) * (unsigned)num_scans) >> 4);
            uint32_t *p = (uint32_t *)(dst_base + (unsigned)n * DISPLAY_WIDTH);
            uint32_t r = h;
            for (int i = 0; i < DISPLAY_WIDTH / 2; i += 4) {
                r = r * 1664525u + 1013904223u;
                p[i]     = s_fx_noise[r         & 15];
                p[i + 1] = s_fx_noise[(r >> 4)  & 15];
                p[i + 2] = s_fx_noise[(r >> 8)  & 15];
                p[i + 3] = s_fx_noise[(r >> 12) & 15];
            }
        }
    }

    /* Wipe / collapse post-pass for bands straddling the visible window:
     * black out hidden scanlines, then draw the bright edge lines. */
    if (clipped) {
        const uint32_t e2 = (uint32_t)ecol | ((uint32_t)ecol << 16);
        for (int n = 0; n < num_scans; n++) {
            const int y = band_y0 + n;
            uint32_t *p = (uint32_t *)(dst_base + (unsigned)n * DISPLAY_WIDTH);
            if ((y >= ea0 && y < ea1) || (y >= eb0 && y < eb1)) {
                for (int i = 0; i < DISPLAY_WIDTH / 2; i++) p[i] = e2;
            } else if (y < wv0 || y >= wv1) {
                for (int i = 0; i < DISPLAY_WIDTH / 2; i++) p[i] = 0;
            }
        }
    }

    /* Visual bell: flash the red bell tag twice over the top-right corner.
     * The 16 px tag spans the whole first character row; each band of that
     * row draws its slice (glyph_row0 offsets into the tag bitmap). */
    if (char_row == 0 && s_bell_show && glyph_row0 < 16)
        draw_bell_tag(dst_base, glyph_row0, num_scans);

}

#undef GPAIR
#undef EMIT_COL_8
#undef EMIT_COL_10
#undef EMIT_COL_12

#undef RENDER_MAX_COLS

/* -------------------------------------------------------------------------
 * Public entry — optionally wrapped in a cycle-count bench (CONFIG_
 * DISPLAY_ISR_BENCH, default y): per-chunk CPU cycles accumulated in DRAM,
 * drained by display_render_bench_get()/_reset() from any task. ~20
 * cycles/chunk overhead when enabled.
 * ---------------------------------------------------------------------- */
#ifdef CONFIG_DISPLAY_ISR_BENCH
#include "esp_cpu.h"

/* Cycle total must be 64-bit: a 30 s window at ~113k cycles/chunk is within
 * 8% of the u32 wrap, and any longer window overflows. The ISR's two-word
 * add can tear under a cross-core read, so the reader spins until the chunk
 * count is stable around the read. */
static DRAM_ATTR volatile uint64_t s_bench_cyc = 0;
static DRAM_ATTR volatile uint32_t s_bench_n   = 0;
static DRAM_ATTR volatile uint32_t s_bench_max = 0;

void display_render_bench_get(uint32_t *avg_cycles, uint32_t *max_cycles, uint32_t *chunks)
{
    uint64_t cyc;
    uint32_t n, n2;
    do {
        n   = s_bench_n;
        cyc = s_bench_cyc;
        n2  = s_bench_n;
    } while (n != n2);
    if (avg_cycles) *avg_cycles = n ? (uint32_t)(cyc / n) : 0;
    if (max_cycles) *max_cycles = s_bench_max;
    if (chunks)     *chunks     = n;
}

void display_render_bench_reset(void)
{
    s_bench_cyc = 0;
    s_bench_n   = 0;
    s_bench_max = 0;
}

void IRAM_ATTR display_render_chunk(color_t *dst, int pos_px, int n_bytes)
{
    const uint32_t t0 = esp_cpu_get_cycle_count();
    render_chunk_body(dst, pos_px, n_bytes);
    const uint32_t dt = esp_cpu_get_cycle_count() - t0;
    s_bench_cyc += dt;
    s_bench_n++;
    if (dt > s_bench_max) s_bench_max = dt;
}
#else
void display_render_bench_get(uint32_t *avg_cycles, uint32_t *max_cycles, uint32_t *chunks)
{
    if (avg_cycles) *avg_cycles = 0;
    if (max_cycles) *max_cycles = 0;
    if (chunks)     *chunks     = 0;
}

void display_render_bench_reset(void) { }

void IRAM_ATTR display_render_chunk(color_t *dst, int pos_px, int n_bytes)
{
    render_chunk_body(dst, pos_px, n_bytes);
}
#endif
