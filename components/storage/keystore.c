/*
 * keystore.c — PIN-unlocked wrapped key store (docs/storage_auth.md).
 *
 * Compiled on BOTH device and simulator. All serialization is explicit
 * little-endian byte layout — the on-disk formats are byte-identical across
 * platforms, which is what makes the sim usable as the PC-side provisioning
 * tool. Crypto: Monocypher (vendored by the libssh2 fork) — crypto_argon2
 * (Argon2id), crypto_aead_lock/unlock (XChaCha20-Poly1305), crypto_wipe.
 *
 * RAM hygiene: the MK lives in .bss (internal SRAM on the S3, never PSRAM)
 * and is crypto_wipe()d on lock. The Argon2 work area is PSRAM on device
 * (plain heap on the host) and wiped after each derivation.
 */

#ifdef _WIN32
#define _CRT_RAND_S          /* rand_s(): must precede the first <stdlib.h> */
#endif

#include "keystore.h"
#include "storage.h"
#include "storage_priv.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "monocypher.h"
#include "storage_cred.h"   /* the shared credential staging buffer */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_random.h"
#include "esp_timer.h"
#elif defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

static const char *TAG = "keystore";

#define KS_MAGIC        "CKS1"
#define KW_MAGIC        "CKW1"
#define KS_VERSION      1
#define KS_SLOTS        4
#define KS_SLOT_SIZE    100
#define KS_HDR_SIZE     (24 + KS_SLOTS * KS_SLOT_SIZE)   /* 424 */
#define KS_FILE         "keystore.kv1"

#define KW_HDR_SIZE     36                /* size of the fixed part before the ciphertext */
#define KW_TAG_SIZE     16
#define KW_CT_MAX       16384             /* sanity cap on wrapped payload */

#define KS_SLOT_EMPTY       0
#define KS_SLOT_PIN         1
#define KS_SLOT_PASSPHRASE  2

#define KS_ALG_ARGON2ID     2

/* Argon2id defaults: 4 MiB work area, 2 passes, 1 lane. The S3 took 1.09 s
 * at 240 MHz. CYBERDECK_BENCH_ARGON2 swept 4 MiB: x1 = 645 ms, x2 = 1090 ms,
 * x3 = 1566 ms. 8 MiB does not allocate. 4 MiB is the hardware ceiling, so
 * memory stays maxed and passes tune the time. Per-store params in the
 * header allow retuning without a format change. */
#define KS_ARGON2_BLOCKS_KIB  4096
#define KS_ARGON2_PASSES      2
#define KS_ARGON2_LANES       1

typedef struct {
    uint8_t  type;
    uint8_t  alg;
    uint32_t blocks_kib;
    uint32_t passes;
    uint8_t  lanes;
    /* Byte 11, ex-reserved: auto-submit hint, 0 for passphrase slots.
     * Not in the AAD. Tampering only breaks auto-submit UX. */
    uint8_t  pin_len;
    uint8_t  salt[16];
    uint8_t  nonce[24];
    uint8_t  wrapped_mk[48];      /* 32 B MK ciphertext + 16 B tag */
} ks_slot_t;

typedef struct {
    uint8_t   version;
    uint8_t   n_slots;
    uint8_t   uuid[16];
    ks_slot_t slot[KS_SLOTS];
} ks_header_t;

/* .bss lands in internal SRAM on the device, never PSRAM. */
static struct {
    bool        hdr_loaded;
    bool        unlocked;
    ks_header_t hdr;
    uint8_t     mk[32];
} s_ks;

static void ks_path(char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%s/" KS_FILE, storage_platform_mount_point());
}

static void kw_path(const char *key_id, char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%s/keys/%s.kw1",
             storage_platform_mount_point(), key_id);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Cryptographically secure random bytes, per platform. */
static esp_err_t ks_random(void *buf, size_t len)
{
#ifdef ESP_PLATFORM
    esp_fill_random(buf, len);
    return ESP_OK;
#elif defined(_WIN32)
    uint8_t *p = buf;
    for (size_t i = 0; i < len; i += sizeof(unsigned int)) {
        unsigned int r;
        if (rand_s(&r) != 0) return ESP_FAIL;      /* RtlGenRandom-backed */
        size_t n = len - i < sizeof(r) ? len - i : sizeof(r);
        memcpy(p + i, &r, n);
    }
    return ESP_OK;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return ESP_FAIL;
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return n == len ? ESP_OK : ESP_FAIL;
#endif
}

/* Atomic write: <path>.tmp then rename, same discipline as storage.c. */
static esp_err_t ks_write_atomic(const char *path,
                                 const void *data, size_t len)
{
    char tmp[176];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open '%s' for write", tmp);
        return ESP_FAIL;
    }
    size_t n = fwrite(data, 1, len, f);
    if (fclose(f) != 0 || n != len) {
        remove(tmp);
        return ESP_FAIL;
    }
    if (rename(tmp, path) == 0)
        return ESP_OK;
    remove(path);                           /* Windows: no clobbering rename */
    if (rename(tmp, path) != 0) {
        ESP_LOGE(TAG, "rename '%s' -> '%s' failed", tmp, path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Explicit byte offsets, not struct punning: layout must match across
 * platforms exactly. */
static void hdr_serialize(const ks_header_t *h, uint8_t out[KS_HDR_SIZE])
{
    memset(out, 0, KS_HDR_SIZE);
    memcpy(out, KS_MAGIC, 4);
    out[4] = h->version;
    out[5] = h->n_slots;
    memcpy(out + 8, h->uuid, 16);
    for (int i = 0; i < KS_SLOTS; i++) {
        uint8_t *s = out + 24 + i * KS_SLOT_SIZE;
        const ks_slot_t *sl = &h->slot[i];
        s[0] = sl->type;
        s[1] = sl->alg;
        put_le32(s + 2, sl->blocks_kib);
        put_le32(s + 6, sl->passes);
        s[10] = sl->lanes;
        s[11] = sl->pin_len;
        memcpy(s + 12, sl->salt, 16);
        memcpy(s + 28, sl->nonce, 24);
        memcpy(s + 52, sl->wrapped_mk, 48);
    }
}

static esp_err_t hdr_parse(const uint8_t in[KS_HDR_SIZE], ks_header_t *h)
{
    if (memcmp(in, KS_MAGIC, 4) != 0) return ESP_ERR_INVALID_CRC;
    memset(h, 0, sizeof(*h));
    h->version = in[4];
    h->n_slots = in[5];
    if (h->version != KS_VERSION) return ESP_ERR_INVALID_CRC;
    if (h->n_slots < 1 || h->n_slots > KS_SLOTS) return ESP_ERR_INVALID_CRC;
    memcpy(h->uuid, in + 8, 16);
    for (int i = 0; i < KS_SLOTS; i++) {
        const uint8_t *s = in + 24 + i * KS_SLOT_SIZE;
        ks_slot_t *sl = &h->slot[i];
        sl->type       = s[0];
        sl->alg        = s[1];
        sl->blocks_kib = get_le32(s + 2);
        sl->passes     = get_le32(s + 6);
        sl->lanes      = s[10];
        sl->pin_len    = s[11];
        memcpy(sl->salt,       s + 12, 16);
        memcpy(sl->nonce,      s + 28, 24);
        memcpy(sl->wrapped_mk, s + 52, 48);
    }
    return ESP_OK;
}

/* Read + parse keystore.kv1 into the cache. */
static esp_err_t ks_load(void)
{
    if (s_ks.hdr_loaded) return ESP_OK;

    char path[160];
    ks_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    uint8_t raw[KS_HDR_SIZE];
    size_t n = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    if (n != KS_HDR_SIZE) {
        ESP_LOGE(TAG, "Store header short read (%zu of %d)", n, KS_HDR_SIZE);
        return ESP_ERR_INVALID_CRC;
    }
    esp_err_t e = hdr_parse(raw, &s_ks.hdr);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Store header corrupt");
        return e;
    }
    s_ks.hdr_loaded = true;
    return ESP_OK;
}

static esp_err_t ks_store_header(void)
{
    uint8_t raw[KS_HDR_SIZE];
    hdr_serialize(&s_ks.hdr, raw);
    char path[160];
    ks_path(path, sizeof(path));
    return ks_write_atomic(path, raw, sizeof(raw));
}

/* Slot AAD: magic ‖ version ‖ store_uuid ‖ slot_index ‖ slot_type ‖ argon2
 * params. Params tampering only DoSes — they are inputs to the derivation. */
#define KS_SLOT_AAD_SIZE 33
static void slot_aad(const ks_header_t *h, int idx,
                     uint8_t out[KS_SLOT_AAD_SIZE])
{
    const ks_slot_t *sl = &h->slot[idx];
    memcpy(out, KS_MAGIC, 4);
    out[4] = h->version;
    memcpy(out + 5, h->uuid, 16);
    out[21] = (uint8_t)idx;
    out[22] = sl->type;
    out[23] = sl->alg;
    put_le32(out + 24, sl->blocks_kib);
    put_le32(out + 28, sl->passes);
    out[32] = sl->lanes;
}

/* Argon2id(PIN, slot salt) -> 32-byte KEK. Work area in PSRAM on device. */
static esp_err_t derive_kek(const ks_slot_t *sl, const char *pin,
                            uint8_t kek[32])
{
    if (sl->alg != KS_ALG_ARGON2ID) return ESP_ERR_INVALID_CRC;
    if (sl->blocks_kib < 8 || sl->blocks_kib > 262144 ||
        sl->passes < 1 || sl->passes > 64 || sl->lanes < 1)
        return ESP_ERR_INVALID_CRC;      /* reject absurd params (DoS cap) */

    size_t area_size = (size_t)sl->blocks_kib * 1024;
    void *area = heap_caps_malloc(area_size,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!area) return ESP_ERR_NO_MEM;

    crypto_argon2_config cfg = {
        .algorithm = CRYPTO_ARGON2_ID,
        .nb_blocks = sl->blocks_kib,
        .nb_passes = sl->passes,
        .nb_lanes  = sl->lanes,
    };
    crypto_argon2_inputs in = {
        .pass      = (const uint8_t *)pin,
        .pass_size = (uint32_t)strlen(pin),
        .salt      = sl->salt,
        .salt_size = sizeof(sl->salt),
    };
    crypto_argon2(kek, 32, area, cfg, in, crypto_argon2_no_extras);

    crypto_wipe(area, area_size);
    heap_caps_free(area);
    return ESP_OK;
}

static bool pin_ok(const char *pin)
{
    if (!pin) return false;
    size_t n = strlen(pin);
    return n >= KEYSTORE_PIN_MIN && n <= KEYSTORE_PIN_MAX;
}

static uint8_t pin_slot_type(const char *pin)
{
    for (const char *p = pin; *p; p++)
        if (*p < '0' || *p > '9') return KS_SLOT_PASSPHRASE;
    return KS_SLOT_PIN;
}

/* Wrap the in-RAM MK into slot @idx with @pin: fresh salt + nonce, KEK
 * derivation, AEAD lock. Header is NOT written here. */
static esp_err_t slot_wrap_mk(int idx, const char *pin)
{
    ks_slot_t *sl = &s_ks.hdr.slot[idx];
    sl->type       = pin_slot_type(pin);
    sl->pin_len    = sl->type == KS_SLOT_PIN ? (uint8_t)strlen(pin) : 0;
    sl->alg        = KS_ALG_ARGON2ID;
    sl->blocks_kib = KS_ARGON2_BLOCKS_KIB;
    sl->passes     = KS_ARGON2_PASSES;
    sl->lanes      = KS_ARGON2_LANES;
    if (ks_random(sl->salt, sizeof(sl->salt)) != ESP_OK ||
        ks_random(sl->nonce, sizeof(sl->nonce)) != ESP_OK)
        return ESP_FAIL;

    uint8_t kek[32];
    esp_err_t e = derive_kek(sl, pin, kek);
    if (e != ESP_OK) return e;

    uint8_t aad[KS_SLOT_AAD_SIZE];
    slot_aad(&s_ks.hdr, idx, aad);
    crypto_aead_lock(sl->wrapped_mk, sl->wrapped_mk + 32, kek, sl->nonce,
                     aad, sizeof(aad), s_ks.mk, 32);
    crypto_wipe(kek, sizeof(kek));
    return ESP_OK;
}

/* A tag mismatch returns ESP_FAIL: the wrong PIN for this slot. */
static esp_err_t slot_unwrap_mk(int idx, const char *pin, uint8_t mk_out[32])
{
    const ks_slot_t *sl = &s_ks.hdr.slot[idx];
    uint8_t kek[32];
    esp_err_t e = derive_kek(sl, pin, kek);
    if (e != ESP_OK) return e;

    uint8_t aad[KS_SLOT_AAD_SIZE];
    slot_aad(&s_ks.hdr, idx, aad);
    int rc = crypto_aead_unlock(mk_out, sl->wrapped_mk + 32, kek, sl->nonce,
                                aad, sizeof(aad), sl->wrapped_mk, 32);
    crypto_wipe(kek, sizeof(kek));
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

keystore_state_t keystore_state(void)
{
    if (s_ks.unlocked) return KEYSTORE_UNLOCKED;
    esp_err_t e = ks_load();
    if (e == ESP_ERR_NOT_FOUND) return KEYSTORE_ABSENT;
    return KEYSTORE_LOCKED;      /* LOCKED covers a valid store and a corrupt one */
}

uint8_t keystore_pin_len(void)
{
    if (ks_load() != ESP_OK) return 0;
    for (int i = 0; i < KS_SLOTS; i++) {
        const ks_slot_t *sl = &s_ks.hdr.slot[i];
        if (sl->type == KS_SLOT_PIN &&
            sl->pin_len >= KEYSTORE_PIN_MIN && sl->pin_len <= KEYSTORE_PIN_MAX)
            return sl->pin_len;
    }
    return 0;
}

/* The on-device gate turns PIN brute force into a real threat: each guess
 * costs only ~1 s of Argon2 derivation. The store keeps the failure counter
 * in <mount>/backoff.cnt. The first few failures cost no wait. After that,
 * each failure doubles the wait, from KS_BACKOFF_BASE_MS up to the
 * KS_BACKOFF_CAP_MS ceiling. The deck has no battery-backed wall clock, so
 * the backoff timer uses MONOTONIC UPTIME. A reboot re-arms the delay in
 * full: counter survives, uptime doesn't, so power-cycling costs more than
 * waiting. */

#define KS_BACKOFF_FILE    "backoff.cnt"
#define KS_BACKOFF_FREE    5         /* delay starts at the 5th failure */
#define KS_BACKOFF_BASE_MS 30000u    /* 5th failure: 30 s               */
#define KS_BACKOFF_CAP_MS  900000u   /* 15 min ceiling                  */

static uint64_t ks_uptime_default(void)
{
#if defined(ESP_PLATFORM)
    return (uint64_t)(esp_timer_get_time() / 1000);
#elif defined(_WIN32)
    return (uint64_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
#endif
}

static uint64_t (*s_uptime)(void) = ks_uptime_default;
static bool     s_bk_loaded = false;
static uint32_t s_bk_count  = 0;
static uint64_t s_bk_until  = 0;     /* uptime ms; 0 = no active wait */

void keystore_set_uptime_hook(uint64_t (*fn)(void))
{
    s_uptime    = fn ? fn : ks_uptime_default;
    s_bk_loaded = false;             /* re-derive the boot penalty */
}

static void bk_path(char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%s/" KS_BACKOFF_FILE, storage_platform_mount_point());
}

static uint32_t bk_delay_ms(uint32_t count)
{
    if (count < KS_BACKOFF_FREE) return 0;
    uint32_t shift = count - KS_BACKOFF_FREE;
    if (shift > 5) shift = 5;        /* 30 s << 5 already tops the cap */
    uint32_t d = KS_BACKOFF_BASE_MS << shift;
    return d > KS_BACKOFF_CAP_MS ? KS_BACKOFF_CAP_MS : d;
}

static void bk_load(void)
{
    if (s_bk_loaded) return;
    s_bk_loaded = true;
    s_bk_count  = 0;
    s_bk_until  = 0;
    char path[160];
    bk_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    unsigned n = 0;
    if (fscanf(f, "%u", &n) == 1) s_bk_count = n;
    fclose(f);
    uint32_t d = bk_delay_ms(s_bk_count);   /* boot pays the full delay */
    if (d) s_bk_until = s_uptime() + d;
}

static void bk_fail(void)
{
    bk_load();
    s_bk_count++;
    char path[160];
    bk_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%u\n", (unsigned)s_bk_count); fclose(f); }
    uint32_t d = bk_delay_ms(s_bk_count);
    if (d) {
        s_bk_until = s_uptime() + d;
        ESP_LOGW(TAG, "Failed attempt %u — next try in %u s",
                 (unsigned)s_bk_count, (unsigned)(d / 1000));
    }
}

static void bk_clear(void)
{
    s_bk_loaded = true;
    s_bk_count  = 0;
    s_bk_until  = 0;
    char path[160];
    bk_path(path, sizeof(path));
    remove(path);
}

uint32_t keystore_backoff_ms(void)
{
    bk_load();
    if (!s_bk_until) return 0;
    uint64_t now = s_uptime();
    if (now >= s_bk_until) { s_bk_until = 0; return 0; }
    return (uint32_t)(s_bk_until - now);
}

static void ks_adopt_plaintext(void);
static void ks_adopt_secrets(void);
static void secrets_wipe_cache(void);

esp_err_t keystore_create(const char *pin)
{
    if (!pin_ok(pin)) return ESP_ERR_INVALID_ARG;

    char path[160];
    ks_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        ESP_LOGE(TAG, "Store already exists — refusing to overwrite");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_ks, 0, sizeof(s_ks));
    s_ks.hdr.version = KS_VERSION;
    s_ks.hdr.n_slots = 1;
    if (ks_random(s_ks.hdr.uuid, sizeof(s_ks.hdr.uuid)) != ESP_OK ||
        ks_random(s_ks.mk, sizeof(s_ks.mk)) != ESP_OK) {
        crypto_wipe(s_ks.mk, sizeof(s_ks.mk));
        return ESP_FAIL;
    }

    esp_err_t e = slot_wrap_mk(0, pin);
    if (e == ESP_OK) e = ks_store_header();
    if (e != ESP_OK) {
        crypto_wipe(&s_ks, sizeof(s_ks));
        return e;
    }
    s_ks.hdr_loaded = true;
    s_ks.unlocked   = true;
    bk_clear();                    /* stale counter from a removed store */
    ESP_LOGI(TAG, "Keystore created (slot 0: %s)",
             s_ks.hdr.slot[0].type == KS_SLOT_PIN ? "pin" : "passphrase");
    /* Adopt existing bare keys now: a fresh store must protect what's
     * already on disk. Waiting for the first keystore_unlock() left keys
     * plaintext indefinitely. The lazy trigger prompts for no unwrapped
     * key, so adoption never fires. */
    ks_adopt_plaintext();
    ks_adopt_secrets();
    return ESP_OK;
}

/* Adopt-on-unlock: wrap bare keys/<id>.pem files and delete the plaintext.
 * Best-effort — LittleFS wear-leveling may keep old blocks until reuse. */
static void ks_adopt_plaintext(void)
{
    char ids[16][STORAGE_KEY_ID_LEN];
    int n = 0;
    if (storage_scan_key_ext(".pem", ids, 16, &n) != ESP_OK || n == 0)
        return;

    char *buf = heap_caps_malloc(KW_CT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) return;

    int adopted = 0;
    for (int i = 0; i < n; i++) {
        char path[160];
        snprintf(path, sizeof(path), "%s/keys/%s.pem",
                 storage_platform_mount_point(), ids[i]);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        size_t len = fread(buf, 1, KW_CT_MAX, f);
        fclose(f);
        if (len == 0 || len == KW_CT_MAX) continue;   /* empty or oversized */
        if (keystore_wrap(ids[i], KEYSTORE_CONTENT_PEM, buf, len) != ESP_OK)
            continue;
        storage_shred_file(path);          /* zero the plaintext, then rm */
        adopted++;
    }
    crypto_wipe(buf, KW_CT_MAX);
    heap_caps_free(buf);
    if (adopted)
        ESP_LOGW(TAG, "Adopted %d plaintext key(s) into the store", adopted);
}

esp_err_t keystore_unlock(const char *pin)
{
    if (s_ks.unlocked) return ESP_OK;
    if (!pin_ok(pin)) return ESP_ERR_INVALID_ARG;
    if (keystore_backoff_ms() > 0) return KEYSTORE_ERR_BACKOFF;

    esp_err_t e = ks_load();
    if (e != ESP_OK) return e;    /* NOT_FOUND or INVALID_CRC */

    for (int i = 0; i < KS_SLOTS; i++) {
        const ks_slot_t *sl = &s_ks.hdr.slot[i];
        if (sl->type != KS_SLOT_PIN && sl->type != KS_SLOT_PASSPHRASE)
            continue;
        e = slot_unwrap_mk(i, pin, s_ks.mk);
        if (e == ESP_ERR_NO_MEM) return e;
        if (e == ESP_OK) {
            s_ks.unlocked = true;
            ESP_LOGI(TAG, "Unlocked via slot %d", i);
            bk_clear();
            ks_adopt_plaintext();
            ks_adopt_secrets();
            return ESP_OK;
        }
    }
    crypto_wipe(s_ks.mk, sizeof(s_ks.mk));
    bk_fail();
    return ESP_FAIL;              /* wrong PIN: no slot's tag verified */
}

void keystore_lock(void)
{
    crypto_wipe(s_ks.mk, sizeof(s_ks.mk));
    s_ks.unlocked = false;
    secrets_wipe_cache();          /* cache shares the MK's lifetime */
}

void keystore_reset_cache(void)
{
    crypto_wipe(s_ks.mk, sizeof(s_ks.mk));
    memset(&s_ks, 0, sizeof(s_ks));
    s_bk_loaded = false;   /* re-read backoff.cnt (factory reset drops it) */
    secrets_wipe_cache();
}

void keystore_wipe(void *buf, size_t len)
{
    if (buf && len) crypto_wipe(buf, len);
}

esp_err_t keystore_change_pin(const char *old_pin, const char *new_pin)
{
    if (!pin_ok(old_pin) || !pin_ok(new_pin)) return ESP_ERR_INVALID_ARG;
    if (keystore_backoff_ms() > 0) return KEYSTORE_ERR_BACKOFF;

    esp_err_t e = ks_load();
    if (e != ESP_OK) return e;

    /* Verify old_pin against the slots even if already unlocked — changing
     * the PIN must prove knowledge of the old one, and identifies WHICH
     * slot to rewrap. */
    bool was_unlocked = s_ks.unlocked;
    uint8_t mk[32];
    int idx = -1;
    for (int i = 0; i < KS_SLOTS; i++) {
        const ks_slot_t *sl = &s_ks.hdr.slot[i];
        if (sl->type != KS_SLOT_PIN && sl->type != KS_SLOT_PASSPHRASE)
            continue;
        e = slot_unwrap_mk(i, old_pin, mk);
        if (e == ESP_ERR_NO_MEM) return e;
        if (e == ESP_OK) { idx = i; break; }
    }
    if (idx < 0) { bk_fail(); return ESP_FAIL; }
    bk_clear();

    /* slot_wrap_mk works on s_ks.mk; install the recovered MK there. */
    memcpy(s_ks.mk, mk, 32);
    crypto_wipe(mk, sizeof(mk));

    ks_slot_t backup = s_ks.hdr.slot[idx];
    e = slot_wrap_mk(idx, new_pin);
    if (e == ESP_OK) e = ks_store_header();
    if (e != ESP_OK) s_ks.hdr.slot[idx] = backup;    /* roll back in RAM */

    if (!was_unlocked)
        crypto_wipe(s_ks.mk, sizeof(s_ks.mk));       /* don't stay unlocked */
    if (e == ESP_OK)
        ESP_LOGI(TAG, "Slot %d re-wrapped with new %s", idx,
                 s_ks.hdr.slot[idx].type == KS_SLOT_PIN ? "pin" : "passphrase");
    return e;
}

/* Path safety is storage_key_id_ok() (storage_priv.h) — one gate shared with
 * the plaintext storage_*_key entry points, which used to have none. */

/* Key AAD: magic ‖ version ‖ content_type ‖ store_uuid ‖ key_id — binding
 * the id and store uuid into the tag kills renaming and transplants. */
static size_t kw_aad(uint8_t content_type, const char *key_id,
                     uint8_t *out /* >= 22 + STORAGE_KEY_ID_LEN */)
{
    memcpy(out, KW_MAGIC, 4);
    out[4] = KS_VERSION;
    out[5] = content_type;
    memcpy(out + 6, s_ks.hdr.uuid, 16);
    size_t idlen = strlen(key_id);
    memcpy(out + 22, key_id, idlen);
    return 22 + idlen;
}

esp_err_t keystore_wrap(const char *key_id, uint8_t content_type,
                        const void *plaintext, size_t len)
{
    if (!storage_key_id_ok(key_id) || !plaintext || len == 0 || len > KW_CT_MAX)
        return ESP_ERR_INVALID_ARG;
    if (!s_ks.unlocked) return ESP_ERR_INVALID_STATE;

    size_t total = KW_HDR_SIZE + len + KW_TAG_SIZE;
    uint8_t *out = heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out) return ESP_ERR_NO_MEM;

    memcpy(out, KW_MAGIC, 4);
    out[4] = KS_VERSION;
    out[5] = content_type;
    out[6] = out[7] = 0;
    if (ks_random(out + 8, 24) != ESP_OK) {
        heap_caps_free(out);
        return ESP_FAIL;
    }
    put_le32(out + 32, (uint32_t)len);

    uint8_t aad[22 + STORAGE_KEY_ID_LEN];
    size_t aad_len = kw_aad(content_type, key_id, aad);
    crypto_aead_lock(out + KW_HDR_SIZE,             /* ciphertext */
                     out + KW_HDR_SIZE + len,       /* tag        */
                     s_ks.mk, out + 8,              /* key, nonce */
                     aad, aad_len, plaintext, len);

    char path[160];
    kw_path(key_id, path, sizeof(path));
    esp_err_t e = ks_write_atomic(path, out, total);
    heap_caps_free(out);
    if (e == ESP_OK)
        ESP_LOGI(TAG, "Wrapped key '%s' (%zu bytes)", key_id, len);
    return e;
}

esp_err_t keystore_unwrap(const char *key_id, void *buf, size_t buf_len,
                          size_t *written, uint8_t *content_type)
{
    if (!storage_key_id_ok(key_id) || !buf || !written) return ESP_ERR_INVALID_ARG;
    *written = 0;

    char path[160];
    kw_path(key_id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;

    if (!s_ks.unlocked) {
        fclose(f);
        return ESP_ERR_INVALID_STATE;    /* locked — caller can prompt */
    }

    uint8_t hdr[KW_HDR_SIZE];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr) ||
        memcmp(hdr, KW_MAGIC, 4) != 0 || hdr[4] != KS_VERSION) {
        fclose(f);
        return ESP_ERR_INVALID_CRC;
    }
    uint32_t ct_len = get_le32(hdr + 32);
    if (ct_len == 0 || ct_len > KW_CT_MAX) {
        fclose(f);
        return ESP_ERR_INVALID_CRC;
    }
    if (ct_len > buf_len) {
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *ct = heap_caps_malloc(ct_len + KW_TAG_SIZE,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ct) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t n = fread(ct, 1, ct_len + KW_TAG_SIZE, f);
    fclose(f);
    if (n != ct_len + KW_TAG_SIZE) {
        heap_caps_free(ct);
        return ESP_ERR_INVALID_CRC;
    }

    uint8_t aad[22 + STORAGE_KEY_ID_LEN];
    size_t aad_len = kw_aad(hdr[5], key_id, aad);
    int rc = crypto_aead_unlock(buf, ct + ct_len, s_ks.mk, hdr + 8,
                                aad, aad_len, ct, ct_len);
    heap_caps_free(ct);
    if (rc != 0) {
        ESP_LOGE(TAG, "Key '%s': tag mismatch (tampered/renamed/foreign)",
                 key_id);
        return ESP_ERR_INVALID_CRC;
    }
    *written = ct_len;
    if (content_type) *content_type = hdr[5];
    return ESP_OK;
}

bool keystore_is_wrapped(const char *key_id)
{
    if (!storage_key_id_ok(key_id)) return false;
    char path[160];
    kw_path(key_id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* Secrets bundle: keys/secrets.kw1 (content_type 2). Raw "ns:key=value\n"
 * lines cache in .bss (internal SRAM) with exactly the MK's lifetime.
 * Unlock loads them lazily; lock wipes them with crypto_wipe(). */

#define KS_SECRETS_MAX 2048

/* The bundle must hold everything the diversion layer can ever put in it.
 * That means one line per profile and one line per WiFi net, each at its
 * field's maximum. If it cannot, keystore_secret_set() returns
 * ESP_ERR_NO_MEM. storage_save_profiles() then falls back to leaving that
 * password in the ini as PLAINTEXT. That is a silent downgrade of exactly
 * what the vault exists to protect. This size derives from the structs.
 * Raising STORAGE_MAX_PROFILES / STORAGE_WIFI_MAX (or a field width) breaks
 * the build here instead of the guarantee. */
_Static_assert(STORAGE_MAX_PROFILES *
                   (sizeof("profile:") - 1 +
                    sizeof(((conn_profile_t *)0)->name) - 1 + 1 +
                    sizeof(((conn_profile_t *)0)->password) - 1 + 1) +
               STORAGE_WIFI_MAX *
                   (sizeof("wifi:") - 1 +
                    sizeof(((wifi_profile_t *)0)->ssid) - 1 + 1 +
                    sizeof(((wifi_profile_t *)0)->password) - 1 + 1)
               < KS_SECRETS_MAX,
               "secrets bundle too small for the worst-case profile/wifi set "
               "— raise KS_SECRETS_MAX (passwords would silently stay "
               "plaintext in the ini otherwise)");

static char   s_sec[KS_SECRETS_MAX];
static size_t s_sec_len;
static bool   s_sec_loaded;

static void secrets_wipe_cache(void)
{
    crypto_wipe(s_sec, sizeof(s_sec));
    s_sec_len    = 0;
    s_sec_loaded = false;
}

static esp_err_t secrets_load(void)
{
    if (s_sec_loaded) return ESP_OK;
    if (!s_ks.unlocked) return ESP_ERR_INVALID_STATE;
    size_t  n  = 0;
    uint8_t ct = 0;
    esp_err_t e = keystore_unwrap(KEYSTORE_SECRETS_ID,
                                  s_sec, sizeof(s_sec) - 1, &n, &ct);
    if (e == ESP_ERR_NOT_FOUND) {              /* no bundle yet: empty */
        s_sec[0] = '\0'; s_sec_len = 0; s_sec_loaded = true;
        return ESP_OK;
    }
    if (e != ESP_OK) return e;
    if (ct != KEYSTORE_CONTENT_SECRETS) {      /* a key named "secrets"?! */
        secrets_wipe_cache();
        return ESP_ERR_INVALID_CRC;
    }
    s_sec[n] = '\0'; s_sec_len = n; s_sec_loaded = true;
    return ESP_OK;
}

static esp_err_t secrets_store(void)
{
    if (s_sec_len == 0) {                      /* emptied: drop the file */
        char path[160];
        kw_path(KEYSTORE_SECRETS_ID, path, sizeof(path));
        remove(path);
        return ESP_OK;
    }
    return keystore_wrap(KEYSTORE_SECRETS_ID, KEYSTORE_CONTENT_SECRETS,
                         s_sec, s_sec_len);
}

/* Value of the "skey=" line, or NULL. *vlen excludes the newline. */
static char *secrets_find(const char *skey, size_t *vlen)
{
    size_t kl = strlen(skey);
    char  *p  = s_sec;
    while (p && *p) {
        char  *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (ll > kl && p[kl] == '=' && strncmp(p, skey, kl) == 0) {
            if (vlen) *vlen = ll - kl - 1;
            return p + kl + 1;
        }
        p = nl ? nl + 1 : NULL;
    }
    return NULL;
}

/* Cut [from, to) out of the cache; zeroes the freed tail. */
static void secrets_cut(char *from, char *to)
{
    size_t tail = s_sec_len - (size_t)(to - s_sec);
    size_t gap  = (size_t)(to - from);
    memmove(from, to, tail + 1);               /* +1 carries the NUL */
    s_sec_len -= gap;
    crypto_wipe(s_sec + s_sec_len + 1, gap);
}

esp_err_t keystore_secret_get(const char *skey, char *out, size_t out_len)
{
    if (!skey || !out || out_len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    if (!s_ks.unlocked) return ESP_ERR_INVALID_STATE;
    esp_err_t e = secrets_load();
    if (e != ESP_OK) return e;
    size_t vlen = 0;
    const char *v = secrets_find(skey, &vlen);
    if (!v) return ESP_ERR_NOT_FOUND;
    if (vlen >= out_len) return ESP_ERR_INVALID_SIZE;
    memcpy(out, v, vlen);
    out[vlen] = '\0';
    return ESP_OK;
}

esp_err_t keystore_secret_set(const char *skey, const char *value)
{
    if (!skey || !skey[0] || strchr(skey, '\n') || strchr(skey, '='))
        return ESP_ERR_INVALID_ARG;
    if (value && strchr(value, '\n')) return ESP_ERR_INVALID_ARG;
    if (!s_ks.unlocked) return ESP_ERR_INVALID_STATE;
    esp_err_t e = secrets_load();
    if (e != ESP_OK) return e;

    size_t vlen  = 0;
    char  *v     = secrets_find(skey, &vlen);
    char  *line  = NULL, *next = NULL;
    if (v) {
        line = v - strlen(skey) - 1;
        next = v + vlen;
        if (*next == '\n') next++;
    }

    /* The capacity check runs BEFORE the cut, and the cut's own bytes count
     * toward the budget. Bailing out after the cut used to leave the cache
     * one entry short of the file's contents. The next successful set then
     * wrote that divergence to flash. A credential silently disappeared,
     * while the caller only saw ESP_ERR_NO_MEM on some unrelated key. */
    if (value && value[0]) {
        size_t need  = strlen(skey) + 1 + strlen(value) + 2;
        size_t freed = line ? (size_t)(next - line) : 0;
        if (s_sec_len - freed + need >= sizeof(s_sec))
            return ESP_ERR_NO_MEM;             /* cache untouched */
    }
    if (line) secrets_cut(line, next);
    if (value && value[0])
        s_sec_len += (size_t)snprintf(s_sec + s_sec_len,
                                      sizeof(s_sec) - s_sec_len,
                                      "%s=%s\n", skey, value);
    return secrets_store();
}

void keystore_secrets_prune(const char *prefix,
                            const char *const *keep, int nkeep)
{
    if (!s_ks.unlocked || secrets_load() != ESP_OK) return;
    size_t plen    = strlen(prefix);
    bool   changed = false;
    char  *p       = s_sec;
    while (*p) {
        char *nl   = strchr(p, '\n');
        char *next = nl ? nl + 1 : p + strlen(p);
        bool  drop = false;
        if (strncmp(p, prefix, plen) == 0) {
            char *eq = memchr(p, '=', (size_t)(next - p));
            if (eq) {
                size_t kl = (size_t)(eq - p) - plen;
                drop = true;
                for (int i = 0; i < nkeep && drop; i++)
                    if (strlen(keep[i]) == kl &&
                        strncmp(keep[i], p + plen, kl) == 0)
                        drop = false;
            }
        }
        if (drop) { secrets_cut(p, next); changed = true; }
        else        p = next;
    }
    if (changed) secrets_store();
}

/* ks_adopt_secrets() migrates plaintext credentials out of profiles.ini
 * and wifi.ini. storage_save_*() in storage.c diverts every password it
 * sees into this bundle while the store stays unlocked. So a load-then-save
 * round trip performs the migration. This runs only when a raw
 * scan finds plaintext on disk. An unconditional round trip would rewrite
 * flash on every unlock. */
static void ks_adopt_secrets(void)
{
    if (!storage_secrets_pending()) return;

    /* Shared cred scratch (storage.h) — far too fat for the unlock
     * worker's stack; the profiles phase finishes before the nets phase,
     * so the union is safe. */
    storage_cred_scratch_t *sc = storage_cred_scratch();
    int n = 0;
    if (storage_load_profiles(sc->u.profiles, &n, STORAGE_MAX_PROFILES)
            == ESP_OK && n > 0)
        storage_save_profiles(sc->u.profiles, n);
    n = 0;
    if (storage_wifi_load(sc->u.nets, &n, STORAGE_WIFI_MAX) == ESP_OK
        && n > 0)
        storage_wifi_save(sc->u.nets, n);
    crypto_wipe(sc, sizeof(*sc));
    ESP_LOGW(TAG, "Adopted plaintext credentials into the secrets bundle");
}

/* Remove-code path: hydrating load (store still unlocked) + RAW write =
 * plaintext restored to the inis; then the bundle file goes away. */
static void secrets_restore_plaintext(void)
{
    if (secrets_load() == ESP_OK && s_sec_len > 0) {
        storage_cred_scratch_t *sc = storage_cred_scratch();
        int n = 0;
        if (storage_load_profiles(sc->u.profiles, &n, STORAGE_MAX_PROFILES)
                == ESP_OK && n > 0)
            storage_profiles_write_raw(sc->u.profiles, n);
        n = 0;
        if (storage_wifi_load(sc->u.nets, &n, STORAGE_WIFI_MAX) == ESP_OK
            && n > 0)
            storage_wifi_write_raw(sc->u.nets, n);
        crypto_wipe(sc, sizeof(*sc));
    }
    char path[160];
    kw_path(KEYSTORE_SECRETS_ID, path, sizeof(path));
    remove(path);
    secrets_wipe_cache();
}

esp_err_t keystore_remove(const char *pin)
{
    if (!pin_ok(pin)) return ESP_ERR_INVALID_ARG;
    if (keystore_backoff_ms() > 0) return KEYSTORE_ERR_BACKOFF;

    esp_err_t e = ks_load();
    if (e != ESP_OK) return e;         /* NOT_FOUND / INVALID_CRC */

    /* Prove knowledge of the code even when already unlocked — removal
     * downgrades every key to plaintext, the store's whole point. */
    bool was_unlocked = s_ks.unlocked;
    uint8_t mk[32];
    int idx = -1;
    for (int i = 0; i < KS_SLOTS; i++) {
        const ks_slot_t *sl = &s_ks.hdr.slot[i];
        if (sl->type != KS_SLOT_PIN && sl->type != KS_SLOT_PASSPHRASE)
            continue;
        e = slot_unwrap_mk(i, pin, mk);
        if (e == ESP_ERR_NO_MEM) return e;
        if (e == ESP_OK) { idx = i; break; }
    }
    if (idx < 0) { bk_fail(); return ESP_FAIL; }
    bk_clear();
    memcpy(s_ks.mk, mk, 32);
    crypto_wipe(mk, sizeof(mk));
    s_ks.unlocked = true;

    /* The secrets bundle reverts to plaintext ini fields (hydrating load
     * + raw write), then its .kw1 goes away — BEFORE the key scan below,
     * so it never gets mistaken for a key to unwrap into a .pem. */
    secrets_restore_plaintext();

    /* Unwrap every keys/<id>.kw1 back to a bare .pem BEFORE dropping the
     * header — a key that fails to unwrap aborts while its plaintext is
     * still recoverable. A crash mid-way leaves .pem + store side by side;
     * the next unlock just adopts the .pem back (ks_adopt_plaintext). */
    char ids[16][STORAGE_KEY_ID_LEN];
    int n = 0;
    storage_scan_key_ext(".kw1", ids, 16, &n);

    char *buf = heap_caps_malloc(KW_CT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        if (!was_unlocked) keystore_lock();
        return ESP_ERR_NO_MEM;
    }

    e = ESP_OK;
    for (int i = 0; i < n; i++) {
        size_t len = 0;
        char path[160];
        e = keystore_unwrap(ids[i], buf, KW_CT_MAX, &len, NULL);
        if (e == ESP_OK) {
            snprintf(path, sizeof(path), "%s/keys/%s.pem",
                     storage_platform_mount_point(), ids[i]);
            e = ks_write_atomic(path, buf, len);
        }
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "Remove aborted: key '%s' failed (%d) — store kept",
                     ids[i], (int)e);
            break;
        }
        kw_path(ids[i], path, sizeof(path));
        remove(path);
    }
    crypto_wipe(buf, KW_CT_MAX);
    heap_caps_free(buf);

    if (e != ESP_OK) {
        if (!was_unlocked) keystore_lock();
        return e;
    }

    char path[160];
    ks_path(path, sizeof(path));
    remove(path);
    keystore_reset_cache();            /* wipe MK + forget header — ABSENT */
    ESP_LOGW(TAG, "Keystore removed — %d key(s) back to plaintext", n);
    return ESP_OK;
}
