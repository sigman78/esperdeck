/*
 * test_storage_kv.c — unit tests for the generic key=value settings API
 * (storage_kv.h): table-driven load/save, defaults, accept-ranges, the
 * atomic-write pair, and the factory-reset registry. Runs against the
 * inert keystore stub — the barest context the public API must serve
 * (docs/extensibility.md, phase 1).
 */

#include "unity.h"
#include "storage.h"
#include "storage_kv.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

/* ------------------------------------------------------------------
 * storage platform seam — throwaway directory
 * ---------------------------------------------------------------- */

#define TEST_MOUNT "kv_test_storage"

const char *storage_platform_mount_point(void) { return TEST_MOUNT; }

esp_err_t storage_platform_init(void)
{
    MKDIR(TEST_MOUNT);
    MKDIR(TEST_MOUNT "/keys");     /* factory reset walks it */
    return ESP_OK;
}

/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t  n8;
    uint16_t n16;
    uint32_t n32;
    bool     flag;
    char     name[8];
} cfg_t;

static const storage_kv_field_t FIELDS[] = {
    { "n8",   offsetof(cfg_t, n8),   STORAGE_KV_U8,   0, 0, 0 },
    { "n16",  offsetof(cfg_t, n16),  STORAGE_KV_U16,  0, 0, 0 },
    { "n32",  offsetof(cfg_t, n32),  STORAGE_KV_U32,  0, 0, 0 },
    { "flag", offsetof(cfg_t, flag), STORAGE_KV_BOOL, 0, 0, 0 },
    { "name", offsetof(cfg_t, name), STORAGE_KV_STR,
      sizeof(((cfg_t *)0)->name), 0, 0 },
    { NULL, 0, 0, 0, 0, 0 },
};

static const cfg_t DEFAULTS = { 7, 700, 70000, true, "dflt" };

#define KV_FILE "kvtest.ini"
#define KV_PATH TEST_MOUNT "/" KV_FILE

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    TEST_ASSERT_NOT_NULL(f);
    fputs(text, f);
    fclose(f);
}

void setUp(void)    { remove(KV_PATH); }
void tearDown(void) {}

/* ------------------------------------------------------------------ */

static void test_roundtrip_all_types(void)
{
    cfg_t c = { 255, 65535, 4000000000u, false, "abcdefg" };
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_kv_save(KV_FILE, NULL, FIELDS, &c));

    cfg_t r = DEFAULTS;
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_kv_load(KV_FILE, NULL, FIELDS, &r));
    TEST_ASSERT_EQUAL_UINT8(255, r.n8);
    TEST_ASSERT_EQUAL_UINT16(65535, r.n16);
    TEST_ASSERT_EQUAL_UINT32(4000000000u, r.n32);
    TEST_ASSERT_FALSE(r.flag);
    TEST_ASSERT_EQUAL_STRING("abcdefg", r.name);
}

static void test_absent_file_not_found_defaults_kept(void)
{
    cfg_t r = DEFAULTS;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND,
                          storage_kv_load(KV_FILE, NULL, FIELDS, &r));
    TEST_ASSERT_EQUAL_UINT8(DEFAULTS.n8, r.n8);
    TEST_ASSERT_EQUAL_UINT32(DEFAULTS.n32, r.n32);
    TEST_ASSERT_EQUAL_STRING(DEFAULTS.name, r.name);
}

static void test_missing_and_unknown_keys(void)
{
    write_text(KV_PATH, "# comment\n[sect]\nn16=42\nbogus=9\n");
    cfg_t r = DEFAULTS;
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_kv_load(KV_FILE, NULL, FIELDS, &r));
    TEST_ASSERT_EQUAL_UINT16(42, r.n16);               /* from the file */
    TEST_ASSERT_EQUAL_UINT8(DEFAULTS.n8, r.n8);        /* untouched     */
    TEST_ASSERT_TRUE(r.flag);
    TEST_ASSERT_EQUAL_STRING(DEFAULTS.name, r.name);
}

static void test_out_of_range_keeps_default(void)
{
    write_text(KV_PATH, "n8=300\nn16=-5\n");
    cfg_t r = DEFAULTS;
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_kv_load(KV_FILE, NULL, FIELDS, &r));
    TEST_ASSERT_EQUAL_UINT8(DEFAULTS.n8, r.n8);        /* > type width  */
    TEST_ASSERT_EQUAL_UINT16(DEFAULTS.n16, r.n16);     /* negative      */
}

static void test_accept_range(void)
{
    /* saver.ini-style bounds: only 1..60 accepted, else default wins. */
    static const storage_kv_field_t RANGED[] = {
        { "idle", offsetof(cfg_t, n32), STORAGE_KV_U32, 0, 1, 60 },
        { NULL, 0, 0, 0, 0, 0 },
    };
    cfg_t r = DEFAULTS;
    write_text(KV_PATH, "idle=0\n");
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_kv_load(KV_FILE, NULL, RANGED, &r));
    TEST_ASSERT_EQUAL_UINT32(DEFAULTS.n32, r.n32);
    write_text(KV_PATH, "idle=60\n");
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_kv_load(KV_FILE, NULL, RANGED, &r));
    TEST_ASSERT_EQUAL_UINT32(60, r.n32);
}

static void test_str_truncates(void)
{
    write_text(KV_PATH, "name=waytoolongvalue\n");
    cfg_t r = DEFAULTS;
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_kv_load(KV_FILE, NULL, FIELDS, &r));
    TEST_ASSERT_EQUAL_STRING("waytool", r.name);       /* 8 incl. NUL */
}

static void test_atomic_pair_replaces(void)
{
    write_text(KV_PATH, "old\n");
    storage_atomic_file_t af;
    FILE *f = storage_atomic_open(&af, KV_PATH);
    TEST_ASSERT_NOT_NULL(f);
    fputs("new\n", f);
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_atomic_close(&af));

    char buf[16] = "";
    f = fopen(KV_PATH, "r");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_NOT_NULL(fgets(buf, sizeof(buf), f));
    fclose(f);
    TEST_ASSERT_EQUAL_STRING("new\n", buf);
}

static void test_factory_reset_covers_registered_only(void)
{
    cfg_t c = DEFAULTS;
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_kv_save(KV_FILE, NULL, FIELDS, &c));
    write_text(TEST_MOUNT "/keepme.ini", "x=1\n");

    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_reset_register(KV_FILE));
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_reset_register(KV_FILE)); /* idempotent */
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_factory_reset());

    FILE *f = fopen(KV_PATH, "r");
    TEST_ASSERT_NULL(f);                               /* registered: gone */
    f = fopen(TEST_MOUNT "/keepme.ini", "r");
    TEST_ASSERT_NOT_NULL(f);                           /* unregistered: kept */
    fclose(f);
    remove(TEST_MOUNT "/keepme.ini");
}

/* ------------------------------------------------------------------
 * Sections — several features share one file (settings.ini model)
 * ---------------------------------------------------------------- */

static void test_sections_roundtrip(void)
{
    /* Two owners, two tables, one file: the second save must append its
     * section without touching the first. */
    static const storage_kv_field_t OTHER[] = {
        { "idle", offsetof(cfg_t, n32), STORAGE_KV_U32, 0, 0, 0 },
        { NULL, 0, 0, 0, 0, 0 },
    };
    cfg_t a = { 11, 1100, 110000, false, "alpha" };
    cfg_t b = DEFAULTS; b.n32 = 42;
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_kv_save(KV_FILE, "one", FIELDS, &a));
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_kv_save(KV_FILE, "two", OTHER, &b));

    cfg_t r = DEFAULTS;
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_kv_load(KV_FILE, "one", FIELDS, &r));
    TEST_ASSERT_EQUAL_UINT8(11, r.n8);
    TEST_ASSERT_EQUAL_STRING("alpha", r.name);
    r = DEFAULTS;
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_kv_load(KV_FILE, "two", OTHER, &r));
    TEST_ASSERT_EQUAL_UINT32(42, r.n32);
    TEST_ASSERT_EQUAL_UINT8(DEFAULTS.n8, r.n8);   /* [one] didn't bleed */
}

static void test_section_save_preserves_foreign_lines(void)
{
    write_text(KV_PATH,
               "# top comment\n"
               "[keep]\n"
               "x=1\n"
               "\n"
               "[mine]\n"
               "n8=99\n"
               "stale=1\n"
               "[tail]\n"
               "y=2\n");
    cfg_t c = DEFAULTS;
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_kv_save(KV_FILE, "mine", FIELDS, &c));

    /* Our section regenerated in place; everything else verbatim. */
    static char text[1024];
    FILE *f = fopen(KV_PATH, "r");
    TEST_ASSERT_NOT_NULL(f);
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(text, "# top comment"));
    TEST_ASSERT_NOT_NULL(strstr(text, "[keep]\nx=1"));
    TEST_ASSERT_NOT_NULL(strstr(text, "[tail]\ny=2"));
    TEST_ASSERT_NULL(strstr(text, "stale=1"));         /* old body gone */

    cfg_t r; memset(&r, 0, sizeof(r));
    TEST_ASSERT_EQUAL_INT(ESP_OK, storage_kv_load(KV_FILE, "mine", FIELDS, &r));
    TEST_ASSERT_EQUAL_UINT8(DEFAULTS.n8, r.n8);
    TEST_ASSERT_EQUAL_STRING(DEFAULTS.name, r.name);
}

static void test_section_absent_not_found(void)
{
    write_text(KV_PATH, "[other]\nn8=1\n");
    cfg_t r = DEFAULTS;
    TEST_ASSERT_EQUAL_INT(ESP_ERR_NOT_FOUND,
                          storage_kv_load(KV_FILE, "mine", FIELDS, &r));
    TEST_ASSERT_EQUAL_UINT8(DEFAULTS.n8, r.n8);        /* untouched */
}

/* ------------------------------------------------------------------ */

int main(void)
{
    storage_init();

    UNITY_BEGIN();
    RUN_TEST(test_roundtrip_all_types);
    RUN_TEST(test_absent_file_not_found_defaults_kept);
    RUN_TEST(test_missing_and_unknown_keys);
    RUN_TEST(test_out_of_range_keeps_default);
    RUN_TEST(test_accept_range);
    RUN_TEST(test_str_truncates);
    RUN_TEST(test_atomic_pair_replaces);
    RUN_TEST(test_factory_reset_covers_registered_only);
    RUN_TEST(test_sections_roundtrip);
    RUN_TEST(test_section_save_preserves_foreign_lines);
    RUN_TEST(test_section_absent_not_found);
    return UNITY_END();
}
