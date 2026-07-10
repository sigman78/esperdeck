/*
 * Spy implementation of display_set_text_buffer().
 *
 * Stores the pointer and dimensions so tests can inspect the cell buffer
 * directly without going through any display or ISR code.
 */
#include "display.h"

const terminal_cell_t *g_test_cell_buf  = NULL;
int                    g_test_cell_cols = 0;
int                    g_test_cell_rows = 0;

color_t display_ansi_to_rgb565(uint8_t ansi)
{
    return ansi;
}

void display_set_text_buffer(const terminal_cell_t *buf, int cols, int rows)
{
    g_test_cell_buf  = buf;
    g_test_cell_cols = cols;
    g_test_cell_rows = rows;
}

void display_set_cursor(int x, int y, cursor_mode_t mode)
{
    (void)x; (void)y; (void)mode;
}
