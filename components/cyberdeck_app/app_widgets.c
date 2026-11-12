/*
 * app_widgets.c — shell-side status strings: the bits of chrome that
 * read shell or service state (wifi, BLE, heap, wall clock). The
 * reusable parts live in components/ui.
 */

#include "app_widgets.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <time.h>

#include "esp_heap_caps.h"   /* free-RAM stats (idfsim stubs it) */

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

/* Free-RAM summary for the header. */
void ram_stats(char *buf, size_t sz)
{
#ifdef BUILD_SIMULATOR
    snprintf(buf, sz, "host build");
#else
    unsigned in = (unsigned)(heap_caps_get_free_size(
                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);
    unsigned ps = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    snprintf(buf, sz, "int %uK  psram %uK", in, ps);
#endif
}
