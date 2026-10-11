/*
 * Unit tests for esp_hid_parse_report_map — the HID report-descriptor parser.
 *
 * The descriptor bytes come straight from the BLE peer, so the parser must
 * survive truncated, oversized and malformed input, and a failed parse must
 * not poison the next one (the parser keeps file-static state).
 *
 * Build & run:
 *   cmake -S tests/esp_hid -B build-hid -G Ninja && cmake --build build-hid
 *   ./build-hid/test_report_map
 */

#include "unity.h"
#include "esp_hid_common.h"
#include <string.h>

/* USB HID boot keyboard descriptor (HID 1.11 appendix E.6), 63 bytes. */
static const uint8_t k_boot_kbd[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,             /* modifiers: 8 bits in */
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,             /* reserved:  8 bits in */
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
    0x91, 0x02,                                     /* LEDs:      5 bits out */
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01,             /* padding:   3 bits out */
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00,             /* keys:     48 bits in */
    0xC0,
};

void setUp(void) {}
void tearDown(void) {}

static void assert_boot_keyboard(esp_hid_report_map_t *map)
{
    TEST_ASSERT_NOT_NULL(map);
    TEST_ASSERT_EQUAL_INT(ESP_HID_USAGE_KEYBOARD, map->usage);
    TEST_ASSERT_EQUAL_HEX16(ESP_HID_APPEARANCE_KEYBOARD, map->appearance);
    /* input REPORT + input BOOT + output REPORT + output BOOT */
    TEST_ASSERT_EQUAL_UINT8(4, map->reports_len);
    TEST_ASSERT_EQUAL_UINT8(ESP_HID_REPORT_TYPE_INPUT,    map->reports[0].report_type);
    TEST_ASSERT_EQUAL_UINT8(ESP_HID_PROTOCOL_MODE_REPORT, map->reports[0].protocol_mode);
    TEST_ASSERT_EQUAL_UINT16(8, map->reports[0].value_len);   /* 64 bits */
    TEST_ASSERT_EQUAL_UINT8(ESP_HID_PROTOCOL_MODE_BOOT,   map->reports[1].protocol_mode);
    TEST_ASSERT_EQUAL_UINT16(8, map->reports[1].value_len);
    TEST_ASSERT_EQUAL_UINT8(ESP_HID_REPORT_TYPE_OUTPUT,   map->reports[2].report_type);
    TEST_ASSERT_EQUAL_UINT16(1, map->reports[2].value_len);   /* 8 bits */
    TEST_ASSERT_EQUAL_UINT16(1, map->reports[3].value_len);
}

void test_boot_keyboard_descriptor_parses(void)
{
    esp_hid_report_map_t *map = esp_hid_parse_report_map(k_boot_kbd, sizeof(k_boot_kbd));
    assert_boot_keyboard(map);
    esp_hid_free_report_map(map);
}

void test_descriptor_truncated_inside_collection_returns_null(void)
{
    /* Usage page, usage, open collection, then nothing: no report was ever
     * completed, so there is no map to return. Used to dereference NULL. */
    esp_hid_report_map_t *map = esp_hid_parse_report_map(k_boot_kbd, 6);
    TEST_ASSERT_NULL(map);
}

void test_empty_descriptor_returns_null(void)
{
    uint8_t none = 0;
    TEST_ASSERT_NULL(esp_hid_parse_report_map(&none, 0));
}

void test_item_missing_its_data_bytes_returns_null(void)
{
    uint8_t bs[] = { 0x05 };   /* USAGE_PAGE with a 1-byte payload, payload absent */
    TEST_ASSERT_NULL(esp_hid_parse_report_map(bs, sizeof(bs)));
}

void test_failed_parse_does_not_poison_the_next(void)
{
    /* A truncated map leaves the parser mid-collection; the next map must
     * still parse from a clean state. */
    TEST_ASSERT_NULL(esp_hid_parse_report_map(k_boot_kbd, sizeof(k_boot_kbd) - 1));
    esp_hid_report_map_t *map = esp_hid_parse_report_map(k_boot_kbd, sizeof(k_boot_kbd));
    assert_boot_keyboard(map);
    esp_hid_free_report_map(map);
}

void test_failed_parse_after_a_report_does_not_poison_the_next(void)
{
    /* Two application collections; the second is cut off after its first
     * report was already recorded into the temp map. */
    uint8_t two[sizeof(k_boot_kbd) * 2];
    memcpy(two, k_boot_kbd, sizeof(k_boot_kbd));
    memcpy(two + sizeof(k_boot_kbd), k_boot_kbd, sizeof(k_boot_kbd));
    TEST_ASSERT_NULL(esp_hid_parse_report_map(two, sizeof(two) - 1));
    esp_hid_report_map_t *map = esp_hid_parse_report_map(k_boot_kbd, sizeof(k_boot_kbd));
    assert_boot_keyboard(map);
    esp_hid_free_report_map(map);
}

void test_too_many_reports_rejected_cleanly(void)
{
    /* 70 report IDs: the first keyboard report adds REPORT + BOOT items and
     * every later one (generic usage after the ID change) adds one, so the
     * 64-item cap is hit. Must fail, not overrun. */
    uint8_t bs[6 + 70 * 8 + 1];
    size_t n = 0;
    memcpy(bs, k_boot_kbd, 6); n = 6;
    for (int i = 1; i <= 70; i++) {
        uint8_t item[8] = { 0x85, (uint8_t)i, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02 };
        memcpy(bs + n, item, 8); n += 8;
    }
    bs[n++] = 0xC0;
    TEST_ASSERT_NULL(esp_hid_parse_report_map(bs, n));
    /* and the parser is reusable afterwards */
    esp_hid_report_map_t *map = esp_hid_parse_report_map(k_boot_kbd, sizeof(k_boot_kbd));
    assert_boot_keyboard(map);
    esp_hid_free_report_map(map);
}

void test_huge_report_size_and_count_do_not_overflow(void)
{
    /* REPORT_SIZE 0xFFFF x REPORT_COUNT 0xFFFF: the bit count saturates
     * instead of wrapping through int; the map is rejected as not whole bytes
     * or accepted with a clamped length, but never corrupts state. */
    uint8_t bs[] = {
        0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,
        0x76, 0xFF, 0xFF, 0x96, 0xFF, 0xFF, 0x81, 0x02,
        0xC0,
    };
    esp_hid_report_map_t *map = esp_hid_parse_report_map(bs, sizeof(bs));
    esp_hid_free_report_map(map);
    map = esp_hid_parse_report_map(k_boot_kbd, sizeof(k_boot_kbd));
    assert_boot_keyboard(map);
    esp_hid_free_report_map(map);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_boot_keyboard_descriptor_parses);
    RUN_TEST(test_descriptor_truncated_inside_collection_returns_null);
    RUN_TEST(test_empty_descriptor_returns_null);
    RUN_TEST(test_item_missing_its_data_bytes_returns_null);
    RUN_TEST(test_failed_parse_does_not_poison_the_next);
    RUN_TEST(test_failed_parse_after_a_report_does_not_poison_the_next);
    RUN_TEST(test_too_many_reports_rejected_cleanly);
    RUN_TEST(test_huge_report_size_and_count_do_not_overflow);
    return UNITY_END();
}
