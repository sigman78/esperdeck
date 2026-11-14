/*
 * app_widgets.c — shell-side status strings: the bits of chrome that
 * read shell or service state (wifi, BLE, heap, wall clock). The
 * reusable parts live in components/ui.
 */

#include "app_widgets.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <time.h>

ui_act_t app_hot_input(const cyberdeck_input_t *ev, ui_key_t k)
{
    switch (ev->type) {
    case CYBERDECK_INPUT_TAP:
        return ui_touch_px(UI_TOUCH_TAP, ev->x, ev->y);
    case CYBERDECK_INPUT_LONG_PRESS:
        return ui_touch_px(UI_TOUCH_HOLD, ev->x, ev->y);
    case CYBERDECK_INPUT_PRESS:
        return ui_touch_px(UI_TOUCH_DOWN, ev->x, ev->y);
    case CYBERDECK_INPUT_RELEASE:
        return ui_touch_px(UI_TOUCH_UP, ev->x, ev->y);
    case CYBERDECK_INPUT_SCROLL:
        return (ui_act_t){ UI_ACT_NONE, 0 };
    default:
        return ui_hot_key(k);
    }
}

const char *wifi_status_str(void)
{
    switch (wifi_manager_get_state()) {
    case WIFI_MGR_CONNECTED:  return wifi_manager_get_ip();
    case WIFI_MGR_CONNECTING: return "connecting...";
    case WIFI_MGR_LOST:       return "reconnecting...";
    case WIFI_MGR_FAILED:     return "failed (retrying)";
    default:                  return "off";
    }
}

const char *ble_status_str(void)
{
    if (!app.ble || !app.ble->get_state) return "n/a";
    switch (app.ble->get_state()) {
    case CYBERDECK_BLE_CONNECTED:    return "connected";
    case CYBERDECK_BLE_CONNECTING:   return "connecting...";
    case CYBERDECK_BLE_PAIRING_SCAN: return "pairing scan";
    case CYBERDECK_BLE_RECONNECT:    return "searching...";
    default:                         return "idle";
    }
}

/* Wall-clock "HH:MM": SNTP time on device (0 until synced — no RTC
 * battery), host clock in the simulator. */
bool clock_str(char *buf, size_t sz)
{
    time_t t = wifi_manager_time();
    if (t == 0) return false;
    struct tm tm;
#ifdef _WIN32
    if (localtime_s(&tm, &t) != 0) return false;  /* UCRT: no localtime_r */
#else
    if (!localtime_r(&t, &tm)) return false;
#endif
    snprintf(buf, sz, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return true;
}

