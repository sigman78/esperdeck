#include "scan8.h"
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static _Alignas(16) uint16_t masks[256][8];
static _Alignas(16) uint16_t pixels[16 * 800 + 16];
static uint8_t rows[100 * 16];
static uint32_t pairs[100][4];

#ifdef TEST_S3
#define KERNEL scan8_s3
#else
#define KERNEL scan8_model
#endif

typedef void (*kernel_t)(uint16_t *, const uint8_t *, const uint32_t (*)[4],
                          const uint16_t (*)[8], unsigned);

static int check(kernel_t kernel)
{
    scan8_masks_init(masks);
    /* Every glyph byte, every RGB565 colour bit, both selection polarities.
     * Vary each column and row to catch stride/order mistakes. */
    for (unsigned bit = 0; bit < 20; bit++) {
        for (unsigned pattern = 0; pattern < 256; pattern++) {
            for (unsigned c = 0; c < 100; c++) {
                uint16_t fg = (uint16_t)(1u << bit);
                uint16_t bg = (uint16_t)~fg;
                if (bit == 16) fg = bg = 0;
                if (bit == 17) fg = bg = UINT16_MAX;
                if (bit == 18) fg = bg = 0x1234;
                if (bit == 19) { fg = 0xABCD; bg = 0x1234; }
                if (c & 1) { uint16_t t = fg; fg = bg; bg = t; }
                pairs[c][0] = bg | ((uint32_t)bg << 16);
                pairs[c][1] = bg | ((uint32_t)fg << 16);
                pairs[c][2] = fg | ((uint32_t)bg << 16);
                pairs[c][3] = fg | ((uint32_t)fg << 16);
                for (unsigned r = 0; r < 16; r++)
                    rows[c * 16 + r] = (uint8_t)(pattern + c * 17 + r * 13);
            }
            memset(pixels, 0xA5, sizeof pixels);
            unsigned count = pattern % 101;
            kernel(pixels + 8, rows, pairs, masks, count);
            for (unsigned r = 0; r < 16; r++) {
                for (unsigned x = 0; x < 800; x++) {
                    unsigned c = x / 8;
                    uint16_t want = 0xA5A5;
                    if (c < count) {
                        unsigned on = (rows[c * 16 + r] >> (7 - x % 8)) & 1;
                        want = (uint16_t)pairs[c][on ? 3 : 0];
                    }
                    if (pixels[8 + r * 800 + x] != want) {
                        fprintf(stderr, "mismatch bit=%u pattern=%u row=%u x=%u\n",
                                bit, pattern, r, x);
                        return 1;
                    }
                }
            }
            for (unsigned i = 0; i < 8; i++)
                if (pixels[i] != 0xA5A5 || pixels[12808 + i] != 0xA5A5)
                    return 2;
        }
#ifdef ESP_PLATFORM
        vTaskDelay(1);
#endif
    }
    puts("PASS: glyph patterns, RGB565 bits, strides, column counts and guards");
    return 0;
}

#ifdef ESP_PLATFORM
static void bench(const char *name, kernel_t kernel)
{
    uint32_t best = UINT32_MAX, worst = 0;
    uint64_t sum = 0;
    for (unsigned i = 0; i < 256; i++) {
        uint32_t start = esp_cpu_get_cycle_count();
        kernel(pixels + 8, rows, pairs, masks, 100);
        uint32_t elapsed = esp_cpu_get_cycle_count() - start;
        if (i >= 16) {
            if (elapsed < best) best = elapsed;
            if (elapsed > worst) worst = elapsed;
            sum += elapsed;
        }
        if (i % 32 == 31) vTaskDelay(1);
    }
    printf("%s cycles/band: min=%lu mean=%lu max=%lu\n", name,
           (unsigned long)best, (unsigned long)(sum / 240), (unsigned long)worst);
}

static void prototype_task(void *arg)
{
    (void)arg;
    if (check(scan8_lut) == 0 && check(KERNEL) == 0) {
        /* Interrupts stay enabled. Min estimates the warm kernel floor;
         * mean/max include scheduling interference, not LCD deadlines. */
        bench("scalar LUT", scan8_lut);
        bench("S3 SIMD", KERNEL);
        bench("S3 SIMD repeat", KERNEL);
        bench("scalar LUT repeat", scan8_lut);
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    if (xTaskCreatePinnedToCore(prototype_task, "scan8_test", 4096, NULL,
                               5, NULL, 1) != pdPASS)
        puts("FAIL: could not create prototype task");
}
#else
int main(void)
{
    int rc = check(scan8_lut);
    return rc ? rc : check(KERNEL);
}
#endif
