/*
 * BLE HID keyboard backend — 5-state machine (NimBLE)
 */

/* This #include must precede the guard below. The preprocessor evaluates
 * the #if before any other include. Without sdkconfig.h, CONFIG_INPUT_*
 * stays undefined and the backend silently compiles out to the stub. */
#include "sdkconfig.h"

#if defined(CONFIG_INPUT_BLE) || defined(CONFIG_INPUT_AUTO)

#include "input_hal_internal.h"
#include "ble_keyboard.h"
#include "hid_keymap.h"
#include "storage.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_hidh.h"
#include "esp_timer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "ble_presence.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "host/util/util.h"          /* ble_hs_util_ensure_addr */
#include "store/config/ble_store_config.h"

#include <stdlib.h>
#include <string.h>

static const char *TAG = "ble_kbd";

static volatile ble_state_t s_state          = BLE_IDLE;
static volatile bool        s_nimble_synced  = false;

/* Consecutive failed opens drive exponential backoff (1s..30s): a persistent
 * failure must not become a battery-burning 1 Hz scan->connect->fail loop. */
static volatile uint8_t s_open_fail_count;

/* Protects s_scan_results / s_scan_count between NimBLE task and app task */
static portMUX_TYPE s_scan_mux = portMUX_INITIALIZER_UNLOCKED;

/* Registry — addresses we auto-reconnect to on boot */
static ble_device_info_t s_registry[STORAGE_BLE_MAX];
static int               s_registry_count = 0;

/* Scan results accumulate during BLE_PAIRING_SCAN. This buffer is larger
 * than the storage registry. A noisy RF environment yields many
 * advertisers, so keep headroom to catch the keyboard among them. */
#define SCAN_MAX 24
static ble_device_info_t s_scan_results[SCAN_MAX];
static uint8_t           s_scan_hid[SCAN_MAX];   /* 1 = looks like a HID device */
static int               s_scan_count = 0;

/* BDA of connected device (NimBLE little-endian: val[0] = LSB) */
static uint8_t s_connected_bda[6];
static esp_hidh_dev_t * volatile s_connected_dev = NULL;   /* also read by the keeper timer */
static char    s_connected_name[64] = "";   /* name of the live device, "" = none */

/* Previous HID report keys — for rollover diffing (emit only new keys) */
static uint8_t s_prev_keys[6];

/* Deck-tracked lock toggles: with a boot-protocol keyboard the HOST owns
 * caps/num state, and the deck is the host. Reset per connection — a
 * fresh keyboard's locks are unknowable, so assume off. */
#define HID_KC_CAPSLOCK  0x39
#define HID_KC_NUMLOCK   0x53
static uint8_t s_locks;

uint8_t ble_keyboard_get_locks(void) { return s_locks; }

/* Typematic auto-repeat: HID keyboards report state changes only, so the host
 * (us) must repeat a held key. The most-recently-pressed key repeats. */
#define KBD_REPEAT_DELAY_US   (400 * 1000)   /* hold time before repeat kicks in */
#define KBD_REPEAT_PERIOD_US  ( 40 * 1000)   /* ~25 chars/sec while held          */
static esp_timer_handle_t s_repeat_timer = NULL;
static uint8_t s_repeat_kc = 0;              /* repeating keycode (0 = none) */
static input_event_t s_repeat_ev;            /* the event to re-post */

/* Re-emit the held key, then re-arm at the repeat cadence. */
static void repeat_timer_cb(void *arg)
{
    (void)arg;
    if (s_repeat_kc == 0) return;
    input_hal_post_event(&s_repeat_ev);
    esp_timer_start_once(s_repeat_timer, KBD_REPEAT_PERIOD_US);
}

/* Start repeating @p kc (its posted event in @p ev) after the initial delay. */
static void repeat_arm(uint8_t kc, const input_event_t *ev)
{
    if (!s_repeat_timer) {
        const esp_timer_create_args_t a = {
            .callback = repeat_timer_cb, .name = "kbd_rpt",
        };
        if (esp_timer_create(&a, &s_repeat_timer) != ESP_OK) return;
    }
    esp_timer_stop(s_repeat_timer);           /* harmless if not running */
    s_repeat_kc = kc;
    s_repeat_ev = *ev;
    esp_timer_start_once(s_repeat_timer, KBD_REPEAT_DELAY_US);
}

static void repeat_stop(void)
{
    if (s_repeat_timer) esp_timer_stop(s_repeat_timer);
    s_repeat_kc = 0;
}

/* Global GAP event listener — drives link encryption/bonding, which esp_hidh's
 * NimBLE backend does not do on its own. */
static struct ble_gap_event_listener s_gap_listener;

/* Link keeper -- see keeper_cb(). Every "keyboard never comes back until a
 * reboot" bug looked the same from outside. Some RAM state left the
 * machine stuck. A scan could fail to start, an open could never return,
 * or a device object could vanish underneath it. Nothing re-drove the
 * state machine after that. This timer re-asserts the invariant "not
 * connected => a scan is running". */
#define KEEPER_PERIOD_US    (5 * 1000 * 1000)
#define OPEN_STUCK_US       (75ULL * 1000 * 1000)   /* 30 s connect + discovery */
static esp_timer_handle_t s_keeper_timer = NULL;
static volatile int64_t   s_connecting_since_us = 0;
static uint8_t            s_no_scan_ticks = 0;   /* consecutive "no scan" samples */

static int gap_event_cb(struct ble_gap_event *event, void *arg);
static void start_scan(int32_t duration_ms);

/* Provided by the NimBLE config store (store/config/src/ble_store_config.c)
 * but not exposed in any public header in ESP-IDF — declared manually, the
 * same way esp-idf's own examples/tests do. */
extern void ble_store_config_init(void);

/* NimBLE stores addresses little-endian; reverse for human-readable log */
static void log_addr(const char *prefix, const uint8_t addr[6])
{
    ESP_LOGI(TAG, "%s %02X:%02X:%02X:%02X:%02X:%02X",
             prefix,
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static bool addr_in_registry(const uint8_t addr[6])
{
    for (int i = 0; i < s_registry_count; i++) {
        if (memcmp(s_registry[i].addr, addr, 6) == 0) return true;
    }
    return false;
}

/* AD structure parsers — raw adv data, stack-agnostic */
static bool ad_has_hid_uuid(const uint8_t *adv_data, uint8_t adv_len)
{
    const uint8_t *p   = adv_data;
    const uint8_t *end = adv_data + adv_len;
    while (p < end) {
        uint8_t len  = p[0];
        if (len == 0 || p + 1 + len > end) break;
        uint8_t type = p[1];
        if (type == 0x02 || type == 0x03) {
            const uint8_t *uuids = p + 2;
            uint8_t count = (len - 1) / 2;
            for (uint8_t i = 0; i < count; i++) {
                uint16_t uuid = (uint16_t)(uuids[2*i]) |
                                ((uint16_t)(uuids[2*i+1]) << 8);
                if (uuid == 0x1812) return true;
            }
        }
        p += 1 + len;
    }
    return false;
}

static void ad_get_name(const uint8_t *adv, uint8_t adv_len,
                        char *dst, size_t dstsz)
{
    const uint8_t *p   = adv;
    const uint8_t *end = adv + adv_len;
    while (p < end) {
        uint8_t len  = p[0];
        if (len == 0 || p + 1 + len > end) break;
        uint8_t type = p[1];
        if (type == 0x08 || type == 0x09) {   /* Short / Complete Local Name */
            size_t nlen = (size_t)(len - 1);
            if (nlen >= dstsz) nlen = dstsz - 1;
            memcpy(dst, p + 2, nlen);
            dst[nlen] = '\0';
            return;
        }
        p += 1 + len;
    }
    snprintf(dst, dstsz, "Unknown");
}

/* AD type 0x19 Appearance (2 bytes LE). HID devices are appearance
 * category 15 (0x03Cx: 0x03C1 keyboard, 0x03C2 mouse, ...). Many keyboards
 * advertise this even when they omit the 0x1812 service UUID. */
static bool ad_is_hid_appearance(const uint8_t *adv, uint8_t adv_len)
{
    const uint8_t *p   = adv;
    const uint8_t *end = adv + adv_len;
    while (p < end) {
        uint8_t len = p[0];
        if (len == 0 || p + 1 + len > end) break;
        if (p[1] == 0x19 && len >= 3) {
            uint16_t ap = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
            return (ap >> 6) == 0x0F;   /* HID category */
        }
        p += 1 + len;
    }
    return false;
}

ble_state_t ble_keyboard_get_state(void) { return s_state; }

const char *ble_keyboard_get_connected_name(void) { return s_connected_name; }

int ble_keyboard_get_scan_results(ble_device_info_t *out, int max)
{
    int n = 0;
    taskENTER_CRITICAL(&s_scan_mux);
    /* If any device looks like a HID (it advertises the HID appearance or
     * service UUID), show only those. In a noisy environment, named non-HID
     * advertisers (lights, speakers, scales) would otherwise bury the
     * keyboard. Fall back to all named devices only when none look like a
     * HID. This keeps an unusual keyboard reachable even if it advertises
     * neither marker. */
    bool any_hid = false;
    for (int i = 0; i < s_scan_count; i++)
        if (s_scan_hid[i]) { any_hid = true; break; }
    for (int i = 0; i < s_scan_count && n < max; i++)
        if (!any_hid || s_scan_hid[i]) out[n++] = s_scan_results[i];
    taskEXIT_CRITICAL(&s_scan_mux);

    /* Label unnamed devices by address so the picker shows something. */
    for (int i = 0; i < n; i++) {
        if (out[i].name[0] == '\0')
            snprintf(out[i].name, sizeof(out[i].name),
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     out[i].addr[5], out[i].addr[4], out[i].addr[3],
                     out[i].addr[2], out[i].addr[1], out[i].addr[0]);
    }
    return n;
}

void ble_keyboard_enter_pairing(void)
{
    if (!s_nimble_synced) {
        ESP_LOGW(TAG, "enter_pairing: NimBLE not yet synced");
        return;
    }
    if (s_state == BLE_CONNECTING) {
        /* An open is in flight on hid_open_task; scanning now would fight
         * it for the single GAP procedure slot and the state machine.
         * The UI keeps showing "connecting..." — retry the menu after. */
        ESP_LOGW(TAG, "enter_pairing: connect in flight, try again shortly");
        return;
    }
    s_open_fail_count = 0;             /* fresh user action, fresh backoff */
    ESP_LOGI(TAG, "Entering pairing mode");
    ble_gap_disc_cancel();
    taskENTER_CRITICAL(&s_scan_mux);
    s_scan_count = 0;
    memset(s_scan_hid, 0, sizeof(s_scan_hid));
    taskEXIT_CRITICAL(&s_scan_mux);
    s_state = BLE_PAIRING_SCAN;
    start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
}

void ble_keyboard_reconnect_start(void)
{
    if (s_state == BLE_CONNECTED) return;
    if (s_registry_count == 0)   return;
    if (!s_nimble_synced)        return;   /* on_sync will start scan */
    ESP_LOGI(TAG, "Reconnect scan for %d known device(s)", s_registry_count);
    s_state = BLE_RECONNECT;
    start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
}

void ble_keyboard_exit_pairing(void)
{
    if (s_state != BLE_PAIRING_SCAN) return;
    ESP_LOGI(TAG, "Leaving pairing mode");
    ble_gap_disc_cancel();
    if (s_registry_count > 0) {
        /* Resume looking for a known keyboard. */
        s_state = BLE_RECONNECT;
        start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
    } else {
        s_state = BLE_IDLE;
    }
}

/* esp_hidh_dev_open() blocks the caller until the GATT connection
 * completes (up to 30 s). It then runs service discovery on the same
 * task. It must not run on the NimBLE host task. That task dispatches
 * the callback it waits for, so calling from there would deadlock. It
 * must not run on the UI/main task either, since that freezes input and
 * can overflow the stack. A dedicated short-lived task runs it instead,
 * matching esp-idf's own esp_hid_host example. */
typedef struct { uint8_t addr[6]; uint8_t addr_type; } hid_open_req_t;

#define HID_OPEN_STACK 5120
static StaticTask_t  s_hid_open_tcb;
static StackType_t  *s_hid_open_stack;   /* PSRAM; no flash writes here */
static volatile bool s_hid_open_live;

static void hid_open_task(void *arg)
{
    hid_open_req_t req = *(hid_open_req_t *)arg;
    free(arg);
    log_addr("Connecting to", req.addr);
    esp_hidh_dev_t *dev = esp_hidh_dev_open(req.addr, ESP_HID_TRANSPORT_BLE,
                                            req.addr_type);
    if (!dev) {
        /* The NimBLE backend posts no OPEN event on failure. Recover here so
         * the state machine does not stay wedged in BLE_CONNECTING forever.
         * Back off before rescanning: 1s, 2s, 4s ... capped at 30s. */
        if (s_open_fail_count < 8) s_open_fail_count++;
        uint32_t backoff_ms = 1000u << (s_open_fail_count - 1);
        if (backoff_ms > 30000u) backoff_ms = 30000u;
        ESP_LOGW(TAG, "connect failed (streak %u); rescan in %lu ms",
                 (unsigned)s_open_fail_count, (unsigned long)backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        s_state = (s_registry_count > 0) ? BLE_RECONNECT : BLE_IDLE;
        if (s_state == BLE_RECONNECT)
            start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
    }
    s_hid_open_live = false;
    vTaskDelete(NULL);
}

static void open_device_async(const uint8_t addr[6], uint8_t addr_type)
{
    /* Both guards used to bail silently. When an open wedged (it could block
     * forever inside esp_hidh), that silence was the whole symptom. The
     * picker and the reconnect scan both looked alive and did nothing. */
    if (s_state == BLE_CONNECTING) {
        ESP_LOGW(TAG, "open declined: another connect is in flight");
        return;
    }
    if (s_hid_open_live) {
        ESP_LOGW(TAG, "open declined: previous open task still running");
        return;
    }

    /* All fallible steps BEFORE committing state — an early return must
     * never leave the machine stuck in BLE_CONNECTING with no task alive. */
    hid_open_req_t *req = malloc(sizeof(*req));
    if (!req) { ESP_LOGE(TAG, "open req alloc failed"); return; }
    memcpy(req->addr, addr, 6);
    req->addr_type = addr_type;

    /* Static task with a PSRAM stack. The old 5 KB internal-heap alloc
     * failed exactly when internal DRAM was tightest: connect time, with
     * NimBLE buffers in flight. That was one cause of "keyboard just stops
     * connecting". */
    if (!s_hid_open_stack)
        s_hid_open_stack = heap_caps_malloc(HID_OPEN_STACK,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_hid_open_stack) {
        ESP_LOGE(TAG, "hid_open stack alloc failed");
        free(req);
        return;
    }

    ble_gap_disc_cancel();
    s_state               = BLE_CONNECTING;
    s_connecting_since_us = esp_timer_get_time();
    s_hid_open_live       = true;
    xTaskCreateStatic(hid_open_task, "hid_open",
                      HID_OPEN_STACK / sizeof(StackType_t),
                      req, 6, s_hid_open_stack, &s_hid_open_tcb);
}

void ble_keyboard_select_device(const uint8_t addr[6], uint8_t addr_type)
{
    /* Clear any leftover LTK only for a never-bonded device so pairing
     * starts clean. Keep the bond for a device already in the registry.
     * ENC_CHANGE drops a stale key later and lets the next try re-pair. */
    if (!addr_in_registry(addr)) {
        ble_addr_t peer = { .type = addr_type };
        memcpy(peer.val, addr, 6);
        ble_store_util_delete_peer(&peer);
    }

    open_device_async(addr, addr_type);
}

void ble_keyboard_forget_device(const uint8_t addr[6])
{
    /* Find the stored addr_type before removing the registry record. */
    uint8_t addr_type = 0;
    for (int i = 0; i < s_registry_count; i++) {
        if (memcmp(s_registry[i].addr, addr, 6) == 0) {
            addr_type = s_registry[i].addr_type;
            break;
        }
    }

    storage_ble_remove(addr);
    storage_ble_list(s_registry, STORAGE_BLE_MAX, &s_registry_count);

    /* Delete the NimBLE bond to free the slot (MAX_BONDS=3) so a re-pair
     * starts from scratch. */
    ble_addr_t peer = { .type = addr_type };
    memcpy(peer.val, addr, 6);
    ble_store_util_delete_peer(&peer);

    if (s_state == BLE_CONNECTED && memcmp(s_connected_bda, addr, 6) == 0 &&
        s_connected_dev) {
        esp_hidh_dev_close(s_connected_dev);
        /* CLOSE_EVT will drive the state machine from here. */
    }
}

void ble_keyboard_forget_all(void)
{
    ESP_LOGW(TAG, "Forgetting ALL bonded devices");
    ble_gap_disc_cancel();
    if (s_connected_dev) {
        esp_hidh_dev_close(s_connected_dev);
        s_connected_dev = NULL;
    }
    s_connected_name[0] = '\0';

    /* Wipe every NimBLE bond (NVS) and our own registry — the clean recovery
     * from a corrupt/stale bond store. Next pairing starts from scratch. */
    ble_store_clear();
    storage_ble_clear();
    storage_ble_list(s_registry, STORAGE_BLE_MAX, &s_registry_count);   /* -> 0 */
    s_state = BLE_IDLE;
}

static void start_scan(int32_t duration_ms)
{
    struct ble_gap_disc_params p = {
        .itvl              = 0x50,
        .window            = 0x30,
        .filter_policy     = BLE_HCI_SCAN_FILT_NO_WL,
        .limited           = 0,
        .passive           = 0,   /* active scan — fetch device names */
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, duration_ms, &p,
                          gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc failed: %d", rc);
    } else if (s_state == BLE_PAIRING_SCAN) {
        ESP_LOGI(TAG, "pairing scan started (rc=%d, %ldms)",
                 rc, (long)duration_ms);
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_gap_disc_desc *d = &event->disc;

        /* Phone-presence rides this scan whenever it is the one running
         * (keyboard disconnected); ble_presence runs its own passive scan
         * only while we hold a connection. */
        ble_presence_on_disc(d->addr.val, d->addr.type, d->rssi);

        if (s_state == BLE_RECONNECT) {
            /* Registry membership alone identifies a bonded keyboard — do NOT
             * also require a HID UUID in the advert; many keyboards omit
             * 0x1812 from their ADV payload and would otherwise never
             * reconnect. Registry holds IDENTITY addresses; keyboards using a
             * static-random or public address match directly, ones that rotate
             * RPAs need host-based privacy (BT_NIMBLE_HOST_BASED_PRIVACY). */
            if (addr_in_registry(d->addr.val)) {
                log_addr("Known device found, connecting", d->addr.val);
                /* Must defer this call. The NimBLE host task runs this
                 * handler, and esp_hidh_dev_open() would block it waiting
                 * for a callback dispatched on this task. */
                open_device_async(d->addr.val, d->addr.type);
            }
            break;
        }

        if (s_state != BLE_PAIRING_SCAN) break;

        /* Pairing scan. A HID service UUID, a HID appearance, or (failing
         * both) a name marks a keyboard. Real keyboards rarely advertise the
         * 0x1812 UUID before connect, so requiring it would hide them. The
         * name and the HID markers can land in separate adv/scan-response
         * events. Merge per address across events. */
        char name[64] = {0};
        ad_get_name(d->data, d->length_data, name, sizeof(name));
        if (strcmp(name, "Unknown") == 0) name[0] = '\0';
        bool has_name = (name[0] != '\0');
        bool hid = ad_has_hid_uuid(d->data, d->length_data) ||
                   ad_is_hid_appearance(d->data, d->length_data);

        if (!hid && !has_name) break;   /* skip unnamed non-HID beacons */

        bool changed = false;
        taskENTER_CRITICAL(&s_scan_mux);
        int idx = -1;
        for (int i = 0; i < s_scan_count; i++) {
            if (memcmp(s_scan_results[i].addr, d->addr.val, 6) == 0) {
                idx = i; break;
            }
        }
        if (idx < 0 && s_scan_count < SCAN_MAX) {
            idx = s_scan_count++;
            memset(&s_scan_results[idx], 0, sizeof(s_scan_results[idx]));
            memcpy(s_scan_results[idx].addr, d->addr.val, 6);
            s_scan_results[idx].addr_type = d->addr.type;
            s_scan_hid[idx] = 0;
            changed = true;
        }
        if (idx >= 0) {
            if (has_name && strcmp(s_scan_results[idx].name, name) != 0) {
                snprintf(s_scan_results[idx].name,
                         sizeof(s_scan_results[idx].name), "%s", name);
                changed = true;
            }
            if (hid && !s_scan_hid[idx]) { s_scan_hid[idx] = 1; changed = true; }
        }
        taskEXIT_CRITICAL(&s_scan_mux);

        if (changed) {
            ESP_LOGI(TAG,
                "scan %02X:%02X:%02X:%02X:%02X:%02X type=%u hid=%d name='%s'",
                d->addr.val[5], d->addr.val[4], d->addr.val[3],
                d->addr.val[2], d->addr.val[1], d->addr.val[0],
                d->addr.type, (int)hid, name);
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "Scan complete (state=%d)", (int)s_state);
        if (s_state == BLE_RECONNECT || s_state == BLE_PAIRING_SCAN) {
            start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
        }
        break;

    /* Connection-oriented events (ENC_CHANGE, REPEAT_PAIRING) do not arrive
     * here. ble_gap_disc registers this callback for discovery events only.
     * esp_hidh owns the connection callback. ble_keyboard_forget_device
     * deletes the bond for stale-bond recovery (see also NimBLE's
     * repeat-pairing Kconfig). */

    default:
        break;
    }
    return 0;
}

static void hidh_callback(void *handler_args, esp_event_base_t base,
                          int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidh_event_t       event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *data  = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT: {
        if (data->open.status != ESP_OK) {
            ESP_LOGW(TAG, "HIDH open failed (status=0x%x), returning to reconnect",
                     data->open.status);
            s_state = BLE_RECONNECT;
            start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
            break;
        }
        /* A link that drops the instant it comes up can tear down the dev
         * between the post and this handler. Touching it then is a
         * use-after-free. Believing it leaves the FSM CONNECTED to a device
         * that no longer exists. That was one of the states only a reboot
         * used to clear. */
        if (!esp_hidh_dev_exists(data->open.dev)) {
            ESP_LOGW(TAG, "HIDH open: device already gone, returning to reconnect");
            s_state = BLE_RECONNECT;
            start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
            break;
        }
        const uint8_t *bda = esp_hidh_dev_bda_get(data->open.dev);
        if (!bda) {
            ESP_LOGE(TAG, "HIDH open: bda_get returned NULL");
            s_state = BLE_RECONNECT;
            start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
            break;
        }
        s_open_fail_count = 0;         /* successful open resets the backoff */
        log_addr("Connected:", bda);
        memcpy(s_connected_bda, bda, 6);
        s_connected_dev = data->open.dev;
        memset(s_prev_keys, 0, sizeof(s_prev_keys));
        s_locks = 0;
        repeat_stop();
        s_state = BLE_CONNECTED;

        /* Save/update the registry record. Prefer the peer's IDENTITY
         * address from the connection descriptor; it survives RPA rotation.
         * Take name/addr_type from the pairing scan buffer, or from the
         * existing record otherwise. Boot-time reconnect has no scan
         * buffer, so skipping the existing-record fallback would overwrite
         * the stored name with "Unknown HID". */
        ble_device_info_t dev = {0};
        memcpy(dev.addr, bda, 6);

        struct ble_gap_conn_desc desc;
        ble_addr_t search = { .type = BLE_ADDR_PUBLIC };
        memcpy(search.val, bda, 6);
        bool have_desc = false;
        for (uint8_t t = 0; t <= 1 && !have_desc; t++) {
            search.type = t;
            have_desc = (ble_gap_conn_find_by_addr(&search, &desc) == 0);
        }
        if (have_desc) {
            memcpy(dev.addr, desc.peer_id_addr.val, 6);
            dev.addr_type = desc.peer_id_addr.type;
        }

        /* Name priority: live GATT Device Name first, then the pairing-scan
         * ADV name, then the stored registry name, then a placeholder.
         * GATT Device Name is authoritative and the only source a
         * boot-time reconnect has. It also heals registry records that
         * older firmware poisoned with "Unknown HID". */
        const char *gatt_name = esp_hidh_dev_name_get(data->open.dev);
        if (gatt_name && gatt_name[0])
            snprintf(dev.name, sizeof(dev.name), "%s", gatt_name);

        bool found = false;
        for (int i = 0; i < s_scan_count && !found; i++) {
            if (memcmp(s_scan_results[i].addr, bda, 6) == 0) {
                if (!have_desc) dev.addr_type = s_scan_results[i].addr_type;
                if (dev.name[0] == '\0')
                    memcpy(dev.name, s_scan_results[i].name, sizeof(dev.name));
                found = true;
            }
        }
        for (int i = 0; i < s_registry_count && !found; i++) {
            if (memcmp(s_registry[i].addr, dev.addr, 6) == 0) {
                if (dev.name[0] == '\0')
                    memcpy(dev.name, s_registry[i].name, sizeof(dev.name));
                if (!have_desc) dev.addr_type = s_registry[i].addr_type;
                found = true;
            }
        }
        if (dev.name[0] == '\0')
            snprintf(dev.name, sizeof(dev.name), "Unknown HID");
        snprintf(s_connected_name, sizeof(s_connected_name), "%s", dev.name);
        storage_ble_save(&dev);
        storage_ble_list(s_registry, STORAGE_BLE_MAX, &s_registry_count);
        break;
    }

    case ESP_HIDH_CLOSE_EVENT:
        ESP_LOGI(TAG, "Keyboard disconnected, restarting reconnect scan");
        s_connected_dev = NULL;
        s_connected_name[0] = '\0';
        memset(s_prev_keys, 0, sizeof(s_prev_keys));
        s_locks = 0;
        repeat_stop();
        s_state = (s_registry_count > 0) ? BLE_RECONNECT : BLE_IDLE;
        ble_gap_disc_cancel();
        if (s_state == BLE_RECONNECT)
            start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
        break;

    case ESP_HIDH_INPUT_EVENT: {
        if (data->input.usage != ESP_HID_USAGE_KEYBOARD) break;
        if (data->input.length < 3) break;

        const uint8_t *report    = data->input.data;
        uint8_t        modifiers = report[0];
        uint8_t n = (data->input.length < 8) ? (uint8_t)data->input.length : 8u;

        /* Boot-protocol reports list ALL held keys every time. Emit only
         * keys absent from the previous report, otherwise holding 'a' and
         * pressing 'b' re-sends 'a' (classic rollover duplication). */
        uint8_t new_keys[6] = {0};
        uint8_t nk = 0;

        for (uint8_t i = 2; i < n; i++) {
            uint8_t kc = report[i];
            if (kc == 0x00 || kc == 0x01) continue;   /* 0x01 is the HID ErrorRollOver code */
            if (nk < sizeof(new_keys)) new_keys[nk++] = kc;

            bool held = false;
            for (uint8_t j = 0; j < sizeof(s_prev_keys); j++) {
                if (s_prev_keys[j] == kc) { held = true; break; }
            }
            if (held) continue;

            /* Lock keys toggle deck-side state and never travel. */
            if (kc == HID_KC_CAPSLOCK) { s_locks ^= BLE_KBD_LOCK_CAPS; continue; }
            if (kc == HID_KC_NUMLOCK)  { s_locks ^= BLE_KBD_LOCK_NUM;  continue; }

            input_event_t ev = { 0 };
            uint8_t len = hid_keymap_translate(kc, modifiers,
                                               s_locks & BLE_KBD_LOCK_CAPS,
                                               ev.buf);
            if (len) {
                ev.type = INPUT_EVENT_KEY;
                ev.len  = len;
            } else {
                /* No bytes of its own (arrows, F-keys, nav cluster).
                 * The key crosses the queue as a HID usage. The consumer
                 * encodes it against live terminal state. */
                ev.type = INPUT_EVENT_HIDKEY;
                ev.key  = kc;
                ev.mods = modifiers;
            }
            input_hal_post_event(&ev);

            repeat_arm(kc, &ev);   /* newest key press takes over repeat */
        }

        /* Stop repeating once the report drops the held key. */
        if (s_repeat_kc != 0) {
            bool still_held = false;
            for (uint8_t j = 0; j < nk; j++)
                if (new_keys[j] == s_repeat_kc) { still_held = true; break; }
            if (!still_held) repeat_stop();
        }

        memcpy(s_prev_keys, new_keys, sizeof(s_prev_keys));
        break;
    }

    case ESP_HIDH_BATTERY_EVENT:
        ESP_LOGI(TAG, "Battery: %d%%", data->battery.level);
        break;

    default:
        break;
    }
}

/* esp_hidh's NimBLE backend opens the connection and discovers services,
 * but it never starts link encryption. Most BLE keyboards keep their HID
 * report characteristics behind encryption/bonding. Without that step,
 * reads fail ("insufficient encryption"), no bond forms, and the keyboard
 * sits blinking until it times out. A global GAP listener sees every
 * connection and drives Just-Works bonding to completion. */
static int sec_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(event->connect.conn_handle, &d) == 0)
                ESP_LOGI(TAG, "link up: role=%s handle=%d",
                         d.role == BLE_GAP_ROLE_MASTER ? "master" : "slave",
                         event->connect.conn_handle);

            /* Ask for a fast connection interval so the flurry of GATT
             * discovery finishes well inside the supervision timeout. */
            struct ble_gap_upd_params up = {
                .itvl_min = 6, .itvl_max = 12, .latency = 0,
                .supervision_timeout = 100,
            };
            ble_gap_update_params(event->connect.conn_handle, &up);

            int rc = ble_gap_security_initiate(event->connect.conn_handle);
            ESP_LOGI(TAG, "initiating pairing (rc=%d)", rc);
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change: status=%d", event->enc_change.status);
        if (event->enc_change.status != 0) {
            /* Encryption failed. This is almost always a stale LTK. A reset
             * or a re-pair elsewhere drops the keyboard's side of the bond.
             * Drop our bond so the next reconnect pairs fresh. This avoids
             * looping forever on a key the peer no longer honours. The link
             * then drops, and CLOSE_EVENT restarts the reconnect scan. */
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0) {
                ESP_LOGW(TAG, "encryption failed - dropping stale bond");
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
        }
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The peer forgot this bond and wants to re-pair. Drop our side too;
         * select_device also clears it up front (belt-and-suspenders). */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
            ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        break;
    }
    return 0;
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    s_nimble_synced = true;
    ESP_LOGI(TAG, "NimBLE synced");
    if (s_registry_count > 0) {
        ESP_LOGI(TAG, "Auto-reconnect: %d device(s) in registry",
                 s_registry_count);
        s_state = BLE_RECONNECT;
        start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
    } else {
        ESP_LOGI(TAG, "No paired devices - awaiting pairing trigger");
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset (reason=%d)", reason);
    s_state        = BLE_IDLE;
    s_nimble_synced = false;
}

static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();   /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* Periodic supervisor. This code does nothing when every layer behaves
 * correctly. It exists because the failure mode is always the same when
 * one layer misbehaves. The keyboard never comes back, and only a reboot
 * helps. Cheap insurance:
 *   CONNECTED  : the device object vanished under us  -> resume scanning.
 *   CONNECTING : an open that never returned          -> cancel it.
 *   RECONNECT  : no scan running                      -> restart it.
 *   IDLE       : devices in the registry              -> resume scanning.
 * This runs on the esp_timer task. NimBLE calls are fine there; flash I/O
 * is not. */
static void keeper_cb(void *arg)
{
    (void)arg;
    if (!s_nimble_synced) return;

    switch (s_state) {
    case BLE_CONNECTED:
        if (s_connected_dev && !esp_hidh_dev_exists(s_connected_dev)) {
            ESP_LOGW(TAG, "keeper: connected device vanished - rescanning");
            s_connected_dev     = NULL;
            s_connected_name[0] = '\0';
            repeat_stop();
            s_state = (s_registry_count > 0) ? BLE_RECONNECT : BLE_IDLE;
            if (s_state == BLE_RECONNECT)
                start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
        }
        break;

    case BLE_CONNECTING:
        if (esp_timer_get_time() - s_connecting_since_us > (int64_t)OPEN_STUCK_US) {
            /* An open should finish (or fail) well inside this. Cancelling
             * the GAP procedure makes the stack report the connect as
             * failed. That unblocks the opener task and lets it unwind
             * normally. */
            ESP_LOGE(TAG, "keeper: connect stuck - cancelling");
            ble_gap_conn_cancel();
            s_connecting_since_us = esp_timer_get_time();   /* don't spam */
        }
        break;

    case BLE_RECONNECT:
    case BLE_PAIRING_SCAN:
        if (!s_hid_open_live && !ble_gap_disc_active() && !ble_gap_conn_active()) {
            /* Two strikes: a scan ends, and the same DISC_COMPLETE callback
             * restarts it. This timer samples often enough to land in that
             * gap. That happens about twice a day with a 10 s scan and a
             * 5 s tick. One miss says nothing. Two in a row means nothing
             * is restarting the scan. */
            if (++s_no_scan_ticks >= 2) {
                ESP_LOGW(TAG, "keeper: no scan running in state %d - restarting",
                         (int)s_state);
                start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
                s_no_scan_ticks = 0;
            }
        } else {
            s_no_scan_ticks = 0;
        }
        break;

    case BLE_IDLE:
        if (s_registry_count > 0 && !s_hid_open_live) {
            ESP_LOGW(TAG, "keeper: idle with %d known device(s) - rescanning",
                     s_registry_count);
            s_state = BLE_RECONNECT;
            start_scan(CONFIG_INPUT_BLE_SCAN_DURATION * 1000);
        }
        break;

    default:
        break;
    }
}

esp_err_t ble_keyboard_backend_init(void)
{
    esp_err_t ret;

    storage_ble_list(s_registry, STORAGE_BLE_MAX, &s_registry_count);
    ESP_LOGI(TAG, "Registry: %d known device(s)", s_registry_count);

    /* NimBLE logs every GATT/SMP procedure at INFO; during the burst of
     * service discovery after a connect this floods the UART and steals CPU.
     * Quiet it (the working reference does the same). */
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Persist bonding keys in NVS */
    ble_store_config_init();

    esp_hidh_config_t hidh_cfg = {
        .callback         = hidh_callback,
        /* The OPEN_EVENT handler persists the bond to littlefs
         * (storage_ble_*). A flash write blows the default 4 KB event-task
         * stack the instant a keyboard connects. Give it real headroom. */
        .event_stack_size = 8192,
        .callback_arg     = NULL,
    };
    ret = esp_hidh_init(&hidh_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "hidh_init: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure the security manager and our callbacks AFTER esp_hidh_init:
     * it installs its own (no-op) sync/reset callbacks that would clobber
     * ours. Just-Works bonding + Secure Connections, no IO capability. This
     * ordering matches the reference implementation exactly. */
    ble_hs_cfg.sm_io_cap         = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 0;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sync_cb           = on_sync;
    ble_hs_cfg.reset_cb          = on_reset;

    /* Global GAP listener drives link encryption/bonding (esp_hidh doesn't).
     * Register before the host task starts so it sees the first connect. */
    ble_gap_event_listener_register(&s_gap_listener, sec_gap_event, NULL);

    nimble_port_freertos_init(nimble_host_task);

    const esp_timer_create_args_t keeper_args = {
        .callback = keeper_cb, .name = "ble_keeper",
    };
    if (esp_timer_create(&keeper_args, &s_keeper_timer) == ESP_OK)
        esp_timer_start_periodic(s_keeper_timer, KEEPER_PERIOD_US);
    else
        ESP_LOGW(TAG, "keeper timer unavailable");

    ESP_LOGI(TAG, "BLE keyboard backend initialised (NimBLE)");
    return ESP_OK;
}

#else

#include "input_hal_internal.h"
#include "ble_keyboard.h"
esp_err_t   ble_keyboard_backend_init(void)                            { return ESP_OK; }
ble_state_t ble_keyboard_get_state(void)                               { return BLE_IDLE; }
uint8_t     ble_keyboard_get_locks(void)                               { return 0; }
const char *ble_keyboard_get_connected_name(void)                      { return ""; }
void        ble_keyboard_enter_pairing(void)                           {}
void        ble_keyboard_exit_pairing(void)                            {}
void        ble_keyboard_reconnect_start(void)                         {}
int         ble_keyboard_get_scan_results(ble_device_info_t *o, int m) { (void)o; (void)m; return 0; }
void        ble_keyboard_select_device(const uint8_t a[6], uint8_t t)  { (void)a; (void)t; }
void        ble_keyboard_forget_device(const uint8_t a[6])             { (void)a; }
void        ble_keyboard_forget_all(void)                              {}

#endif
