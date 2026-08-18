/*
 * Argon2id KDF parameter sweep — see bench_argon2.h.
 */
#include "bench_argon2.h"

#ifdef CONFIG_CYBERDECK_BENCH_ARGON2

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "monocypher.h"

static const char *TAG = "bench_argon2";

/* Mirrors derive_kek() in components/storage/keystore.c: same work-area
 * allocation (PSRAM, 8-bit), same Monocypher entry point, dummy inputs. */
static int64_t time_one(uint32_t blocks_kib, uint32_t passes)
{
    void *area = heap_caps_malloc((size_t)blocks_kib * 1024,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!area) return -1;

    crypto_argon2_config cfg = {
        .algorithm = CRYPTO_ARGON2_ID,
        .nb_blocks = blocks_kib,
        .nb_passes = passes,
        .nb_lanes  = 1,
    };
    static const uint8_t salt[16] = { 0xc5, 0xbe, 0x0e };
    crypto_argon2_inputs in = {
        .pass      = (const uint8_t *)"000000",
        .pass_size = 6,
        .salt      = salt,
        .salt_size = sizeof(salt),
    };
    uint8_t kek[32];

    int64_t t0 = esp_timer_get_time();
    crypto_argon2(kek, sizeof(kek), area, cfg, in, crypto_argon2_no_extras);
    int64_t us = esp_timer_get_time() - t0;

    crypto_wipe(kek, sizeof(kek));
    crypto_wipe(area, (size_t)blocks_kib * 1024);
    heap_caps_free(area);
    return us;
}

static void sweep_task(void *parent)
{
    static const struct { uint32_t kib, passes; } pts[] = {
        { 1024, 3 }, { 2048, 3 }, { 4096, 1 }, { 4096, 2 },
        { 4096, 3 },                       /* keystore.c default */
        { 8192, 1 }, { 8192, 2 },
    };

    ESP_LOGW(TAG, "Argon2id sweep (1 lane, PSRAM work area) — several seconds");
    for (size_t i = 0; i < sizeof pts / sizeof pts[0]; i++) {
        /* second run reported: first touches freshly-mapped PSRAM pages */
        int64_t us = time_one(pts[i].kib, pts[i].passes);
        if (us >= 0) us = time_one(pts[i].kib, pts[i].passes);
        if (us < 0) {
            ESP_LOGW(TAG, "%4u KiB x%u: alloc failed (PSRAM headroom)",
                     (unsigned)pts[i].kib, (unsigned)pts[i].passes);
            continue;
        }
        ESP_LOGI(TAG, "%4u KiB x%u passes: %6lld ms  (%.2f attempts/s)",
                 (unsigned)pts[i].kib, (unsigned)pts[i].passes,
                 us / 1000, 1e6 / (double)us);
    }
    ESP_LOGW(TAG, "sweep done — normal boot continues");
    xTaskNotifyGive((TaskHandle_t)parent);
    vTaskDelete(NULL);
}

void bench_argon2_run(void)
{
    /* crypto_argon2 keeps a ~1 KiB working block (plus hash state) on the
     * stack — too deep for the 3.5 KB main task. The real unlock path has
     * the same need: derive on a worker task with >= 4 KB headroom. */
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (xTaskCreate(sweep_task, "argon2_bench", 8192, self, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "bench task create failed");
        return;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* keep boot serialized */
}

#endif /* CONFIG_CYBERDECK_BENCH_ARGON2 */
