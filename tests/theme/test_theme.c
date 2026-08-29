/*
 * test_theme.c — the invariant the scrollbar regression lacked. Every
 * baked overlay style must keep readable text on its own background,
 * for every theme the shell sets. The BRIGHT|DIM collision
 * (fg == bg == 0x7BEF) is what these assertions catch.
 */

#include "unity.h"
#include "app_theme.h"
#include <stdio.h>

/* Same luma recipe as the display's mono effect: 0..62. */
static int luma(color_t p)
{
    const int r6 = ((p >> 11) & 0x1F) << 1;
    const int g6 = (p >> 5) & 0x3F;
    const int b6 = (p & 0x1F) << 1;
    return (r6 * 5 + g6 * 9 + b6 * 2) >> 4;
}

static int iabs(int v) { return v < 0 ? -v : v; }

/* The two themes the shell sets (app_internal.h UI_FG/UI_BG and the
 * hostkey mismatch hazard theme). */
static const struct { color_t fg, bg; const char *name; } THEMES[] = {
    { RGB565(85, 255, 85), RGB565(0, 0, 0),  "default green" },
    { RGB565(255, 255, 255), RGB565(96, 0, 0), "hostkey hazard" },
};

static display_overlay_style_t s_pal[UI_PAL_COUNT];

void setUp(void) {}
void tearDown(void) {}

static void test_every_entry_readable(void)
{
    for (unsigned t = 0; t < sizeof THEMES / sizeof *THEMES; t++) {
        ui_theme_build(THEMES[t].fg, THEMES[t].bg, s_pal);
        for (int i = 0; i < UI_PAL_COUNT; i++) {
            char msg[64];
            snprintf(msg, sizeof(msg), "%s: style %d accent %d",
                     THEMES[t].name, i / OVERLAY_ACCENTS, i % OVERLAY_ACCENTS);
            TEST_ASSERT_MESSAGE(s_pal[i].fg != s_pal[i].bg, msg);
            TEST_ASSERT_MESSAGE(iabs(luma(s_pal[i].fg) - luma(s_pal[i].bg)) >= 6,
                                msg);
        }
    }
}

static void test_track_regression(void)
{
    /* One gauge look, period: medium white on dark tint, for every
     * accent and every theme. Differentiating TRACK rows per accent
     * must land here first, not surface as a stray-pen scrollbar. */
    for (unsigned t = 0; t < sizeof THEMES / sizeof *THEMES; t++) {
        ui_theme_build(THEMES[t].fg, THEMES[t].bg, s_pal);
        const display_overlay_style_t *tr =
            &s_pal[UI_TRACK * OVERLAY_ACCENTS];
        for (int a = 0; a < OVERLAY_ACCENTS; a++) {
            TEST_ASSERT_EQUAL_HEX16(0x7BEF, tr[a].fg);
            TEST_ASSERT_EQUAL_HEX16(0x39E7, tr[a].bg);
        }
    }
}

static void test_qr_stays_dark_on_white(void)
{
    /* BAR white must stay a pure white surface with dark modules. */
    ui_theme_build(THEMES[0].fg, THEMES[0].bg, s_pal);
    const display_overlay_style_t *qr =
        &s_pal[UI_BAR * OVERLAY_ACCENTS + OVERLAY_COL_WHITE];
    TEST_ASSERT_EQUAL_HEX16(RGB565(255, 255, 255), qr->bg);
    TEST_ASSERT_TRUE(luma(qr->fg) < 16);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_entry_readable);
    RUN_TEST(test_track_regression);
    RUN_TEST(test_qr_stays_dark_on_white);
    return UNITY_END();
}
