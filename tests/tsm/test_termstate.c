/*
 * test_termstate.c -- Unity tests for tsm termstate
 *
 * Covers: color conversion, charset translation, terminal model
 * (cursor movement, SGR, erase, scroll, alt-screen, charsets, wrap).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "tsm.h"
#include "color.h"
#include "charsets.h"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Feed a C string (no NUL) to the terminal. */
static void feed(tsm_t *t, const char *s)
{
    tsm_feed(t, (const uint8_t *)s, strlen(s));
}

/* Get cell at (col, row). */
static tsm_cell_t cell(tsm_t *t, int col, int row)
{
    return tsm_row(t, row)[col];
}

/* Get codepoint at (col, row). */
static uint16_t cp_at(tsm_t *t, int col, int row)
{
    return cell(t, col, row).cp;
}

void setUp(void) {}
void tearDown(void) {}

/* ════════════════════════════════════════════════════════════════════════════
 * color.c tests
 * ════════════════════════════════════════════════════════════════════════════ */

void test_color_rgb_black(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x0000, color_rgb(0, 0, 0));
}

void test_color_rgb_white(void)
{
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, color_rgb(255, 255, 255));
}

void test_color_rgb_red(void)
{
    /* r=0xF8 → bits[15:11]=11111; g=0, b=0 → 0xF800 */
    TEST_ASSERT_EQUAL_HEX16(0xF800, color_rgb(0xFF, 0, 0));
}

void test_color_rgb_green(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x07E0, color_rgb(0, 0xFF, 0));
}

void test_color_rgb_blue(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x001F, color_rgb(0, 0, 0xFF));
}

void test_color_ansi_named_black(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x0000, color_ansi(0));
}

void test_color_ansi_named_white(void)
{
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, color_ansi(15));
}

void test_color_ansi_cube_first(void)
{
    /* Index 16: r=0, g=0, b=0 → black */
    TEST_ASSERT_EQUAL_HEX16(0x0000, color_ansi(16));
}

void test_color_ansi_cube_white(void)
{
    /* Index 231: r=5, g=5, b=5 → 255,255,255 → 0xFFFF */
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, color_ansi(231));
}

void test_color_ansi_cube_red(void)
{
    /* Index 196: r=5,g=0,b=0 → r=255 → 0xF800 */
    TEST_ASSERT_EQUAL_HEX16(0xF800, color_ansi(196));
}

void test_color_ansi_grayscale_first(void)
{
    /* Index 232: v=8 → RGB565(8,8,8) = (0<<11)|(0<<5)|0 ≈ 0x0841 */
    uint16_t got = color_ansi(232);
    /* Just check it's not black and not white */
    TEST_ASSERT_NOT_EQUAL(0x0000, got);
    TEST_ASSERT_NOT_EQUAL(0xFFFF, got);
}

void test_color_ansi_grayscale_last(void)
{
    /* Index 255: v=238 → close to white but not 255 */
    uint16_t got = color_ansi(255);
    TEST_ASSERT_NOT_EQUAL(0x0000, got);
}

/* ════════════════════════════════════════════════════════════════════════════
 * charsets.c tests
 * ════════════════════════════════════════════════════════════════════════════ */

void test_charset_ascii_identity(void)
{
    for (uint8_t i = 0x20; i < 0x7F; i++)
        TEST_ASSERT_EQUAL_HEX16(i, charset_xlat(CHARSET_ASCII, i));
}

void test_charset_dec_gfx_horizontal_line(void)
{
    /* 'q' (0x71) → U+2500 ─ */
    TEST_ASSERT_EQUAL_HEX16(0x2500, charset_xlat(CHARSET_DEC_GFX, 0x71));
}

void test_charset_dec_gfx_box_upper_left(void)
{
    /* 'l' (0x6C) → U+250C ┌ */
    TEST_ASSERT_EQUAL_HEX16(0x250C, charset_xlat(CHARSET_DEC_GFX, 0x6C));
}

void test_charset_dec_gfx_box_cross(void)
{
    /* 'n' (0x6E) → U+253C ┼ */
    TEST_ASSERT_EQUAL_HEX16(0x253C, charset_xlat(CHARSET_DEC_GFX, 0x6E));
}

void test_charset_dec_gfx_diamond(void)
{
    /* '`' (0x60) → U+25C6 ◆ */
    TEST_ASSERT_EQUAL_HEX16(0x25C6, charset_xlat(CHARSET_DEC_GFX, 0x60));
}

void test_charset_dec_gfx_tilde(void)
{
    /* '~' (0x7E) → U+00B7 · */
    TEST_ASSERT_EQUAL_HEX16(0x00B7, charset_xlat(CHARSET_DEC_GFX, 0x7E));
}

void test_charset_dec_gfx_below_range_passthrough(void)
{
    /* 0x5F '_' is below 0x60 → identity */
    TEST_ASSERT_EQUAL_HEX16(0x5F, charset_xlat(CHARSET_DEC_GFX, 0x5F));
}

void test_charset_dec_gfx_ascii_passthrough(void)
{
    /* 'A' (0x41) — not in remapped range */
    TEST_ASSERT_EQUAL_HEX16(0x41, charset_xlat(CHARSET_DEC_GFX, 0x41));
}

/* ════════════════════════════════════════════════════════════════════════════
 * tsm_new / lifecycle
 * ════════════════════════════════════════════════════════════════════════════ */

void test_tsm_new_basic(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(80, tsm_cols(t));
    TEST_ASSERT_EQUAL_INT(24, tsm_rows(t));
    tsm_free(t);
}

void test_tsm_new_initial_cursor(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(0, col);
    TEST_ASSERT_EQUAL_UINT8(0, row);
    TEST_ASSERT_TRUE(vis);
    tsm_free(t);
}

void test_tsm_new_initial_cells_blank(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    /* All cells should be spaces with default colors */
    for (int r = 0; r < 24; r++)
        for (int c = 0; c < 80; c++) {
            tsm_cell_t ce = cell(t, (uint8_t)c, (uint8_t)r);
            TEST_ASSERT_EQUAL_HEX16(' ', ce.cp);
        }
    tsm_free(t);
}

void test_tsm_new_null_on_zero_cols(void)
{
    TEST_ASSERT_NULL(tsm_new(0, 24, 0));
}

void test_tsm_new_null_on_zero_rows(void)
{
    TEST_ASSERT_NULL(tsm_new(80, 0, 0));
}

/* ════════════════════════════════════════════════════════════════════════════
 * Print / cursor advance / auto-wrap
 * ════════════════════════════════════════════════════════════════════════════ */

void test_print_writes_cell(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "A");
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 0));
    tsm_free(t);
}

void test_print_advances_cursor(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "AB");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(2, col);
    TEST_ASSERT_EQUAL_UINT8(0, row);
    tsm_free(t);
}

void test_print_auto_wrap(void)
{
    tsm_t *t = tsm_new(10, 5, 0);
    /* Fill first row exactly */
    feed(t, "0123456789");
    /* Next char should wrap to row 1, col 0 */
    feed(t, "X");
    TEST_ASSERT_EQUAL_HEX16('X', cp_at(t, 0, 1));
    tsm_free(t);
}

void test_print_utf8_two_byte(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    /* U+00E9 é — UTF-8: 0xC3 0xA9 */
    uint8_t bytes[] = {0xC3, 0xA9};
    tsm_feed(t, bytes, 2);
    TEST_ASSERT_EQUAL_HEX16(0x00E9, cp_at(t, 0, 0));
    tsm_free(t);
}

void test_print_utf8_three_byte(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    /* U+2500 ─ — UTF-8: 0xE2 0x94 0x80 */
    uint8_t bytes[] = {0xE2, 0x94, 0x80};
    tsm_feed(t, bytes, 3);
    TEST_ASSERT_EQUAL_HEX16(0x2500, cp_at(t, 0, 0));
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * C0 controls
 * ════════════════════════════════════════════════════════════════════════════ */

void test_c0_cr(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "ABC\r");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(0, col);
    TEST_ASSERT_EQUAL_UINT8(0, row);
    tsm_free(t);
}

void test_c0_lf_moves_down(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\n");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(1, row);
    tsm_free(t);
}

void test_c0_bs_moves_left(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "AB\x08");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(1, col);
    tsm_free(t);
}

void test_c0_ht_tab_stop(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\t");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(8, col);
    tsm_free(t);
}

void test_c0_lf_scrolls_at_bottom(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "A\r\nB\r\nC\r\n");
    /* After CR+LF at bottom row, rows scroll up: row0='B', row1='C', row2=blank */
    TEST_ASSERT_EQUAL_HEX16('B', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16('C', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 2));
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Cursor positioning (CSI)
 * ════════════════════════════════════════════════════════════════════════════ */

void test_csi_cup_moves_cursor(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[5;10H");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(9, col);   /* 1-based → 0-based */
    TEST_ASSERT_EQUAL_UINT8(4, row);
    tsm_free(t);
}

void test_csi_cup_default_params(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "ABCDE");
    feed(t, "\x1b[H");  /* home */
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(0, col);
    TEST_ASSERT_EQUAL_UINT8(0, row);
    tsm_free(t);
}

void test_csi_cursor_up(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[5;1H\x1b[2A");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(2, row);
    tsm_free(t);
}

void test_csi_cursor_down(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[3B");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(3, row);
    tsm_free(t);
}

void test_csi_cursor_forward(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[5C");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(5, col);
    tsm_free(t);
}

void test_csi_cursor_backward(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[1;10H\x1b[3D");   /* row 1 col 10 (1-based), then back 3 */
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(6, col);  /* col 9 (0-based) - 3 = 6 */
    tsm_free(t);
}

void test_csi_cha(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "ABCDE\x1b[3G");  /* CHA: move to col 3 (1-based) */
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(2, col);  /* 0-based */
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Erase operations
 * ════════════════════════════════════════════════════════════════════════════ */

void test_csi_ed2_clears_screen(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "AAAA\nBBBB\nCCCC");
    feed(t, "\x1b[2J");
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 10; c++)
            TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, (uint8_t)c, (uint8_t)r));
    tsm_free(t);
}

void test_csi_el0_erase_to_end_of_line(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "ABCDEFGHIJ");
    feed(t, "\x1b[1;4H\x1b[K");  /* move to col 4 row 1, erase to end */
    /* cols 0-2 = 'A','B','C'; cols 3-9 = ' ' */
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16('B', cp_at(t, 1, 0));
    TEST_ASSERT_EQUAL_HEX16('C', cp_at(t, 2, 0));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 3, 0));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 9, 0));
    tsm_free(t);
}

void test_csi_el1_erase_to_start_of_line(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "ABCDEFGHIJ");
    feed(t, "\x1b[1;5H\x1b[1K");  /* move to col 5, erase to start */
    /* cols 0-4 = ' '; cols 5-9 = 'F'-'J' */
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 4, 0));
    TEST_ASSERT_EQUAL_HEX16('F', cp_at(t, 5, 0));
    tsm_free(t);
}

void test_csi_el2_erase_full_line(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "ABCDEFGHIJ");
    feed(t, "\x1b[2K");
    for (int c = 0; c < 10; c++)
        TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, (uint8_t)c, 0));
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * SGR attributes
 * ════════════════════════════════════════════════════════════════════════════ */

void test_sgr_bold(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[1mA");
    TEST_ASSERT_BITS(CELL_ATTR_BOLD, CELL_ATTR_BOLD, cell(t, 0, 0).attrs);
    tsm_free(t);
}

void test_sgr_reset(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[1;4mA\x1b[0mB");
    TEST_ASSERT_BITS(CELL_ATTR_BOLD | CELL_ATTR_UNDERLINE, CELL_ATTR_BOLD | CELL_ATTR_UNDERLINE,
                     cell(t, 0, 0).attrs);
    TEST_ASSERT_EQUAL_UINT8(0, cell(t, 1, 0).attrs);
    tsm_free(t);
}

void test_sgr_256_fg(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[38;5;196mA");  /* 256-color fg: index 196 = bright red */
    uint16_t expected = color_ansi(196);
    TEST_ASSERT_EQUAL_HEX16(expected, cell(t, 0, 0).fg);
    tsm_free(t);
}

void test_sgr_256_bg(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[48;5;21mA");  /* 256-color bg: index 21 = bright blue */
    uint16_t expected = color_ansi(21);
    TEST_ASSERT_EQUAL_HEX16(expected, cell(t, 0, 0).bg);
    tsm_free(t);
}

void test_sgr_truecolor_fg(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[38;2;255;0;0mA");  /* truecolor red */
    TEST_ASSERT_EQUAL_HEX16(color_rgb(255, 0, 0), cell(t, 0, 0).fg);
    tsm_free(t);
}

void test_sgr_default_colors_restored(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[31;42mA\x1b[39;49mB");  /* set fg+bg, then reset both */
    TEST_ASSERT_EQUAL_HEX16(COLOR_DEFAULT_FG, cell(t, 1, 0).fg);
    TEST_ASSERT_EQUAL_HEX16(COLOR_DEFAULT_BG, cell(t, 1, 0).bg);
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Scrolling
 * ════════════════════════════════════════════════════════════════════════════ */

void test_scroll_region_decstbm(void)
{
    tsm_t *t = tsm_new(10, 5, 0);
    /* Set scroll region rows 2-4 (1-based) */
    feed(t, "\x1b[2;4r");
    /* Put content in rows */
    feed(t, "\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC\x1b[4;1HD\x1b[5;1HE");
    /* Position in scroll region and do LF to scroll */
    feed(t, "\x1b[4;1H\n");
    /* Row 1 (0-based 0) = 'A' unchanged */
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 0));
    /* Row 5 (0-based 4) = 'E' unchanged */
    TEST_ASSERT_EQUAL_HEX16('E', cp_at(t, 0, 4));
    /* Scroll region scrolled up: row 1 now has 'C', row 2 has 'D', row 3 blank */
    TEST_ASSERT_EQUAL_HEX16('C', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16('D', cp_at(t, 0, 2));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 3));
    tsm_free(t);
}

void test_csi_su_scroll_up(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC");
    feed(t, "\x1b[1S");  /* scroll up 1 */
    TEST_ASSERT_EQUAL_HEX16('B', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16('C', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 2));
    tsm_free(t);
}

void test_csi_sd_scroll_down(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC");
    feed(t, "\x1b[1T");  /* scroll down 1 */
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16('B', cp_at(t, 0, 2));
    tsm_free(t);
}

/* ── Row-ring: full-screen scrolls rotate a base index instead of moving
 * cells, so everything below runs with a non-zero base to prove the ring
 * mapping composes with the rest of the model. ── */

/* Rotate the full-screen ring by n: n LFs issued from the bottom row. */
static void rotate_ring(tsm_t *t, int n)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "\x1b[%d;1H", tsm_rows(t));
    feed(t, buf);
    for (int i = 0; i < n; i++) feed(t, "\n");
}

void test_ring_scroll_wraps_past_rows(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    /* ls-style: print at bottom, CRLF, repeat — 4 scrolls on a 3-row grid
     * wraps the base (4 % 3 = 1). */
    feed(t, "\x1b[3;1H");
    feed(t, "1\r\n2\r\n3\r\n4\r\n");
    TEST_ASSERT_EQUAL_HEX16('3', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16('4', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 2));
    /* Writes after the wrap land where the mapping says they do. */
    feed(t, "\x1b[1;5HX");
    TEST_ASSERT_EQUAL_HEX16('X', cp_at(t, 4, 0));
    tsm_free(t);
}

void test_ring_alt_screen_roundtrip(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    rotate_ring(t, 2);                       /* primary base = 2 */
    feed(t, "\x1b[1;1HP\x1b[2;1HQ\x1b[3;1HR");
    feed(t, "\x1b[?1049h");                  /* enter alt */
    feed(t, "\x1b[3;1Halt\n\n");             /* rotate the ALT ring too */
    feed(t, "\x1b[?1049l");                  /* back to primary */
    /* Primary content must survive the alt session's own rotations. */
    TEST_ASSERT_EQUAL_HEX16('P', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16('Q', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16('R', cp_at(t, 0, 2));
    tsm_free(t);
}

void test_ring_then_decstbm_partial_scroll(void)
{
    tsm_t *t = tsm_new(10, 5, 0);
    rotate_ring(t, 2);                       /* base = 2 */
    feed(t, "\x1b[2;4r");                    /* region rows 2-4 (1-based) */
    feed(t, "\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC\x1b[4;1HD\x1b[5;1HE");
    feed(t, "\x1b[4;1H\n");                  /* LF at region bottom */
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16('C', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16('D', cp_at(t, 0, 2));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 3));
    TEST_ASSERT_EQUAL_HEX16('E', cp_at(t, 0, 4));
    tsm_free(t);
}

void test_ring_then_insert_delete_lines(void)
{
    tsm_t *t = tsm_new(10, 4, 0);
    rotate_ring(t, 1);                       /* base = 1 */
    feed(t, "\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC\x1b[4;1HD");
    feed(t, "\x1b[2;1H\x1b[2L");             /* insert 2 lines at row 2 */
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 2));
    TEST_ASSERT_EQUAL_HEX16('B', cp_at(t, 0, 3));
    feed(t, "\x1b[2;1H\x1b[2M");             /* delete them again */
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16('B', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 2));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 3));
    tsm_free(t);
}

void test_ring_then_insert_delete_chars(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    rotate_ring(t, 1);
    feed(t, "\x1b[2;1HABCDEF");
    feed(t, "\x1b[2;3H\x1b[2P");             /* DCH 2 at col 3 */
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16('B', cp_at(t, 1, 1));
    TEST_ASSERT_EQUAL_HEX16('E', cp_at(t, 2, 1));
    TEST_ASSERT_EQUAL_HEX16('F', cp_at(t, 3, 1));
    feed(t, "\x1b[2;3H\x1b[2@");             /* ICH 2 back */
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 2, 1));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 3, 1));
    TEST_ASSERT_EQUAL_HEX16('E', cp_at(t, 4, 1));
    tsm_free(t);
}

void test_ring_reverse_index_wraps_negative(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "\x1b[1;1HA\x1b[2;1HB");
    feed(t, "\x1b[1;1H\x1bM");               /* RI at top: base 0 -> rows-1 */
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16('B', cp_at(t, 0, 2));
    feed(t, "\x1b[1;1HZ");
    TEST_ASSERT_EQUAL_HEX16('Z', cp_at(t, 0, 0));
    tsm_free(t);
}

/* ── Batched print: wrap and autowrap-off edges ── */

void test_print_span_wraps_across_rows(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    /* 25 chars from col 6 (0-based 5): fills cols 5-9, wraps twice. */
    feed(t, "\x1b[1;6HABCDEFGHIJKLMNOPQRSTUVWXY");
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 5, 0));
    TEST_ASSERT_EQUAL_HEX16('E', cp_at(t, 9, 0));
    TEST_ASSERT_EQUAL_HEX16('F', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16('O', cp_at(t, 9, 1));
    TEST_ASSERT_EQUAL_HEX16('P', cp_at(t, 0, 2));
    TEST_ASSERT_EQUAL_HEX16('Y', cp_at(t, 9, 2));
    /* Last char landed in the last column: wrap still pending. */
    int cx, cy; bool vis;
    tsm_cursor(t, &cx, &cy, &vis);
    TEST_ASSERT_EQUAL_INT(9, cx);
    TEST_ASSERT_EQUAL_INT(2, cy);
    tsm_free(t);
}

void test_print_span_autowrap_off_parks_at_margin(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "\x1b[?7l");                     /* DECAWM off */
    feed(t, "\x1b[2;6HABCDEFGHIJ");          /* 10 chars from col 5 */
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 5, 1));
    TEST_ASSERT_EQUAL_HEX16('D', cp_at(t, 8, 1));
    /* Chars past the margin overwrite the last column in turn. */
    TEST_ASSERT_EQUAL_HEX16('J', cp_at(t, 9, 1));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 2));
    int cx, cy; bool vis;
    tsm_cursor(t, &cx, &cy, &vis);
    TEST_ASSERT_EQUAL_INT(9, cx);
    TEST_ASSERT_EQUAL_INT(1, cy);
    tsm_free(t);
}

void test_print_span_wrap_scrolls_ring_at_bottom(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    /* 15 chars at the bottom row: wraps once, scrolling the full screen —
     * 'top' (row 0) scrolls off, the filled row lands on row 1. */
    feed(t, "\x1b[1;1Htop");
    feed(t, "\x1b[3;1HABCDEFGHIJKLMNO");
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16('J', cp_at(t, 9, 1));
    TEST_ASSERT_EQUAL_HEX16('K', cp_at(t, 0, 2));
    TEST_ASSERT_EQUAL_HEX16('O', cp_at(t, 4, 2));
    tsm_free(t);
}

void test_ring_hard_reset_clears_base(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    rotate_ring(t, 2);
    feed(t, "\x1b[1;1HA");
    feed(t, "\x1b" "c");                     /* RIS */
    for (int r = 0; r < 3; r++)
        TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, r));
    feed(t, "Z");
    TEST_ASSERT_EQUAL_HEX16('Z', cp_at(t, 0, 0));
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Insert / delete
 * ════════════════════════════════════════════════════════════════════════════ */

void test_csi_il_insert_line(void)
{
    tsm_t *t = tsm_new(10, 4, 0);
    feed(t, "\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC\x1b[4;1HD");
    feed(t, "\x1b[2;1H\x1b[1L");  /* cursor row 2, insert 1 line */
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 1));  /* new blank line */
    TEST_ASSERT_EQUAL_HEX16('B', cp_at(t, 0, 2));
    TEST_ASSERT_EQUAL_HEX16('C', cp_at(t, 0, 3));  /* D scrolled off */
    tsm_free(t);
}

void test_csi_dl_delete_line(void)
{
    tsm_t *t = tsm_new(10, 4, 0);
    feed(t, "\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC\x1b[4;1HD");
    feed(t, "\x1b[2;1H\x1b[1M");  /* cursor row 2, delete 1 line */
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16('C', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16('D', cp_at(t, 0, 2));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 3));
    tsm_free(t);
}

void test_csi_ich_insert_chars(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "ABCDE");
    feed(t, "\x1b[1;3H\x1b[2@");  /* pos col 3, insert 2 chars */
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16('B', cp_at(t, 1, 0));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 2, 0));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 3, 0));
    TEST_ASSERT_EQUAL_HEX16('C', cp_at(t, 4, 0));
    tsm_free(t);
}

void test_csi_dch_delete_chars(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "ABCDE");
    feed(t, "\x1b[1;2H\x1b[2P");  /* pos col 2, delete 2 chars */
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16('D', cp_at(t, 1, 0));
    TEST_ASSERT_EQUAL_HEX16('E', cp_at(t, 2, 0));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 3, 0));
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Save / restore cursor
 * ════════════════════════════════════════════════════════════════════════════ */

void test_decsc_decrc(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[5;10H\x1b""7");   /* move to (10,5) then DECSC */
    feed(t, "\x1b[1;1H");           /* move away */
    feed(t, "\x1b""8");             /* DECRC */
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(9, col);
    TEST_ASSERT_EQUAL_UINT8(4, row);
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Alt screen
 * ════════════════════════════════════════════════════════════════════════════ */

void test_alt_screen_switch(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "Primary");
    feed(t, "\x1b[?1049h");  /* switch to alt screen */
    /* Alt screen should be blank */
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 0));
    feed(t, "Alt");
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 0));
    feed(t, "\x1b[?1049l");  /* switch back to primary */
    /* Primary screen content restored */
    TEST_ASSERT_EQUAL_HEX16('P', cp_at(t, 0, 0));
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Cursor visibility
 * ════════════════════════════════════════════════════════════════════════════ */

void test_dectcem_hide_show(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    bool vis; int c, r;
    feed(t, "\x1b[?25l");   /* hide cursor */
    tsm_cursor(t, &c, &r, &vis);
    TEST_ASSERT_FALSE(vis);
    feed(t, "\x1b[?25h");   /* show cursor */
    tsm_cursor(t, &c, &r, &vis);
    TEST_ASSERT_TRUE(vis);
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * ESC sequences
 * ════════════════════════════════════════════════════════════════════════════ */

void test_esc_ri_reverse_index(void)
{
    tsm_t *t = tsm_new(10, 5, 0);
    feed(t, "\x1b[3;1H");  /* row 3 */
    feed(t, "\x1bM");       /* RI — reverse index */
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(1, row);  /* row 3 (1-based) - 1 = row 2 (1-based) = row 1 (0-based) */
    tsm_free(t);
}

void test_esc_ri_scrolls_at_top(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC");
    feed(t, "\x1b[1;1H\x1bM");  /* RI at top of screen → scroll down */
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 0));  /* new blank row at top */
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16('B', cp_at(t, 0, 2));
    tsm_free(t);
}

void test_esc_nel(void)
{
    tsm_t *t = tsm_new(10, 5, 0);
    feed(t, "\x1b[1;5H");   /* col 5, row 1 */
    feed(t, "\x1b" "E");     /* NEL — next line */
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(0, col);
    TEST_ASSERT_EQUAL_UINT8(1, row);
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Charset designation
 * ════════════════════════════════════════════════════════════════════════════ */

void test_charset_dec_gfx_active(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b(0");   /* designate G0 = DEC Special Graphics */
    feed(t, "q");        /* 'q' → U+2500 ─ */
    TEST_ASSERT_EQUAL_HEX16(0x2500, cp_at(t, 0, 0));
    feed(t, "\x1b(B");   /* restore G0 = ASCII */
    feed(t, "q");
    TEST_ASSERT_EQUAL_HEX16('q', cp_at(t, 1, 0));
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Dirty tracking
 * ════════════════════════════════════════════════════════════════════════════ */

void test_dirty_after_print(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    tsm_clear_dirty(t);
    feed(t, "ABC");
    const tsm_row_dirty_t *d = tsm_dirty(t);
    TEST_ASSERT_EQUAL_UINT8(0, d[0].l);
    TEST_ASSERT_EQUAL_UINT8(2, d[0].r);
    /* Other rows clean */
    TEST_ASSERT_GREATER_THAN_UINT8(d[1].r, d[1].l);
    tsm_free(t);
}

void test_dirty_cleared(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "ABC");
    tsm_clear_dirty(t);
    const tsm_row_dirty_t *d = tsm_dirty(t);
    for (int r = 0; r < 3; r++)
        TEST_ASSERT_GREATER_THAN_UINT8(d[r].r, d[r].l);
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * ESC RIS — full reset
 * ════════════════════════════════════════════════════════════════════════════ */

void test_esc_ris_full_reset(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "\x1b[1mABCDE");
    feed(t, "\x1b" "c");  /* RIS */
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(0, col);
    TEST_ASSERT_EQUAL_UINT8(0, row);
    /* Screen cleared */
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 0));
    /* Attrs reset */
    TEST_ASSERT_EQUAL_UINT8(0, cell(t, 0, 0).attrs);
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * DA1 / DSR / CPR response callback
 * ════════════════════════════════════════════════════════════════════════════ */

static char   s_resp_buf[64];
static size_t s_resp_len;

static void capture_response(const char *data, size_t len, void *user)
{
    (void)user;
    if (len < sizeof(s_resp_buf)) {
        memcpy(s_resp_buf, data, len);
        s_resp_len = len;
    }
}

static void clear_response(void)
{
    memset(s_resp_buf, 0, sizeof(s_resp_buf));
    s_resp_len = 0;
}

void test_da1_response(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    clear_response();
    tsm_set_response_cb(t, capture_response, NULL);
    feed(t, "\x1b[c");   /* DA1 — no param */
    TEST_ASSERT_EQUAL_size_t(7, s_resp_len);
    TEST_ASSERT_EQUAL_MEMORY("\x1b[?1;2c", s_resp_buf, 7);
    tsm_free(t);
}

void test_da1_param0(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    clear_response();
    tsm_set_response_cb(t, capture_response, NULL);
    feed(t, "\x1b[0c");  /* DA1 — param 0, same reply */
    TEST_ASSERT_EQUAL_size_t(7, s_resp_len);
    TEST_ASSERT_EQUAL_MEMORY("\x1b[?1;2c", s_resp_buf, 7);
    tsm_free(t);
}

void test_dsr_status(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    clear_response();
    tsm_set_response_cb(t, capture_response, NULL);
    feed(t, "\x1b[5n");  /* DSR — status report */
    TEST_ASSERT_EQUAL_size_t(4, s_resp_len);
    TEST_ASSERT_EQUAL_MEMORY("\x1b[0n", s_resp_buf, 4);
    tsm_free(t);
}

void test_dsr_cpr(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    clear_response();
    tsm_set_response_cb(t, capture_response, NULL);
    feed(t, "\x1b[3;6H");  /* move to row=3, col=6 (1-based) */
    feed(t, "\x1b[6n");    /* CPR */
    /* expect ESC [ 3 ; 6 R */
    TEST_ASSERT_EQUAL_size_t(6, s_resp_len);
    TEST_ASSERT_EQUAL_MEMORY("\x1b[3;6R", s_resp_buf, 6);
    tsm_free(t);
}

void test_no_response_when_cb_null(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    /* No callback set — must not crash */
    feed(t, "\x1b[c");
    feed(t, "\x1b[5n");
    feed(t, "\x1b[6n");
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Alt screen — dirty tracking + variant escapes
 * ════════════════════════════════════════════════════════════════════════════ */

/* switch_to_primary marks rows dirty → display updates */
void test_alt_screen_exit_marks_dirty(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "HELLO");
    tsm_clear_dirty(t);          /* simulate post-flush state */
    feed(t, "\x1b[?1049h");     /* enter alt screen */
    tsm_clear_dirty(t);          /* simulate flush */
    feed(t, "\x1b[?1049l");     /* exit alt screen */
    /* All rows must be dirty so renderer re-copies primary */
    const tsm_row_dirty_t *d = tsm_dirty(t);
    for (int r = 0; r < tsm_rows(t); r++)
        TEST_ASSERT_TRUE(d[r].l <= d[r].r);
    tsm_free(t);
}

/* ?47l exits alt screen */
void test_alt_screen_47_exit(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "HELLO");
    feed(t, "\x1b[?47h");   /* enter alt screen */
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 0));
    feed(t, "\x1b[?47l");   /* exit alt screen */
    TEST_ASSERT_EQUAL_HEX16('H', cp_at(t, 0, 0));
    tsm_free(t);
}

/* ?1047l exits alt screen */
void test_alt_screen_1047_exit(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "HELLO");
    feed(t, "\x1b[?1047h");
    feed(t, "\x1b[?1047l");
    TEST_ASSERT_EQUAL_HEX16('H', cp_at(t, 0, 0));
    tsm_free(t);
}

/* Hard reset from alt screen: t->cells is primary, display is blank */
void test_reset_from_alt_screen(void)
{
    tsm_t *t = tsm_new(10, 3, 0);
    feed(t, "\x1b[?1049h");   /* enter alt */
    feed(t, "ALT");
    tsm_reset(t);
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_INT(0, col);
    TEST_ASSERT_EQUAL_INT(0, row);
    /* Active screen is blank (primary erased by reset) */
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 0));
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Synchronized output — mode ?2026
 * ════════════════════════════════════════════════════════════════════════════ */

void test_sync_initial_state(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    TEST_ASSERT_FALSE(tsm_sync_update(t));
    tsm_free(t);
}

void test_sync_mode_bsu(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[?2026h");
    TEST_ASSERT_TRUE(tsm_sync_update(t));
    tsm_free(t);
}

void test_sync_mode_esu(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[?2026l");
    TEST_ASSERT_FALSE(tsm_sync_update(t));
    tsm_free(t);
}

void test_sync_bsu_esu_roundtrip(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[?2026h");
    TEST_ASSERT_TRUE(tsm_sync_update(t));
    feed(t, "\x1b[?2026l");
    TEST_ASSERT_FALSE(tsm_sync_update(t));
    tsm_free(t);
}

void test_sync_decrqm_inactive(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    clear_response();
    tsm_set_response_cb(t, capture_response, NULL);
    feed(t, "\x1b[?2026$p");
    /* mode reset → N=2: CSI ? 2026 ; 2 $ y  (11 bytes) */
    TEST_ASSERT_EQUAL_size_t(11, s_resp_len);
    TEST_ASSERT_EQUAL_MEMORY("\x1b[?2026;2$y", s_resp_buf, 11);
    tsm_free(t);
}

void test_sync_decrqm_active(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[?2026h");
    clear_response();
    tsm_set_response_cb(t, capture_response, NULL);
    feed(t, "\x1b[?2026$p");
    /* mode set → N=1: CSI ? 2026 ; 1 $ y  (11 bytes) */
    TEST_ASSERT_EQUAL_size_t(11, s_resp_len);
    TEST_ASSERT_EQUAL_MEMORY("\x1b[?2026;1$y", s_resp_buf, 11);
    tsm_free(t);
}

void test_sync_reset_clears_mode(void)
{
    tsm_t *t = tsm_new(80, 24, 0);
    feed(t, "\x1b[?2026h");
    TEST_ASSERT_TRUE(tsm_sync_update(t));
    tsm_reset(t);
    TEST_ASSERT_FALSE(tsm_sync_update(t));
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * scrollback
 * ════════════════════════════════════════════════════════════════════════════ */

/* Fill a 3-row terminal with numbered lines so history is identifiable.
 * The newline goes BEFORE each line rather than after: a trailing newline
 * would scroll once more and leave a blank row on screen, making every
 * expectation below one off from what you would naively write. So n lines
 * leaves exactly n-3..n-1 on screen and 0..n-4 in history. */
static tsm_t *sb_term(int sb_lines, int nlines)
{
    tsm_t *t = tsm_new(10, 3, sb_lines);
    if (!t) return NULL;
    for (int i = 0; i < nlines; i++) {
        char line[16];
        snprintf(line, sizeof(line), "%s%d", i ? "\r\n" : "", i);
        feed(t, line);
    }
    return t;
}

/* First codepoint of a view row, as the digit it encodes. */
static int row_digit(tsm_t *t, int row)
{
    return (int)tsm_row(t, row)[0].cp - '0';
}

/* Built with scrollback off, every entry point must be inert rather than
 * merely unused — the ring pointer is NULL and nothing may dereference it. */
void test_sb_disabled_stores_nothing(void)
{
    tsm_t *t = sb_term(0, 10);
    TEST_ASSERT_EQUAL_INT(0, tsm_sb_capacity(t));
    TEST_ASSERT_EQUAL_INT(0, tsm_sb_len(t));
    TEST_ASSERT_EQUAL_INT(0, tsm_sb_scroll(t, 5));
    TEST_ASSERT_EQUAL_INT(0, tsm_sb_scroll(t, -5));
    TEST_ASSERT_EQUAL_INT(0, tsm_sb_offset(t));
    tsm_sb_reset(t);

    /* The view is still the live grid, and the clear paths still work. */
    TEST_ASSERT_EQUAL_UINT16('7', tsm_row(t, 0)[0].cp);
    feed(t, "\x1b[3J");
    feed(t, "\x1b" "c");
    TEST_ASSERT_EQUAL_INT(0, tsm_sb_len(t));
    tsm_free(t);
}

/* Capacity is what was allocated; length is what has accumulated. The app
 * keys off capacity, so the two must not be conflated. */
void test_sb_capacity_distinct_from_length(void)
{
    tsm_t *t = tsm_new(10, 3, 50);
    TEST_ASSERT_EQUAL_INT(50, tsm_sb_capacity(t));
    TEST_ASSERT_EQUAL_INT(0,  tsm_sb_len(t));      /* fresh: nothing yet */
    feed(t, "a\r\nb\r\nc\r\nd");
    TEST_ASSERT_EQUAL_INT(50, tsm_sb_capacity(t)); /* unchanged by use   */
    TEST_ASSERT_EQUAL_INT(1,  tsm_sb_len(t));
    tsm_free(t);
}

void test_sb_accumulates_evicted_rows(void)
{
    /* 6 lines on a 3-row screen: 3,4,5 stay visible, 0,1,2 become history. */
    tsm_t *t = sb_term(100, 6);
    TEST_ASSERT_EQUAL_INT(3, tsm_sb_len(t));
    tsm_free(t);
}

void test_sb_capacity_caps_length(void)
{
    tsm_t *t = sb_term(2, 20);
    TEST_ASSERT_EQUAL_INT(2, tsm_sb_len(t));
    tsm_free(t);
}

void test_sb_scroll_reveals_history(void)
{
    tsm_t *t = sb_term(100, 6);        /* screen shows 3,4,5; history 0,1,2 */
    TEST_ASSERT_EQUAL_INT(3, row_digit(t, 0));

    TEST_ASSERT_EQUAL_INT(1, tsm_sb_scroll(t, 1));
    TEST_ASSERT_EQUAL_INT(2, row_digit(t, 0));   /* one row of history */
    TEST_ASSERT_EQUAL_INT(3, row_digit(t, 1));   /* live grid shifted down */
    TEST_ASSERT_EQUAL_INT(4, row_digit(t, 2));

    TEST_ASSERT_EQUAL_INT(3, tsm_sb_scroll(t, 2));
    TEST_ASSERT_EQUAL_INT(0, row_digit(t, 0));   /* oldest stored row */
    TEST_ASSERT_EQUAL_INT(1, row_digit(t, 1));
    TEST_ASSERT_EQUAL_INT(2, row_digit(t, 2));
    tsm_free(t);
}

void test_sb_scroll_clamps_both_ends(void)
{
    tsm_t *t = sb_term(100, 6);
    TEST_ASSERT_EQUAL_INT(3, tsm_sb_scroll(t, 999));   /* clamped to sb_len */
    TEST_ASSERT_EQUAL_INT(0, tsm_sb_scroll(t, -999));  /* clamped to live   */
    tsm_free(t);
}

void test_sb_reset_returns_to_live(void)
{
    tsm_t *t = sb_term(100, 6);
    tsm_sb_scroll(t, 2);
    tsm_sb_reset(t);
    TEST_ASSERT_EQUAL_INT(0, tsm_sb_offset(t));
    TEST_ASSERT_EQUAL_INT(3, row_digit(t, 0));
    tsm_free(t);
}

/* Output arriving while scrolled back must not drag the view forward. */
void test_sb_view_holds_content_as_ring_grows(void)
{
    tsm_t *t = sb_term(100, 6);
    tsm_sb_scroll(t, 2);
    int top = row_digit(t, 0);
    TEST_ASSERT_EQUAL_INT(1, top);

    feed(t, "9\r\n");
    TEST_ASSERT_EQUAL_INT(3, tsm_sb_offset(t));   /* followed the ring */
    TEST_ASSERT_EQUAL_INT(top, row_digit(t, 0));  /* same content on screen */
    tsm_free(t);
}

/* Alt-screen apps repaint their own viewport; that is not session history. */
void test_sb_alt_screen_does_not_feed_history(void)
{
    tsm_t *t = sb_term(100, 6);
    int before = tsm_sb_len(t);

    feed(t, "\x1b[?1049h");                       /* enter alt screen */
    feed(t, "a\r\nb\r\nc\r\nd\r\ne\r\n");         /* scrolls the alt grid */
    TEST_ASSERT_EQUAL_INT(before, tsm_sb_len(t));

    feed(t, "\x1b[?1049l");                       /* back to primary  */
    TEST_ASSERT_EQUAL_INT(before, tsm_sb_len(t));
    tsm_free(t);
}

void test_sb_scroll_refused_on_alt_screen(void)
{
    tsm_t *t = sb_term(100, 6);
    feed(t, "\x1b[?1049h");
    TEST_ASSERT_EQUAL_INT(0, tsm_sb_scroll(t, 2));
    tsm_free(t);
}

/* Entering the alt screen while scrolled back must snap to live, or the
 * app would draw into a viewport partly showing history. */
void test_sb_alt_screen_entry_snaps_to_live(void)
{
    tsm_t *t = sb_term(100, 6);
    tsm_sb_scroll(t, 2);
    feed(t, "\x1b[?1049h");
    TEST_ASSERT_EQUAL_INT(0, tsm_sb_offset(t));
    tsm_free(t);
}

/* ED 2 is what a full-screen app or `clear` sends on the way out; it must
 * leave history alone. Sharing a case body with ED 3 once made quitting mc
 * wipe the whole buffer. */
void test_sb_ed2_keeps_history(void)
{
    tsm_t *t = sb_term(100, 6);
    int before = tsm_sb_len(t);
    TEST_ASSERT_GREATER_THAN_INT(0, before);
    feed(t, "\x1b[2J");
    TEST_ASSERT_EQUAL_INT(before, tsm_sb_len(t));
    feed(t, "\x1b[J");                            /* ED 0, default param */
    feed(t, "\x1b[1J");                           /* ED 1                */
    TEST_ASSERT_EQUAL_INT(before, tsm_sb_len(t));
    tsm_free(t);
}

/* The full quit-a-full-screen-app sequence, in the order a real one sends
 * it: alt screen, redraw, clear, leave. History must come back untouched. */
void test_sb_survives_alt_screen_app_exit(void)
{
    tsm_t *t = sb_term(100, 6);
    int before = tsm_sb_len(t);

    feed(t, "\x1b[?1049h");                       /* enter alt        */
    feed(t, "panel\r\npanel\r\npanel\r\npanel");  /* app draws        */
    feed(t, "\x1b[2J");                           /* app clears       */
    feed(t, "\x1b[?1049l");                       /* leave alt        */

    TEST_ASSERT_EQUAL_INT(before, tsm_sb_len(t));
    TEST_ASSERT_EQUAL_INT(before, tsm_sb_scroll(t, before));
    tsm_free(t);
}

void test_sb_ed3_clears_history(void)
{
    tsm_t *t = sb_term(100, 6);
    TEST_ASSERT_GREATER_THAN_INT(0, tsm_sb_len(t));
    feed(t, "\x1b[3J");
    TEST_ASSERT_EQUAL_INT(0, tsm_sb_len(t));
    TEST_ASSERT_EQUAL_INT(0, tsm_sb_offset(t));
    tsm_free(t);
}

void test_sb_hard_reset_clears_history(void)
{
    tsm_t *t = sb_term(100, 6);
    tsm_sb_scroll(t, 2);
    feed(t, "\x1b" "c");                          /* RIS ('c' would extend the hex escape) */
    TEST_ASSERT_EQUAL_INT(0, tsm_sb_len(t));
    TEST_ASSERT_EQUAL_INT(0, tsm_sb_offset(t));
    tsm_free(t);
}

/* A scroll region is an app managing a pane, not the session scrolling. */
void test_sb_partial_region_scroll_is_not_history(void)
{
    tsm_t *t = tsm_new(10, 5, 100);
    feed(t, "\x1b[2;4r");                         /* DECSTBM rows 2..4 */
    feed(t, "\x1b[4;1H");                         /* park on region bottom */
    feed(t, "a\r\nb\r\nc\r\nd\r\n");
    TEST_ASSERT_EQUAL_INT(0, tsm_sb_len(t));
    tsm_free(t);
}

/* The ring wraps; history must still read oldest-to-newest across the seam. */
void test_sb_ring_wraps_in_order(void)
{
    tsm_t *t = sb_term(3, 20);                    /* far more lines than slots */
    TEST_ASSERT_EQUAL_INT(3, tsm_sb_len(t));

    /* 20 lines on a 3-row screen: 17,18,19 live, so history holds 14,15,16. */
    tsm_sb_scroll(t, 3);
    TEST_ASSERT_EQUAL_UINT16('1', tsm_row(t, 0)[0].cp);
    TEST_ASSERT_EQUAL_UINT16('4', tsm_row(t, 0)[1].cp);
    TEST_ASSERT_EQUAL_UINT16('1', tsm_row(t, 1)[0].cp);
    TEST_ASSERT_EQUAL_UINT16('5', tsm_row(t, 1)[1].cp);
    TEST_ASSERT_EQUAL_UINT16('1', tsm_row(t, 2)[0].cp);
    TEST_ASSERT_EQUAL_UINT16('6', tsm_row(t, 2)[1].cp);
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * main
 * ════════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    UNITY_BEGIN();

    /* color */
    RUN_TEST(test_color_rgb_black);
    RUN_TEST(test_color_rgb_white);
    RUN_TEST(test_color_rgb_red);
    RUN_TEST(test_color_rgb_green);
    RUN_TEST(test_color_rgb_blue);
    RUN_TEST(test_color_ansi_named_black);
    RUN_TEST(test_color_ansi_named_white);
    RUN_TEST(test_color_ansi_cube_first);
    RUN_TEST(test_color_ansi_cube_white);
    RUN_TEST(test_color_ansi_cube_red);
    RUN_TEST(test_color_ansi_grayscale_first);
    RUN_TEST(test_color_ansi_grayscale_last);

    /* charsets */
    RUN_TEST(test_charset_ascii_identity);
    RUN_TEST(test_charset_dec_gfx_horizontal_line);
    RUN_TEST(test_charset_dec_gfx_box_upper_left);
    RUN_TEST(test_charset_dec_gfx_box_cross);
    RUN_TEST(test_charset_dec_gfx_diamond);
    RUN_TEST(test_charset_dec_gfx_tilde);
    RUN_TEST(test_charset_dec_gfx_below_range_passthrough);
    RUN_TEST(test_charset_dec_gfx_ascii_passthrough);

    /* lifecycle */
    RUN_TEST(test_tsm_new_basic);
    RUN_TEST(test_tsm_new_initial_cursor);
    RUN_TEST(test_tsm_new_initial_cells_blank);
    RUN_TEST(test_tsm_new_null_on_zero_cols);
    RUN_TEST(test_tsm_new_null_on_zero_rows);

    /* print / wrap / utf8 */
    RUN_TEST(test_print_writes_cell);
    RUN_TEST(test_print_advances_cursor);
    RUN_TEST(test_print_auto_wrap);
    RUN_TEST(test_print_utf8_two_byte);
    RUN_TEST(test_print_utf8_three_byte);

    /* C0 */
    RUN_TEST(test_c0_cr);
    RUN_TEST(test_c0_lf_moves_down);
    RUN_TEST(test_c0_bs_moves_left);
    RUN_TEST(test_c0_ht_tab_stop);
    RUN_TEST(test_c0_lf_scrolls_at_bottom);

    /* cursor movement */
    RUN_TEST(test_csi_cup_moves_cursor);
    RUN_TEST(test_csi_cup_default_params);
    RUN_TEST(test_csi_cursor_up);
    RUN_TEST(test_csi_cursor_down);
    RUN_TEST(test_csi_cursor_forward);
    RUN_TEST(test_csi_cursor_backward);
    RUN_TEST(test_csi_cha);

    /* erase */
    RUN_TEST(test_csi_ed2_clears_screen);
    RUN_TEST(test_csi_el0_erase_to_end_of_line);
    RUN_TEST(test_csi_el1_erase_to_start_of_line);
    RUN_TEST(test_csi_el2_erase_full_line);

    /* SGR */
    RUN_TEST(test_sgr_bold);
    RUN_TEST(test_sgr_reset);
    RUN_TEST(test_sgr_256_fg);
    RUN_TEST(test_sgr_256_bg);
    RUN_TEST(test_sgr_truecolor_fg);
    RUN_TEST(test_sgr_default_colors_restored);

    /* scrolling */
    RUN_TEST(test_scroll_region_decstbm);
    RUN_TEST(test_csi_su_scroll_up);
    RUN_TEST(test_csi_sd_scroll_down);

    /* row ring (base != 0 compositions) */
    RUN_TEST(test_ring_scroll_wraps_past_rows);
    RUN_TEST(test_ring_alt_screen_roundtrip);
    RUN_TEST(test_ring_then_decstbm_partial_scroll);
    RUN_TEST(test_ring_then_insert_delete_lines);
    RUN_TEST(test_ring_then_insert_delete_chars);
    RUN_TEST(test_ring_reverse_index_wraps_negative);
    RUN_TEST(test_ring_hard_reset_clears_base);

    /* batched print */
    RUN_TEST(test_print_span_wraps_across_rows);
    RUN_TEST(test_print_span_autowrap_off_parks_at_margin);
    RUN_TEST(test_print_span_wrap_scrolls_ring_at_bottom);

    /* insert / delete */
    RUN_TEST(test_csi_il_insert_line);
    RUN_TEST(test_csi_dl_delete_line);
    RUN_TEST(test_csi_ich_insert_chars);
    RUN_TEST(test_csi_dch_delete_chars);

    /* save/restore */
    RUN_TEST(test_decsc_decrc);

    /* alt screen */
    RUN_TEST(test_alt_screen_switch);
    RUN_TEST(test_alt_screen_exit_marks_dirty);
    RUN_TEST(test_alt_screen_47_exit);
    RUN_TEST(test_alt_screen_1047_exit);
    RUN_TEST(test_reset_from_alt_screen);

    /* cursor visibility */
    RUN_TEST(test_dectcem_hide_show);

    /* ESC sequences */
    RUN_TEST(test_esc_ri_reverse_index);
    RUN_TEST(test_esc_ri_scrolls_at_top);
    RUN_TEST(test_esc_nel);

    /* charset */
    RUN_TEST(test_charset_dec_gfx_active);

    /* dirty */
    RUN_TEST(test_dirty_after_print);
    RUN_TEST(test_dirty_cleared);

    /* reset */
    RUN_TEST(test_esc_ris_full_reset);

    /* DA1 / DSR / CPR */
    RUN_TEST(test_da1_response);
    RUN_TEST(test_da1_param0);
    RUN_TEST(test_dsr_status);
    RUN_TEST(test_dsr_cpr);
    RUN_TEST(test_no_response_when_cb_null);

    /* Synchronized output — mode ?2026 */
    RUN_TEST(test_sync_initial_state);
    RUN_TEST(test_sync_mode_bsu);
    RUN_TEST(test_sync_mode_esu);
    RUN_TEST(test_sync_bsu_esu_roundtrip);
    RUN_TEST(test_sync_decrqm_inactive);
    RUN_TEST(test_sync_decrqm_active);
    RUN_TEST(test_sync_reset_clears_mode);

    /* scrollback */
    RUN_TEST(test_sb_disabled_stores_nothing);
    RUN_TEST(test_sb_capacity_distinct_from_length);
    RUN_TEST(test_sb_accumulates_evicted_rows);
    RUN_TEST(test_sb_capacity_caps_length);
    RUN_TEST(test_sb_scroll_reveals_history);
    RUN_TEST(test_sb_scroll_clamps_both_ends);
    RUN_TEST(test_sb_reset_returns_to_live);
    RUN_TEST(test_sb_view_holds_content_as_ring_grows);
    RUN_TEST(test_sb_alt_screen_does_not_feed_history);
    RUN_TEST(test_sb_scroll_refused_on_alt_screen);
    RUN_TEST(test_sb_alt_screen_entry_snaps_to_live);
    RUN_TEST(test_sb_ed2_keeps_history);
    RUN_TEST(test_sb_survives_alt_screen_app_exit);
    RUN_TEST(test_sb_ed3_clears_history);
    RUN_TEST(test_sb_hard_reset_clears_history);
    RUN_TEST(test_sb_partial_region_scroll_is_not_history);
    RUN_TEST(test_sb_ring_wraps_in_order);

    return UNITY_END();
}
