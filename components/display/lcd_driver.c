/*
 * lcd_driver.c — hardware LCD driver for Waveshare ESP32-S3-Touch-LCD-7:
 * GPIO init, RGB panel config, DMA/bounce-buffer setup, and the thin
 * on_bounce_empty ISR wrapper. Pixel rendering lives in display_render.c.
 */

#include "display.h"
#include "display_render.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lcd_driver";

/* Backlight GPIO (active-high) */
#define PIN_NUM_BK_LIGHT  2

static esp_lcd_panel_handle_t panel_handle  = NULL;

/* Bounce-buffer fill callback — ISR context. */
static IRAM_ATTR bool on_bounce_empty(esp_lcd_panel_handle_t panel,
                                      void *buf,
                                      int   pos_px,
                                      int   len_bytes,
                                      void *user_ctx)
{
    (void)panel; (void)user_ctx;
    display_render_chunk((color_t *)buf, pos_px, len_bytes);
    return false;
}

static esp_err_t init_backlight(void)
{
    gpio_config_t bk_gpio_config = {
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_NUM_BK_LIGHT,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(PIN_NUM_BK_LIGHT, 1);   /* active-high ON */
    ESP_LOGI(TAG, "Backlight initialized (GPIO %d)", PIN_NUM_BK_LIGHT);
    return ESP_OK;
}

static esp_err_t panel_bringup(void)
{
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz          = 16 * 1000 * 1000,
            .h_res            = DISPLAY_WIDTH,
            .v_res            = DISPLAY_HEIGHT,
            .hsync_pulse_width = 4,
            .hsync_back_porch  = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch  = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = 1,
        },
        .data_width          = 16,
        .bits_per_pixel      = 16,
        .num_fbs             = 0,
        .flags.no_fb         = 1,
        .bounce_buffer_size_px = display_bounce_px(),
        .hsync_gpio_num      = 46,
        .vsync_gpio_num      = 3,
        .de_gpio_num         = 5,
        .pclk_gpio_num       = 7,
        .disp_gpio_num       = -1,
        .data_gpio_nums      = {14, 38, 18, 17, 10, 39, 0, 45, 48, 47, 21, 1, 2, 42, 41, 40},
    };

    ESP_LOGI(TAG, "Creating RGB panel...");
    esp_err_t ret = esp_lcd_new_rgb_panel(&panel_config, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RGB panel: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));

    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_bounce_empty = on_bounce_empty,
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, NULL));

    /* esp_lcd allocates its own DMA bounce buffers internally whenever the
     * caller sets bounce_buffer_size_px, so this code needs no separate
     * allocation. */
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    ESP_LOGI(TAG, "LCD initialized: %dx%d, bounce buffer %d bytes, ISR core %d",
             DISPLAY_WIDTH, DISPLAY_HEIGHT,
             (int)(display_bounce_px() * sizeof(color_t)),
             xPortGetCoreID());
    return ESP_OK;
}

/* esp_lcd_new_rgb_panel() pins the LCD interrupt to the calling core. Core 0
 * already carries WiFi, NimBLE, and the SSH ingest path. The bounce ISR
 * alone can eat a third to half of it. This code starts panel bring-up
 * from a transient core-1 task, so the ISR lands on the near-idle shell
 * core. */
typedef struct {
    TaskHandle_t caller;
    esp_err_t    result;
} bringup_ctx_t;

static void panel_bringup_task(void *arg)
{
    bringup_ctx_t *ctx = arg;
    ctx->result = panel_bringup();
    xTaskNotifyGive(ctx->caller);
    vTaskDelete(NULL);
}

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing RGB LCD panel (bounce buffer mode)");

    init_backlight();

    bringup_ctx_t ctx = {
        .caller = xTaskGetCurrentTaskHandle(),
        .result = ESP_FAIL,
    };
    if (xTaskCreatePinnedToCore(panel_bringup_task, "lcd_init", 4096, &ctx,
                                10, NULL, 1) != pdPASS) {
        ESP_LOGW(TAG, "lcd_init task failed; panel ISR stays on core %d",
                 xPortGetCoreID());
        return panel_bringup();
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return ctx.result;
}

esp_lcd_panel_handle_t display_get_panel(void)
{
    return panel_handle;
}

esp_err_t display_set_backlight(uint8_t brightness)
{
    /* TODO: PWM for gradual brightness control */
    (void)brightness;
    return ESP_OK;
}
