/*
 * app_wifi.h — saved-network helpers for the Setup pages (internal).
 * The edits go through storage_wifi_save(), so passwords divert into the
 * secrets bundle as usual. No call returns a password.
 */

#pragma once

#include "esp_err.h"
#include "storage.h"
#include <stdbool.h>

/* Pure list edits (no storage, no keystore): array in, array out.
 * locked = the keystore is LOCKED. Then an add must carry no password
 * and must not replace a marker row: ESP_ERR_INVALID_STATE.
 * ESP_ERR_NO_MEM when the list is full, ESP_ERR_INVALID_ARG on bad input. */
esp_err_t app_wifi_list_add(wifi_profile_t *nets, int *n, int max,
                            const char *ssid, const char *password,
                            bool locked);
esp_err_t app_wifi_list_forget(wifi_profile_t *nets, int *n, int i);
esp_err_t app_wifi_list_move(wifi_profile_t *nets, int n, int i, int dir);

/* Saved SSIDs in preference order; bundled[i] is true while the password
 * is still the locked-bundle marker. Returns the count. */
int app_wifi_list(char ssids[][33], bool *bundled, int max);

/* Each call saves, then gives the list to wifi_manager and keeps a live
 * link. If the joined network goes, it disconnects and tries the next.
 * ESP_ERR_INVALID_STATE: the locked store refuses the change. */
esp_err_t app_wifi_add(const char *ssid, const char *password);
esp_err_t app_wifi_forget(int i);
esp_err_t app_wifi_move(int i, int dir);

/* settings.ini [wifi] seeded: set after the Kconfig fallback seed, or
 * when the user edits the list. The seed never runs again. */
bool app_wifi_seeded(void);
void app_wifi_set_seeded(void);
