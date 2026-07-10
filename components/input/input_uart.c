/*
 * USB-Serial-JTAG input backend
 *
 * Installs the USB-Serial-JTAG driver (no-op if IDF startup already did it)
 * and reads bytes with usb_serial_jtag_read_bytes() in a dedicated task.
 *
 * input_uart_backend_init() is always defined so the linker is satisfied
 * regardless of which CONFIG_INPUT_* value is active.
 */

#include "sdkconfig.h"
#include "input_hal_internal.h"
#include "esp_log.h"

static const char *TAG = "uart_input";

#if defined(CONFIG_INPUT_UART) || defined(CONFIG_INPUT_AUTO)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"

static void uart_input_task(void *arg)
{
    uint8_t ch;
    while (1) {
        int n = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(10));
        if (n <= 0) continue;
        input_event_t ev = { .type = INPUT_EVENT_KEY, .len = 1, .buf = { ch } };
        input_hal_post_event(&ev);
    }
}

#endif /* CONFIG_INPUT_UART || CONFIG_INPUT_AUTO */

esp_err_t input_uart_backend_init(void)
{
#if defined(CONFIG_INPUT_UART) || defined(CONFIG_INPUT_AUTO)
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = 256,
        .tx_buffer_size = 256,
    };
    esp_err_t ret = usb_serial_jtag_driver_install(&cfg);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "USB-JTAG driver already installed, reusing");
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install: %s", esp_err_to_name(ret));
        return ret;
    }

    BaseType_t rc = xTaskCreatePinnedToCore(
        uart_input_task, "uart_input",
        2048, NULL, 5, NULL, 0);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "failed to create uart_input task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB-Serial-JTAG input backend initialised");
    return ESP_OK;
#else
    return ESP_OK;  /* backend not configured, no-op */
#endif
}
