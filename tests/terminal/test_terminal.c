/*
 * Host-based unit tests for the terminal component.
 *
 * Build & run:
 *   cd tests/terminal
 *   cmake -B build && cmake --build build
 *   ./build/test_terminal
 */

#include "unity.h"
#include "terminal.h"
#include <string.h>

/* ── Spy state exposed by display_stub.c ────────────────────────────────── */
extern const terminal_cell_t *g_test_cell_buf;
extern int                    g_test_cell_cols;
extern int                    g_test_cell_rows;

/* Small terminal for predictable tests. */
#define TEST_COLS 20
#define TEST_ROWS  5

/* Convenience accessor. */
static const terminal_cell_t *cell_at(int col, int row)
{
    return &g_test_cell_buf[row * TEST_COLS + col];
}

void setUp(void)    { terminal_init(TEST_COLS, TEST_ROWS); }
void tearDown(void) { /* static term struct is re-initialised in setUp */  }


/* ════════════════════════════════════════════════════════════════════════════
 * Initialisation
 * ══════════════════════════════════════════════════════════════════════════ */

void test_init_registers_buffer_with_display(void)
{
    TEST_ASSERT_NOT_NULL(g_test_cell_buf);
    TEST_ASSERT_EQUAL_INT(TEST_COLS, g_test_cell_cols);
    TEST_ASSERT_EQUAL_INT(TEST_ROWS, g_test_cell_rows);
}

void test_init_clears_buffer_to_spaces(void)
{
    for (int r = 0; r < TEST_ROWS; r++)
        for (int c = 0; c < TEST_COLS; c++)
            TEST_ASSERT_EQUAL_UINT16(0x0020, cell_at(c, r)->cp);
}

void test_init_default_colors(void)
{
    /* Default: fg=7 (white), bg=0 (black), no attrs. */
    TEST_ASSERT_EQUAL_UINT8(7, cell_at(0, 0)->fg_color);
    TEST_ASSERT_EQUAL_UINT8(0, cell_at(0, 0)->bg_color);
    TEST_ASSERT_EQUAL_UINT8(0, cell_at(0, 0)->attrs);
}


/* ════════════════════════════════════════════════════════════════════════════
 * Basic text output
 * ══════════════════════════════════════════════════════════════════════════ */

void test_write_ascii_codepoints(void)
{
    terminal_print("Hello");
    TEST_ASSERT_EQUAL_UINT16('H', cell_at(0, 0)->cp);
    TEST_ASSERT_EQUAL_UINT16('e', cell_at(1, 0)->cp);
    TEST_ASSERT_EQUAL_UINT16('l', cell_at(2, 0)->cp);
    TEST_ASSERT_EQUAL_UINT16('l', cell_at(3, 0)->cp);
    TEST_ASSERT_EQUAL_UINT16('o', cell_at(4, 0)->cp);
    /* Rest of row still blank. */
    TEST_ASSERT_EQUAL_UINT16(0x0020, cell_at(5, 0)->cp);
}

void test_newline_advances_to_next_row(void)
{
    terminal_print("A\nB");
    TEST_ASSERT_EQUAL_UINT16('A', cell_at(0, 0)->cp);
    TEST_ASSERT_EQUAL_UINT16('B', cell_at(0, 1)->cp);
}

void test_carriage_return_resets_column(void)
{
    terminal_print("AB\rC");
    /* C overwrites A; B survives. */
    TEST_ASSERT_EQUAL_UINT16('C', cell_at(0, 0)->cp);
    TEST_ASSERT_EQUAL_UINT16('B', cell_at(1, 0)->cp);
}

void test_backspace_decrements_column(void)
{
    terminal_print("AB\bC");   /* AB, back, C → AC */
    TEST_ASSERT_EQUAL_UINT16('A', cell_at(0, 0)->cp);
    TEST_ASSERT_EQUAL_UINT16('C', cell_at(1, 0)->cp);
}

void test_backspace_does_not_go_negative(void)
{
    terminal_print("\b\bX");   /* two backspaces from col 0 — no underflow */
    TEST_ASSERT_EQUAL_UINT16('X', cell_at(0, 0)->cp);
}

void test_line_wrap_at_column_boundary(void)
{
    /* Fill one line exactly, then write one more character. */
    char buf[TEST_COLS + 2];
    memset(buf, 'X', TEST_COLS);
    buf[TEST_COLS]     = 'Y';
    buf[TEST_COLS + 1] = '\0';
    terminal_print(buf);

    TEST_ASSERT_EQUAL_UINT16('X', cell_at(TEST_COLS - 1, 0)->cp);
    TEST_ASSERT_EQUAL_UINT16('Y', cell_at(0,             1)->cp);
}


/* ════════════════════════════════════════════════════════════════════════════
 * Color & attribute API
 * ══════════════════════════════════════════════════════════════════════════ */

void test_set_color_applied_to_subsequent_cells(void)
{
    terminal_set_color(3, 5);   /* fg=3 yellow, bg=5 magenta */
    terminal_print("X");
    TEST_ASSERT_EQUAL_UINT8(3, cell_at(0, 0)->fg_color);
    TEST_ASSERT_EQUAL_UINT8(5, cell_at(0, 0)->bg_color);
}

void test_color_change_mid_line(void)
{
    terminal_set_color(1, 0);
    terminal_print("A");
    terminal_set_color(2, 0);
    terminal_print("B");
    TEST_ASSERT_EQUAL_UINT8(1, cell_at(0, 0)->fg_color);
    TEST_ASSERT_EQUAL_UINT8(2, cell_at(1, 0)->fg_color);
}

void test_set_attrs_bold(void)
{
    terminal_set_attrs(ATTR_BOLD);
    terminal_print("Z");
    TEST_ASSERT_BITS(ATTR_BOLD, ATTR_BOLD, cell_at(0, 0)->attrs);
}

void test_set_attrs_cleared_independently(void)
{
    terminal_set_attrs(ATTR_BOLD | ATTR_UNDERLINE);
    terminal_print("A");
    terminal_set_attrs(0);
    terminal_print("B");
    TEST_ASSERT_EQUAL_UINT8(ATTR_BOLD | ATTR_UNDERLINE, cell_at(0, 0)->attrs);
    TEST_ASSERT_EQUAL_UINT8(0,                          cell_at(1, 0)->attrs);
}


/* ════════════════════════════════════════════════════════════════════════════
 * Cursor positioning
 * ══════════════════════════════════════════════════════════════════════════ */

void test_set_cursor_positions_write(void)
{
    terminal_set_cursor(5, 2);
    terminal_print("@");
    TEST_ASSERT_EQUAL_UINT16('@',   cell_at(5, 2)->cp);
    TEST_ASSERT_EQUAL_UINT16(0x0020, cell_at(4, 2)->cp);   /* left untouched */
}

void test_get_cursor_tracks_position(void)
{
    int x, y;
    terminal_set_cursor(3, 1);
    terminal_get_cursor(&x, &y);
    TEST_ASSERT_EQUAL_INT(3, x);
    TEST_ASSERT_EQUAL_INT(1, y);
}

void test_cursor_advances_after_write(void)
{
    int x, y;
    terminal_set_cursor(0, 0);
    terminal_print("ABC");
    terminal_get_cursor(&x, &y);
    TEST_ASSERT_EQUAL_INT(3, x);
    TEST_ASSERT_EQUAL_INT(0, y);
}


/* ════════════════════════════════════════════════════════════════════════════
 * Scrolling
 * ══════════════════════════════════════════════════════════════════════════ */

void test_scroll_on_row_overflow(void)
{
    /* Write a distinct character into col 0 of every row. */
    for (int r = 0; r < TEST_ROWS; r++) {
        terminal_set_cursor(0, r);
        char ch[2] = { (char)('0' + r), '\0' };
        terminal_print(ch);
    }
    /* A newline from the last row triggers a scroll. */
    terminal_set_cursor(0, TEST_ROWS - 1);
    terminal_print("\nZ");

    /* Row 0 now holds what was in row 1. */
    TEST_ASSERT_EQUAL_UINT16('1', cell_at(0, 0)->cp);
    /* Last row holds the new character. */
    TEST_ASSERT_EQUAL_UINT16('Z', cell_at(0, TEST_ROWS - 1)->cp);
}

void test_clear_resets_all_cells_to_space(void)
{
    terminal_print("Hello World");
    terminal_clear();
    for (int r = 0; r < TEST_ROWS; r++)
        for (int c = 0; c < TEST_COLS; c++)
            TEST_ASSERT_EQUAL_UINT16(0x0020, cell_at(c, r)->cp);
}

void test_clear_resets_cursor_to_origin(void)
{
    terminal_set_cursor(5, 3);
    terminal_clear();
    int x, y;
    terminal_get_cursor(&x, &y);
    TEST_ASSERT_EQUAL_INT(0, x);
    TEST_ASSERT_EQUAL_INT(0, y);
}


/* ════════════════════════════════════════════════════════════════════════════
 * UTF-8 decoding
 * ══════════════════════════════════════════════════════════════════════════ */

void test_utf8_2byte_sequence(void)
{
    /* é = U+00E9, UTF-8: 0xC3 0xA9 */
    terminal_write("\xc3\xa9", 2);
    TEST_ASSERT_EQUAL_UINT16(0x00E9, cell_at(0, 0)->cp);
}

void test_utf8_3byte_sequence(void)
{
    /* ╔ = U+2554, UTF-8: 0xE2 0x95 0x94 */
    terminal_write("\xe2\x95\x94", 3);
    TEST_ASSERT_EQUAL_UINT16(0x2554, cell_at(0, 0)->cp);
}

void test_utf8_4byte_replaced_with_fffd(void)
{
    /* 😀 = U+1F600, UTF-8: 0xF0 0x9F 0x98 0x80 → U+FFFD replacement */
    terminal_write("\xf0\x9f\x98\x80", 4);
    TEST_ASSERT_EQUAL_UINT16(0xFFFD, cell_at(0, 0)->cp);
    /* Exactly one cell consumed — cursor is at col 1. */
    TEST_ASSERT_EQUAL_UINT16(0x0020, cell_at(1, 0)->cp);
}

void test_utf8_sequence_split_across_writes(void)
{
    /* Send ╔ (3-byte) as 1 + 2 bytes across two separate write calls. */
    terminal_write("\xe2", 1);             /* lead only — nothing emitted yet */
    TEST_ASSERT_EQUAL_UINT16(0x0020, cell_at(0, 0)->cp);

    terminal_write("\x95\x94", 2);         /* two continuations — now complete */
    TEST_ASSERT_EQUAL_UINT16(0x2554, cell_at(0, 0)->cp);
}

void test_utf8_stray_continuation_skipped(void)
{
    /* 0x80 is a continuation byte with no preceding lead — must be ignored. */
    terminal_write("\x80" "A", 2);
    TEST_ASSERT_EQUAL_UINT16('A', cell_at(0, 0)->cp);
}

void test_utf8_interrupted_sequence_recovers(void)
{
    /* Lead byte 0xE2 followed immediately by ASCII 'B' — abort the sequence
     * and emit 'B' at col 0. */
    terminal_write("\xe2" "B", 2);
    TEST_ASSERT_EQUAL_UINT16('B', cell_at(0, 0)->cp);
}

void test_utf8_mixed_ascii_and_multibyte(void)
{
    /* "A" + é (U+00E9) + "Z" in one buffer. */
    terminal_write("A\xc3\xa9Z", 4);
    TEST_ASSERT_EQUAL_UINT16('A',    cell_at(0, 0)->cp);
    TEST_ASSERT_EQUAL_UINT16(0x00E9, cell_at(1, 0)->cp);
    TEST_ASSERT_EQUAL_UINT16('Z',    cell_at(2, 0)->cp);
}

void test_utf8_state_persists_across_writes(void)
{
    /* Send a 3-byte char one byte at a time across three separate writes. */
    terminal_write("\xe2", 1);
    TEST_ASSERT_EQUAL_UINT16(0x0020, cell_at(0, 0)->cp);   /* not yet */
    terminal_write("\x95", 1);
    TEST_ASSERT_EQUAL_UINT16(0x0020, cell_at(0, 0)->cp);   /* still not */
    terminal_write("\x94", 1);
    TEST_ASSERT_EQUAL_UINT16(0x2554, cell_at(0, 0)->cp);   /* now ╔ */
}


/* ════════════════════════════════════════════════════════════════════════════
 * Test runner
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    UNITY_BEGIN();

    /* Init */
    RUN_TEST(test_init_registers_buffer_with_display);
    RUN_TEST(test_init_clears_buffer_to_spaces);
    RUN_TEST(test_init_default_colors);

    /* Text output */
    RUN_TEST(test_write_ascii_codepoints);
    RUN_TEST(test_newline_advances_to_next_row);
    RUN_TEST(test_carriage_return_resets_column);
    RUN_TEST(test_backspace_decrements_column);
    RUN_TEST(test_backspace_does_not_go_negative);
    RUN_TEST(test_line_wrap_at_column_boundary);

    /* Colors & attrs */
    RUN_TEST(test_set_color_applied_to_subsequent_cells);
    RUN_TEST(test_color_change_mid_line);
    RUN_TEST(test_set_attrs_bold);
    RUN_TEST(test_set_attrs_cleared_independently);

    /* Cursor */
    RUN_TEST(test_set_cursor_positions_write);
    RUN_TEST(test_get_cursor_tracks_position);
    RUN_TEST(test_cursor_advances_after_write);

    /* Scrolling */
    RUN_TEST(test_scroll_on_row_overflow);
    RUN_TEST(test_clear_resets_all_cells_to_space);
    RUN_TEST(test_clear_resets_cursor_to_origin);

    /* UTF-8 */
    RUN_TEST(test_utf8_2byte_sequence);
    RUN_TEST(test_utf8_3byte_sequence);
    RUN_TEST(test_utf8_4byte_replaced_with_fffd);
    RUN_TEST(test_utf8_sequence_split_across_writes);
    RUN_TEST(test_utf8_stray_continuation_skipped);
    RUN_TEST(test_utf8_interrupted_sequence_recovers);
    RUN_TEST(test_utf8_mixed_ascii_and_multibyte);
    RUN_TEST(test_utf8_state_persists_across_writes);

    return UNITY_END();
}
