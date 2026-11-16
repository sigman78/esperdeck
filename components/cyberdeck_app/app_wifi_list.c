/*
 * app_wifi_list.c — pure edits of the saved-network array. No storage,
 * no keystore: the caller passes the lock state (host-testable).
 */

#include "app_wifi.h"
#include "keystore.h"   /* keystore_wipe */

#include <string.h>

esp_err_t app_wifi_list_add(wifi_profile_t *nets, int *n, int max,
                            const char *ssid, const char *password,
                            bool locked)
{
    if (!nets || !n || !ssid || !password || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (strlen(ssid) >= sizeof(nets->ssid) ||
        strlen(password) >= sizeof(nets->password) ||
        strcmp(password, STORAGE_PW_BUNDLED) == 0)
        return ESP_ERR_INVALID_ARG;

    int at = -1;
    for (int i = 0; i < *n; i++)
        if (strcmp(nets[i].ssid, ssid) == 0) { at = i; break; }

    /* Locked: no password can enter the bundle. A marker row stays a
     * marker, or its bundle entry loses its owner. */
    if (locked && (password[0] ||
                   (at >= 0 && strcmp(nets[at].password, STORAGE_PW_BUNDLED) == 0)))
        return ESP_ERR_INVALID_STATE;

    if (at < 0) {
        if (*n >= max) return ESP_ERR_NO_MEM;
        at = (*n)++;
        memset(&nets[at], 0, sizeof(nets[at]));
        memcpy(nets[at].ssid, ssid, strlen(ssid) + 1);
    }
    memcpy(nets[at].password, password, strlen(password) + 1);
    return ESP_OK;
}

esp_err_t app_wifi_list_forget(wifi_profile_t *nets, int *n, int i)
{
    if (!nets || !n || i < 0 || i >= *n) return ESP_ERR_INVALID_ARG;
    for (int j = i; j + 1 < *n; j++) nets[j] = nets[j + 1];
    memset(&nets[--(*n)], 0, sizeof(nets[0]));
    return ESP_OK;
}

esp_err_t app_wifi_list_move(wifi_profile_t *nets, int n, int i, int dir)
{
    const int j = i + dir;
    if (!nets || (dir != -1 && dir != 1) || i < 0 || i >= n || j < 0 || j >= n)
        return ESP_ERR_INVALID_ARG;
    wifi_profile_t t = nets[i];
    nets[i] = nets[j];
    nets[j] = t;
    keystore_wipe(&t, sizeof t);
    return ESP_OK;
}
