/*
 * wifi_provision — device implementation (ESP SoftAP provisioning manager).
 */

#include "wifi_provision.h"
#include "wifi_manager.h"
#include "storage.h"
#include "keystore.h"       /* keystore_wipe — PSKs must not linger in .bss */

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"

#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"
#include "qrcode.h"

#include <string.h>
#include <stdio.h>

static const char *TAG = "wifi_prov";

static volatile int s_state = WIFI_PROV_ST_IDLE;
static char s_service[32] = "";
static char s_pop[16]     = "";
static char s_qr[192]     = "";
static char s_got_ssid[33] = "";
static char s_got_pass[65] = "";
static esp_netif_t *s_ap_netif = NULL;   /* SoftAP netif, destroyed on stop */
static bool s_handler   = false;         /* prov event handler registered   */

/* Bit-packed QR module matrix (RAM-frugal: ~253 B vs a byte-per-module). */
#define QR_MAX 45
static uint8_t s_qr_mod[(QR_MAX * QR_MAX + 7) / 8];
static int     s_qr_size = 0;

static inline void qr_setbit(int i)  { s_qr_mod[i >> 3] |= (uint8_t)(1u << (i & 7)); }
static inline bool qr_getbit(int i)  { return (s_qr_mod[i >> 3] >> (i & 7)) & 1u; }

static void qr_display_cb(esp_qrcode_handle_t qr)
{
    int sz = esp_qrcode_get_size(qr);
    if (sz <= 0 || sz > QR_MAX) { s_qr_size = 0; return; }
    memset(s_qr_mod, 0, sizeof(s_qr_mod));
    for (int y = 0; y < sz; y++)
        for (int x = 0; x < sz; x++)
            if (esp_qrcode_get_module(qr, x, y)) qr_setbit(y * QR_MAX + x);
    s_qr_size = sz;
}

static void generate_qr(const char *text)
{
    s_qr_size = 0;
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func       = qr_display_cb;   /* copy modules, don't print   */
    cfg.max_qrcode_version = 6;               /* our payload -> ~v4 (33 mod) */
    cfg.qrcode_ecc_level   = ESP_QRCODE_ECC_LOW;
    esp_err_t e = esp_qrcode_generate(&cfg, text);
    if (e != ESP_OK)
        ESP_LOGW(TAG, "QR generate failed: %s", esp_err_to_name(e));
    else
        ESP_LOGI(TAG, "QR generated: %d modules", s_qr_size);
}

int wifi_provision_qr_size(void) { return s_qr_size; }

bool wifi_provision_qr_module(int x, int y)
{
    if (x < 0 || y < 0 || x >= s_qr_size || y >= s_qr_size) return false;
    return qr_getbit(y * QR_MAX + x);
}

/* Merge the freshly provisioned network into wifi.ini as the preferred (first)
 * profile, dropping any prior entry with the same SSID. Runs on the event
 * task; storage writes are atomic. */
static void save_creds(void)
{
    wifi_profile_t old[STORAGE_WIFI_MAX];
    int n = 0;
    storage_wifi_load(old, &n, STORAGE_WIFI_MAX);

    wifi_profile_t merged[STORAGE_WIFI_MAX];
    int w = 0;
    snprintf(merged[w].ssid,     sizeof(merged[w].ssid),     "%s", s_got_ssid);
    snprintf(merged[w].password, sizeof(merged[w].password), "%s", s_got_pass);
    w++;
    for (int i = 0; i < n && w < STORAGE_WIFI_MAX; i++) {
        if (strcmp(old[i].ssid, s_got_ssid) == 0) continue;   /* replace dup */
        merged[w++] = old[i];
    }
    if (storage_wifi_save(merged, w) == ESP_OK)
        ESP_LOGI(TAG, "Saved '%s' to wifi.ini (%d profile(s))", s_got_ssid, w);
    else
        ESP_LOGE(TAG, "Failed to save wifi.ini");

    /* Both arrays hold every stored PSK in the clear on the event task's
     * stack, and s_got_pass would otherwise sit in .bss for the rest of the
     * boot. storage_wifi_save() has taken its copy — scrub all three. */
    keystore_wipe(old,    sizeof(old));
    keystore_wipe(merged, sizeof(merged));
    keystore_wipe(s_got_pass, sizeof(s_got_pass));
}

static void prov_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base != WIFI_PROV_EVENT) return;
    switch (id) {
    case WIFI_PROV_START:
        s_state = WIFI_PROV_ST_ACTIVE;
        break;
    case WIFI_PROV_CRED_RECV: {
        const wifi_sta_config_t *c = (const wifi_sta_config_t *)data;
        snprintf(s_got_ssid, sizeof(s_got_ssid), "%s", (const char *)c->ssid);
        snprintf(s_got_pass, sizeof(s_got_pass), "%s", (const char *)c->password);
        s_state = WIFI_PROV_ST_RECEIVED;
        ESP_LOGI(TAG, "Credentials received for '%s'", s_got_ssid);
        break;
    }
    case WIFI_PROV_CRED_FAIL:
        ESP_LOGW(TAG, "Provisioning failed (bad password or AP not found)");
        s_state = WIFI_PROV_ST_FAILED;
        break;
    case WIFI_PROV_CRED_SUCCESS:
        save_creds();
        s_state = WIFI_PROV_ST_SUCCESS;
        break;
    case WIFI_PROV_END:
        ESP_LOGI(TAG, "Provisioning service ended");
        break;
    default:
        break;
    }
}

esp_err_t wifi_provision_start(void)
{
    if (s_state != WIFI_PROV_ST_IDLE) return ESP_ERR_INVALID_STATE;

    /* Ensure the driver is up and park wifi_manager so its retry loop won't
     * fight the provisioning manager over the STA interface. */
    if (wifi_manager_init() != ESP_OK) return ESP_FAIL;
    wifi_manager_disconnect();

    /* SoftAP scheme needs an AP netif; create it for the session. */
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();

    if (!s_handler) {
        esp_err_t e = esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                                 &prov_event_handler, NULL);
        if (e != ESP_OK) return e;
        s_handler = true;
    }

    wifi_prov_mgr_config_t config = {
        .scheme               = wifi_prov_scheme_softap,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    esp_err_t err = wifi_prov_mgr_init(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "prov_mgr_init: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_service, sizeof(s_service), "CYBERDECK-%02X%02X", mac[4], mac[5]);
    snprintf(s_pop,     sizeof(s_pop),     "%02X%02X%02X%02X",
             mac[0], mac[3], mac[4], mac[5]);
    s_got_ssid[0] = '\0';

    s_state = WIFI_PROV_ST_ACTIVE;   /* publish before the async START event */
    err = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1, s_pop,
                                           s_service, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start_provisioning: %s", esp_err_to_name(err));
        wifi_prov_mgr_deinit();
        s_state = WIFI_PROV_ST_IDLE;
        return err;
    }

    snprintf(s_qr, sizeof(s_qr),
             "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"softap\"}",
             s_service, s_pop);
    generate_qr(s_qr);
    ESP_LOGI(TAG, "SoftAP provisioning up: join '%s', PoP '%s'", s_service, s_pop);
    return ESP_OK;
}

void wifi_provision_stop(void)
{
    if (s_state == WIFI_PROV_ST_IDLE) return;

    /* deinit stops the service if still running, and frees the httpd/protocomm
     * buffers — the internal RAM we care about. */
    wifi_prov_mgr_deinit();
    if (s_handler) {
        esp_event_handler_unregister(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                     &prov_event_handler);
        s_handler = false;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);   /* drop the AP radio, back to plain STA */

    /* Destroy the AP netif — otherwise it lingers as 192.168.4.1 and lwip keeps
     * routing/DNS through it, so outbound TCP (SSH) times out until a reboot.
     * Then re-assert the STA netif as the default route. */
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) esp_netif_set_default_netif(sta);

    s_state   = WIFI_PROV_ST_IDLE;
    s_qr_size = 0;
    s_qr[0] = s_service[0] = s_pop[0] = '\0';
    ESP_LOGI(TAG, "Provisioning stopped, AP netif removed");
}

int         wifi_provision_state(void)         { return s_state; }
const char *wifi_provision_service_name(void)  { return s_service; }
const char *wifi_provision_pop(void)           { return s_pop; }
const char *wifi_provision_qr_payload(void)    { return s_qr; }
const char *wifi_provision_ssid(void)          { return s_got_ssid; }
