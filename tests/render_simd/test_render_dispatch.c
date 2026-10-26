#include "render_internal.h"
#include <stdio.h>
#include <string.h>

render_geom_t g_rs;
static _Alignas(16) color_t scalar[DISPLAY_WIDTH * 16 + 16], simd[DISPLAY_WIDTH * 16 + 16];
static uint8_t rows[FONT_MAX_CACHE_BYTES];
static uint32_t palettes[2][FONT_MAX_COLS][4];
_Static_assert(FONT_MAX_COLS == DISPLAY_WIDTH / 8, "columns follow driver width");
_Static_assert(FONT_MAX_CACHE_BYTES >= (DISPLAY_WIDTH / 8) * 16, "8 cache");
_Static_assert(FONT_MAX_CACHE_BYTES >= (DISPLAY_WIDTH / 10) * 40, "10 cache");
_Static_assert(FONT_MAX_CACHE_BYTES >= (DISPLAY_WIDTH / 12) * 48, "12 cache");

int main(void)
{
    for (unsigned fw = 8; fw <= 12; fw += 2) {
        g_rs = (render_geom_t){fw, fw * 2, fw == 8 ? 16 : fw, fw == 8 ? 16 : fw * 4};
        unsigned cols = DISPLAY_WIDTH / fw, band = (unsigned)g_rs.band;
        for (unsigned i = 0; i < sizeof rows; i++) rows[i] = (uint8_t)(i * 73 + 17);
        for (unsigned c = 0; c < cols; c++)
        for (unsigned p = 0; p < 2; p++) {
            uint16_t fg = (uint16_t)(c * 3917 + p * 577), bg = (uint16_t)~fg;
            palettes[p][c][0] = bg | (uint32_t)bg << 16;
            palettes[p][c][1] = bg | (uint32_t)fg << 16;
            palettes[p][c][2] = fg | (uint32_t)bg << 16;
            palettes[p][c][3] = fg | (uint32_t)fg << 16;
        }
        for (unsigned row0 = 0; row0 < fw * 2; row0 += band)
        for (unsigned align = 0; align < 8; align += 2)
        for (int off = -6; off <= 4; off++)
        for (unsigned scan = 0; scan < 2; scan++) {
            int8_t offsets[16];
            for (unsigned y = 0; y < band; y++) offsets[y] = (int8_t)off;
            scan_ctx_t cx = {.dst = scalar + 8 + align, .rows = rows,
                .pr = {palettes[0], palettes[1]}, .xoff = off == -6 ? NULL : offsets,
                .ncols = cols, .num_scans = band, .glyph_row0 = row0, .scan_on = scan};
            memset(scalar, 0xa5, sizeof scalar); memset(simd, 0xa5, sizeof simd);
            render_scan_band_scalar(&cx);
            cx.dst = simd + 8 + align;
            render_scan_band(&cx);
            if (memcmp(scalar, simd, sizeof scalar)) {
                printf("FAIL dispatch panel=%d fw=%u row0=%u align=%u off=%d scan=%u\n",
                       DISPLAY_WIDTH, fw, row0, align, off, scan);
                return 1;
            }
        }
    }
    printf("PASS production dispatch panel=%d, all fonts, scalar parity and fallback\n", DISPLAY_WIDTH);
    return 0;
}
