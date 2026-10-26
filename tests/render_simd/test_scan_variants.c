#include "scan8.h"
#include "scan_variants.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
void scan8_guarded(uint16_t *, const uint8_t *, const uint32_t (*)[4],
                   const uint32_t (*)[4], const uint16_t (*)[8], unsigned);
#endif

int scan_variants_test(void)
{
    /* malloc alignment on the host is sufficient for the portable model;
     * target SIMD buffers have an explicit alignment contract. */
#ifdef ESP_PLATFORM
    uint16_t *buf = heap_caps_aligned_alloc(16, (12800 + 16) * 2, MALLOC_CAP_INTERNAL);
    uint16_t (*masks)[8] = heap_caps_aligned_alloc(16, 4096, MALLOC_CAP_INTERNAL);
#else
    uint16_t *buf = malloc((12800 + 16) * 2);
    uint16_t (*masks)[8] = malloc(4096);
#endif
    uint8_t *rows = malloc(4800);
    uint32_t (*even)[4] = malloc(1600), (*odd)[4] = malloc(1600);
    if (!buf || !masks || !rows || !even || !odd) abort();
    scan8_masks_init(masks);
    for (unsigned width = 8; width <= 12; width += 2) {
        unsigned cols = 800 / width, height = width * 2;
        unsigned band = width == 8 ? 16 : width, stride = width == 8 ? 16 : height * 2;
        for (unsigned pattern = 0; pattern < 256; pattern++) {
            for (unsigned c = 0; c < cols; c++) {
                for (unsigned parity = 0; parity < 2; parity++) {
                    uint32_t (*pr)[4] = parity ? odd : even;
                    uint16_t fg = (uint16_t)(c * 3917 + pattern * 233 + parity * 571);
                    uint16_t bg = (uint16_t)(~fg ^ (parity * 0x2929));
                    if (pattern == 0) fg = bg = 0;
                    if (pattern == 255) fg = bg = 0xffff;
                    pr[c][0] = bg | ((uint32_t)bg << 16);
                    pr[c][1] = bg | ((uint32_t)fg << 16);
                    pr[c][2] = fg | ((uint32_t)bg << 16);
                    pr[c][3] = fg | ((uint32_t)fg << 16);
                }
                for (unsigned y = 0; y < height; y++) {
                    unsigned bits = (pattern * 17 + c * 31 + y * 73) & ((1u << width) - 1);
                    if (width == 8) rows[c * stride + y] = (uint8_t)bits;
                    else ((uint16_t *)(rows + c * stride))[y] = (uint16_t)bits;
                }
            }
            for (unsigned row0 = 0; row0 < height; row0 += band) {
                for (int off = -5; off <= 4; off++) {
                    memset(buf, 0xA5, (12800 + 16) * 2);
                    uint16_t *out = buf + 8;
#ifdef ESP_PLATFORM
                    if (width == 8) scan8_guarded(out, rows, even, odd, masks, cols);
                    else if (width == 10) scan10_guarded(out, rows, even, odd, masks, row0);
                    else scan12_guarded(out, rows, even, odd, masks, row0);
#else
                    scan_wide_model(out, rows, even, odd, masks, row0, width);
#endif
                    for (unsigned y = 0; y < band; y++) {
                        for (unsigned x = cols * width; x < 800; x++) out[y * 800 + x] = 0;
#ifdef ESP_PLATFORM
                        if (shift_line_guarded(out + y * 800, off)) abort();
#else
                        shift_line_model(out + y * 800, off);
#endif
                        for (int x = 0; x < 800; x++) {
                            int sx = x - off * 2;
                            uint16_t want = 0;
                            if (sx >= 0 && sx < (int)(cols * width)) {
                                unsigned c = (unsigned)sx / width, p = (unsigned)sx % width;
                                unsigned bits = width == 8 ? rows[c * stride + row0 + y]
                                    : ((uint16_t *)(rows + c * stride))[row0 + y];
                                unsigned on = (bits >> (width - 1 - p)) & 1;
                                want = (uint16_t)(y & 1 ? odd[c][on ? 3 : 0] : even[c][on ? 3 : 0]);
                            }
                            if (out[y * 800 + x] != want) {
                                printf("FAIL variants width=%u pattern=%u row0=%u off=%d y=%u x=%d got=%04x want=%04x\n",
                                       width, pattern, row0, off, y, x, out[y * 800 + x], want);
                                return 1;
                            }
                        }
                    }
                    for (unsigned i = 0; i < 8; i++)
                        if (buf[i] != 0xA5A5) return 2;
                    for (unsigned i = 8 + band * 800; i < 12816; i++)
                        if (buf[i] != 0xA5A5) return 3;
                }
            }
#ifdef ESP_PLATFORM
            if (pattern % 16 == 15) vTaskDelay(1);
#endif
        }
        printf("PASS variants width=%u: both palettes, half-rows, offsets -5..4, margins and guards\n", width);
    }
    free(odd); free(even); free(rows); free(masks); free(buf);
    return 0;
}
#ifndef ESP_PLATFORM
int main(void) { return scan_variants_test(); }
#endif
