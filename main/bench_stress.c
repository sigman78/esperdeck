/*
 * ISR render bench stress mode — see bench_stress.h.
 */
#include "bench_stress.h"

#ifdef CONFIG_CYBERDECK_BENCH_STRESS

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "display.h"
#include "vterm.h"

static const char *TAG = "bench_stress";

font_size_t bench_stress_font_override(font_size_t normal)
{
    (void)normal;
#if defined(CONFIG_CYBERDECK_BENCH_STRESS_FONT_12X24)
    return FONT_SIZE_12X24;
#elif defined(CONFIG_CYBERDECK_BENCH_STRESS_FONT_10X20)
    return FONT_SIZE_10X20;
#else
    return FONT_SIZE_8X16;
#endif
}

/* Repaint the whole grid with dense glyphs, per-cell SGR color changes and
 * 25% bold — strictly denser than any real terminal screen (which is
 * mostly blank), so the logged max is a worst-case chunk time. */
static void bench_task(void *arg)
{
    (void)arg;
    const int cols = display_text_cols(), rows = display_text_rows();
    static char line[1024];
    static const char set[] = "@WM#g&%8QRBdKXhE";
    unsigned frame = 0;
    TickType_t last = xTaskGetTickCount();
    display_render_bench_reset();
    for (;;) {
        for (int r = 0; r < rows; r++) {
            int n = snprintf(line, sizeof line, "\x1b[%d;1H", r + 1);
            for (int c = 0; c < cols && n < (int)sizeof line - 16; c++) {
                n += snprintf(line + n, sizeof line - n, "\x1b[%d;%dm%c",
                              ((c + r) & 3) == 0, 31 + (int)((r + c + frame) % 7u),
                              set[(r * 3 + c + frame) % (unsigned)(sizeof set - 1)]);
            }
            vterm_feed(line, (size_t)n);
        }
        vterm_flush();   /* vterm_feed only parses; flush pushes the cells */
        frame++;
        vTaskDelay(pdMS_TO_TICKS(100));
        if ((xTaskGetTickCount() - last) * portTICK_PERIOD_MS >= 5000) {
            uint32_t avg, mx, chunks;
            display_render_bench_get(&avg, &mx, &chunks);
            display_render_bench_reset();
            ESP_LOGI(TAG, "font=%s avg=%u cyc (%u us) max=%u cyc (%u us) chunks=%u",
                     font_size_name(font_active_size()),
                     (unsigned)avg, (unsigned)(avg / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ),
                     (unsigned)mx, (unsigned)(mx / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ),
                     (unsigned)chunks);
            last = xTaskGetTickCount();
        }
    }
}

bool bench_stress_start(void)
{
    ESP_LOGW(TAG, "BENCH STRESS build: font=%s — no shell, no network",
             font_size_name(font_active_size()));
    xTaskCreatePinnedToCore(bench_task, "bench", 8192, NULL, 5, NULL, 0);
    return true;
}

#endif /* CONFIG_CYBERDECK_BENCH_STRESS */
