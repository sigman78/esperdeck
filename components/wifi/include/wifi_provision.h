/*
 * wifi_provision — SoftAP WiFi onboarding via the ESP provisioning manager.
 *
 * Starts a temporary SoftAP + HTTP provisioning service. A phone running the
 * "ESP SoftAP Provisioning" app (or the QR flow) sends WiFi credentials. The
 * manager tests them and, on success, saves them into wifi.ini as the
 * preferred network.
 *
 * The design chooses SoftAP deliberately: it needs no BLE, so the HID
 * keyboard's NimBLE stack stays untouched. wifi_provision_stop() deinits the
 * manager as soon as the flow ends, to reclaim internal RAM.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* UI-facing provisioning state (poll from the shell). */
enum {
    WIFI_PROV_ST_IDLE = 0,
    WIFI_PROV_ST_ACTIVE,     /* SoftAP up, waiting for the phone      */
    WIFI_PROV_ST_RECEIVED,   /* credentials received, testing them    */
    WIFI_PROV_ST_SUCCESS,    /* connected + saved to wifi.ini         */
    WIFI_PROV_ST_FAILED,     /* wrong password / AP not found         */
};

/** Begin SoftAP provisioning. Parks wifi_manager for the duration. */
esp_err_t wifi_provision_start(void);

/** Stop + deinit the provisioning service (reclaims RAM), restore STA mode. */
void wifi_provision_stop(void);

/** Current WIFI_PROV_ST_* value. */
int wifi_provision_state(void);

/** SoftAP SSID the phone should join (e.g. "CYBERDECK-1A2B"). */
const char *wifi_provision_service_name(void);

/** Proof-of-possession the phone must enter. */
const char *wifi_provision_pop(void);

/** JSON payload for the provisioning QR code. */
const char *wifi_provision_qr_payload(void);

/** Side length (modules) of the generated QR, or 0 if none. */
int wifi_provision_qr_size(void);

/** True if the QR module at (x,y) is dark. Out-of-range returns false. */
bool wifi_provision_qr_module(int x, int y);

/** SSID the manager received/provisioned ("" until CRED_RECV). */
const char *wifi_provision_ssid(void);
