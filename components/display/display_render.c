/*
 * display_render.c — shared rendering core.
 *
 * Compiled for BOTH the ESP32 target (called from the bounce-buffer ISR)
 * and the PC simulator (called from the SDL2 frame loop).
 *
 * No SDL headers, no esp_lcd headers — only display.h and font.h.
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

/* Overlay accent palettes (index 0 is a sentinel → use s_overlay_fg). Kept
 * in DRAM so the bounce-buffer ISR can read them without touching flash.
 *
 * Dual palette: TEXT accents (colored glyphs on the dark screen) use the
 * classic VGA bright-16 tones; INVERSE cells (solid button bars, chips) use
 * muted companions of the same hues — a whole screen of full-saturation
 * bars reads as motley, and dark labels want a calmer mid-tone behind them.
 * WHITE stays pure in both (QR codes need true white modules). */
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
 * Per-column rendering cache.
 * Static (not stack) to avoid blowing the ISR stack on ESP32.
 * DRAM_ATTR keeps it in internal SRAM — reachable from ISR without Flash cache.
 * ---------------------------------------------------------------------- */
#define RENDER_MAX_COLS  (DISPLAY_WIDTH / FONT_WIDTH)   /* 100 / 80 / 66 */

/* uint32_t (pixel-pair) stores per character column. Even FONT_WIDTH keeps
 * every column start 4-byte aligned (FONT_WIDTH px × 2 B is a multiple of 4). */
#define COL_WORDS        (FONT_WIDTH / 2)

/* Overlay DIM scrim style (the shade drawn behind a modal):
 *   0 = halved   — every pixel at 50% brightness (default). Done per-cell in
 *       build_row_cache with zero extra per-band work — the cheapest ISR path.
 *   1 = dithered — checkerboard: alternate pixels kept at full brightness, the
 *       rest blacked. A crisper look, but NOT an ISR win — it adds a per-band
 *       post-pass over scrim columns plus a fixed flag-check every frame, so it
 *       is off by default. Set to 1 (e.g. from the build system) to enable; the
 *       dither code compiles out completely when 0. */
#ifndef OVERLAY_DIM_DITHER
#define OVERLAY_DIM_DITHER 0
#endif

/* Column cache as parallel arrays. bg/xorfg carry TWO variants selected per
 * scanline: [0] = normal, [1] = scanline-dimmed (the CRT-scanline effect).
 * Selecting the variant per scanline keeps the hot loop free of any
 * per-pixel effect cost — the "shader" work all happens per-cell here. */
static DRAM_ATTR const font_row_t *s_cc_glyph[RENDER_MAX_COLS];
static DRAM_ATTR uint16_t           s_cc_bg[2][RENDER_MAX_COLS];
static DRAM_ATTR uint16_t           s_cc_xf[2][RENDER_MAX_COLS];
static DRAM_ATTR uint8_t            s_cc_ul[RENDER_MAX_COLS];
#if OVERLAY_DIM_DITHER
static DRAM_ATTR uint8_t            s_cc_dim[RENDER_MAX_COLS];
#endif

/* Cache validity — with sub-row bounce bands (BOUNCE_BUFFER_HEIGHT <
 * FONT_HEIGHT) a character row spans several consecutive bands; only the
 * first band of the row rebuilds the cache, the rest reuse it. */
static DRAM_ATTR int     s_cc_row     = -1;  /* char row currently cached */
static DRAM_ATTR int     s_cc_scan_on = 0;   /* dim variant [1] populated? */
static DRAM_ATTR uint8_t s_cc_frame   = 0;   /* g_fx_frame at build time — a
                                                band whose row-first band was
                                                clipped out must not reuse a
                                                previous frame's cache */

/* -------------------------------------------------------------------------
 * Carry-safe RGB565 helpers (per-field shift/mask math — a masked
 * right-shift can only shrink a 565 field, never bleed into a neighbour).
 * ---------------------------------------------------------------------- */

/* Scanline dim: 93.75% brightness, the one level that read as CRT texture
 * on-glass — everything stronger (87.5% and below) read as bars. */
static inline uint16_t fx_dim565(uint16_t p)
{
    return (uint16_t)(p - ((p >> 4) & 0x0861));
}

/* Luma-map onto a monochrome phosphor ramp: mode 1 = green, 2 = amber. */
static inline uint16_t fx_mono565(uint16_t p, uint8_t mode)
{
    const uint32_t r6 = ((p >> 11) & 0x1F) << 1;
    const uint32_t g6 = (p >> 5) & 0x3F;
    const uint32_t b6 = (p & 0x1F) << 1;
    const uint32_t y  = (r6 * 5 + g6 * 9 + b6 * 2) >> 4;   /* 0..62 */
    if (mode == 1)
        return (uint16_t)(y << 5);                            /* green  */
    return (uint16_t)(((y >> 1) << 11) | (((y * 11) >> 4) << 5)); /* amber */
}

/* -------------------------------------------------------------------------
 * Branchless pixel-pair → 32-bit word (little-endian, 2×RGB565).
 *
 *   bit   = (gb >> (FONT_WIDTH-1-p)) & 1     (leftmost pixel = top bit)
 *   mask  = 0xFFFF if bit=1, 0x0000 if bit=0  →  (uint16_t)(0u - bit)
 *   pixel = bg ^ (xorfg & mask)        → bg when bit=0, fg when bit=1
 *
 * Left pixel in low 16 bits, right pixel in high 16 bits (little-endian).
 * ---------------------------------------------------------------------- */
#define GPAIR(gb, p0, p1, bg_v, xor_v)                                          \
    (   (uint32_t)((uint16_t)((bg_v) ^ ((xor_v) &                               \
            (uint16_t)(0u - (((unsigned)(gb) >> (FONT_WIDTH - 1u - (p0))) & 1u))))) \
    |  ((uint32_t)((uint16_t)((bg_v) ^ ((xor_v) &                               \
            (uint16_t)(0u - (((unsigned)(gb) >> (FONT_WIDTH - 1u - (p1))) & 1u))))) << 16) )

/* Emit one full character column (COL_WORDS aligned uint32 stores) —
 * unrolled per FONT_WIDTH so the hot loop stays branch- and loop-free. */
#if FONT_WIDTH == 8
#define EMIT_COL(d, gb, bg_v, xor_v) do {                                       \
    (d)[0] = GPAIR(gb, 0, 1, bg_v, xor_v);                                      \
    (d)[1] = GPAIR(gb, 2, 3, bg_v, xor_v);                                      \
    (d)[2] = GPAIR(gb, 4, 5, bg_v, xor_v);                                      \
    (d)[3] = GPAIR(gb, 6, 7, bg_v, xor_v);                                      \
} while (0)
#elif FONT_WIDTH == 10
#define EMIT_COL(d, gb, bg_v, xor_v) do {                                       \
    (d)[0] = GPAIR(gb, 0, 1, bg_v, xor_v);                                      \
    (d)[1] = GPAIR(gb, 2, 3, bg_v, xor_v);                                      \
    (d)[2] = GPAIR(gb, 4, 5, bg_v, xor_v);                                      \
    (d)[3] = GPAIR(gb, 6, 7, bg_v, xor_v);                                      \
    (d)[4] = GPAIR(gb, 8, 9, bg_v, xor_v);                                      \
} while (0)
#elif FONT_WIDTH == 12
#define EMIT_COL(d, gb, bg_v, xor_v) do {                                       \
    (d)[0] = GPAIR(gb, 0,  1,  bg_v, xor_v);                                    \
    (d)[1] = GPAIR(gb, 2,  3,  bg_v, xor_v);                                    \
    (d)[2] = GPAIR(gb, 4,  5,  bg_v, xor_v);                                    \
    (d)[3] = GPAIR(gb, 6,  7,  bg_v, xor_v);                                    \
    (d)[4] = GPAIR(gb, 8,  9,  bg_v, xor_v);                                    \
    (d)[5] = GPAIR(gb, 10, 11, bg_v, xor_v);                                    \
} while (0)
#else
#error "display_render: unsupported FONT_WIDTH (add an EMIT_COL variant)"
#endif

/* -------------------------------------------------------------------------
 * ANSI-256 colour → RGB565.
 * IRAM_ATTR: callable from ESP32 ISR without going through Flash cache.
 * ---------------------------------------------------------------------- */
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

/* -------------------------------------------------------------------------
 * Terminal BELL → visual bell. A speakerless alert: display_bell() arms a
 * brief red "BEL" tag that flashes in the top-right corner, drawn over the top
 * band by the ISR (so it works during a session with no overlay involved).
 * ---------------------------------------------------------------------- */
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
    const int ncols = s_cell_cols;
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
             * wrap back into the glow window (~6.5 s) and re-tint a stale
             * row. Only past the halfway point (gf is clamped ≤ 120), so
             * this write is rare — it races display_fx_touch_row at most
             * once per ~3 s per idle row, and a lost touch just skips one
             * glow (self-heals on the row's next flush). */
            g_fx_row_stamp[cr] = (uint8_t)(g_fx_frame - gf);
    }
#endif

    for (int c = 0; c < ncols; c++) {
        color_t fg, bg;
        const font_row_t *glyph;
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
                /* Solid bar: muted accent background. Colored bars (1-6)
                 * carry LIGHT text — a pale 3/4-to-white tint of their own
                 * hue; focus (BRIGHT) washes the bar pastel and flips the
                 * text dark. Gray/white surfaces (0,7) always keep dark
                 * text: QR modules must stay dark-on-white. */
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
            /* Focus glow: wash the background 50% toward white (carry-safe
             * half-sum), keeping the dark text — the focused bar turns
             * pastel and the label gains contrast instead of losing it.
             * Per-cell cache-build work — the free effect tier. */
            if (ov_attrs & OVERLAY_ATTR_BRIGHT)
                bg = (color_t)(((bg >> 1) & 0x7BEF) + 0x7BEF);
            glyph = font_get_glyph(ov_cp);
            if (ov_attrs & OVERLAY_ATTR_BOLD) {
                /* Real bold face when stored; misses keep the normal glyph.
                 * The `bold` local stays 0 — bold-pop is a terminal effect
                 * and must not recolor overlay chrome. */
                const font_row_t *ob = font_get_glyph_bold(ov_cp);
                if (ob) glyph = ob;
            }
        } else {
            const terminal_cell_t *cell = &row_cells[c];
            fg = cell->fg_color;
            bg = cell->bg_color;
            if (cell->attrs & ATTR_REVERSE) { color_t t = fg; fg = bg; bg = t; }
            underline = cell->attrs & ATTR_UNDERLINE;
            bold      = cell->attrs & ATTR_BOLD;
            glyph = font_get_glyph(cell->cp);
            if (bold) {
                /* Real bold face when stored (subset A); misses keep the
                 * normal glyph. The bold-pop color effect applies either
                 * way — it is an independent, user-tunable emphasis. */
                const font_row_t *bg2 = font_get_glyph_bold(cell->cp);
                if (bg2) glyph = bg2;
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

        s_cc_glyph[c] = glyph;
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
 *
 * Bands must start on a bounce-buffer boundary: BOUNCE_BUFFER_HEIGHT is even
 * and divides FONT_HEIGHT, so a band never straddles a character row and may
 * cover either a whole row (8x16) or half of one (10x20, 12x24).
 *
 * pos_px   — index of first pixel in the full framebuffer
 *            (= start_scanline × DISPLAY_WIDTH)
 * n_bytes  — byte count of the band
 *            (= DISPLAY_WIDTH × BOUNCE_BUFFER_HEIGHT × sizeof(color_t))
 */
void IRAM_ATTR display_render_chunk(color_t *dst, int pos_px, int n_bytes)
{
    if (!dst) return;

    /* Cell buffer not yet registered — fill black. */
    if (!s_cell_buf || s_cell_cols <= 0 || s_cell_rows <= 0) {
        uint32_t *p = (uint32_t *)dst;
        int words = n_bytes >> 2;
        for (int i = 0; i < words; i++) p[i] = 0;
        return;
    }

    const int start_scan = pos_px / DISPLAY_WIDTH;
    const int num_scans  = (n_bytes >> 1) / DISPLAY_WIDTH;  /* n_bytes/2 = pixels */
    const int char_row   = start_scan / FONT_HEIGHT;
    /* First glyph scanline of this band. Non-zero only with sub-row bounce
     * bands (BOUNCE_BUFFER_HEIGHT < FONT_HEIGHT); always a multiple of the
     * band height, so band starts stay even (scanline-parity effects rely
     * on it — see the _Static_asserts in display.h). */
    const int glyph_row0 = start_scan % FONT_HEIGHT;

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
        /* Band fully hidden: black + any edge lines that fall in it.
         * Invalidate the row cache: a later band of this row (or of a frame
         * long after — the uint8 frame stamp can wrap) must never reuse a
         * cache this path skipped rebuilding. */
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

    /* Build the per-column cache for this character row. With sub-row bands
     * only the row's first band rebuilds; later bands reuse the cache (the
     * DMA feeds bands strictly in order). scan_on is captured at build time
     * so the cache fill and the scanline loop always agree. */
    const int scan_on = g_fx_cfg.scanlines ? 1 : 0;
    if (glyph_row0 == 0 || s_cc_row != char_row || s_cc_scan_on != scan_on
            || s_cc_frame != g_fx_frame) {
        build_row_cache(char_row, scan_on);
        s_cc_row     = char_row;
        s_cc_scan_on = scan_on;
        s_cc_frame   = g_fx_frame;
    }
    const int ncols = s_cell_cols;

    /* Right margin beyond the text area, in uint32 (pixel-pair) words.
     * Zero for 8x16/10x20; 4 words (8 px) for 12x24 (66*12 = 792 < 800). */
    const int margin_words = (DISPLAY_WIDTH - ncols * FONT_WIDTH) >> 1;

    /* ------------------------------------------------------------------
     * Render scanlines.
     * glyph scanline index == glyph_row0 + scanline index within the band
     * (bands never straddle a character row).
     *
     * Inner loop: two adjacent columns per iteration → 2×FONT_WIDTH pixels
     * → 2×COL_WORDS uint32_t writes, naturally aligned.
     * ------------------------------------------------------------------ */
    color_t *dst_base = dst;

    for (int n = 0; n < num_scans; n++) {
        const int gl = glyph_row0 + n;
        /* CRT scanlines: odd scanlines read the pre-dimmed colour variant.
         * Bands start on even scanlines, so n's parity is global parity. */
        const int sel = n & scan_on & 1;
        const uint16_t *bgv = s_cc_bg[sel];
        const uint16_t *xfv = s_cc_xf[sel];
        uint32_t *d = (uint32_t *)(dst_base + (unsigned)n * DISPLAY_WIDTH);

        int c = 0;
        for (; c + 1 < ncols; c += 2) {
            const font_row_t b0 = s_cc_glyph[c    ] ? s_cc_glyph[c    ][gl] : 0u;
            const font_row_t b1 = s_cc_glyph[c + 1] ? s_cc_glyph[c + 1][gl] : 0u;
            const uint16_t bg0 = bgv[c    ], xf0 = xfv[c    ];
            const uint16_t bg1 = bgv[c + 1], xf1 = xfv[c + 1];

            EMIT_COL(d,             b0, bg0, xf0);
            EMIT_COL(d + COL_WORDS, b1, bg1, xf1);
            d += 2 * COL_WORDS;
        }

        /* Trailing odd column (defensive; all three grids are even-col). */
        if (c < ncols) {
            const font_row_t b0  = s_cc_glyph[c] ? s_cc_glyph[c][gl] : 0u;
            const uint16_t bg0 = bgv[c], xf0 = xfv[c];
            EMIT_COL(d, b0, bg0, xf0);
            d += COL_WORDS;
        }

        /* Blank the margin right of the text area (black). */
        for (int i = 0; i < margin_words; i++) d[i] = 0;
    }

    /* ------------------------------------------------------------------
     * Underline post-pass — force fg on the last two scanlines of the
     * character CELL (not the band) for any underlined column. With
     * sub-row bands those scanlines live in the row's last band only.
     * Touches only flagged columns, off the hot loop.
     * ------------------------------------------------------------------ */
    {
        int ul_first = (FONT_HEIGHT - 2) - glyph_row0;  /* band-relative */
        if (ul_first < 0) ul_first = 0;
        if (ul_first < num_scans) {
            for (int c = 0; c < ncols; c++) {
                if (!s_cc_ul[c]) continue;
                for (int n = ul_first; n < num_scans; n++) {
                    const int sel = n & scan_on & 1;   /* match scanline variant */
                    const uint16_t fg = (uint16_t)(s_cc_bg[sel][c] ^ s_cc_xf[sel][c]);
                    const uint32_t fg2 = (uint32_t)fg | ((uint32_t)fg << 16);
                    uint32_t *p = (uint32_t *)(dst_base + (unsigned)n * DISPLAY_WIDTH
                                               + c * FONT_WIDTH);
                    for (int w = 0; w < COL_WORDS; w++) p[w] = fg2;
                }
            }
        }
    }

#if OVERLAY_DIM_DITHER
    /* ------------------------------------------------------------------
     * Dithered dim post-pass — for overlay DIM (scrim) columns, black out a
     * checkerboard of pixels, leaving the rest at full brightness. FONT_WIDTH
     * and FONT_HEIGHT are even, so the pattern stays screen-aligned and the
     * parity reduces to the band scanline (n): even n blacks the odd pixels
     * (high half of each packed pair), odd n blacks the even pixels. Cheap:
     * 4 word-ANDs per scanline, only on flagged columns.
     * ------------------------------------------------------------------ */
    for (int c = 0; c < ncols; c++) {
        if (!s_cc_dim[c]) continue;
        for (int n = 0; n < num_scans; n++) {
            const uint32_t m = (n & 1) ? 0xFFFF0000u : 0x0000FFFFu;
            uint32_t *p = (uint32_t *)(dst_base + (unsigned)n * DISPLAY_WIDTH
                                       + c * FONT_WIDTH);
            for (int w = 0; w < COL_WORDS; w++) p[w] &= m;
        }
    }
#endif

    /* ------------------------------------------------------------------
     * Cursor overlay — XOR every pixel in the cursor region with
     * 0xFFFFFFFF, inverting all bits and guaranteeing contrast.
     * FONT_WIDTH px × 2 B = COL_WORDS × uint32_t per scanline.
     * Alignment is guaranteed: dst_base is 4-byte aligned,
     * DISPLAY_WIDTH×2 B = 1600 B and cx×FONT_WIDTH×2 B are both ×4.
     * The underscore lives on the last two scanlines of the CELL; a block
     * cursor spans every band of its row.
     * ------------------------------------------------------------------ */
    if (s_cursor_mode != CURSOR_NONE && s_cursor_visible &&
            s_cursor_y == char_row && s_cursor_x >= 0 && s_cursor_x < ncols) {

        const int cx       = s_cursor_x;
        const int px_start = cx * FONT_WIDTH;
        int first = 0;

        if (s_cursor_mode == CURSOR_UNDERSCORE) {
            first = (FONT_HEIGHT - 2) - glyph_row0;   /* band-relative */
            if (first < 0) first = 0;
        }
        for (int n = first; n < num_scans; n++) {
            uint32_t *p = (uint32_t *)(dst_base + n * DISPLAY_WIDTH + px_start);
            for (int w = 0; w < COL_WORDS; w++) p[w] ^= 0xFFFFFFFFu;
        }
    }

    /* ------------------------------------------------------------------
     * Signal-loss static burst — replace a few pseudo-randomly chosen
     * scanlines of this band with grayscale snow. Transient (frame-
     * bounded) by design: this is the one per-pixel-ish pass allowed.
     * ------------------------------------------------------------------ */
    if (g_fx_static_left > 0 && num_scans > 0) {
        int nl = g_fx_cfg.static_lines;
        if (nl > 4) nl = 4;
        /* Seed by BAND (not char row): with sub-row bands a per-row seed
         * would draw the same snow lines in both halves of the row, and the
         * pairing reads as structure instead of noise. */
        uint32_t h = ((uint32_t)(start_scan / BOUNCE_BUFFER_HEIGHT) * 2654435761u)
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

    /* ------------------------------------------------------------------
     * Wipe / collapse post-pass for bands straddling the visible window:
     * black out hidden scanlines, then draw the bright edge lines.
     * ------------------------------------------------------------------ */
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
#undef EMIT_COL
#undef COL_WORDS
#undef RENDER_MAX_COLS
