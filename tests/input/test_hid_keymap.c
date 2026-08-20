/*
 * test_hid_keymap.c -- Unity tests for the BLE HID keycode translator.
 *
 * This is the device's only keyboard path, and a wrong entry in the usage-ID
 * table is invisible until someone presses that key on real hardware.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "unity.h"
#include "hid_keymap.h"
#include "vtkeys.h"

/* HID modifier byte bits, as the keyboard sends them. */
#define M_LCTRL   (1u << 0)
#define M_LSHIFT  (1u << 1)
#define M_LALT    (1u << 2)
#define M_LGUI    (1u << 3)
#define M_RCTRL   (1u << 4)
#define M_RSHIFT  (1u << 5)
#define M_RALT    (1u << 6)

void setUp(void) {}
void tearDown(void) {}

/* Translate and compare against a literal byte sequence. */
static void expect(uint8_t keycode, uint8_t mods, bool app, const char *want)
{
    uint8_t buf[VTKEYS_MAX_LEN];
    uint8_t n = hid_keymap_translate(keycode, mods, app, buf);
    TEST_ASSERT_EQUAL_UINT8(strlen(want), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)want, buf, n);
}

/* ── Printable keys ──────────────────────────────────────────────────────── */

void test_printable_unshifted_and_shifted(void)
{
    expect(0x04, 0,        false, "a");
    expect(0x04, M_LSHIFT, false, "A");
    expect(0x1E, 0,        false, "1");
    expect(0x1E, M_LSHIFT, false, "!");
    expect(0x2C, 0,        false, " ");
}

void test_printable_right_hand_modifiers_count(void)
{
    expect(0x04, M_RSHIFT, false, "A");
    expect(0x04, M_RCTRL,  false, "\x01");
}

void test_ctrl_letters(void)
{
    expect(0x04, M_LCTRL, false, "\x01");   /* Ctrl+A */
    expect(0x06, M_LCTRL, false, "\x03");   /* Ctrl+C */
    expect(0x17, M_LCTRL, false, "\x14");   /* Ctrl+T */
}

/* Ctrl+2 is the canonical way to type NUL. */
void test_ctrl_two_is_nul(void)
{
    uint8_t buf[VTKEYS_MAX_LEN];
    uint8_t n = hid_keymap_translate(0x1F, M_LCTRL, false, buf);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_UINT8(0x00, buf[0]);
}

void test_alt_prefixes_escape(void)
{
    expect(0x04, M_LALT,           false, "\x1b" "a");
    expect(0x04, M_LALT | M_LCTRL, false, "\x1b\x01");
}

void test_control_characters(void)
{
    expect(0x28, 0, false, "\r");     /* Enter     */
    expect(0x29, 0, false, "\x1b");   /* Escape    */
    expect(0x2A, 0, false, "\x7f");   /* Backspace */
    expect(0x2B, 0, false, "\t");     /* Tab       */
    expect(0x58, 0, false, "\r");     /* Numpad Enter */
}

/* ── Special keys via vtkeys ─────────────────────────────────────────────── */

/* Usage IDs are adjacent and easy to transpose; pin each one. */
void test_arrow_usage_ids(void)
{
    expect(0x4F, 0, false, "\x1b[C");   /* Right */
    expect(0x50, 0, false, "\x1b[D");   /* Left  */
    expect(0x51, 0, false, "\x1b[B");   /* Down  */
    expect(0x52, 0, false, "\x1b[A");   /* Up    */
}

void test_nav_cluster_usage_ids(void)
{
    expect(0x49, 0, false, "\x1b[2~");  /* Insert  */
    expect(0x4A, 0, false, "\x1b[H");   /* Home    */
    expect(0x4B, 0, false, "\x1b[5~");  /* PageUp  */
    expect(0x4C, 0, false, "\x1b[3~");  /* Delete  */
    expect(0x4D, 0, false, "\x1b[F");   /* End     */
    expect(0x4E, 0, false, "\x1b[6~");  /* PageDn  */
}

void test_function_key_usage_ids(void)
{
    expect(0x3A, 0, false, "\x1bOP");    /* F1  */
    expect(0x3D, 0, false, "\x1bOS");    /* F4  */
    expect(0x3E, 0, false, "\x1b[15~");  /* F5  */
    expect(0x45, 0, false, "\x1b[24~");  /* F12 */
}

void test_arrows_follow_application_cursor_mode(void)
{
    expect(0x52, 0, true, "\x1bOA");
    expect(0x50, 0, true, "\x1bOD");
}

/* ── Modifiers on special keys ───────────────────────────────────────────── */

void test_ctrl_arrows_encode_modifier(void)
{
    expect(0x50, M_LCTRL, false, "\x1b[1;5D");
    expect(0x4F, M_LCTRL, false, "\x1b[1;5C");
}

/* The binding the deck reserves for its own scrollback. */
void test_shift_pageup_pagedown(void)
{
    expect(0x4B, M_LSHIFT, false, "\x1b[5;2~");
    expect(0x4E, M_LSHIFT, false, "\x1b[6;2~");
}

void test_modifier_beats_application_cursor(void)
{
    expect(0x50, M_LCTRL, true, "\x1b[1;5D");
}

/* A held Windows key must not corrupt the modifier parameter. */
void test_gui_modifier_ignored_on_special_keys(void)
{
    expect(0x50, M_LCTRL | M_LGUI, false, "\x1b[1;5D");
}

void test_right_hand_modifiers_on_special_keys(void)
{
    expect(0x50, M_RCTRL,  false, "\x1b[1;5D");
    expect(0x4B, M_RSHIFT, false, "\x1b[5;2~");
    expect(0x52, M_RALT,   false, "\x1b[1;3A");
}

/* ── Unmapped ────────────────────────────────────────────────────────────── */

void test_unknown_keycode_returns_zero(void)
{
    uint8_t buf[VTKEYS_MAX_LEN];
    TEST_ASSERT_EQUAL_UINT8(0, hid_keymap_translate(0x00, 0, false, buf));
    TEST_ASSERT_EQUAL_UINT8(0, hid_keymap_translate(0xFF, 0, false, buf));
    TEST_ASSERT_EQUAL_UINT8(0, hid_keymap_translate(0xE0, 0, false, buf));  /* LCtrl itself */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_printable_unshifted_and_shifted);
    RUN_TEST(test_printable_right_hand_modifiers_count);
    RUN_TEST(test_ctrl_letters);
    RUN_TEST(test_ctrl_two_is_nul);
    RUN_TEST(test_alt_prefixes_escape);
    RUN_TEST(test_control_characters);
    RUN_TEST(test_arrow_usage_ids);
    RUN_TEST(test_nav_cluster_usage_ids);
    RUN_TEST(test_function_key_usage_ids);
    RUN_TEST(test_arrows_follow_application_cursor_mode);
    RUN_TEST(test_ctrl_arrows_encode_modifier);
    RUN_TEST(test_shift_pageup_pagedown);
    RUN_TEST(test_modifier_beats_application_cursor);
    RUN_TEST(test_gui_modifier_ignored_on_special_keys);
    RUN_TEST(test_right_hand_modifiers_on_special_keys);
    RUN_TEST(test_unknown_keycode_returns_zero);
    return UNITY_END();
}
