/*
 * wifi_provision — host/sim stub. No real provisioning; just enough state so
 * the shell's provisioning screen can be exercised in the simulator.
 */

#include "wifi_provision.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int  s_state = WIFI_PROV_ST_IDLE;
static char s_qr[192] = "";

esp_err_t wifi_provision_start(void)
{
    s_state = WIFI_PROV_ST_ACTIVE;
    snprintf(s_qr, sizeof(s_qr),
             "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"softap\"}",
             wifi_provision_service_name(), wifi_provision_pop());
    return ESP_OK;
}

void wifi_provision_stop(void)          { s_state = WIFI_PROV_ST_IDLE; s_qr[0] = '\0'; }
int  wifi_provision_state(void)         { return s_state; }
const char *wifi_provision_service_name(void) { return "CYBERDECK-SIM"; }
const char *wifi_provision_pop(void)          { return "1A2B3C4D"; }
const char *wifi_provision_qr_payload(void)   { return s_qr; }
const char *wifi_provision_ssid(void)         { return ""; }

/* Fake-but-plausible QR for layout preview: finder squares, timing lines,
 * hashed data modules. NOT scannable — the host build has no qrcodegen; the
 * device generates the real one. 33 modules matches the ~v4 code the real
 * provisioning payload produces. */
#define SIM_QR_SIZE 33

static bool fake_finder(int x, int y, int size)
{
    static const int ox[3] = { 0, 1, 0 };        /* corner selector */
    for (int c = 0; c < 3; c++) {
        int dx = x - (ox[c]     ? size - 7 : 0);
        int dy = y - (c == 2    ? size - 7 : 0);
        if (dx < 0 || dx > 6 || dy < 0 || dy > 6) continue;
        return dx == 0 || dx == 6 || dy == 0 || dy == 6 ||
               (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4);
    }
    return false;
}

static bool fake_qr_module(int x, int y, int size, uint32_t seed)
{
    if (x < 0 || y < 0 || x >= size || y >= size) return false;
    bool in_finder =
        (x < 8 && y < 8) || (x >= size - 8 && y < 8) || (x < 8 && y >= size - 8);
    if (in_finder) return fake_finder(x, y, size);
    if (x == 6 || y == 6) return ((x + y) & 1) == 0;   /* timing lines */
    uint32_t h = (((uint32_t)x * 31u + (uint32_t)y) ^ seed) * 2654435761u;
    h ^= h >> 15;
    return (h >> 3) & 1;
}

int  wifi_provision_qr_size(void)
{
    return s_state == WIFI_PROV_ST_IDLE ? 0 : SIM_QR_SIZE;
}
bool wifi_provision_qr_module(int x, int y)
{
    return fake_qr_module(x, y, SIM_QR_SIZE, 0x5EED0001u);
}
