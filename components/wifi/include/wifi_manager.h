/*
 * WiFi manager — profile-driven STA lifecycle.
 *
 * wifi_manager_connect() takes a list of stored profiles and works through
 * them in order (async, event-driven) until one yields an IP. On link loss
 * it retries automatically; wifi_manager_disconnect() stops that.
 *
 * Simulator: always CONNECTED after connect() (host has networking);
 * init performs WSAStartup.
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include "storage.h"    /* wifi_profile_t */
#include <stdbool.h>
#include <time.h>

typedef enum {
    WIFI_MGR_IDLE = 0,      /* no connect requested (or intentionally down) */
    WIFI_MGR_CONNECTING,    /* cycling through profiles                     */
    WIFI_MGR_CONNECTED,     /* associated, got IP                           */
    WIFI_MGR_LOST,          /* had a link, lost it — auto-retrying          */
    WIFI_MGR_FAILED,        /* full profile cycle failed — slow retry       */
} wifi_mgr_state_t;

/** One-time driver init. Idempotent. Call after esp_netif/event loop init. */
esp_err_t wifi_manager_init(void);

/**
 * Start connecting. This copies the profiles; it tries them in array
 * order, cycling with backoff until one connects or wifi_manager_disconnect()
 * stops it.
 */
esp_err_t wifi_manager_connect(const wifi_profile_t *profiles, int count);

/** Intentional disconnect — stops auto-retry, state returns to IDLE. */
esp_err_t wifi_manager_disconnect(void);

/**
 * Adopt a connection established outside the manager (e.g. by the provisioning
 * manager). Loads @p profiles as the new preference list and resumes
 * auto-reconnect. If the link is already up (state CONNECTED), the code
 * keeps it as-is — no disruptive reconnect. Otherwise, it starts a normal
 * connect.
 */
/**
 * Hand over the WiFi credential a PAST firmware persisted into the
 * driver's NVS (captured once at init, NVS copy cleared). Returns true
 * and wipes the stash on first call with a credential present — the
 * caller folds it into storage (the secrets bundle). False otherwise.
 */
bool wifi_manager_take_nvs_cred(wifi_profile_t *out);

/**
 * Clear the driver's NVS credential copy. Call this ONLY after the
 * migration has verifiably saved it to storage, and only before any
 * connect on this boot. It resets the driver; STA mode is re-asserted.
 */
void wifi_manager_clear_nvs_cred(void);

esp_err_t wifi_manager_adopt(const wifi_profile_t *profiles, int count,
                             const char *ssid);

wifi_mgr_state_t wifi_manager_get_state(void);
bool             wifi_manager_is_connected(void);
const char      *wifi_manager_get_ip(void);
/** SSID of the connected (or currently attempted) profile, "" if none. */
const char      *wifi_manager_get_ssid(void);
/** RSSI of the current AP in dBm (negative), 0 when not connected. */
int              wifi_manager_get_rssi(void);

/** Wall-clock epoch seconds from a one-shot NTP fetch (started at first
 *  link-up), 0 until synced. The SYSTEM clock is deliberately never set.
 *  Stepping it decades forward mid-handshake corrupts libssh2's blocking-
 *  timeout arithmetic (elapsed = difftime(time(NULL), start)). */
time_t           wifi_manager_time(void);

#endif // WIFI_MANAGER_H
