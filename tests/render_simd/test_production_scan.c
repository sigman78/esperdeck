#include "render_simd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int production_scan_test(void)
{
    const unsigned widths[] = {16, 24, 32, 320, 480, 640, 792, 800, 808, 1024};
    uint16_t *allocation = malloc((1024 * 16 + 32) * 2);
    uint8_t *rows = malloc(128 * 48);
    uint32_t (*pal[2])[4] = {malloc(128 * 16), malloc(128 * 16)};
    if (!allocation || !rows || !pal[0] || !pal[1]) abort();
    for (unsigned w = 8; w <= 12; w += 2) {
        unsigned band = w == 8 ? 16 : w, rowbytes = w == 8 ? 1 : 2;
        for (unsigned wi = 0; wi < sizeof widths / sizeof *widths; wi++) {
            unsigned pixels = widths[wi], cols = pixels / w;
            const unsigned seeds = 16;
            for (unsigned seed = 0; seed < seeds; seed++) {
                for (unsigned c = 0; c < cols; c++) {
                    for (unsigned p = 0; p < 2; p++) {
                        uint16_t fg = (uint16_t)(c * 3917 + seed * 233 + p * 577);
                        uint16_t bg = (uint16_t)~fg;
                        pal[p][c][0] = bg | (uint32_t)bg << 16;
                        pal[p][c][1] = bg | (uint32_t)fg << 16;
                        pal[p][c][2] = fg | (uint32_t)bg << 16;
                        pal[p][c][3] = fg | (uint32_t)fg << 16;
                    }
                    for (unsigned y = 0; y < w * 2; y++) {
                        unsigned bits = (seed * 257 + c * 31 + y * 73) & ((1u << w) - 1);
                        if (rowbytes == 1) rows[c * 16 + y] = (uint8_t)bits;
                        else ((uint16_t *)rows)[c * w * 2 + y] = (uint16_t)bits;
                    }
                }
                for (unsigned row0 = 0; row0 < w * 2; row0 += band)
                for (unsigned alignment = 0; alignment < 8; alignment += 2)
                for (int off = -5; off <= 4; off++) {
                    memset(allocation, 0xa5, (1024 * 16 + 32) * 2);
                    int8_t offsets[16];
                    for (unsigned y = 0; y < band; y++) offsets[y] = (int8_t)(y & 1 ? off : -5 + (off + 5 + (int)y) % 10);
                    uint16_t *dst = allocation + 8 + alignment;
                    render_simd_ctx_t cx = {dst, rows, {pal[0], pal[1]}, offsets,
                        pixels, w, cols, band, row0};
                    if (alignment) {
                        if (render_simd_scan(&cx)) return 9;
                        for (unsigned i = 0; i < 1024 * 16 + 32; i++)
                            if (allocation[i] != 0xa5a5) return 10;
                        cx.offsets = NULL;
                        memset(offsets, 0, sizeof offsets);
                    }
                    if (!render_simd_scan(&cx)) return 1;
                    for (unsigned y = 0; y < band; y++)
                    for (unsigned x = 0; x < pixels; x++) {
                        int sx = (int)x - offsets[y] * 2;
                        uint16_t want = 0;
                        if (sx >= 0 && (unsigned)sx < cols * w) {
                            unsigned c = (unsigned)sx / w, p = (unsigned)sx % w;
                            unsigned bits = rowbytes == 1 ? rows[c * 16 + row0 + y] :
                                ((uint16_t *)rows)[c * w * 2 + row0 + y];
                            want = (uint16_t)pal[y & 1][c][(bits >> (w - 1 - p)) & 1 ? 3 : 0];
                        }
                        if (dst[y * pixels + x] != want) {
                            printf("FAIL production w=%u pixels=%u row0=%u align=%u off=%d y=%u x=%u\n",
                                   w, pixels, row0, alignment * 2, off, y, x);
                            return 2;
                        }
                    }
                    unsigned start = 8 + alignment, end = start + pixels * band;
                    for (unsigned i = 0; i < start; i++) if (allocation[i] != 0xa5a5) return 3;
                    for (unsigned i = end; i < 1024 * 16 + 32; i++) if (allocation[i] != 0xa5a5) return 4;
                }
            }
        }
        printf("PASS production SIMD width=%u: variable panel widths, palettes, offsets, alignment, guards\n", w);
    }
    uint16_t *dst = allocation + 8;
    memset(allocation, 0xa5, 64);
    render_simd_ctx_t bad = {dst, rows, {pal[0], pal[1]}, NULL, 802, 8, 100, 16, 0};
    if (render_simd_scan(&bad)) return 5;
    bad.width = 800; bad.num_scans = 1;
    if (render_simd_scan(&bad)) return 6;
    bad.num_scans = 16; bad.cols = 101;
    if (render_simd_scan(&bad)) return 7;
    for (unsigned i = 0; i < 32; i++) if (allocation[i] != 0xa5a5) return 8;
    free(pal[1]); free(pal[0]); free(rows); free(allocation);
    return 0;
}
int main(void) { return production_scan_test(); }
