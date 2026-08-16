/*
 * render_scan.c — the HOT per-size band scan (instantiated from
 * render_scan.inc) plus the passes that touch its output: underline,
 * DIM dither, and the cursor overlay. Owns the cursor state.
 */

#include "render_internal.h"

/* Branchless pixel pair → one little-endian uint32 (2×RGB565), leftmost
 * pixel = top bit: mask = 0u - bit (0xFFFF/0x0000), pixel = bg ^ (xf &
 * mask) with xf = fg^bg. Forced inline — innermost loop, shy -Os inliner. */
RENDER_FORCE_INLINE uint32_t scan_gpair(unsigned row, int w, int p,
                                        uint16_t bg, uint16_t xf)
{
    const uint16_t p0 = (uint16_t)(bg ^ (xf &
            (uint16_t)(0u - ((row >> (w - 1 - p)) & 1u))));
    const uint16_t p1 = (uint16_t)(bg ^ (xf &
            (uint16_t)(0u - ((row >> (w - 2 - p)) & 1u))));
    return (uint32_t)p0 | ((uint32_t)p1 << 16);
}

/* One real function per linked size — see render_scan.inc. */
#if FONT_RT_8X16
#define SCAN_W   8
#define SCAN_ROW uint8_t
#define SCAN_FN  scan_band_8x16
#include "render_scan.inc"
#undef SCAN_W
#undef SCAN_ROW
#undef SCAN_FN
#endif

#if FONT_RT_10X20
#define SCAN_W   10
#define SCAN_ROW uint16_t
#define SCAN_FN  scan_band_10x20
#include "render_scan.inc"
#undef SCAN_W
#undef SCAN_ROW
#undef SCAN_FN
#endif

#if FONT_RT_12X24
#define SCAN_W   12
#define SCAN_ROW uint16_t
#define SCAN_FN  scan_band_12x24
#include "render_scan.inc"
#undef SCAN_W
#undef SCAN_ROW
#undef SCAN_FN
#endif

IRAM_ATTR void render_scan_band(const scan_ctx_t *cx)
{
    switch (g_rs.fw) {
#if FONT_RT_8X16
    case 8:  scan_band_8x16(cx);  break;
#endif
#if FONT_RT_10X20
    case 10: scan_band_10x20(cx); break;
#endif
#if FONT_RT_12X24
    case 12: scan_band_12x24(cx); break;
#endif
    default: break;   /* font_init guarantees a linked size */
    }
}

/* Underline post-pass — force fg on the cell's last two scanlines.
 * Touches only flagged columns; skipped entirely when the row has none. */
IRAM_ATTR void render_underline_pass(const scan_ctx_t *cx)
{
    if (!cx->any_ul)
        return;
    const int fw        = g_rs.fw;
    const int col_words = fw >> 1;
    int ul_first = (g_rs.fh - 2) - cx->glyph_row0;   /* band-relative */
    if (ul_first < 0) ul_first = 0;
    if (ul_first >= cx->num_scans)
        return;
    for (int c = 0; c < cx->ncols; c++) {
        if (!cx->ul[c]) continue;
        for (int n = ul_first; n < cx->num_scans; n++) {
            const int sel = n & cx->scan_on & 1;   /* match scanline variant */
            const uint16_t fg = (uint16_t)(cx->bg[sel][c] ^ cx->xf[sel][c]);
            const uint32_t fg2 = (uint32_t)fg | ((uint32_t)fg << 16);
            uint32_t *p = (uint32_t *)(cx->dst + (unsigned)n * DISPLAY_WIDTH
                                       + c * fw);
            for (int w = 0; w < col_words; w++) p[w] = fg2;
        }
    }
}

#if OVERLAY_DIM_DITHER
/* Dithered dim post-pass — checkerboard blackout on scrim columns (cell
 * widths are even, so pair parity reduces to the band scanline's). */
IRAM_ATTR void render_dim_pass(const scan_ctx_t *cx)
{
    if (!cx->any_dim)
        return;
    const int fw        = g_rs.fw;
    const int col_words = fw >> 1;
    for (int c = 0; c < cx->ncols; c++) {
        if (!cx->dim[c]) continue;
        for (int n = 0; n < cx->num_scans; n++) {
            const uint32_t m = (n & 1) ? 0xFFFF0000u : 0x0000FFFFu;
            uint32_t *p = (uint32_t *)(cx->dst + (unsigned)n * DISPLAY_WIDTH
                                       + c * fw);
            for (int w = 0; w < col_words; w++) p[w] &= m;
        }
    }
}
#endif

/* Cursor — updated via display_set_cursor(); blink from the frame tick. */
#define CURSOR_BLINK_FRAMES  15   /* toggle every 15 frames ≈ 250 ms at 60 fps */

static DRAM_ATTR struct {
    int           x, y;
    cursor_mode_t mode;
    uint32_t      blink_count;
    bool          visible;
} s_cursor = { .mode = CURSOR_NONE, .visible = true };

void display_set_cursor(int x, int y, cursor_mode_t mode)
{
    if (x != s_cursor.x || y != s_cursor.y) {
        s_cursor.blink_count = 0;
        s_cursor.visible     = true;
    }
    s_cursor.x    = x;
    s_cursor.y    = y;
    s_cursor.mode = mode;
}

IRAM_ATTR void render_cursor_tick(void)
{
    if (++s_cursor.blink_count >= CURSOR_BLINK_FRAMES) {
        s_cursor.blink_count = 0;
        s_cursor.visible     = !s_cursor.visible;
    }
}

/* Cursor overlay — XOR the region (guaranteed contrast). Underscore =
 * cell's last two scanlines; block spans every band of its row. */
IRAM_ATTR void render_cursor_pass(const scan_ctx_t *cx, int char_row)
{
    if (s_cursor.mode == CURSOR_NONE || !s_cursor.visible ||
            s_cursor.y != char_row || s_cursor.x < 0 || s_cursor.x >= cx->ncols)
        return;

    const int fw        = g_rs.fw;
    const int col_words = fw >> 1;
    const int px_start  = s_cursor.x * fw;
    int first = 0;

    if (s_cursor.mode == CURSOR_UNDERSCORE) {
        first = (g_rs.fh - 2) - cx->glyph_row0;   /* band-relative */
        if (first < 0) first = 0;
    }
    for (int n = first; n < cx->num_scans; n++) {
        uint32_t *p = (uint32_t *)(cx->dst + n * DISPLAY_WIDTH + px_start);
        for (int w = 0; w < col_words; w++) p[w] ^= 0xFFFFFFFFu;
    }
}
