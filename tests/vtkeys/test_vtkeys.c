/*
 * test_vtkeys.c -- Unity tests for the shared key-sequence encoder.
 *
 * Covers: unmodified sequences in both cursor modes, xterm modifier
 * encoding for both key families, and the bounds the input queue relies on.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "unity.h"
#include "vtkeys.h"

void setUp(void) {}
void tearDown(void) {}

/* Encode and compare against a literal sequence. */
static void expect(vtkey_t key, uint8_t mods, bool app, const char *want)
{
    uint8_t buf[VTKEYS_MAX_LEN];
    size_t  n = vtkeys_encode(key, mods, app, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(strlen(want), n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)want, buf, n);
}

/* ── Unmodified ──────────────────────────────────────────────────────────── */

void test_arrows_normal_cursor(void)
{
    expect(VTKEY_UP,    0, false, "\x1b[A");
    expect(VTKEY_DOWN,  0, false, "\x1b[B");
    expect(VTKEY_RIGHT, 0, false, "\x1b[C");
    expect(VTKEY_LEFT,  0, false, "\x1b[D");
}

void test_arrows_application_cursor(void)
{
    expect(VTKEY_UP,    0, true, "\x1bOA");
    expect(VTKEY_DOWN,  0, true, "\x1bOB");
    expect(VTKEY_RIGHT, 0, true, "\x1bOC");
    expect(VTKEY_LEFT,  0, true, "\x1bOD");
}

/* Home/End follow DECCKM exactly as the arrows do. */
void test_home_end_follow_cursor_mode(void)
{
    expect(VTKEY_HOME, 0, false, "\x1b[H");
    expect(VTKEY_END,  0, false, "\x1b[F");
    expect(VTKEY_HOME, 0, true,  "\x1bOH");
    expect(VTKEY_END,  0, true,  "\x1bOF");
}

void test_tilde_family_ignores_cursor_mode(void)
{
    expect(VTKEY_INSERT, 0, false, "\x1b[2~");
    expect(VTKEY_DELETE, 0, false, "\x1b[3~");
    expect(VTKEY_PGUP,   0, false, "\x1b[5~");
    expect(VTKEY_PGDN,   0, false, "\x1b[6~");
    expect(VTKEY_PGUP,   0, true,  "\x1b[5~");
}

/* F1-F4 are SS3 whatever DECCKM says; F5+ are tilde-family. */
void test_function_keys(void)
{
    expect(VTKEY_F1,  0, false, "\x1bOP");
    expect(VTKEY_F4,  0, false, "\x1bOS");
    expect(VTKEY_F1,  0, true,  "\x1bOP");
    expect(VTKEY_F5,  0, false, "\x1b[15~");
    expect(VTKEY_F10, 0, false, "\x1b[21~");
    expect(VTKEY_F12, 0, false, "\x1b[24~");
}

/* ── Modifiers ───────────────────────────────────────────────────────────── */

void test_modifier_parameter_weights(void)
{
    expect(VTKEY_UP, VTMOD_SHIFT,                          false, "\x1b[1;2A");
    expect(VTKEY_UP, VTMOD_ALT,                            false, "\x1b[1;3A");
    expect(VTKEY_UP, VTMOD_SHIFT | VTMOD_ALT,              false, "\x1b[1;4A");
    expect(VTKEY_UP, VTMOD_CTRL,                           false, "\x1b[1;5A");
    expect(VTKEY_UP, VTMOD_CTRL  | VTMOD_SHIFT,            false, "\x1b[1;6A");
    expect(VTKEY_UP, VTMOD_CTRL  | VTMOD_ALT,              false, "\x1b[1;7A");
    expect(VTKEY_UP, VTMOD_CTRL | VTMOD_ALT | VTMOD_SHIFT, false, "\x1b[1;8A");
}

/* Word-wise movement in readline/vim — the whole point of the exercise. */
void test_ctrl_arrows(void)
{
    expect(VTKEY_LEFT,  VTMOD_CTRL, false, "\x1b[1;5D");
    expect(VTKEY_RIGHT, VTMOD_CTRL, false, "\x1b[1;5C");
}

void test_modified_tilde_family(void)
{
    expect(VTKEY_PGUP,   VTMOD_SHIFT, false, "\x1b[5;2~");
    expect(VTKEY_PGDN,   VTMOD_SHIFT, false, "\x1b[6;2~");
    expect(VTKEY_DELETE, VTMOD_CTRL,  false, "\x1b[3;5~");
    expect(VTKEY_F12,    VTMOD_CTRL | VTMOD_ALT | VTMOD_SHIFT,
                                      false, "\x1b[24;8~");
}

/* SS3 has nowhere to put a parameter, so a modifier forces CSI even when
 * the unmodified key would have gone out as SS3. */
void test_modified_keys_are_always_csi(void)
{
    expect(VTKEY_LEFT, VTMOD_CTRL, true,  "\x1b[1;5D");
    expect(VTKEY_HOME, VTMOD_CTRL, true,  "\x1b[1;5H");
    expect(VTKEY_F1,   VTMOD_CTRL, false, "\x1b[1;5P");
}

/* A GUI/meta bit from a backend has no xterm weight and must not shift the
 * parameter — otherwise Ctrl+Left under a held Windows key sends garbage. */
void test_unencodable_modifier_bits_ignored(void)
{
    expect(VTKEY_LEFT, VTMOD_CTRL | 0x08u, false, "\x1b[1;5D");
    expect(VTKEY_LEFT, VTMOD_CTRL | 0xF8u, false, "\x1b[1;5D");
}

/* ── Bounds ──────────────────────────────────────────────────────────────── */

void test_unknown_keys_write_nothing(void)
{
    uint8_t buf[VTKEYS_MAX_LEN];
    TEST_ASSERT_EQUAL_size_t(0, vtkeys_encode(VTKEY_NONE, 0, false, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_size_t(0, vtkeys_encode((vtkey_t)200, 0, false, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_size_t(0, vtkeys_encode(VTKEY_UP, 0, false, NULL, 8));
}

/* A short buffer must be refused outright, never partially filled. */
void test_short_buffer_leaves_output_untouched(void)
{
    uint8_t buf[VTKEYS_MAX_LEN];
    memset(buf, 0xAA, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(0, vtkeys_encode(VTKEY_F12, VTMOD_CTRL, false, buf, 3));
    for (size_t i = 0; i < sizeof(buf); i++)
        TEST_ASSERT_EQUAL_UINT8(0xAA, buf[i]);
}

/* INPUT_EVENT_MAX_LEN is 8 and the input queue copies straight into it, so
 * no reachable combination may exceed VTKEYS_MAX_LEN. */
void test_no_sequence_exceeds_max_len(void)
{
    uint8_t buf[VTKEYS_MAX_LEN];
    for (int k = VTKEY_UP; k <= VTKEY_F12; k++)
        for (uint8_t m = 0; m <= 7; m++)
            for (int app = 0; app <= 1; app++) {
                size_t n = vtkeys_encode((vtkey_t)k, m, app != 0, buf, sizeof(buf));
                TEST_ASSERT_GREATER_THAN_size_t(0, n);
                TEST_ASSERT_LESS_OR_EQUAL_size_t(VTKEYS_MAX_LEN, n);
            }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_arrows_normal_cursor);
    RUN_TEST(test_arrows_application_cursor);
    RUN_TEST(test_home_end_follow_cursor_mode);
    RUN_TEST(test_tilde_family_ignores_cursor_mode);
    RUN_TEST(test_function_keys);
    RUN_TEST(test_modifier_parameter_weights);
    RUN_TEST(test_ctrl_arrows);
    RUN_TEST(test_modified_tilde_family);
    RUN_TEST(test_modified_keys_are_always_csi);
    RUN_TEST(test_unencodable_modifier_bits_ignored);
    RUN_TEST(test_unknown_keys_write_nothing);
    RUN_TEST(test_short_buffer_leaves_output_untouched);
    RUN_TEST(test_no_sequence_exceeds_max_len);
    return UNITY_END();
}
