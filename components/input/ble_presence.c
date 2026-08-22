/*
 * ble_presence.c — phone-proximity sensing over BLE. See ble_presence.h.
 *
 * Two data paths feed the same sighting logic:
 *
 *  - PIGGYBACK: ble_keyboard's GAP handler forwards every discovery event
 *    here (ble_presence_on_disc). Costs nothing extra while the keyboard's
 *    scan runs (keyboard disconnected — its "not connected => scan running"
 *    invariant).
 *  - OWN SCAN: while the keyboard is CONNECTED its scan is stopped, so a
 *    1 Hz keeper timer starts a low-duty PASSIVE scan of our own. When the
 *    keyboard disconnects we cancel ours so its scan can restart; its
 *    keeper retries on the EALREADY it may briefly see (~1 s window).
 *
 * RPA resolution is done in SOFTWARE (the Bluetooth `ah` function: one
 * AES-128 over the advertisement's 24-bit prand, compared against its
 * 24-bit hash) instead of the controller resolving list — full control,
 * no interaction with the keyboard's privacy settings, and hardware AES
 * makes it ~microseconds. The last matched RPA is cached, so the steady
 * state costs one memcmp per advertisement until the phone rotates its
 * address (~15 min).
 *
 * BYTE-ORDER NOTE (the classic trap): NimBLE addresses and the SMP-
 * distributed IRK are little-endian; the spec's ah() is written MSB-first.
 * The IRK is reversed into the AES key and prand/hash are assembled from
 * addr bytes [5..3]/[2..0]. If enrollment succeeds but sightings never
 * resolve, this mapping is the first suspect.
 */

#include "ble_presence.h"

/* Same compile-out convention as ble_keyboard.c: without a BLE input
 * backend there is no NimBLE to link against — the API degrades to inert
 * stubs (never enrolled, never present). */
#include "sdkconfig.h"

#if defined(CONFIG_INPUT_BLE) || defined(CONFIG_INPUT_AUTO)

/* sdkconfig is developer-local (gitignored), so enforce the one setting
 * this module depends on here: enrolling while the keyboard is CONNECTED
 * needs a second concurrent connection. With only one, enroll still works
 * whenever the keyboard link is down. */
#if defined(CONFIG_BT_NIMBLE_MAX_CONNECTIONS) && CONFIG_BT_NIMBLE_MAX_CONNECTIONS < 2
#warning "ble_presence: set BT_NIMBLE_MAX_CONNECTIONS >= 2 to enroll a phone while the keyboard is connected"
#endif

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"

#include "mbedtls/aes.h"

#include "ble_keyboard.h"
#include "storage.h"

static const char *TAG = "ble_presence";

/* Sighting windows. Continuity advertisements run at a couple of Hz on an
 * idle iPhone; PRESENT_MS is generous so a missed burst does not flap. */
#define PRESENT_MS      45000
/* "Near" tier: smoothed RSSI above NEAR_DBM counts as within arm's reach
 * (~1-2 m for a phone on a desk; pockets and bodies cost 10+ dB — calibrate
 * against the live dBm in the P status toast). 6 dB of hysteresis so the
 * chip does not flap at the boundary. */
#define NEAR_DBM        (-68)
#define NEAR_EXIT_DBM   (-74)
#define SCAN_ITVL       0x0140     /* 200 ms  */
#define SCAN_WINDOW     0x0030     /* 30 ms => ~15% duty */

/* ---- enrolled phone (persisted) ---------------------------------------- */

#define PHONE_MAGIC 0x314E4850u    /* "PHN1" */
typedef struct {
    uint32_t magic;
    uint8_t  ident_type;
    uint8_t  ident[6];             /* identity address, NimBLE byte order */
    uint8_t  irk[16];              /* little-endian, as SMP delivered it  */
} phone_rec_t;

static phone_rec_t s_phone;
static bool        s_have_phone;

/* ---- runtime state ------------------------------------------------------ */

static volatile int64_t s_last_seen_us = -1;
static volatile int     s_rssi_ema;          /* x1, coarse */
static uint8_t          s_last_rpa[6];       /* fast path: current rotation */
static bool             s_last_rpa_valid;

static ble_presence_enroll_t s_enroll = BLE_PRESENCE_IDLE;
static uint16_t              s_enroll_conn = BLE_HS_CONN_HANDLE_NONE;

static bool                s_own_scan;       /* we started the current scan */
static esp_timer_handle_t  s_keeper;

/* ---- persistence -------------------------------------------------------- */

static void phone_path(char *buf, size_t n)
{
    snprintf(buf, n, "%s/phone.bin", storage_platform_mount_point());
}

static void phone_save(void)
{
    char p[128];
    phone_path(p, sizeof(p));
    FILE *f = fopen(p, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s", p);
        return;
    }
    fwrite(&s_phone, sizeof(s_phone), 1, f);
    fclose(f);
}

static void phone_load(void)
{
    char p[128];
    phone_path(p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (!f)
        return;
    phone_rec_t r;
    if (fread(&r, sizeof(r), 1, f) == 1 && r.magic == PHONE_MAGIC) {
        s_phone = r;
        s_have_phone = true;
        ESP_LOGI(TAG, "phone enrolled: ident %02X:..:%02X",
                 r.ident[5], r.ident[0]);
    }
    fclose(f);
}

/* ---- RPA resolution ----------------------------------------------------- */

/* ah(IRK, prand): AES-128(k, 0..0 || prand) & 0xFFFFFF, spec byte order —
 * the SMP-little-endian stored IRK reversed into the MSB-first AES key.
 *
 * On this stack this path is a FALLBACK: bonding places the peer IRK in
 * the controller's resolving list, so the enrolled phone's advertisements
 * arrive already resolved (see on_disc below) and its raw RPAs never reach
 * us. The software resolver covers configurations where that is not true
 * (resolving list full or cleared while phone.bin survives). */
static bool rpa_matches(const uint8_t val[6])
{
    uint8_t key[16], pt[16] = { 0 }, ct[16];
    for (int i = 0; i < 16; i++)
        key[i] = s_phone.irk[15 - i];
    pt[13] = val[5];                           /* prand (top bits 01, checked
                                                * by the caller) */
    pt[14] = val[4];
    pt[15] = val[3];

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, 128);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, pt, ct);
    mbedtls_aes_free(&aes);

    return ct[13] == val[2] && ct[14] == val[1] && ct[15] == val[0];
}

void ble_presence_on_disc(const uint8_t val[6], uint8_t addr_type, int rssi)
{
    if (!s_have_phone)
        return;

    bool hit = false;
    /* Identity address in the report — CONFIRMED ON HARDWARE (2026-08-21):
     * the bonded phone's advertisements arrive controller-resolved as the
     * identity with a BLE_ADDR_*_ID type (observed type=2 at ~2 s cadence).
     * Match on the ADDRESS alone: the stored identity type (0/1) never
     * equals the resolved delivery type (2/3), and that mismatch was
     * exactly the first bring-up bug. */
    if (memcmp(val, s_phone.ident, 6) == 0) {
        hit = true;
    } else if ((addr_type == BLE_ADDR_RANDOM ||
                addr_type == BLE_ADDR_RANDOM_ID) &&
               (val[5] & 0xC0) == 0x40) {      /* resolvable private addr */
        if (s_last_rpa_valid && memcmp(val, s_last_rpa, 6) == 0)
            hit = true;                        /* current rotation, cached */
        else if (rpa_matches(val)) {
            memcpy(s_last_rpa, val, 6);
            s_last_rpa_valid = true;
            hit = true;
        }
    }
    if (!hit)
        return;

    s_last_seen_us = esp_timer_get_time();
    s_rssi_ema = s_rssi_ema ? (s_rssi_ema * 3 + rssi) / 4 : rssi;
}

/* ---- public state ------------------------------------------------------- */

bool ble_presence_enrolled(void) { return s_have_phone; }
int  ble_presence_rssi(void)     { return s_rssi_ema;   }

uint32_t ble_presence_age_ms(void)
{
    const int64_t seen = s_last_seen_us;
    if (seen < 0)
        return UINT32_MAX;
    const int64_t age = (esp_timer_get_time() - seen) / 1000;
    return age > UINT32_MAX ? UINT32_MAX : (uint32_t)age;
}

bool ble_presence_present(void)
{
    return s_have_phone && ble_presence_age_ms() < PRESENT_MS;
}

bool ble_presence_near(void)
{
    static bool s_near;                        /* hysteresis memory */
    if (!ble_presence_present()) {
        s_near = false;
    } else if (s_near) {
        s_near = s_rssi_ema >= NEAR_EXIT_DBM;
    } else {
        s_near = s_rssi_ema >= NEAR_DBM;
    }
    return s_near;
}

ble_presence_enroll_t ble_presence_enroll_state(void) { return s_enroll; }

void ble_presence_forget(void)
{
    /* Delete the NimBLE bond too — a stale deck-side bond makes the next
     * pairing attempt fail confusingly once the phone has forgotten us
     * (and it keeps the dead IRK in the controller resolving list). */
    if (s_have_phone) {
        ble_addr_t id = { .type = s_phone.ident_type };
        memcpy(id.val, s_phone.ident, 6);
        ble_store_util_delete_peer(&id);
    }

    s_have_phone = false;
    s_last_rpa_valid = false;
    s_last_seen_us = -1;
    s_rssi_ema = 0;
    memset(&s_phone, 0, sizeof(s_phone));
    char p[128];
    phone_path(p, sizeof(p));
    remove(p);
    ESP_LOGI(TAG, "phone forgotten (identity + bond)");
}

/* ---- own passive scan (keyboard connected) ------------------------------ */

static int presence_scan_cb(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        ble_presence_on_disc(ev->disc.addr.val, ev->disc.addr.type,
                             ev->disc.rssi);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_own_scan = false;                   /* keeper restarts if needed */
        break;
    default:
        break;
    }
    return 0;
}

static void keeper_tick(void *arg)
{
    (void)arg;
    if (!s_have_phone || s_enroll == BLE_PRESENCE_ADVERTISING)
        return;

    const bool kbd_connected = ble_keyboard_get_state() == BLE_CONNECTED;
    if (kbd_connected && !s_own_scan) {
        struct ble_gap_disc_params p = {
            .itvl              = SCAN_ITVL,
            .window            = SCAN_WINDOW,
            .filter_policy     = BLE_HCI_SCAN_FILT_NO_WL,
            .passive           = 1,
            .filter_duplicates = 0,            /* every advert refreshes age */
        };
        int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p,
                              presence_scan_cb, NULL);
        if (rc == 0)
            s_own_scan = true;
        /* EALREADY: someone else's scan is up — piggyback covers us. */
    } else if (!kbd_connected && s_own_scan) {
        /* Hand the radio back: the keyboard's own keeper re-asserts its
         * "disconnected => scanning" invariant and tolerates the ~1 s
         * window where our cancel races its restart. */
        ble_gap_disc_cancel();
        s_own_scan = false;
    }
}

/* ---- enrollment (peripheral advertising + bond) ------------------------- */

static void enroll_advertise(void);

static int enroll_gap_cb(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) {
            enroll_advertise();                /* connect failed: re-adv */
            break;
        }
        s_enroll_conn = ev->connect.conn_handle;
        ESP_LOGI(TAG, "enroll: phone connected, requesting pairing");
        /* Peripheral side: a security request nudges iOS to initiate
         * pairing (Just Works — the deck advertises NO_IO). */
        ble_gap_security_initiate(s_enroll_conn);
        break;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (ev->enc_change.status != 0)
            break;
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(ev->enc_change.conn_handle, &d) != 0)
            break;
        struct ble_store_key_sec   k = { .peer_addr = d.peer_id_addr };
        struct ble_store_value_sec v;
        if (ble_store_read_peer_sec(&k, &v) == 0 && v.irk_present) {
            s_phone.magic      = PHONE_MAGIC;
            s_phone.ident_type = d.peer_id_addr.type;
            memcpy(s_phone.ident, d.peer_id_addr.val, 6);
            memcpy(s_phone.irk, v.irk, 16);
            s_have_phone = true;
            s_last_rpa_valid = false;
            phone_save();
            s_enroll = BLE_PRESENCE_ENROLLED_NOW;
            ESP_LOGI(TAG, "enroll: bonded, IRK stored");
            ble_gap_terminate(ev->enc_change.conn_handle,
                              BLE_ERR_REM_USER_CONN_TERM);
        } else {
            ESP_LOGW(TAG, "enroll: bonded but no IRK distributed");
        }
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        s_enroll_conn = BLE_HS_CONN_HANDLE_NONE;
        if (s_enroll == BLE_PRESENCE_ADVERTISING)
            enroll_advertise();                /* gave up mid-way: re-adv */
        break;

    default:
        break;
    }
    return 0;
}

static void enroll_advertise(void)
{
    struct ble_hs_adv_fields f = { 0 };
    const char *name = "CYBERDECK";
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)name;
    f.name_len = strlen(name);
    f.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_set_fields: %d", rc);
        return;
    }
    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &p,
                           enroll_gap_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY)
        ESP_LOGW(TAG, "adv_start: %d", rc);
}

void ble_presence_enroll_start(void)
{
    if (s_enroll == BLE_PRESENCE_ADVERTISING)
        return;
    s_enroll = BLE_PRESENCE_ADVERTISING;
    enroll_advertise();
    ESP_LOGI(TAG, "enroll: advertising as CYBERDECK — pair from the phone");
}

void ble_presence_enroll_stop(void)
{
    if (s_enroll == BLE_PRESENCE_IDLE)
        return;
    s_enroll = BLE_PRESENCE_IDLE;
    ble_gap_adv_stop();
    if (s_enroll_conn != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(s_enroll_conn, BLE_ERR_REM_USER_CONN_TERM);
}

/* ---- init --------------------------------------------------------------- */

void ble_presence_init(void)
{
    phone_load();
    const esp_timer_create_args_t a = {
        .callback = keeper_tick,
        .name     = "ble_presence",
    };
    if (esp_timer_create(&a, &s_keeper) == ESP_OK)
        esp_timer_start_periodic(s_keeper, 1000000);   /* 1 s */
}

#else /* no BLE backend: inert stubs */

void ble_presence_init(void) {}
bool ble_presence_enrolled(void) { return false; }
bool ble_presence_present(void) { return false; }
uint32_t ble_presence_age_ms(void) { return UINT32_MAX; }
int ble_presence_rssi(void) { return 0; }
void ble_presence_enroll_start(void) {}
void ble_presence_enroll_stop(void) {}
ble_presence_enroll_t ble_presence_enroll_state(void) { return BLE_PRESENCE_IDLE; }
void ble_presence_forget(void) {}
void ble_presence_on_disc(const uint8_t val[6], uint8_t addr_type, int rssi)
{ (void)val; (void)addr_type; (void)rssi; }

#endif /* CONFIG_INPUT_BLE || CONFIG_INPUT_AUTO */
