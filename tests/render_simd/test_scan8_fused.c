#include "scan8_fused.h"
#include "scan8.h"
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"
#include "scan_variants.h"
#define KERNEL scan8_fused_line
#else
#define KERNEL scan8_fused_line_model
#endif

static _Alignas(16) uint16_t masks[256][8], buf[816];
static uint8_t rows[1600];
static uint32_t pairs[100][4];

int scan8_fused_test(void)
{
    scan8_masks_init(masks);
    for (unsigned seed = 0; seed < 256; seed++) {
        for (unsigned c = 0; c < 100; c++) {
            uint16_t fg = (uint16_t)(seed * 233 + c * 3917);
            uint16_t bg = (uint16_t)(~fg);
            if (!seed) fg = bg = 0;
            if (seed == 255) fg = bg = 0xffff;
            pairs[c][0] = bg | ((uint32_t)bg << 16);
            pairs[c][3] = fg | ((uint32_t)fg << 16);
            for (unsigned y = 0; y < 16; y++)
                rows[c * 16 + y] = (uint8_t)(seed + c * 31 + y * 73);
        }
        for (unsigned y = 0; y < 16; y++) {
            for (int off = -5; off <= 4; off++) {
                memset(buf, 0xa5, sizeof buf);
                KERNEL(buf + 8, rows + y, pairs, masks, off);
                for (int x = 0; x < 800; x++) {
                    int sx = x - off * 2;
                    uint16_t want = 0;
                    if (sx >= 0 && sx < 800) {
                        unsigned c = (unsigned)sx / 8, bit = 7 - (unsigned)sx % 8;
                        want = (uint16_t)pairs[c][(rows[c * 16 + y] >> bit) & 1 ? 3 : 0];
                    }
                    if (buf[x + 8] != want) {
                        printf("FAIL fused seed=%u y=%u off=%d x=%d got=%04x want=%04x\n",
                               seed, y, off, x, buf[x + 8], want);
                        return 1;
                    }
                }
                for (unsigned i = 0; i < 8; i++)
                    if (buf[i] != 0xa5a5 || buf[808 + i] != 0xa5a5) return 2;
            }
        }
#ifdef ESP_PLATFORM
        if (seed % 16 == 15) vTaskDelay(1);
#endif
    }
    puts("PASS fused: 256 seeds, 16 glyph rows, offsets -5..4, clipping and guards");
    return 0;
}
#ifndef ESP_PLATFORM
int main(void) { return scan8_fused_test(); }
#else
extern void scan8_guarded(uint16_t *, const uint8_t *, const uint32_t (*)[4],
                         const uint32_t (*)[4], const uint16_t (*)[8], unsigned);
void scan8_fused_bench(uint16_t *dst)
{
    for (unsigned c = 0; c < 100; c++) {
        uint16_t fg = (uint16_t)(c * 3917 + 123), bg = (uint16_t)~fg;
        pairs[c][0] = bg | ((uint32_t)bg << 16);
        pairs[c][3] = fg | ((uint32_t)fg << 16);
        for (unsigned y = 0; y < 16; y++) rows[c * 16 + y] = (uint8_t)(c * 31 + y * 73);
    }
    for (int off = -5; off <= 4; off++) {
        for (unsigned order = 0; order < 6; order++) {
            unsigned mode = order < 3 ? order : 5 - order;
            uint64_t total = 0;
            uint32_t min = UINT32_MAX, max = 0;
            for (unsigned run = 0; run < 116; run++) {
                uint32_t start = esp_cpu_get_cycle_count();
                if (mode == 0) {
                    scan8_guarded(dst, rows, pairs, pairs, masks, 100);
                    for (unsigned y = 0; y < 16; y++) shift_line_guarded(dst + y * 800, off);
                } else {
                    for (unsigned y = 0; y < 16; y++) {
                        scan8_fused_line(dst + y * 800, rows + y, pairs, masks, mode == 1 ? 0 : off);
                        if (mode == 1) shift_line_guarded(dst + y * 800, off);
                    }
                }
                uint32_t cycles = esp_cpu_get_cycle_count() - start;
                if (run >= 16) {
                    total += cycles;
                    if (cycles < min) min = cycles;
                    if (cycles > max) max = cycles;
                }
            }
            printf("FUSED_MICRO offset=%d mode=%s run=%u avg=%lu min=%lu max=%lu\n",
                   off, mode == 0 ? "band-two-pass" : mode == 1 ? "line-two-pass" : "line-fused",
                   order, (unsigned long)(total / 100), (unsigned long)min, (unsigned long)max);
            vTaskDelay(1);
        }
    }
}
#endif
