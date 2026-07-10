/*
 * WiFi manager — device implementation (esp_wifi STA).
 *
 * All state transitions run on the default event-loop task; the app polls
 * wifi_manager_get_state() from its own task. Strings (ip, ssid) are fully
 * written before the state that advertises them is published.
 */

#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include <string.h>

static const char *TAG = "wifi_manager";

#define RETRY_CYCLE_DELAY_US   (5 * 1000 * 1000)   /* pause between cycles */

static bool                      s_driver_up   = false;
static volatile wifi_mgr_state_t s_state       = WIFI_MGR_IDLE;
static volatile bool             s_intentional = false;
static bool                      s_started     = false;  /* esp_wifi_start done */

static wifi_profile_t s_profiles[STORAGE_WIFI_MAX];
static int            s_profile_count = 0;
static int            s_profile_idx   = 0;
static int            s_cycle_fails   = 0;

static char s_ip[16]   = "";
static char s_ssid[33] = "";

static esp_timer_handle_t s_retry_timer = NULL;

/* ---- wall clock: one-shot NTP, system clock deliberately untouched ----
 * Stepping the system clock decades forward mid-session corrupts libssh2's
 * blocking-timeout arithmetic (elapsed = difftime(time(NULL), start)), so
 * we keep the epoch as a private offset against the monotonic esp_timer. */
static volatile int64_t s_ntp_offset_us = 0;   /* epoch_us - esp_timer_us */
static volatile bool    s_ntp_running   = false;

static void ntp_task(void *arg)
{
    (void)arg;
    for (int attempt = 0; attempt < 6 && s_ntp_offset_us == 0; attempt++) {
        if (attempt) vTaskDelay(pdMS_TO_TICKS(5000));

        struct addrinfo hints = { .ai_family   = AF_INET,
                                  .ai_socktype = SOCK_DGRAM };
        struct addrinfo *res = NULL;
        if (getaddrinfo(CONFIG_CYBERDECK_NTP_SERVER, "123",
                        &hints, &res) != 0 || !res)
            continue;

        int sock = socket(res->ai_family, SOCK_DGRAM, IPPROTO_UDP);
        if (sock >= 0) {
            struct timeval tv = { .tv_sec = 3 };
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            uint8_t pkt[48] = { 0x1B };        /* LI=0 VN=3 Mode=3 client */
            if (sendto(sock, pkt, sizeof(pkt), 0,
                       res->ai_addr, res->ai_addrlen) == sizeof(pkt) &&
                recv(sock, pkt, sizeof(pkt), 0) == (int)sizeof(pkt)) {
                /* Transmit timestamp: seconds since 1900 at offset 40. */
                uint32_t secs = ((uint32_t)pkt[40] << 24) |
                                ((uint32_t)pkt[41] << 16) |
                                ((uint32_t)pkt[42] << 8)  | pkt[43];
                if (secs > 2208988800u) {      /* sanity: past 1970 */
                    s_ntp_offset_us =
                        (int64_t)(secs - 2208988800u) * 1000000LL
                        - esp_timer_get_time();
                    ESP_LOGI(TAG, "NTP synced via %s",
                             CONFIG_CYBERDECK_NTP_SERVER);
                }
            }
            close(sock);
        }
        freeaddrinfo(res);
    }
    s_ntp_running = false;
    vTaskDelete(NULL);
}

/* Fire-and-forget; called from the event handler, so only task creation
 * happens here (never a blocking cross-thread call). Stack lives in PSRAM
 * (no flash writes on this task) so it costs zero scarce internal DRAM. */
#define NTP_TASK_STACK 4096
static StaticTask_t  s_ntp_tcb;
static StackType_t  *s_ntp_stack;

static void wifi_manager_kick_ntp(void)
{
    if (s_ntp_offset_us != 0 || s_ntp_running) return;
    if (!s_ntp_stack)
        s_ntp_stack = heap_caps_malloc(NTP_TASK_STACK,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ntp_stack) return;          /* no PSRAM? just skip the clock */
    s_ntp_running = true;
    xTaskCreateStatic(ntp_task, "ntp", NTP_TASK_STACK / sizeof(StackType_t),
                      NULL, 3, s_ntp_stack, &s_ntp_tcb);
}

time_t wifi_manager_time(void)
{
    if (s_ntp_offset_us == 0) return 0;
    return (time_t)((s_ntp_offset_us + esp_timer_get_time()) / 1000000LL);
}

/* Apply s_profiles[s_profile_idx] and (re)connect. Event-loop task ctx. */
static void try_current_profile(void)
{
    const wifi_profile_t *p = &s_profiles[s_profile_idx];

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, p->ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, p->password, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode =
        p->password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable  = true;
    cfg.sta.pmf_cfg.required = false;

    snprintf(s_ssid, sizeof(s_ssid), "%s", p->ssid);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Trying SSID '%s' (%d/%d)",
             p->ssid, s_profile_idx + 1, s_profile_count);
    esp_wifi_connect();
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (s_intentional || s_profile_count == 0) return;
    try_current_profile();
}

/* Advance to the next profile; after a full failed cycle, back off. */
static void advance_profile(void)
{
    s_profile_idx = (s_profile_idx + 1) % s_profile_count;
    s_cycle_fails++;

    if (s_cycle_fails >= s_profile_count) {
        /* Every profile failed once this cycle — slow down */
        s_cycle_fails = 0;
        if (s_state != WIFI_MGR_LOST)
            s_state = WIFI_MGR_FAILED;
        ESP_LOGW(TAG, "All %d profile(s) failed; retrying in %ds",
                 s_profile_count, (int)(RETRY_CYCLE_DELAY_US / 1000000));
        esp_timer_start_once(s_retry_timer, RETRY_CYCLE_DELAY_US);
    } else {
        try_current_profile();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (!s_intentional && s_profile_count > 0)
            try_current_profile();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip[0] = '\0';
        if (s_intentional) {
            s_state = WIFI_MGR_IDLE;
            return;
        }
        if (s_state == WIFI_MGR_CONNECTED) {
            ESP_LOGW(TAG, "Link lost, reconnecting");
            s_state = WIFI_MGR_LOST;
            s_cycle_fails = 0;
            try_current_profile();     /* same AP first */
        } else if (s_profile_count > 0) {
            advance_profile();
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_cycle_fails = 0;
        s_state = WIFI_MGR_CONNECTED;   /* publish after strings are ready */
        ESP_LOGI(TAG, "Connected to '%s', IP %s", s_ssid, s_ip);

        /* First link-up: fetch wall time once, on a task of its own —
         * never a blocking call from inside this event handler. */
        wifi_manager_kick_ntp();
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_driver_up) return ESP_OK;

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) return err;

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    const esp_timer_create_args_t targs = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    s_driver_up = true;
    ESP_LOGI(TAG, "WiFi driver initialized");
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const wifi_profile_t *profiles, int count)
{
    if (!profiles || count <= 0) return ESP_ERR_INVALID_ARG;
    if (count > STORAGE_WIFI_MAX) count = STORAGE_WIFI_MAX;

    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) return err;

    memcpy(s_profiles, profiles, sizeof(wifi_profile_t) * count);
    s_profile_count = count;
    s_profile_idx   = 0;
    s_cycle_fails   = 0;
    s_intentional   = false;
    s_state         = WIFI_MGR_CONNECTING;

    if (!s_started) {
        s_started = true;
        return esp_wifi_start();       /* STA_START event kicks the connect */
    }
    try_current_profile();
    return ESP_OK;
}

esp_err_t wifi_manager_disconnect(void)
{
    s_intentional = true;
    esp_timer_stop(s_retry_timer);
    s_state = WIFI_MGR_IDLE;
    s_ip[0] = '\0';
    return esp_wifi_disconnect();
}

esp_err_t wifi_manager_adopt(const wifi_profile_t *profiles, int count,
                             const char *ssid)
{
    if (!profiles || count <= 0) return ESP_ERR_INVALID_ARG;
    if (count > STORAGE_WIFI_MAX) count = STORAGE_WIFI_MAX;

    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK) return err;

    memcpy(s_profiles, profiles, sizeof(wifi_profile_t) * count);
    s_profile_count = count;
    s_profile_idx   = 0;
    s_cycle_fails   = 0;
    s_intentional   = false;           /* resume auto-reconnect on drops */
    if (ssid && ssid[0]) snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);

    if (s_state == WIFI_MGR_CONNECTED)
        return ESP_OK;                 /* provisioning already connected us */

    /* Not (yet) associated — do a normal connect. */
    s_state = WIFI_MGR_CONNECTING;
    if (!s_started) { s_started = true; return esp_wifi_start(); }
    try_current_profile();
    return ESP_OK;
}

wifi_mgr_state_t wifi_manager_get_state(void) { return s_state; }
bool wifi_manager_is_connected(void) { return s_state == WIFI_MGR_CONNECTED; }
const char *wifi_manager_get_ip(void)   { return s_ip; }
const char *wifi_manager_get_ssid(void) { return s_ssid; }

int wifi_manager_get_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_state == WIFI_MGR_CONNECTED &&
        esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        return ap.rssi;
    return 0;
}
