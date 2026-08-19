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

/* ------------------------------------------------------------------
 * storage platform seam — throwaway directory, no walk-up magic
 * ---------------------------------------------------------------- */

#define TEST_MOUNT "ks_test_storage"

const char *storage_platform_mount_point(void) { return TEST_MOUNT; }

esp_err_t storage_platform_init(void)
{
    MKDIR(TEST_MOUNT);
    MKDIR(TEST_MOUNT "/keys");
    return ESP_OK;
}

/* ------------------------------------------------------------------
 * File helpers
 * ---------------------------------------------------------------- */

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

/* ------------------------------------------------------------------ */

void setUp(void)
{
    storage_factory_reset();   /* removes keystore.kv1 + keys/*, wipes MK */
}

void tearDown(void) {}

/* ------------------------------------------------------------------
 * Store lifecycle
 * ---------------------------------------------------------------- */

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

/* ------------------------------------------------------------------
 * Wrap / unwrap
 * ---------------------------------------------------------------- */

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

/* ------------------------------------------------------------------
 * PIN change
 * ---------------------------------------------------------------- */

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

    /* Key file untouched — only the 100 B slot was rewrapped */
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

/* ------------------------------------------------------------------
 * Adoption + storage integration
 * ---------------------------------------------------------------- */

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

/* ------------------------------------------------------------------
 * Remove (decommission back to plaintext)
 * ---------------------------------------------------------------- */

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

/* ------------------------------------------------------------------
 * Failed-attempt backoff
 * ---------------------------------------------------------------- */

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

/* ------------------------------------------------------------------ */

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
    return UNITY_END();
}
