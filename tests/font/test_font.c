/*
 * Font decoder unit tests — prove font_decode_glyph() reproduces the
 * pre-compression Terminus tables byte-for-byte.
 *
 * golden_WxH.h holds a CRC32 per codepoint for the regular and bold faces.
 * scratchpad gen_goldens.py generates it from the reference tables.
 * It also stores a few full-row spot glyphs for debuggable failures.
 *
 * The bold expectation encodes the renderer semantics.
 * It expects the real bold glyph where the old bold table stored one.
 * Elsewhere, it expects the regular glyph.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "font.h"
#include "tui_symbols_golden.h"

#if FONT_RT_12X24
#include "golden_12x24.h"
#define TEST_SIZE   FONT_SIZE_12X24
#define TEST_W      12
#define TEST_H      24
#elif FONT_RT_10X20
#include "golden_10x20.h"
#define TEST_SIZE   FONT_SIZE_10X20
#define TEST_W      10
#define TEST_H      20
#else
#include "golden_8x16.h"
#define TEST_SIZE   FONT_SIZE_8X16
#define TEST_W      8
#define TEST_H      16
#endif

#define GB  FONT_GLYPH_BYTES(TEST_W, TEST_H)

/* zlib-compatible CRC32, bitwise (poly 0xEDB88320) */
static uint32_t crc32z(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

void setUp(void)    {}
void tearDown(void) {}

static void test_geometry(void)
{
    TEST_ASSERT_EQUAL_INT(TEST_W, font_width());
    TEST_ASSERT_EQUAL_INT(TEST_H, font_height());
    TEST_ASSERT_EQUAL_UINT((unsigned)GB, (unsigned)font_glyph_bytes());
}

/* Every covered codepoint, regular face, must match its reference CRC. */
static void test_all_regular(void)
{
    uint8_t out[GB];
    int bad = 0;
    for (int i = 0; i < GOLDEN_NUM; i++) {
        font_decode_glyph(golden[i].cp, false, out);
        if (crc32z(out, GB) != golden[i].crc_reg) {
            if (bad < 10)
                printf("regular U+%04X mismatch\n", golden[i].cp);
            bad++;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, bad, "regular glyphs differ");
}

/* Every covered codepoint, bold semantics. */
static void test_all_bold(void)
{
    uint8_t out[GB];
    int bad = 0;
    for (int i = 0; i < GOLDEN_NUM; i++) {
        font_decode_glyph(golden[i].cp, true, out);
        if (crc32z(out, GB) != golden[i].crc_bold) {
            if (bad < 10)
                printf("bold U+%04X mismatch\n", golden[i].cp);
            bad++;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, bad, "bold glyphs differ");
}

/* Full-row comparison for a handful of glyphs — readable failure output. */
static void test_spot_rows(void)
{
    uint8_t out[GB];
    for (int i = 0; i < SPOT_NUM; i++) {
        font_decode_glyph(spot_reg[i].cp, false, out);
        TEST_ASSERT_EQUAL_MEMORY(spot_reg[i].rows, out, GB);
        font_decode_glyph(spot_bold[i].cp, true, out);
        TEST_ASSERT_EQUAL_MEMORY(spot_bold[i].rows, out, GB);
    }
}

/* An uncovered codepoint decodes as '?', bold included.
 * U+E800 is private-use, not in Terminus.
 * It also falls outside the sprite slot range (U+E000 to U+E00F).
 * That range decodes from the sprite table instead; see test_sprites. */
static void test_fallback(void)
{
    uint8_t out[GB], q[GB];
    font_decode_glyph(0x003F, false, q);
    font_decode_glyph(0xE800, false, out);
    TEST_ASSERT_EQUAL_MEMORY(q, out, GB);
    font_decode_glyph(0xE800, true, out);    /* outside bold subset too */
    TEST_ASSERT_EQUAL_MEMORY(q, out, GB);
}

/* Dynamic sprite glyphs: unset slots are blank; set rows come back
 * byte-exact (bold identical — pixel art is not smeared); an update is
 * visible THROUGH the decoded-glyph cache (set() must invalidate); clear
 * blanks again; the first cp past the slot range takes the '?' fallback. */
static void test_sprites(void)
{
    uint8_t out[GB], zero[GB], pat1[GB], pat2[GB], q[GB];
    memset(zero, 0, GB);
    for (int i = 0; i < (int)GB; i++) {
        pat1[i] = (uint8_t)(i * 7 + 1);
        pat2[i] = (uint8_t)(0xA5 ^ i);
    }
    const uint16_t cp = font_sprite_cp(3);

    font_decode_glyph(cp, false, out);            /* unset: blank */
    TEST_ASSERT_EQUAL_MEMORY(zero, out, GB);

    font_sprite_set(3, pat1);
    font_decode_glyph(cp, false, out);            /* fill */
    TEST_ASSERT_EQUAL_MEMORY(pat1, out, GB);
    font_decode_glyph(cp, false, out);            /* cache hit */
    TEST_ASSERT_EQUAL_MEMORY(pat1, out, GB);
    font_decode_glyph(cp, true, out);             /* bold matches the regular bitmap */
    TEST_ASSERT_EQUAL_MEMORY(pat1, out, GB);

    font_sprite_set(3, pat2);                     /* update while cached */
    font_decode_glyph(cp, false, out);
    TEST_ASSERT_EQUAL_MEMORY(pat2, out, GB);
    font_decode_glyph(cp, true, out);
    TEST_ASSERT_EQUAL_MEMORY(pat2, out, GB);

    font_sprite_clear(3);
    font_decode_glyph(cp, false, out);
    TEST_ASSERT_EQUAL_MEMORY(zero, out, GB);

    font_decode_glyph(0x003F, false, q);          /* just past the range */
    font_decode_glyph((uint16_t)(FONT_SPRITE_BASE + FONT_SPRITE_COUNT),
                      false, out);
    TEST_ASSERT_EQUAL_MEMORY(q, out, GB);

    /* rows16 packing: uint16 row values land in the active layout (u8 rows
     * truncate, u16 rows little-endian). */
    uint16_t r16[TEST_H];
    uint8_t  want[GB];
    for (int r = 0; r < TEST_H; r++) {
        r16[r] = (uint16_t)(0x0155 + r * 0x0101);
        if (FONT_ROW_BYTES(TEST_W) == 1) {
            want[r] = (uint8_t)r16[r];
        } else {
            want[2 * r]     = (uint8_t)r16[r];
            want[2 * r + 1] = (uint8_t)(r16[r] >> 8);
        }
    }
    font_sprite_set_rows16(3, r16);
    font_decode_glyph(cp, false, out);
    TEST_ASSERT_EQUAL_MEMORY(want, out, GB);

    font_sprite_clear_all();
    font_decode_glyph(cp, false, out);
    TEST_ASSERT_EQUAL_MEMORY(zero, out, GB);
}

/* U+0020 decodes to all-zero rows (the renderer's blank fast path must
 * agree with the decoder). */
static void test_blank(void)
{
    uint8_t out[GB], zero[GB];
    memset(zero, 0, GB);
    font_decode_glyph(0x0020, false, out);
    TEST_ASSERT_EQUAL_MEMORY(zero, out, GB);
}

/* The decoded-glyph cache holds 256 entries, 2-way.
 * The golden set has about 1470 codepoints times 2 faces.
 * The sweeps above already overcommit the cache about 11x and force constant eviction.
 * Every CRC check above therefore goes through a fill.
 *
 * This test covers what those sweeps do not.
 * It checks that a cache hit returns the same bytes as the fill that populated it.
 * It also checks that a refill after eviction stays correct.
 * A slot-indexing or victim-selection bug shows up here as a wrong glyph, not a corrupted one. */
static void test_cache_hit_and_refill(void)
{
    uint8_t first[GB], again[GB];
    int bad = 0;

    /* fill (or hit), then a guaranteed hit on the same key */
    for (int i = 0; i < GOLDEN_NUM; i++) {
        font_decode_glyph(golden[i].cp, false, first);
        font_decode_glyph(golden[i].cp, false, again);
        if (memcmp(first, again, GB) != 0) {
            if (bad < 10) printf("hit != fill at U+%04X\n", golden[i].cp);
            bad++;
        }
    }

    /* sweep the bold face to evict everything, then re-check a sample of the
     * regular face against its reference — a refill must be identical */
    for (int i = 0; i < GOLDEN_NUM; i++)
        font_decode_glyph(golden[i].cp, true, again);
    for (int i = 0; i < GOLDEN_NUM; i += 7) {
        font_decode_glyph(golden[i].cp, false, again);
        if (crc32z(again, GB) != golden[i].crc_reg) {
            if (bad < 10) printf("refill wrong at U+%04X\n", golden[i].cp);
            bad++;
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, bad, "glyph cache hit/refill disagrees");
}

static void test_tui_symbols_have_distinct_bitmaps(void)
{
    uint8_t glyphs[sizeof(symbol_golden) / sizeof(symbol_golden[0])][GB];
    uint8_t bold[GB], question[GB], blank[GB] = {0};
    font_decode_glyph('?', false, question);
    for (size_t i = 0; i < sizeof(symbol_golden) / sizeof(symbol_golden[0]); i++) {
        font_decode_glyph(symbol_golden[i].cp, false, glyphs[i]);
        TEST_ASSERT_EQUAL_HEX32(symbol_golden[i].crc, crc32z(glyphs[i], GB));
        TEST_ASSERT_TRUE_MESSAGE(memcmp(question, glyphs[i], GB) != 0, "symbol falls back to '?'");
        TEST_ASSERT_TRUE_MESSAGE(memcmp(blank, glyphs[i], GB) != 0, "symbol is blank");
        font_decode_glyph(symbol_golden[i].cp, true, bold);
        TEST_ASSERT_EQUAL_MEMORY(glyphs[i], bold, GB);
        for (size_t j = 0; j < i; j++)
            TEST_ASSERT_TRUE_MESSAGE(memcmp(glyphs[j], glyphs[i], GB) != 0, "symbols must differ");
    }
}

static void test_related_symbols_mirror(void)
{
    const uint16_t pairs[][2] = {{0x276F, 0x276E}, {0x25D0, 0x25D1}};
    uint8_t left[GB], right[GB];
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        font_decode_glyph(pairs[i][0], false, left);
        font_decode_glyph(pairs[i][1], false, right);
        for (int y = 0; y < TEST_H; y++) {
            size_t offset = y * FONT_ROW_BYTES(TEST_W);
            uint16_t row = left[offset], expected = right[offset];
            if (FONT_ROW_BYTES(TEST_W) == 2) {
                row |= (uint16_t)left[offset + 1] << 8;
                expected |= (uint16_t)right[offset + 1] << 8;
            }
            uint16_t mirrored = 0;
            for (int x = 0; x < TEST_W; x++)
                mirrored |= ((row >> x) & 1u) << (TEST_W - x - 1);
            TEST_ASSERT_EQUAL_HEX16(expected, mirrored);
        }
    }
}

static void test_removed_circled_operators_fall_back(void)
{
    uint8_t question[GB], out[GB];
    font_decode_glyph('?', false, question);
    for (uint16_t cp = 0x2295; cp <= 0x2296; cp++) {
        font_decode_glyph(cp, false, out);
        TEST_ASSERT_EQUAL_MEMORY(question, out, GB);
        font_decode_glyph(cp, true, out);
        TEST_ASSERT_EQUAL_MEMORY(question, out, GB);
    }
}

int main(void)
{
    font_init(TEST_SIZE);
    UNITY_BEGIN();
    RUN_TEST(test_geometry);
    RUN_TEST(test_tui_symbols_have_distinct_bitmaps);
    RUN_TEST(test_related_symbols_mirror);
    RUN_TEST(test_removed_circled_operators_fall_back);
    RUN_TEST(test_spot_rows);
    RUN_TEST(test_all_regular);
    RUN_TEST(test_all_bold);
    RUN_TEST(test_fallback);
    RUN_TEST(test_blank);
    RUN_TEST(test_cache_hit_and_refill);
    RUN_TEST(test_sprites);
    return UNITY_END();
}
