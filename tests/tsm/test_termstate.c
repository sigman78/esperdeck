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
    return tsm_screen(t)[row * tsm_cols(t) + col];
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
    tsm_t *t = tsm_new(80, 24);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(80, tsm_cols(t));
    TEST_ASSERT_EQUAL_INT(24, tsm_rows(t));
    tsm_free(t);
}

void test_tsm_new_initial_cursor(void)
{
    tsm_t *t = tsm_new(80, 24);
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(0, col);
    TEST_ASSERT_EQUAL_UINT8(0, row);
    TEST_ASSERT_TRUE(vis);
    tsm_free(t);
}

void test_tsm_new_initial_cells_blank(void)
{
    tsm_t *t = tsm_new(80, 24);
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
    TEST_ASSERT_NULL(tsm_new(0, 24));
}

void test_tsm_new_null_on_zero_rows(void)
{
    TEST_ASSERT_NULL(tsm_new(80, 0));
}

/* ════════════════════════════════════════════════════════════════════════════
 * Print / cursor advance / auto-wrap
 * ════════════════════════════════════════════════════════════════════════════ */

void test_print_writes_cell(void)
{
    tsm_t *t = tsm_new(80, 24);
    feed(t, "A");
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 0));
    tsm_free(t);
}

void test_print_advances_cursor(void)
{
    tsm_t *t = tsm_new(80, 24);
    feed(t, "AB");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(2, col);
    TEST_ASSERT_EQUAL_UINT8(0, row);
    tsm_free(t);
}

void test_print_auto_wrap(void)
{
    tsm_t *t = tsm_new(10, 5);
    /* Fill first row exactly */
    feed(t, "0123456789");
    /* Next char should wrap to row 1, col 0 */
    feed(t, "X");
    TEST_ASSERT_EQUAL_HEX16('X', cp_at(t, 0, 1));
    tsm_free(t);
}

void test_print_utf8_two_byte(void)
{
    tsm_t *t = tsm_new(80, 24);
    /* U+00E9 é — UTF-8: 0xC3 0xA9 */
    uint8_t bytes[] = {0xC3, 0xA9};
    tsm_feed(t, bytes, 2);
    TEST_ASSERT_EQUAL_HEX16(0x00E9, cp_at(t, 0, 0));
    tsm_free(t);
}

void test_print_utf8_three_byte(void)
{
    tsm_t *t = tsm_new(80, 24);
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
    tsm_t *t = tsm_new(80, 24);
    feed(t, "ABC\r");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(0, col);
    TEST_ASSERT_EQUAL_UINT8(0, row);
    tsm_free(t);
}

void test_c0_lf_moves_down(void)
{
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\n");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(1, row);
    tsm_free(t);
}

void test_c0_bs_moves_left(void)
{
    tsm_t *t = tsm_new(80, 24);
    feed(t, "AB\x08");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(1, col);
    tsm_free(t);
}

void test_c0_ht_tab_stop(void)
{
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\t");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(8, col);
    tsm_free(t);
}

void test_c0_lf_scrolls_at_bottom(void)
{
    tsm_t *t = tsm_new(10, 3);
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
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\x1b[5;10H");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(9, col);   /* 1-based → 0-based */
    TEST_ASSERT_EQUAL_UINT8(4, row);
    tsm_free(t);
}

void test_csi_cup_default_params(void)
{
    tsm_t *t = tsm_new(80, 24);
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
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\x1b[5;1H\x1b[2A");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(2, row);
    tsm_free(t);
}

void test_csi_cursor_down(void)
{
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\x1b[3B");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(3, row);
    tsm_free(t);
}

void test_csi_cursor_forward(void)
{
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\x1b[5C");
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(5, col);
    tsm_free(t);
}

void test_csi_cursor_backward(void)
{
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\x1b[1;10H\x1b[3D");   /* row 1 col 10 (1-based), then back 3 */
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(6, col);  /* col 9 (0-based) - 3 = 6 */
    tsm_free(t);
}

void test_csi_cha(void)
{
    tsm_t *t = tsm_new(80, 24);
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
    tsm_t *t = tsm_new(10, 3);
    feed(t, "AAAA\nBBBB\nCCCC");
    feed(t, "\x1b[2J");
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 10; c++)
            TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, (uint8_t)c, (uint8_t)r));
    tsm_free(t);
}

void test_csi_el0_erase_to_end_of_line(void)
{
    tsm_t *t = tsm_new(10, 3);
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
    tsm_t *t = tsm_new(10, 3);
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
    tsm_t *t = tsm_new(10, 3);
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
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\x1b[1mA");
    TEST_ASSERT_BITS(CELL_ATTR_BOLD, CELL_ATTR_BOLD, cell(t, 0, 0).attrs);
    tsm_free(t);
}

void test_sgr_reset(void)
{
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\x1b[1;4mA\x1b[0mB");
    TEST_ASSERT_BITS(CELL_ATTR_BOLD | CELL_ATTR_UNDERLINE, CELL_ATTR_BOLD | CELL_ATTR_UNDERLINE,
                     cell(t, 0, 0).attrs);
    TEST_ASSERT_EQUAL_UINT8(0, cell(t, 1, 0).attrs);
    tsm_free(t);
}

void test_sgr_256_fg(void)
{
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\x1b[38;5;196mA");  /* 256-color fg: index 196 = bright red */
    uint16_t expected = color_ansi(196);
    TEST_ASSERT_EQUAL_HEX16(expected, cell(t, 0, 0).fg);
    tsm_free(t);
}

void test_sgr_256_bg(void)
{
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\x1b[48;5;21mA");  /* 256-color bg: index 21 = bright blue */
    uint16_t expected = color_ansi(21);
    TEST_ASSERT_EQUAL_HEX16(expected, cell(t, 0, 0).bg);
    tsm_free(t);
}

void test_sgr_truecolor_fg(void)
{
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\x1b[38;2;255;0;0mA");  /* truecolor red */
    TEST_ASSERT_EQUAL_HEX16(color_rgb(255, 0, 0), cell(t, 0, 0).fg);
    tsm_free(t);
}

void test_sgr_default_colors_restored(void)
{
    tsm_t *t = tsm_new(80, 24);
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
    tsm_t *t = tsm_new(10, 5);
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
    tsm_t *t = tsm_new(10, 3);
    feed(t, "\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC");
    feed(t, "\x1b[1S");  /* scroll up 1 */
    TEST_ASSERT_EQUAL_HEX16('B', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16('C', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 2));
    tsm_free(t);
}

void test_csi_sd_scroll_down(void)
{
    tsm_t *t = tsm_new(10, 3);
    feed(t, "\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC");
    feed(t, "\x1b[1T");  /* scroll down 1 */
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 0));
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16('B', cp_at(t, 0, 2));
    tsm_free(t);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Insert / delete
 * ════════════════════════════════════════════════════════════════════════════ */

void test_csi_il_insert_line(void)
{
    tsm_t *t = tsm_new(10, 4);
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
    tsm_t *t = tsm_new(10, 4);
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
    tsm_t *t = tsm_new(10, 3);
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
    tsm_t *t = tsm_new(10, 3);
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
    tsm_t *t = tsm_new(80, 24);
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
    tsm_t *t = tsm_new(10, 3);
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
    tsm_t *t = tsm_new(80, 24);
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
    tsm_t *t = tsm_new(10, 5);
    feed(t, "\x1b[3;1H");  /* row 3 */
    feed(t, "\x1bM");       /* RI — reverse index */
    int col, row; bool vis;
    tsm_cursor(t, &col, &row, &vis);
    TEST_ASSERT_EQUAL_UINT8(1, row);  /* row 3 (1-based) - 1 = row 2 (1-based) = row 1 (0-based) */
    tsm_free(t);
}

void test_esc_ri_scrolls_at_top(void)
{
    tsm_t *t = tsm_new(10, 3);
    feed(t, "\x1b[1;1HA\x1b[2;1HB\x1b[3;1HC");
    feed(t, "\x1b[1;1H\x1bM");  /* RI at top of screen → scroll down */
    TEST_ASSERT_EQUAL_HEX16(' ', cp_at(t, 0, 0));  /* new blank row at top */
    TEST_ASSERT_EQUAL_HEX16('A', cp_at(t, 0, 1));
    TEST_ASSERT_EQUAL_HEX16('B', cp_at(t, 0, 2));
    tsm_free(t);
}

void test_esc_nel(void)
{
    tsm_t *t = tsm_new(10, 5);
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
    tsm_t *t = tsm_new(80, 24);
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
    tsm_t *t = tsm_new(10, 3);
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
    tsm_t *t = tsm_new(10, 3);
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
    tsm_t *t = tsm_new(10, 3);
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
    tsm_t *t = tsm_new(80, 24);
    clear_response();
    tsm_set_response_cb(t, capture_response, NULL);
    feed(t, "\x1b[c");   /* DA1 — no param */
    TEST_ASSERT_EQUAL_size_t(7, s_resp_len);
    TEST_ASSERT_EQUAL_MEMORY("\x1b[?1;2c", s_resp_buf, 7);
    tsm_free(t);
}

void test_da1_param0(void)
{
    tsm_t *t = tsm_new(80, 24);
    clear_response();
    tsm_set_response_cb(t, capture_response, NULL);
    feed(t, "\x1b[0c");  /* DA1 — param 0, same reply */
    TEST_ASSERT_EQUAL_size_t(7, s_resp_len);
    TEST_ASSERT_EQUAL_MEMORY("\x1b[?1;2c", s_resp_buf, 7);
    tsm_free(t);
}

void test_dsr_status(void)
{
    tsm_t *t = tsm_new(80, 24);
    clear_response();
    tsm_set_response_cb(t, capture_response, NULL);
    feed(t, "\x1b[5n");  /* DSR — status report */
    TEST_ASSERT_EQUAL_size_t(4, s_resp_len);
    TEST_ASSERT_EQUAL_MEMORY("\x1b[0n", s_resp_buf, 4);
    tsm_free(t);
}

void test_dsr_cpr(void)
{
    tsm_t *t = tsm_new(80, 24);
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
    tsm_t *t = tsm_new(80, 24);
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
    tsm_t *t = tsm_new(10, 3);
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
    tsm_t *t = tsm_new(10, 3);
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
    tsm_t *t = tsm_new(10, 3);
    feed(t, "HELLO");
    feed(t, "\x1b[?1047h");
    feed(t, "\x1b[?1047l");
    TEST_ASSERT_EQUAL_HEX16('H', cp_at(t, 0, 0));
    tsm_free(t);
}

/* Hard reset from alt screen: t->cells is primary, display is blank */
void test_reset_from_alt_screen(void)
{
    tsm_t *t = tsm_new(10, 3);
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
    tsm_t *t = tsm_new(80, 24);
    TEST_ASSERT_FALSE(tsm_sync_update(t));
    tsm_free(t);
}

void test_sync_mode_bsu(void)
{
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\x1b[?2026h");
    TEST_ASSERT_TRUE(tsm_sync_update(t));
    tsm_free(t);
}

void test_sync_mode_esu(void)
{
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\x1b[?2026l");
    TEST_ASSERT_FALSE(tsm_sync_update(t));
    tsm_free(t);
}

void test_sync_bsu_esu_roundtrip(void)
{
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\x1b[?2026h");
    TEST_ASSERT_TRUE(tsm_sync_update(t));
    feed(t, "\x1b[?2026l");
    TEST_ASSERT_FALSE(tsm_sync_update(t));
    tsm_free(t);
}

void test_sync_decrqm_inactive(void)
{
    tsm_t *t = tsm_new(80, 24);
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
    tsm_t *t = tsm_new(80, 24);
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
    tsm_t *t = tsm_new(80, 24);
    feed(t, "\x1b[?2026h");
    TEST_ASSERT_TRUE(tsm_sync_update(t));
    tsm_reset(t);
    TEST_ASSERT_FALSE(tsm_sync_update(t));
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

    return UNITY_END();
}
