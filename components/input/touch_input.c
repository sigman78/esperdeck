/*
 * Touch input backend — GT911 via esp_lcd_touch_gt911 component
 *
 * Polls GT911 at ~50ms intervals using the official Espressif component API.
 * Detects tap (touch down + up within 300ms) and long-press (held >= 500ms).
 * Posts INPUT_EVENT_TAP / INPUT_EVENT_LONG_PRESS with x,y coordinates.
 *
 * The Waveshare Touch-LCD-7 routes GT911 RST and INT through I2C IO expanders
 * (0x24 and 0x38) rather than direct GPIOs.  The custom reset sequence runs
 * before esp_lcd_touch_new_i2c_gt911(); rst_gpio_num/int_gpio_num are set to
 * GPIO_NUM_NC so the component skips its own reset path.
 */

//#if defined(CONFIG_INPUT_TOUCH) || defined(CONFIG_INPUT_AUTO)

#include "input_hal_internal.h"
#include "display.h"     /* DISPLAY_WIDTH — the edge strip is measured from it */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include <string.h>

static const char *TAG = "touch_input";

/* GT911 I2C address (ADDR pin low = 0x5D, high = 0x14; Waveshare board = 0x14) */
#define GT911_ADDR          0x5D

/* Timing thresholds (ms) */
#define TAP_MAX_MS          300
#define LONG_PRESS_MS       500

/* Poll interval (ms) */
#define POLL_INTERVAL_MS    50

/* Waveshare Touch-LCD-7 IO expander addresses and reset values */
#define IOEXP_ADDR_24       0x24
#define IOEXP_ADDR_38       0x38
#define IOEXP_24_INIT       0x01
#define IOEXP_38_ASSERT     0x2C   /* RST asserted */
#define IOEXP_38_DEASSERT   0x2E   /* RST released */
#define RESET_ASSERT_US     (100 * 1000)
#define RESET_GPIO_LOW_US   (100 * 1000)
#define RESET_HOLD_US       (200 * 1000)

/* Vertical travel that turns an edge press into a scroll drag. Below this a
 * press is still a tap: fingers wobble, and a 2 px twitch must not eat the
 * tap that was meant. */
#define SCROLL_START_PX     12

/* Touch state machine states */
typedef enum {
    STATE_IDLE,
    STATE_TOUCHING,
    STATE_SCROLLING,     /* edge drag in progress; taps suppressed */
    STATE_WAITING_LIFT,
} touch_state_t;

static esp_lcd_touch_handle_t s_tp = NULL;

/* Right-edge strip width in pixels; 0 = gesture off. Written from the app
 * task (menu toggle), read by the poll task — a plain int is atomic on the
 * S3 and a torn read is impossible for a value this size. */
static volatile int s_scroll_edge_px = 0;

void input_hal_set_scroll_edge(int width_px)
{
    s_scroll_edge_px = width_px < 0 ? 0 : width_px;
}

/* Did a press at @p x start inside the armed right-edge strip? */
static inline bool in_scroll_edge(uint16_t x)
{
    int w = s_scroll_edge_px;
    return w > 0 && (int)x >= DISPLAY_WIDTH - w;
}

/* ------------------------------------------------------------------ */
/* Poll task                                                            */
/* ------------------------------------------------------------------ */

static void touch_poll_task(void *arg)
{
    touch_state_t state       = STATE_IDLE;
    int64_t       touch_start = 0;
    uint16_t      touch_x     = 0;
    uint16_t      touch_y     = 0;
    uint16_t      last_y      = 0;   /* SCROLLING: y of the last event sent */

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));

        esp_lcd_touch_read_data(s_tp);   /* fetch latest sample; clears status reg internally */

        esp_lcd_touch_point_data_t pt = {0};
        uint8_t cnt = 0;
        esp_lcd_touch_get_data(s_tp, &pt, &cnt, 1);
        bool pressed = (cnt > 0);

        int64_t now_ms = (int64_t)(esp_timer_get_time() / 1000);

        switch (state) {
        case STATE_IDLE:
            if (pressed) {
                touch_x     = pt.x;
                touch_y     = pt.y;
                touch_start = now_ms;
                state       = STATE_TOUCHING;
                ESP_LOGD(TAG, "touch down x=%u y=%u", touch_x, touch_y);
            }
            break;

        case STATE_TOUCHING: {
            int64_t elapsed = now_ms - touch_start;

            /* An edge press that has travelled far enough becomes a scroll
             * drag. Checked before the long-press branch so a slow deliberate
             * drag is not stolen by the 500 ms timer. */
            if (pressed && in_scroll_edge(touch_x)) {
                int dy = (int)pt.y - (int)touch_y;
                if (dy > SCROLL_START_PX || dy < -SCROLL_START_PX) {
                    last_y = pt.y;
                    state  = STATE_SCROLLING;
                    ESP_LOGD(TAG, "scroll drag begins x=%u y=%u", touch_x, touch_y);
                    break;
                }
            }

            if (elapsed >= LONG_PRESS_MS) {
                input_event_t ev = {
                    .type = INPUT_EVENT_LONG_PRESS,
                    .len  = 0,
                    .x    = touch_x,
                    .y    = touch_y,
                };
                ESP_LOGD(TAG, "long-press x=%u y=%u", touch_x, touch_y);
                input_hal_post_event(&ev);
                state = STATE_WAITING_LIFT;
            } else if (!pressed && elapsed < TAP_MAX_MS) {
                input_event_t ev = {
                    .type = INPUT_EVENT_TAP,
                    .len  = 0,
                    .x    = touch_x,
                    .y    = touch_y,
                };
                ESP_LOGD(TAG, "tap x=%u y=%u elapsed=%lldms", touch_x, touch_y, elapsed);
                input_hal_post_event(&ev);
                state = STATE_IDLE;
            } else if (!pressed) {
                ESP_LOGD(TAG, "lift in dead zone (%lldms), no event", elapsed);
                state = STATE_IDLE;
            }
            break;
        }

        case STATE_SCROLLING:
            if (!pressed) {
                ESP_LOGD(TAG, "scroll drag ends");
                state = STATE_IDLE;
                break;
            }
            /* One event per poll that actually moved. The consumer converts
             * pixels to rows, so no font knowledge is needed here. */
            if (pt.y != last_y) {
                input_event_t ev = {
                    .type = INPUT_EVENT_SCROLL,
                    .len  = 0,
                    .x    = pt.x,
                    .y    = pt.y,
                    .dy   = (int16_t)((int)pt.y - (int)last_y),
                };
                last_y = pt.y;
                input_hal_post_event(&ev);
            }
            break;

        case STATE_WAITING_LIFT:
            if (!pressed) {
                ESP_LOGD(TAG, "finger lifted after long-press");
                state = STATE_IDLE;
            }
            break;

        default:
            state = STATE_IDLE;
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Waveshare hardware reset sequence                                    */
/* ------------------------------------------------------------------ */

#ifdef CONFIG_INPUT_TOUCH_WAVESHARE_RESET
static esp_err_t gt911_waveshare_reset(i2c_master_bus_handle_t bus)
{
    esp_err_t ret;

    /* Configure GPIO as output, drive HIGH before sequence */
    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_INPUT_TOUCH_RESET_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ret = gpio_config(&io_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(ret));
        return ret;
    }
    gpio_set_level(CONFIG_INPUT_TOUCH_RESET_GPIO, 1);

    /* IO expander 0x24 — write 0x01 */
    i2c_master_dev_handle_t iox24 = NULL;
    i2c_device_config_t cfg24 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = IOEXP_ADDR_24,
        .scl_speed_hz    = 400000,
    };
    ret = i2c_master_bus_add_device(bus, &cfg24, &iox24);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add ioexp 0x24 failed: %s", esp_err_to_name(ret));
        return ret;
    }
    uint8_t cmd = IOEXP_24_INIT;
    ret = i2c_master_transmit(iox24, &cmd, 1, 100);
    i2c_master_bus_rm_device(iox24);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ioexp 0x24 write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* IO expander 0x38 — assert reset (0x2C) */
    i2c_master_dev_handle_t iox38 = NULL;
    i2c_device_config_t cfg38 = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = IOEXP_ADDR_38,
        .scl_speed_hz    = 400000,
    };
    ret = i2c_master_bus_add_device(bus, &cfg38, &iox38);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add ioexp 0x38 failed: %s", esp_err_to_name(ret));
        return ret;
    }
    cmd = IOEXP_38_ASSERT;
    ret = i2c_master_transmit(iox38, &cmd, 1, 100);
    if (ret != ESP_OK) {
        i2c_master_bus_rm_device(iox38);
        ESP_LOGE(TAG, "ioexp 0x38 assert failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_rom_delay_us(RESET_ASSERT_US);

    /* GPIO LOW */
    gpio_set_level(CONFIG_INPUT_TOUCH_RESET_GPIO, 0);
    esp_rom_delay_us(RESET_GPIO_LOW_US);

    /* IO expander 0x38 — deassert reset (0x2E) */
    cmd = IOEXP_38_DEASSERT;
    ret = i2c_master_transmit(iox38, &cmd, 1, 100);
    i2c_master_bus_rm_device(iox38);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ioexp 0x38 deassert failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_rom_delay_us(RESET_HOLD_US);

    ESP_LOGI(TAG, "GT911 hardware reset complete");
    return ESP_OK;
}
#endif /* CONFIG_INPUT_TOUCH_WAVESHARE_RESET */

/* ------------------------------------------------------------------ */
/* Init                                                                 */
/* ------------------------------------------------------------------ */

esp_err_t touch_input_backend_init(void)
{
    ESP_LOGI(TAG, "GT911 touch backend init called");
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port            = CONFIG_INPUT_TOUCH_I2C_PORT,
        .sda_io_num          = CONFIG_INPUT_TOUCH_SDA_PIN,
        .scl_io_num          = CONFIG_INPUT_TOUCH_SCL_PIN,
        .clk_source          = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt   = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus_handle = NULL;
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(ret));
        return ret;
    }

#ifdef CONFIG_INPUT_TOUCH_WAVESHARE_RESET
    ret = gt911_waveshare_reset(bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GT911 reset failed: %s", esp_err_to_name(ret));
        i2c_del_master_bus(bus_handle);
        return ret;
    }
#endif

    /* Bridge: I2C master bus → LCD panel IO (interface required by esp_lcd_touch) */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr            = GT911_ADDR,
        .control_phase_bytes = 1,
        .dc_bit_offset       = 0,
        .lcd_cmd_bits        = 16,
        .scl_speed_hz        = 400000,
        .flags.disable_control_phase = 1,
    };
    ret = esp_lcd_new_panel_io_i2c_v2(bus_handle, &io_cfg, &io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_i2c_v2 failed: %s", esp_err_to_name(ret));
        i2c_del_master_bus(bus_handle);
        return ret;
    }

    /* GT911 — no direct RST/INT GPIOs on Waveshare; IO expanders handled above */
    esp_lcd_touch_io_gt911_config_t gt911_cfg = { .dev_addr = GT911_ADDR };
    esp_lcd_touch_config_t tp_cfg = {
        .x_max        = 800,
        .y_max        = 480,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels       = { .reset = 0, .interrupt = 0 },
        .flags        = { .swap_xy = 0, .mirror_x = 0, .mirror_y = 0 },
        .driver_data  = &gt911_cfg,
    };
    ret = esp_lcd_touch_new_i2c_gt911(io_handle, &tp_cfg, &s_tp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_touch_new_i2c_gt911 failed: %s", esp_err_to_name(ret));
        esp_lcd_panel_io_del(io_handle);
        i2c_del_master_bus(bus_handle);
        return ret;
    }

    /* Permanent task, PSRAM stack (I2C polling only — no flash writes):
     * keeps 3 KB of internal DRAM for NimBLE/WiFi. */
#define TOUCH_TASK_STACK 3072
    static StaticTask_t s_touch_tcb;
    StackType_t *touch_stack = heap_caps_malloc(TOUCH_TASK_STACK,
                                                MALLOC_CAP_SPIRAM |
                                                MALLOC_CAP_8BIT);
    BaseType_t xret = pdFAIL;
    if (touch_stack &&
        xTaskCreateStaticPinnedToCore(
            touch_poll_task,
            "touch_poll",
            TOUCH_TASK_STACK / sizeof(StackType_t),
            NULL,
            4,
            touch_stack,
            &s_touch_tcb,
            0   /* core 0 */
        ) != NULL)
        xret = pdPASS;
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "failed to create touch_poll_task");
        esp_lcd_touch_del(s_tp);
        esp_lcd_panel_io_del(io_handle);
        i2c_del_master_bus(bus_handle);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "GT911 touch backend initialised (I2C port %d, SDA=%d, SCL=%d)",
             CONFIG_INPUT_TOUCH_I2C_PORT,
             CONFIG_INPUT_TOUCH_SDA_PIN,
             CONFIG_INPUT_TOUCH_SCL_PIN);
    return ESP_OK;
}

//#endif /* CONFIG_INPUT_TOUCH || CONFIG_INPUT_AUTO */

/* ------------------------------------------------------------------ */
/* No-op stub when touch is disabled                                    */
/* ------------------------------------------------------------------ */

//#if !defined(CONFIG_INPUT_TOUCH) && !defined(CONFIG_INPUT_AUTO)
//#include "esp_err.h"
//esp_err_t touch_input_backend_init(void) { return ESP_OK; }
//#endif
