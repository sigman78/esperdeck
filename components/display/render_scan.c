/*
 * render_scan.c — the HOT per-size band scan (instantiated from
 * render_scan.inc) plus the passes that touch its output: underline,
 * DIM dither, and the cursor overlay. Owns the cursor state.
 */

#include "render_internal.h"

/*
 * Reference definition of the pixel-pair packing, kept for documentation and
 * for the host golden tests: leftmost pixel = top glyph bit, packed into the
 * LOW half of a little-endian uint32; mask = 0u - bit, pixel = bg ^ (xf & mask)
 * with xf = fg^bg.
 *
 * The scan no longer evaluates this per pixel. build_pair_lut() in
 * render_cache.c precomputes all four outcomes per cell once per ROW and the
 * scan indexes them with the glyph's two bits — see render_scan.inc.
 */
RENDER_FORCE_INLINE uint32_t scan_gpair(unsigned row, int w, int p,
                                        uint16_t bg, uint16_t xf)
{
    const uint16_t p0 = (uint16_t)(bg ^ (xf &
            (uint16_t)(0u - ((row >> (w - 1 - p)) & 1u))));
    const uint16_t p1 = (uint16_t)(bg ^ (xf &
            (uint16_t)(0u - ((row >> (w - 2 - p)) & 1u))));
    return (uint32_t)p0 | ((uint32_t)p1 << 16);
}
/* Unused once the LUT took over the scan; kept above as the spec
 * (docs/tight-loops.md §5 — keep the reference implementation in the tree). */

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

/* Both post-passes below write at absolute cell positions, so they take the
 * same per-scanline wobble offset the scan used — otherwise underlines and
 * the cursor would stay pinned while their glyphs wobble out from under
 * them. Clipped to the line, since a displaced cell can fall off the edge. */
#define SCAN_NW  (DISPLAY_WIDTH / 2)

/* Underline post-pass — force fg on the cell's last two scanlines.
 * Touches only flagged columns; skipped entirely when the row has none. */
IRAM_ATTR void render_underline_pass(const scan_ctx_t *cx)
{
    if (!cx->any_ul)
        return;
    const int col_words = g_rs.fw >> 1;
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
            uint32_t *const line =
                (uint32_t *)(cx->dst + (unsigned)n * DISPLAY_WIDTH);
            const int koff = cx->xoff ? cx->xoff[n] : 0;
            int w0 = koff + c * col_words;
            int w1 = w0 + col_words;
            if (w0 < 0)       w0 = 0;
            if (w1 > SCAN_NW) w1 = SCAN_NW;
            for (int w = w0; w < w1; w++) line[w] = fg2;
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
#define CURSOR_BLINK_FRAMES  15   /* toggle every 15 frames ≈ 385 ms at 39 Hz */

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

    const int col_words = g_rs.fw >> 1;
    int first = 0;

    if (s_cursor.mode == CURSOR_UNDERSCORE) {
        first = (g_rs.fh - 2) - cx->glyph_row0;   /* band-relative */
        if (first < 0) first = 0;
    }
    for (int n = first; n < cx->num_scans; n++) {
        uint32_t *const line = (uint32_t *)(cx->dst + n * DISPLAY_WIDTH);
        const int koff = cx->xoff ? cx->xoff[n] : 0;
        int w0 = koff + s_cursor.x * col_words;
        int w1 = w0 + col_words;
        if (w0 < 0)       w0 = 0;
        if (w1 > SCAN_NW) w1 = SCAN_NW;
        for (int w = w0; w < w1; w++) line[w] ^= 0xFFFFFFFFu;
    }
}
