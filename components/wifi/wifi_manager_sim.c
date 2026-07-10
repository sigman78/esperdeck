/*
 * wifi_manager_sim.c — simulator backend. The host already has networking,
 * so connect() succeeds immediately; init() performs WSAStartup for the
 * Winsock-based SSH path.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include "wifi_manager.h"
#include "esp_log.h"
#include <stdio.h>

static const char *TAG = "wifi_sim";

static wifi_mgr_state_t s_state = WIFI_MGR_IDLE;
static char s_ssid[33] = "";

esp_err_t wifi_manager_init(void)
{
    static bool s_wsa_up = false;
    if (s_wsa_up) return ESP_OK;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        ESP_LOGE(TAG, "WSAStartup failed");
        return ESP_FAIL;
    }
    s_wsa_up = true;
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const wifi_profile_t *profiles, int count)
{
    if (!profiles || count <= 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) return err;
    snprintf(s_ssid, sizeof(s_ssid), "%s", profiles[0].ssid);
    s_state = WIFI_MGR_CONNECTED;
    ESP_LOGI(TAG, "WiFi mocked (simulator), 'connected' to '%s'", s_ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_disconnect(void)
{
    s_state = WIFI_MGR_IDLE;
    return ESP_OK;
}

esp_err_t wifi_manager_adopt(const wifi_profile_t *profiles, int count,
                             const char *ssid)
{
    if (!profiles || count <= 0) return ESP_ERR_INVALID_ARG;
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid && ssid[0] ? ssid : profiles[0].ssid);
    s_state = WIFI_MGR_CONNECTED;
    return ESP_OK;
}

wifi_mgr_state_t wifi_manager_get_state(void) { return s_state; }
bool wifi_manager_is_connected(void) { return s_state == WIFI_MGR_CONNECTED; }
const char *wifi_manager_get_ip(void)   { return "127.0.0.1"; }
const char *wifi_manager_get_ssid(void) { return s_ssid; }
int wifi_manager_get_rssi(void) { return s_state == WIFI_MGR_CONNECTED ? -55 : 0; }
time_t wifi_manager_time(void) { return time(NULL); }   /* host clock */
