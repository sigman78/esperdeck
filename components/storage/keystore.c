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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_random.h"
#endif

static const char *TAG = "keystore";

/* -------------------------------------------------------------------------
 * On-disk formats (docs/storage_auth.md)
 * ---------------------------------------------------------------------- */

#define KS_MAGIC        "CKS1"
#define KW_MAGIC        "CKW1"
#define KS_VERSION      1
#define KS_SLOTS        4
#define KS_SLOT_SIZE    100
#define KS_HDR_SIZE     (24 + KS_SLOTS * KS_SLOT_SIZE)   /* 424 */
#define KS_FILE         "keystore.kv1"

#define KW_HDR_SIZE     36                /* fixed part before ciphertext  */
#define KW_TAG_SIZE     16
#define KW_CT_MAX       16384             /* sanity cap on wrapped payload */

#define KS_SLOT_EMPTY       0
#define KS_SLOT_PIN         1
#define KS_SLOT_PASSPHRASE  2

#define KS_ALG_ARGON2ID     2

/* Argon2id defaults: 4 MiB work area, 2 passes, 1 lane — measured 1.09 s on
 * the S3 @ 240 MHz (CYBERDECK_BENCH_ARGON2 sweep: 4 MiB x1 = 645 ms, x2 =
 * 1090 ms, x3 = 1566 ms; 8 MiB is not allocatable). 4 MiB is the hardware
 * ceiling, so memory stays maxed and passes tune the time. Per-store params
 * in the header allow retuning without a format change. */
#define KS_ARGON2_BLOCKS_KIB  4096
#define KS_ARGON2_PASSES      2
#define KS_ARGON2_LANES       1

typedef struct {
    uint8_t  type;
    uint8_t  alg;
    uint32_t blocks_kib;
    uint32_t passes;
    uint8_t  lanes;
    uint8_t  pin_len;             /* auto-submit hint (byte 11, ex-reserved);
                                   * 0 for passphrase slots. NOT in the AAD —
                                   * tampering only breaks auto-submit UX  */
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

/* -------------------------------------------------------------------------
 * State — .bss lands in internal SRAM on the device, never PSRAM
 * ---------------------------------------------------------------------- */

static struct {
    bool        hdr_loaded;
    bool        unlocked;
    ks_header_t hdr;
    uint8_t     mk[32];
} s_ks;

/* -------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------- */

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

/* -------------------------------------------------------------------------
 * Header (de)serialization — explicit offsets, no struct punning
 * ---------------------------------------------------------------------- */

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

/* Try to unwrap the MK from slot @idx with @pin into @mk_out.
 * ESP_FAIL = tag mismatch (wrong PIN for this slot). */
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

/* -------------------------------------------------------------------------
 * Public API — store lifecycle
 * ---------------------------------------------------------------------- */

keystore_state_t keystore_state(void)
{
    if (s_ks.unlocked) return KEYSTORE_UNLOCKED;
    esp_err_t e = ks_load();
    if (e == ESP_ERR_NOT_FOUND) return KEYSTORE_ABSENT;
    return KEYSTORE_LOCKED;      /* present (or present-but-corrupt) */
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
    ESP_LOGI(TAG, "Keystore created (slot 0: %s)",
             s_ks.hdr.slot[0].type == KS_SLOT_PIN ? "pin" : "passphrase");
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
        remove(path);
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
            ks_adopt_plaintext();
            return ESP_OK;
        }
    }
    crypto_wipe(s_ks.mk, sizeof(s_ks.mk));
    return ESP_FAIL;              /* wrong PIN: no slot's tag verified */
}

void keystore_lock(void)
{
    crypto_wipe(s_ks.mk, sizeof(s_ks.mk));
    s_ks.unlocked = false;
}

void keystore_reset_cache(void)
{
    crypto_wipe(s_ks.mk, sizeof(s_ks.mk));
    memset(&s_ks, 0, sizeof(s_ks));
}

esp_err_t keystore_change_pin(const char *old_pin, const char *new_pin)
{
    if (!pin_ok(old_pin) || !pin_ok(new_pin)) return ESP_ERR_INVALID_ARG;

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
    if (idx < 0) return ESP_FAIL;

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

/* -------------------------------------------------------------------------
 * Public API — wrapped key files
 * ---------------------------------------------------------------------- */

static bool key_id_ok(const char *key_id)
{
    if (!key_id || !key_id[0]) return false;
    if (strlen(key_id) >= STORAGE_KEY_ID_LEN) return false;
    if (strchr(key_id, '/') || strchr(key_id, '\\')) return false;
    return true;
}

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
    if (!key_id_ok(key_id) || !plaintext || len == 0 || len > KW_CT_MAX)
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
    if (!key_id_ok(key_id) || !buf || !written) return ESP_ERR_INVALID_ARG;
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
    if (!key_id_ok(key_id)) return false;
    char path[160];
    kw_path(key_id, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}
