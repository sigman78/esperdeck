/*
 * Input HAL — queue + backend dispatch
 */

#include "input_hal.h"
#include "input_hal_internal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"

#define INPUT_QUEUE_LEN  16

static const char *TAG = "input_hal";
static QueueHandle_t s_queue = NULL;

void input_hal_post_event(const input_event_t *ev)
{
    if (!s_queue) return;
    if (xQueueSendToBack(s_queue, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "input queue full, event dropped");
    }
}

esp_err_t input_hal_init(void)
{
    if (s_queue) return ESP_OK;  /* already initialised */

    ESP_LOGI(TAG, "input HAL init ---");

    s_queue = xQueueCreate(INPUT_QUEUE_LEN, sizeof(input_event_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "failed to create input queue");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    esp_err_t r;

    ESP_LOGI(TAG, "input HAL BLE init");
    r = ble_keyboard_backend_init();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "BLE keyboard init failed: %s", esp_err_to_name(r));
#if defined(CONFIG_INPUT_BLE)
        ret = r;   /* fatal in BLE-only mode */
#endif
    }

    ESP_LOGI(TAG, "input HAL UART init");
    r = input_uart_backend_init();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "UART input init failed: %s", esp_err_to_name(r));
#if defined(CONFIG_INPUT_UART)
        ret = r;   /* fatal in UART-only mode */
#endif
    }

    ESP_LOGI(TAG, "input HAL Touch init");
    r = touch_input_backend_init();
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "touch input init failed: %s", esp_err_to_name(r));
#if defined(CONFIG_INPUT_TOUCH)
        ret = r;   /* fatal in touch-only mode */
#endif
    }

    ESP_LOGI(TAG, "input HAL init done ---");

    return ret;
}

bool input_hal_read(input_event_t *ev, uint32_t timeout_ms)
{
    TickType_t ticks = timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY;
    return xQueueReceive(s_queue, ev, ticks) == pdTRUE;
}
