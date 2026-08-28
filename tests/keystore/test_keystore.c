/*
 * test_keystore.c — unit tests for the PIN-unlocked wrapped key store
 * (components/storage/keystore.c) and its storage.c integration.
 *
 * Runs against a throwaway ks_test_storage/ directory. Every test starts
 * from a factory-reset state (no store, no keys, MK wiped).
 */

#include "unity.h"
#include "keystore.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

/* Test storage platform seam: a throwaway directory, no walk-up discovery. */

#define TEST_MOUNT "ks_test_storage"

const char *storage_platform_mount_point(void) { return TEST_MOUNT; }

esp_err_t storage_platform_init(void)
{
    MKDIR(TEST_MOUNT);
    MKDIR(TEST_MOUNT "/keys");
    return ESP_OK;
}

static long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

static int read_file(const char *path, unsigned char *buf, size_t bufsz,
                     size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    *len = fread(buf, 1, bufsz, f);
    fclose(f);
    return 0;
}

static int write_file(const char *path, const void *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(buf, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

static void flip_byte(const char *path, long offset)
{
    static unsigned char buf[32768];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(0, read_file(path, buf, sizeof(buf), &len));
    TEST_ASSERT_TRUE((long)len > offset);
    buf[offset] ^= 0xff;
    TEST_ASSERT_EQUAL_INT(0, write_file(path, buf, len));
}

static void copy_file(const char *src, const char *dst)
{
    static unsigned char buf[32768];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(0, read_file(src, buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_INT(0, write_file(dst, buf, len));
}

#define KW1(id)  TEST_MOUNT "/keys/" id ".kw1"
#define PEM(id)  TEST_MOUNT "/keys/" id ".pem"
#define KV1      TEST_MOUNT "/keystore.kv1"

static const char TEST_PEM[] =
    "-----BEGIN OPENSSH PRIVATE KEY-----\n"
    "b3BlbnNzaC1rZXktdjEAAAAABG5vbmUAAAAEbm9uZQAAAAAAAAABAAAAMwAAAAtzc2gtZW\n"
    "QyNTUxOQAAACBOTA5ISCJNU1FURVNUS0VZTk9UUkVBTNOTFAKEDATAAAA\n"
    "-----END OPENSSH PRIVATE KEY-----\n";

void setUp(void)
{
    storage_factory_reset();   /* removes keystore.kv1 + keys/*, wipes MK */
}

void tearDown(void) {}

static void test_absent_store_is_feature_off(void)
{
    TEST_ASSERT_EQUAL_INT(KEYSTORE_ABSENT, keystore_state());
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND, keystore_unlock("1234"));

    /* Plaintext passthrough: storage behaves exactly as before */
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        storage_set_key("plain", TEST_PEM, strlen(TEST_PEM)));
    TEST_ASSERT_TRUE(file_size(PEM("plain")) > 0);       /* .pem, not .kw1 */
    TEST_ASSERT_EQUAL_INT(-1, file_size(KW1("plain")));

    char buf[4096];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_get_key("plain", buf,
                                                  sizeof(buf), &n));
    TEST_ASSERT_EQUAL_size_t(strlen(TEST_PEM), n);
    TEST_ASSERT_EQUAL_STRING(TEST_PEM, buf);
}

static void test_create_persists_and_unlocks(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    TEST_ASSERT_EQUAL_INT(KEYSTORE_UNLOCKED, keystore_state());
    TEST_ASSERT_EQUAL_INT(424, (int)file_size(KV1));   /* documented layout */

    /* Creating over an existing store must refuse */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE, keystore_create("9999"));

    keystore_lock();
    TEST_ASSERT_EQUAL_INT(KEYSTORE_LOCKED, keystore_state());

    /* Survives a "reboot" (cache reset re-reads the file) */
    keystore_reset_cache();
    TEST_ASSERT_EQUAL_INT(KEYSTORE_LOCKED, keystore_state());
}

static void test_unlock_wrong_then_right(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    keystore_lock();

    clock_t t0 = clock();
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, keystore_unlock("4321"));
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    printf("  [info] wrong-PIN attempt: %.0f ms (Argon2id 4MiB x3)\n", ms);
    TEST_ASSERT_EQUAL_INT(KEYSTORE_LOCKED, keystore_state());

    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_unlock("1234"));
    TEST_ASSERT_EQUAL_INT(KEYSTORE_UNLOCKED, keystore_state());
}

static void test_bad_pins_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, keystore_create(""));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, keystore_create(NULL));
    char long_pin[80];
    memset(long_pin, '7', sizeof(long_pin) - 1);
    long_pin[sizeof(long_pin) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, keystore_create(long_pin));
}

static void test_corrupt_header_distinguished_from_wrong_pin(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    keystore_lock();
    keystore_reset_cache();

    flip_byte(KV1, 0);                     /* magic */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_CRC, keystore_unlock("1234"));
}

static void test_wrap_unwrap_roundtrip_binary(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));

    unsigned char blob[517];
    for (size_t i = 0; i < sizeof(blob); i++) blob[i] = (unsigned char)(i * 7);
    blob[0] = 0; blob[100] = 0;            /* embedded NULs must survive */

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_wrap("blob", 42, blob, sizeof(blob)));
    TEST_ASSERT_TRUE(keystore_is_wrapped("blob"));
    /* 52 B overhead: 36 header + 16 tag */
    TEST_ASSERT_EQUAL_INT((int)sizeof(blob) + 52, (int)file_size(KW1("blob")));

    unsigned char out[1024];
    size_t n = 0;
    uint8_t ctype = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_unwrap("blob", out, sizeof(out), &n, &ctype));
    TEST_ASSERT_EQUAL_size_t(sizeof(blob), n);
    TEST_ASSERT_EQUAL_UINT8(42, ctype);
    TEST_ASSERT_EQUAL_MEMORY(blob, out, sizeof(blob));

    /* Too-small buffer is a clean error, not truncation */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE,
        keystore_unwrap("blob", out, sizeof(blob) - 1, &n, NULL));
}

static void test_locked_store_refuses_key_ops(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_wrap("k", KEYSTORE_CONTENT_PEM, TEST_PEM, strlen(TEST_PEM)));
    keystore_lock();

    char out[4096];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE,
        keystore_unwrap("k", out, sizeof(out), &n, NULL));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE,
        keystore_wrap("k2", KEYSTORE_CONTENT_PEM, TEST_PEM, 10));

    /* storage_get_key surfaces the locked error for the unlock screen */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE,
        storage_get_key("k", out, sizeof(out), &n));
}

static void test_tampered_ciphertext_fails(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_wrap("k", KEYSTORE_CONTENT_PEM, TEST_PEM, strlen(TEST_PEM)));

    flip_byte(KW1("k"), 36);               /* first ciphertext byte */

    char out[4096];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_CRC,
        keystore_unwrap("k", out, sizeof(out), &n, NULL));
}

static void test_rename_attack_killed_by_aad(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_wrap("bandit", KEYSTORE_CONTENT_PEM,
                      TEST_PEM, strlen(TEST_PEM)));

    copy_file(KW1("bandit"), KW1("opnsense"));   /* the doc's example */

    char out[4096];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_unwrap("bandit", out, sizeof(out), &n, NULL));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_CRC,
        keystore_unwrap("opnsense", out, sizeof(out), &n, NULL));
}

static void test_cross_store_transplant_rejected(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_wrap("k", KEYSTORE_CONTENT_PEM, TEST_PEM, strlen(TEST_PEM)));

    static unsigned char kw1[8192];
    size_t kw1_len = 0;
    TEST_ASSERT_EQUAL_INT(0, read_file(KW1("k"), kw1, sizeof(kw1), &kw1_len));

    /* New store, same PIN — different uuid and MK */
    remove(KV1);
    keystore_reset_cache();
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    TEST_ASSERT_EQUAL_INT(0, write_file(KW1("k"), kw1, kw1_len));

    char out[4096];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_CRC,
        keystore_unwrap("k", out, sizeof(out), &n, NULL));
}

static void test_change_pin_rewraps_slot_only(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_wrap("k", KEYSTORE_CONTENT_PEM, TEST_PEM, strlen(TEST_PEM)));
    keystore_lock();

    static unsigned char before[8192];
    size_t before_len = 0;
    TEST_ASSERT_EQUAL_INT(0,
        read_file(KW1("k"), before, sizeof(before), &before_len));

    TEST_ASSERT_EQUAL_INT(ESP_FAIL, keystore_change_pin("9999", "5678"));
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_change_pin("1234", "5678"));
    /* change_pin on a locked store must not leave it unlocked */
    TEST_ASSERT_EQUAL_INT(KEYSTORE_LOCKED, keystore_state());

    /* keystore_change_pin rewraps only the 100 B slot; the key file stays untouched. */
    static unsigned char after[8192];
    size_t after_len = 0;
    TEST_ASSERT_EQUAL_INT(0,
        read_file(KW1("k"), after, sizeof(after), &after_len));
    TEST_ASSERT_EQUAL_size_t(before_len, after_len);
    TEST_ASSERT_EQUAL_MEMORY(before, after, before_len);

    TEST_ASSERT_EQUAL_INT(ESP_FAIL, keystore_unlock("1234"));
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_unlock("5678"));

    char out[4096];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_unwrap("k", out, sizeof(out), &n, NULL));
    TEST_ASSERT_EQUAL_size_t(strlen(TEST_PEM), n);
}

static void test_pin_len_hint_for_autosubmit(void)
{
    TEST_ASSERT_EQUAL_INT(0, keystore_pin_len());          /* absent store  */
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    keystore_lock();
    keystore_reset_cache();                                /* "reboot"      */
    TEST_ASSERT_EQUAL_INT(4, keystore_pin_len());          /* works LOCKED  */

    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_change_pin("1234", "567890"));
    TEST_ASSERT_EQUAL_INT(6, keystore_pin_len());

    /* Passphrase slot: no hint — the unlock UI submits on Enter alone */
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_change_pin("567890", "open sesame"));
    TEST_ASSERT_EQUAL_INT(0, keystore_pin_len());
}

static void test_adopt_plaintext_on_unlock(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    keystore_lock();

    /* Bare .pem lands next to a locked store (e.g. pre-store leftovers) */
    TEST_ASSERT_EQUAL_INT(0,
        write_file(PEM("legacy"), TEST_PEM, strlen(TEST_PEM)));

    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_unlock("1234"));

    TEST_ASSERT_EQUAL_INT(-1, file_size(PEM("legacy")));    /* gone     */
    TEST_ASSERT_TRUE(keystore_is_wrapped("legacy"));        /* wrapped  */

    char out[4096];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        storage_get_key("legacy", out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_STRING(TEST_PEM, out);
}

static void test_storage_set_key_wraps_when_unlocked(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));

    TEST_ASSERT_EQUAL_INT(ESP_OK,
        storage_set_key("k", TEST_PEM, strlen(TEST_PEM)));
    TEST_ASSERT_EQUAL_INT(-1, file_size(PEM("k")));    /* no plaintext */
    TEST_ASSERT_TRUE(keystore_is_wrapped("k"));

    char out[4096];
    size_t n = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_get_key("k", out, sizeof(out), &n));
    TEST_ASSERT_EQUAL_size_t(strlen(TEST_PEM), n);
    TEST_ASSERT_EQUAL_STRING(TEST_PEM, out);
}

static void test_list_keys_unions_pem_and_kw1(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_wrap("wrapped", KEYSTORE_CONTENT_PEM,
                      TEST_PEM, strlen(TEST_PEM)));
    keystore_lock();
    TEST_ASSERT_EQUAL_INT(0,
        write_file(PEM("plain"), TEST_PEM, strlen(TEST_PEM)));
    /* Same stem in both forms must appear once */
    TEST_ASSERT_EQUAL_INT(0,
        write_file(PEM("wrapped"), TEST_PEM, strlen(TEST_PEM)));

    char ids[8][STORAGE_KEY_ID_LEN];
    int count = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_list_keys(ids, 8, &count));
    TEST_ASSERT_EQUAL_INT(2, count);
    TEST_ASSERT_EQUAL_STRING("plain",   ids[0]);   /* sorted */
    TEST_ASSERT_EQUAL_STRING("wrapped", ids[1]);
}

static void test_delete_key_removes_kw1(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_wrap("k", KEYSTORE_CONTENT_PEM, TEST_PEM, strlen(TEST_PEM)));
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_delete_key("k"));
    TEST_ASSERT_FALSE(keystore_is_wrapped("k"));
}

static void test_factory_reset_removes_store(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_wrap("k", KEYSTORE_CONTENT_PEM, TEST_PEM, strlen(TEST_PEM)));

    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_factory_reset());

    TEST_ASSERT_EQUAL_INT(KEYSTORE_ABSENT, keystore_state());
    TEST_ASSERT_EQUAL_INT(-1, file_size(KV1));
    TEST_ASSERT_FALSE(keystore_is_wrapped("k"));

    char ids[8][STORAGE_KEY_ID_LEN];
    int count = -1;
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_list_keys(ids, 8, &count));
    TEST_ASSERT_EQUAL_INT(0, count);
}

static void test_remove_restores_plaintext(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_wrap("id_rm", KEYSTORE_CONTENT_PEM,
                      TEST_PEM, sizeof(TEST_PEM) - 1));
    keystore_lock();

    /* Wrong code refuses; store and wrapped key untouched */
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, keystore_remove("9999"));
    TEST_ASSERT_EQUAL_INT(KEYSTORE_LOCKED, keystore_state());
    TEST_ASSERT_TRUE(file_size(KW1("id_rm")) > 0);

    /* Right code while LOCKED: store gone, key back byte-identical */
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_remove("1234"));
    TEST_ASSERT_EQUAL_INT(KEYSTORE_ABSENT, keystore_state());
    TEST_ASSERT_EQUAL_INT(-1, file_size(KV1));
    TEST_ASSERT_EQUAL_INT(-1, file_size(KW1("id_rm")));

    static unsigned char buf[4096];
    size_t len = 0;
    TEST_ASSERT_EQUAL_INT(0, read_file(PEM("id_rm"), buf, sizeof(buf), &len));
    TEST_ASSERT_EQUAL_UINT(sizeof(TEST_PEM) - 1, len);
    TEST_ASSERT_EQUAL_MEMORY(TEST_PEM, buf, len);
}

static void test_create_adopts_existing_plaintext(void)
{
    /* A fresh store must protect keys already on disk — adoption cannot
     * wait for the first unlock, which the lazy trigger may never fire. */
    TEST_ASSERT_EQUAL_INT(0,
        write_file(PEM("id_pre"), TEST_PEM, sizeof(TEST_PEM) - 1));
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    TEST_ASSERT_TRUE(keystore_is_wrapped("id_pre"));
    TEST_ASSERT_EQUAL_INT(-1, file_size(PEM("id_pre")));   /* shredded */

    static char buf[4096];
    size_t n = 0;
    uint8_t ctype = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_unwrap("id_pre", buf, sizeof(buf), &n, &ctype));
    TEST_ASSERT_EQUAL_UINT(sizeof(TEST_PEM) - 1, n);
    TEST_ASSERT_EQUAL_MEMORY(TEST_PEM, buf, n);
}

static void test_remove_requires_code_even_when_unlocked(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));  /* UNLOCKED */
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, keystore_remove("0000"));
    TEST_ASSERT_EQUAL_INT(KEYSTORE_UNLOCKED, keystore_state());  /* kept */
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_remove("1234"));
    TEST_ASSERT_EQUAL_INT(KEYSTORE_ABSENT, keystore_state());
}

static void test_secrets_roundtrip(void)
{
    char val[64];

    /* Locked / absent store refuses */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE,
                          keystore_secret_get("profile:a", val, sizeof(val)));
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));

    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND,
                          keystore_secret_get("profile:a", val, sizeof(val)));
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_secret_set("profile:a", "hunter2"));
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_secret_set("wifi:HomeAP", "psk-1"));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          keystore_secret_get("profile:a", val, sizeof(val)));
    TEST_ASSERT_EQUAL_STRING("hunter2", val);

    /* Replace + persistence across lock/unlock */
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_secret_set("profile:a", "swordfish"));
    keystore_lock();
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE,
                          keystore_secret_get("profile:a", val, sizeof(val)));
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_unlock("1234"));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          keystore_secret_get("profile:a", val, sizeof(val)));
    TEST_ASSERT_EQUAL_STRING("swordfish", val);
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          keystore_secret_get("wifi:HomeAP", val, sizeof(val)));
    TEST_ASSERT_EQUAL_STRING("psk-1", val);

    /* Remove entry; emptied bundle drops the file */
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_secret_set("profile:a", NULL));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND,
                          keystore_secret_get("profile:a", val, sizeof(val)));
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_secret_set("wifi:HomeAP", ""));
    TEST_ASSERT_EQUAL_INT(-1, file_size(KW1("secrets")));
}

static void test_profiles_divert_and_hydrate(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));

    conn_profile_t p;
    memset(&p, 0, sizeof(p));
    snprintf(p.name, sizeof(p.name), "alpha");
    snprintf(p.host, sizeof(p.host), "10.0.0.1");
    snprintf(p.user, sizeof(p.user), "root");
    p.port = 22;
    p.auth = STORAGE_AUTH_PASSWORD;
    snprintf(p.password, sizeof(p.password), "hunter2");

    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_save_profiles(&p, 1));

    /* The ini on flash carries the @bundle marker, never the plaintext */
    static unsigned char raw[2048];
    size_t rn = 0;
    TEST_ASSERT_EQUAL_INT(0,
        read_file(TEST_MOUNT "/profiles.ini", raw, sizeof(raw) - 1, &rn));
    raw[rn] = 0;
    TEST_ASSERT_NULL(strstr((char *)raw, "hunter2"));
    TEST_ASSERT_NOT_NULL(strstr((char *)raw, "password=" STORAGE_PW_BUNDLED));
    TEST_ASSERT_TRUE(file_size(KW1("secrets")) > 0);

    /* ...but an unlocked load hydrates it back */
    conn_profile_t got[STORAGE_MAX_PROFILES];
    int n = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        storage_load_profiles(got, &n, STORAGE_MAX_PROFILES));
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("hunter2", got[0].password);

    /* Locked load: metadata + the marker (visibly "not usable yet",
     * distinguishable from a genuinely empty password) */
    keystore_lock();
    n = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        storage_load_profiles(got, &n, STORAGE_MAX_PROFILES));
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING(STORAGE_PW_BUNDLED, got[0].password);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", got[0].host);
}

static void test_wifi_plaintext_without_store(void)
{
    /* No keystore at all: WiFi persistence is bit-identical plaintext —
     * the store-less deck must keep connecting at boot. */
    wifi_profile_t w;
    memset(&w, 0, sizeof(w));
    snprintf(w.ssid, sizeof(w.ssid), "HomeAP");
    snprintf(w.password, sizeof(w.password), "plainpsk");
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_wifi_save(&w, 1));

    static unsigned char raw[1024];
    size_t rn = 0;
    TEST_ASSERT_EQUAL_INT(0,
        read_file(TEST_MOUNT "/wifi.ini", raw, sizeof(raw) - 1, &rn));
    raw[rn] = 0;
    TEST_ASSERT_NOT_NULL(strstr((char *)raw, "password=plainpsk"));

    wifi_profile_t got[STORAGE_WIFI_MAX];
    int n = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        storage_wifi_load(got, &n, STORAGE_WIFI_MAX));
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("plainpsk", got[0].password);
}

static void test_wifi_divert_and_hydrate(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    wifi_profile_t w;
    memset(&w, 0, sizeof(w));
    snprintf(w.ssid, sizeof(w.ssid), "HomeAP");
    snprintf(w.password, sizeof(w.password), "secretpsk");
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_wifi_save(&w, 1));

    static unsigned char raw[1024];
    size_t rn = 0;
    TEST_ASSERT_EQUAL_INT(0,
        read_file(TEST_MOUNT "/wifi.ini", raw, sizeof(raw) - 1, &rn));
    raw[rn] = 0;
    TEST_ASSERT_NULL(strstr((char *)raw, "secretpsk"));
    TEST_ASSERT_NOT_NULL(strstr((char *)raw, "password=" STORAGE_PW_BUNDLED));

    wifi_profile_t got[STORAGE_WIFI_MAX];
    int n = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        storage_wifi_load(got, &n, STORAGE_WIFI_MAX));
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("secretpsk", got[0].password);

    /* Locked: the marker shows, never the PSK, never a fake open net */
    keystore_lock();
    n = 0;
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        storage_wifi_load(got, &n, STORAGE_WIFI_MAX));
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING(STORAGE_PW_BUNDLED, got[0].password);
}

static void test_secrets_adoption_from_plaintext_ini(void)
{
    /* Plaintext-era inis (written store-less) adopt at store creation */
    static const char INI[] =
        "[bandit]\nhost=h1\nport=22\nuser=u\nauth=password\n"
        "password=oldpass\n\n";
    TEST_ASSERT_EQUAL_INT(0,
        write_file(TEST_MOUNT "/profiles.ini", INI, sizeof(INI) - 1));
    static const char WIFI[] = "[net0]\nssid=HomeAP\npassword=wifipass\n\n";
    TEST_ASSERT_EQUAL_INT(0,
        write_file(TEST_MOUNT "/wifi.ini", WIFI, sizeof(WIFI) - 1));

    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));

    static unsigned char raw[2048];
    size_t rn = 0;
    TEST_ASSERT_EQUAL_INT(0,
        read_file(TEST_MOUNT "/profiles.ini", raw, sizeof(raw) - 1, &rn));
    raw[rn] = 0;
    TEST_ASSERT_NULL(strstr((char *)raw, "oldpass"));
    rn = 0;
    TEST_ASSERT_EQUAL_INT(0,
        read_file(TEST_MOUNT "/wifi.ini", raw, sizeof(raw) - 1, &rn));
    raw[rn] = 0;
    TEST_ASSERT_NULL(strstr((char *)raw, "wifipass"));

    char val[64];
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_secret_get("profile:bandit", val, sizeof(val)));
    TEST_ASSERT_EQUAL_STRING("oldpass", val);
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_secret_get("wifi:HomeAP", val, sizeof(val)));
    TEST_ASSERT_EQUAL_STRING("wifipass", val);
}

static void test_remove_restores_secret_plaintext(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    conn_profile_t p;
    memset(&p, 0, sizeof(p));
    snprintf(p.name, sizeof(p.name), "alpha");
    snprintf(p.host, sizeof(p.host), "h");
    p.port = 22;
    p.auth = STORAGE_AUTH_PASSWORD;
    snprintf(p.password, sizeof(p.password), "hunter2");
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_save_profiles(&p, 1));

    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_remove("1234"));

    static unsigned char raw[2048];
    size_t rn = 0;
    TEST_ASSERT_EQUAL_INT(0,
        read_file(TEST_MOUNT "/profiles.ini", raw, sizeof(raw) - 1, &rn));
    raw[rn] = 0;
    TEST_ASSERT_NOT_NULL(strstr((char *)raw, "password=hunter2"));
    TEST_ASSERT_EQUAL_INT(-1, file_size(KW1("secrets")));
}

static void test_secrets_prune_and_list_exclusion(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    conn_profile_t p[2];
    memset(p, 0, sizeof(p));
    snprintf(p[0].name, sizeof(p[0].name), "keep");
    snprintf(p[1].name, sizeof(p[1].name), "gone");
    for (int i = 0; i < 2; i++) {
        p[i].port = 22;
        p[i].auth = STORAGE_AUTH_PASSWORD;
        snprintf(p[i].host, sizeof(p[i].host), "h%d", i);
        snprintf(p[i].password, sizeof(p[i].password), "pw%d", i);
    }
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_save_profiles(p, 2));

    /* The bundle never shows up as a key */
    char ids[8][STORAGE_KEY_ID_LEN];
    int count = -1;
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_list_keys(ids, 8, &count));
    for (int i = 0; i < count; i++)
        TEST_ASSERT_TRUE(strcmp(ids[i], "secrets") != 0);

    /* storage_save_profiles drops the "gone" secret on resave; "keep" survives. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_save_profiles(p, 1));
    char val[64];
    TEST_ASSERT_EQUAL_INT(ESP_OK,
        keystore_secret_get("profile:keep", val, sizeof(val)));
    TEST_ASSERT_EQUAL_STRING("pw0", val);
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND,
        keystore_secret_get("profile:gone", val, sizeof(val)));
}

static uint64_t s_fake_ms;
static uint64_t fake_uptime(void) { return s_fake_ms; }

static void test_backoff_after_failures(void)
{
    keystore_set_uptime_hook(fake_uptime);
    s_fake_ms = 1000;

    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    keystore_lock();

    /* The first four failures are free */
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT(0, keystore_backoff_ms());
        TEST_ASSERT_EQUAL_INT(ESP_FAIL, keystore_unlock("0000"));
    }
    TEST_ASSERT_EQUAL_UINT(0, keystore_backoff_ms());

    /* The 5th arms 30 s and blocks EVERY verifying op — right PIN too */
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, keystore_unlock("0000"));
    TEST_ASSERT_TRUE(keystore_backoff_ms() > 0);
    TEST_ASSERT_EQUAL_INT(KEYSTORE_ERR_BACKOFF, keystore_unlock("1234"));
    TEST_ASSERT_EQUAL_INT(KEYSTORE_ERR_BACKOFF,
                          keystore_change_pin("1234", "5678"));
    TEST_ASSERT_EQUAL_INT(KEYSTORE_ERR_BACKOFF, keystore_remove("1234"));

    s_fake_ms += 30000;                        /* the wait passes */
    TEST_ASSERT_EQUAL_UINT(0, keystore_backoff_ms());

    /* The 6th failure doubles the wait */
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, keystore_unlock("0000"));
    uint32_t d = keystore_backoff_ms();
    TEST_ASSERT_TRUE(d > 30000 && d <= 60000);

    /* Success clears the counter; failures are free again */
    s_fake_ms += 60000;
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_unlock("1234"));
    TEST_ASSERT_EQUAL_UINT(0, keystore_backoff_ms());
    keystore_lock();
    TEST_ASSERT_EQUAL_INT(ESP_FAIL, keystore_unlock("0000"));
    TEST_ASSERT_EQUAL_UINT(0, keystore_backoff_ms());

    keystore_set_uptime_hook(NULL);
}

static void test_backoff_survives_reboot(void)
{
    keystore_set_uptime_hook(fake_uptime);
    s_fake_ms = 1000;
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));
    keystore_lock();
    for (int i = 0; i < 5; i++)
        TEST_ASSERT_EQUAL_INT(ESP_FAIL, keystore_unlock("0000"));
    TEST_ASSERT_TRUE(keystore_backoff_ms() > 0);

    /* "Reboot": cached state dropped, uptime restarts at zero — the
     * persisted counter re-arms the CURRENT delay in full, so
     * power-cycling never skips the wait. */
    keystore_reset_cache();
    s_fake_ms = 0;
    TEST_ASSERT_TRUE(keystore_backoff_ms() >= 29000);
    TEST_ASSERT_EQUAL_INT(KEYSTORE_ERR_BACKOFF, keystore_unlock("1234"));
    s_fake_ms += 30000;
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_unlock("1234"));

    keystore_set_uptime_hook(NULL);
}

/* A key id reaching a filename must never escape keys/. These values come
 * back from profiles.ini, which a hand edit or a restored backup controls —
 * unguarded, storage_delete_key() was an arbitrary unlink. */
static void test_key_id_path_traversal_rejected(void)
{
    static const char *const EVIL[] = {
        "../../keystore", "../secrets", "a/b", "a\\b", "C:evil", "",
    };
    char   buf[128];
    size_t got = 0;

    for (int i = 0; i < (int)(sizeof(EVIL) / sizeof(EVIL[0])); i++) {
        TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                              storage_set_key(EVIL[i], "x", 1));
        TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                              storage_get_key(EVIL[i], buf, sizeof(buf), &got));
        TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                              storage_delete_key(EVIL[i]));
        TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                              storage_key_info(EVIL[i], NULL, 0, NULL, 0));
        TEST_ASSERT_FALSE(keystore_is_wrapped(EVIL[i]));
    }
    /* storage_set_key rejects an over-long id too; letting it through would truncate to a colliding id. */
    char toolong[STORAGE_KEY_ID_LEN + 8];
    memset(toolong, 'k', sizeof(toolong) - 1);
    toolong[sizeof(toolong) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG,
                          storage_set_key(toolong, "x", 1));

    /* ...and an ordinary id still works, so the gate isn't over-tight */
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_set_key("ok-id_1", "pem", 3));
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          storage_get_key("ok-id_1", buf, sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT(3, got);
}

/* A failed set must leave the cache exactly as it found it.
 * Cutting the old entry before checking whether the new value fits
 * would silently lose that line. The next successful set would then
 * write the loss through to flash. */
static void test_secret_set_overflow_leaves_cache_intact(void)
{
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_create("1234"));

    char big[512];
    memset(big, 'v', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    /* Fill most of the 2048-byte bundle, then keep "victim" as the entry
     * whose survival we care about. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_secret_set("pad1", big));
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_secret_set("pad2", big));
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_secret_set("pad3", big));
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_secret_set("victim", "keepme"));

    /* Replacing "victim" with something too large must fail cleanly.
     * The six bytes it frees can't cover the 512 bytes it wants. */
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NO_MEM, keystore_secret_set("victim", big));

    char out[64];
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_secret_get("victim", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("keepme", out);

    /* The damage surfaces later: a LATER successful set persists the cache.
     * Victim must survive a store-and-reload round trip. */
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_secret_set("pad3", NULL)); /* remove */
    keystore_lock();
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_unlock("1234"));
    TEST_ASSERT_EQUAL_INT(ESP_OK, keystore_secret_get("victim", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("keepme", out);
}

int main(void)
{
    storage_init();

    UNITY_BEGIN();
    RUN_TEST(test_absent_store_is_feature_off);
    RUN_TEST(test_create_persists_and_unlocks);
    RUN_TEST(test_unlock_wrong_then_right);
    RUN_TEST(test_bad_pins_rejected);
    RUN_TEST(test_corrupt_header_distinguished_from_wrong_pin);
    RUN_TEST(test_wrap_unwrap_roundtrip_binary);
    RUN_TEST(test_locked_store_refuses_key_ops);
    RUN_TEST(test_tampered_ciphertext_fails);
    RUN_TEST(test_rename_attack_killed_by_aad);
    RUN_TEST(test_cross_store_transplant_rejected);
    RUN_TEST(test_change_pin_rewraps_slot_only);
    RUN_TEST(test_pin_len_hint_for_autosubmit);
    RUN_TEST(test_adopt_plaintext_on_unlock);
    RUN_TEST(test_storage_set_key_wraps_when_unlocked);
    RUN_TEST(test_list_keys_unions_pem_and_kw1);
    RUN_TEST(test_delete_key_removes_kw1);
    RUN_TEST(test_factory_reset_removes_store);
    RUN_TEST(test_remove_restores_plaintext);
    RUN_TEST(test_create_adopts_existing_plaintext);
    RUN_TEST(test_remove_requires_code_even_when_unlocked);
    RUN_TEST(test_backoff_after_failures);
    RUN_TEST(test_backoff_survives_reboot);
    RUN_TEST(test_secrets_roundtrip);
    RUN_TEST(test_profiles_divert_and_hydrate);
    RUN_TEST(test_wifi_plaintext_without_store);
    RUN_TEST(test_wifi_divert_and_hydrate);
    RUN_TEST(test_secrets_adoption_from_plaintext_ini);
    RUN_TEST(test_remove_restores_secret_plaintext);
    RUN_TEST(test_secrets_prune_and_list_exclusion);
    RUN_TEST(test_key_id_path_traversal_rejected);
    RUN_TEST(test_secret_set_overflow_leaves_cache_intact);
    return UNITY_END();
}
