/*
 * Cyberdeck SSH Terminal — device composition root.
 *
 * Hardware/IDF initialization lives here. Everything user-facing (profile
 * picker, pairing UI, session flow) lives in the cyberdeck_app shell.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "ble_keyboard.h"
#include "cyberdeck_app.h"
#include "display.h"
#include "font.h"
#include "input_hal.h"
#include "splash.h"
#include "ssh_client.h"
#include "storage.h"
#include "vterm.h"
#include "wifi_manager.h"

static const char *TAG = "cyberdeck";

/* Disabled bool Kconfig options emit no #define at all, so guard it. */
#ifndef CONFIG_SSH_AUTO_RECONNECT
#define CONFIG_SSH_AUTO_RECONNECT 0
#endif

/* -------------------------------------------------------------------------
 * Heap diagnostic helper
 * ---------------------------------------------------------------------- */
static void log_heap(const char *label)
{
    ESP_LOGI(TAG, "Heap %-30s  total=%7u  int=%7u  int_blk=%7u",
             label,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

/* -------------------------------------------------------------------------
 * System init helpers
 * ---------------------------------------------------------------------- */
static void init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

static void init_network(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /* Timezone for the on-screen clock; SNTP itself starts on link-up. */
    setenv("TZ", CONFIG_CYBERDECK_TZ, 1);
    tzset();
}

/* -------------------------------------------------------------------------
 * BLE keyboard ops for the shell (device backend = ble_keyboard/NimBLE)
 * ---------------------------------------------------------------------- */
static int ble_ops_get_state(void) { return (int)ble_keyboard_get_state(); }

static const cyberdeck_ble_ops_t s_ble_ops = {
    .get_state        = ble_ops_get_state,
    .enter_pairing    = ble_keyboard_enter_pairing,
    .exit_pairing     = ble_keyboard_exit_pairing,
    .get_scan_results = ble_keyboard_get_scan_results,
    .select_device    = ble_keyboard_select_device,
    .get_name         = ble_keyboard_get_connected_name,
    .forget           = ble_keyboard_forget_all,
};

/* -------------------------------------------------------------------------
 * Main task — pumps input into the shell
 * ---------------------------------------------------------------------- */
static uint64_t now_ms(void)
{
    return (uint64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void main_task(void *pvParameters)
{
    (void)pvParameters;
    ESP_LOGI(TAG, "Main task started");

    for (;;) {
        cyberdeck_app_tick(now_ms());

        input_event_t ev;
        if (!input_hal_read(&ev, 50))
            continue;

        cyberdeck_input_t app_ev = {
            .type = ev.type,
            .len  = ev.len,
            .x    = ev.x,
            .y    = ev.y,
        };
        memcpy(app_ev.buf, ev.buf, sizeof(app_ev.buf));
        cyberdeck_app_handle_input(&app_ev, now_ms());
    }
}

/* -------------------------------------------------------------------------
 * Application entry point
 * ---------------------------------------------------------------------- */
void app_main(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "Cyberdeck SSH Terminal");
    ESP_LOGI(TAG, "ESP32-S3 @ %d MHz", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    ESP_LOGI(TAG, "===========================================");

    log_heap("boot");
    init_nvs();

    if (storage_init() != ESP_OK) {
        ESP_LOGW(TAG, "Storage init failed — using Kconfig defaults");
    }

    init_network();
    log_heap("after netif_init");

    display_init();
    font_init();
    vterm_init(CONFIG_TERMINAL_WIDTH, CONFIG_TERMINAL_HEIGHT);
    splash_show();
    log_heap("after display+vterm");

    if (wifi_manager_init() != ESP_OK) {
        ESP_LOGW(TAG, "WiFi init failed (non-fatal)");
    }

    if (input_hal_init() != ESP_OK) {
        ESP_LOGW(TAG, "Input HAL init failed (non-fatal)");
    }
    /* Reconnect a bonded keyboard in the background from the moment we
     * boot — pairing a new one is reachable from the shell ('b' / touch). */
    ble_keyboard_reconnect_start();

    ssh_client_init();
    log_heap("after input+ssh init");

    cyberdeck_app_config_t app_cfg = {
        .boot_delay_ms      = 1500,
        .ssh_retry_delay_ms = 5000,
        .auto_reconnect     = CONFIG_SSH_AUTO_RECONNECT,
        .version            = esp_app_get_description()->version,
        .fallback_host      = CONFIG_SSH_DEFAULT_HOST,
        .fallback_port      = CONFIG_SSH_DEFAULT_PORT,
        .fallback_user      = CONFIG_SSH_DEFAULT_USER,
        .fallback_password  = CONFIG_SSH_DEFAULT_PASSWORD,
        .fallback_wifi_ssid     = CONFIG_WIFI_SSID,
        .fallback_wifi_password = CONFIG_WIFI_PASSWORD,
        .ble = &s_ble_ops,
    };
    ESP_ERROR_CHECK(cyberdeck_app_init(&app_cfg, now_ms()));

    /* The shell writes profiles/known-hosts to flash from this task, so its
     * stack must be internal DRAM (flash ops forbid external-RAM stacks).
     * Static (.bss) rather than heap: by this point NimBLE + WiFi + the
     * 2x12 KB overlay have fragmented internal DRAM, and the 12 KB
     * contiguous heap_caps_malloc here failed intermittently — boot then
     * halted on the splash screen with everything else still running.
     * Same RAM cost, zero fragmentation risk, and the heap keeps a big
     * block free for NimBLE's connect-time allocations. */
#define MAIN_TASK_STACK 12288
    static StaticTask_t s_main_task_tcb;
    static StackType_t  s_main_task_stack[MAIN_TASK_STACK / sizeof(StackType_t)];

    xTaskCreateStaticPinnedToCore(
        main_task,
        "main_task",
        MAIN_TASK_STACK / sizeof(StackType_t),
        NULL,
        5,
        s_main_task_stack,
        &s_main_task_tcb,
        1
    );
#undef MAIN_TASK_STACK

    log_heap("after shell init");
    ESP_LOGI(TAG, "System initialized");
}
