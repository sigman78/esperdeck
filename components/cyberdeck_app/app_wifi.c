/*
 * app_wifi.c — saved-network helpers for the Setup pages. See app_wifi.h.
 *
 * Credentials stage in the shared cred scratch. Each path wipes it before
 * kick_wifi reuses it. A locked save writes marker rows back unchanged
 * (storage_wifi_write_raw), so move and forget cannot downgrade them.
 */

#include "app_wifi.h"
#include "app_internal.h"
#include "app_settings.h"
#include "keystore.h"
#include "storage_cred.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#define SCRATCH_WIPE(n) keystore_wipe((n), sizeof(wifi_profile_t) * STORAGE_WIFI_MAX)

bool app_wifi_seeded(void)
{
    app_wifi_cfg_t c = { .seeded = false };
    storage_kv_load(cyberdeck_settings_ini, app_wifi_section, app_wifi_fields, &c);
    return c.seeded;
}

void app_wifi_set_seeded(void)
{
    if (app_wifi_seeded()) return;
    const app_wifi_cfg_t c = { .seeded = true };
    storage_kv_save(cyberdeck_settings_ini, app_wifi_section, app_wifi_fields, &c);
}

int app_wifi_list(char ssids[][33], bool *bundled, bool *open, int max)
{
    wifi_profile_t *nets = storage_cred_scratch()->u.nets;
    int n = 0;
    storage_wifi_load(nets, &n, STORAGE_WIFI_MAX);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        snprintf(ssids[i], 33, "%.32s", nets[i].ssid);
        if (bundled)
            bundled[i] = strcmp(nets[i].password, STORAGE_PW_BUNDLED) == 0;
        if (open) open[i] = nets[i].password[0] == '\0';
    }
    SCRATCH_WIPE(nets);
    return n;
}

/* Give the saved list to the manager. A live link on a kept network stays
 * up. Otherwise this drops the old list and tries the next network. */
static void apply(wifi_profile_t *nets, int n, const char *cur)
{
    const bool was_up = wifi_manager_is_connected() && cur[0];
    bool kept = false;
    int u = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(nets[i].ssid, cur) == 0) kept = true;
        if (strcmp(nets[i].password, STORAGE_PW_BUNDLED) == 0) continue;
        if (u != i) nets[u] = nets[i];
        u++;
    }
    if (was_up && kept) {
        if (u > 0) wifi_manager_adopt(nets, u, cur);
        SCRATCH_WIPE(nets);
        return;
    }
    SCRATCH_WIPE(nets);
    if (was_up || u == 0) wifi_manager_disconnect();
    kick_wifi();
}

/* Save the edited scratch list, mark the list user-owned, apply it. */
static esp_err_t commit(wifi_profile_t *nets, int n)
{
    char cur[33];
    snprintf(cur, sizeof cur, "%.32s", wifi_manager_get_ssid());
    const esp_err_t e = storage_wifi_save(nets, n);
    if (e != ESP_OK) {
        SCRATCH_WIPE(nets);
        return e;
    }
    app_wifi_set_seeded();
    apply(nets, n, cur);
    return ESP_OK;
}

esp_err_t app_wifi_add(const char *ssid, const char *password)
{
    wifi_profile_t *nets = storage_cred_scratch()->u.nets;
    int n = 0;
    storage_wifi_load(nets, &n, STORAGE_WIFI_MAX);
    const esp_err_t e = app_wifi_list_add(nets, &n, STORAGE_WIFI_MAX, ssid,
                                          password,
                                          keystore_state() == KEYSTORE_LOCKED);
    if (e != ESP_OK) {
        SCRATCH_WIPE(nets);
        return e;
    }
    return commit(nets, n);
}

esp_err_t app_wifi_forget(int i)
{
    wifi_profile_t *nets = storage_cred_scratch()->u.nets;
    int n = 0;
    storage_wifi_load(nets, &n, STORAGE_WIFI_MAX);
    const esp_err_t e = app_wifi_list_forget(nets, &n, i);
    if (e != ESP_OK) {
        SCRATCH_WIPE(nets);
        return e;
    }
    return commit(nets, n);
}

/* The manager cannot join one SSID. List order is the preference. So the
 * network goes first and the link restarts on the saved list. */
esp_err_t app_wifi_join(int i)
{
    wifi_profile_t *nets = storage_cred_scratch()->u.nets;
    int n = 0;
    storage_wifi_load(nets, &n, STORAGE_WIFI_MAX);
    esp_err_t e = i >= 0 && i < n ? ESP_OK : ESP_ERR_INVALID_ARG;
    for (int k = i; k > 0 && e == ESP_OK; k--)
        e = app_wifi_list_move(nets, n, k, -1);
    if (e == ESP_OK) e = storage_wifi_save(nets, n);
    SCRATCH_WIPE(nets);
    if (e != ESP_OK) return e;
    app_wifi_set_seeded();
    wifi_manager_disconnect();
    kick_wifi();
    return ESP_OK;
}

esp_err_t app_wifi_move(int i, int dir)
{
    wifi_profile_t *nets = storage_cred_scratch()->u.nets;
    int n = 0;
    storage_wifi_load(nets, &n, STORAGE_WIFI_MAX);
    const esp_err_t e = app_wifi_list_move(nets, n, i, dir);
    if (e != ESP_OK) {
        SCRATCH_WIPE(nets);
        return e;
    }
    return commit(nets, n);
}
