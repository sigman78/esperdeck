/*
 * Terminal emulator implementation — low-level cell buffer API.
 *
 * Provides raw Unicode cell writes, cursor control and scrolling.
 * No VT/ANSI escape sequences are interpreted here; use the vterm
 * component for a full VT emulator on top of the display subsystem.
 */

#include "terminal.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "terminal";

static struct {
    int cols;
    int rows;
    int cursor_x;
    int cursor_y;
    cursor_mode_t cursor_mode;
    terminal_cell_t *buffer;
    uint8_t current_fg;
    uint8_t current_bg;
    uint8_t current_attrs;
    bool initialized;
    /* UTF-8 decode state — persists across terminal_write calls */
    struct {
        uint16_t acc;     /* codepoint accumulator */
        uint8_t  left;    /* continuation bytes still expected */
        uint8_t  discard; /* 1 = consuming a >BMP sequence, emit nothing */
    } utf8;
} term;

esp_err_t terminal_init(int cols, int rows)
{
    ESP_LOGI(TAG, "Initializing terminal (%dx%d)", cols, rows);

    term.cols          = cols;
    term.rows          = rows;
    term.cursor_x      = 0;
    term.cursor_y      = 0;
    term.cursor_mode   = CURSOR_BLOCK;
    term.current_fg    = 7;
    term.current_bg    = 0;
    term.current_attrs = 0;
    term.utf8.acc      = 0;
    term.utf8.left     = 0;
    term.utf8.discard  = 0;

    size_t buf_sz = (size_t)cols * (size_t)rows * sizeof(terminal_cell_t);
    term.buffer = malloc(buf_sz);
    if (!term.buffer) {
        ESP_LOGE(TAG, "Failed to allocate terminal buffer");
        return ESP_ERR_NO_MEM;
    }

    color_t def_fg = display_ansi_to_rgb565(term.current_fg);
    color_t def_bg = display_ansi_to_rgb565(term.current_bg);
    for (int i = 0; i < cols * rows; i++) {
        term.buffer[i].cp       = 0x0020;
        term.buffer[i].fg_color = def_fg;
        term.buffer[i].bg_color = def_bg;
        term.buffer[i].attrs    = 0;
    }

    term.initialized = true;

    display_set_text_buffer(term.buffer, cols, rows);
    display_set_cursor(0, 0, term.cursor_mode);

    ESP_LOGI(TAG, "Terminal initialized: %dx%d, buffer %zu bytes",
             cols, rows, buf_sz);
    return ESP_OK;
}

static void put_char(uint16_t cp)
{
    if (!term.initialized) return;

    if (cp == '\n') {
        term.cursor_x = 0;
        term.cursor_y++;
        if (term.cursor_y >= term.rows) {
            terminal_scroll_up(1);
            term.cursor_y = term.rows - 1;
        }
        return;
    }
    if (cp == '\r') { term.cursor_x = 0; return; }
    if (cp == '\b') { if (term.cursor_x > 0) term.cursor_x--; return; }

    if (term.cursor_x >= term.cols) {
        term.cursor_x = 0;
        term.cursor_y++;
        if (term.cursor_y >= term.rows) {
            terminal_scroll_up(1);
            term.cursor_y = term.rows - 1;
        }
    }

    int idx = term.cursor_y * term.cols + term.cursor_x;
    term.buffer[idx].cp       = cp;
    term.buffer[idx].fg_color = display_ansi_to_rgb565(term.current_fg);
    term.buffer[idx].bg_color = display_ansi_to_rgb565(term.current_bg);
    term.buffer[idx].attrs    = term.current_attrs;
    term.cursor_x++;
}

/*
 * Feed one raw byte through the UTF-8 → Unicode decoder.
 * Sequences above U+FFFF are replaced with U+FFFD.
 */
static void utf8_feed(uint8_t byte)
{
    if (term.utf8.left > 0) {
        if ((byte & 0xC0u) == 0x80u) {
            if (!term.utf8.discard)
                term.utf8.acc = (uint16_t)((term.utf8.acc << 6) | (byte & 0x3Fu));
            if (--term.utf8.left == 0) {
                if (!term.utf8.discard) put_char(term.utf8.acc);
                term.utf8.acc     = 0;
                term.utf8.discard = 0;
            }
        } else {
            term.utf8.left    = 0;
            term.utf8.acc     = 0;
            term.utf8.discard = 0;
            utf8_feed(byte);
        }
        return;
    }

    if      (byte < 0x80u)               { put_char((uint16_t)byte); }
    else if ((byte & 0xE0u) == 0xC0u)    { term.utf8.acc = byte & 0x1Fu; term.utf8.left = 1; term.utf8.discard = 0; }
    else if ((byte & 0xF0u) == 0xE0u)    { term.utf8.acc = byte & 0x0Fu; term.utf8.left = 2; term.utf8.discard = 0; }
    else if ((byte & 0xF8u) == 0xF0u)    { put_char(0xFFFDu); term.utf8.acc = 0; term.utf8.left = 3; term.utf8.discard = 1; }
}

void terminal_write(const char *data, size_t len)
{
    if (!term.initialized) return;
    for (size_t i = 0; i < len; i++)
        utf8_feed((uint8_t)data[i]);
    display_set_cursor(term.cursor_x, term.cursor_y, term.cursor_mode);
}

void terminal_print(const char *str)
{
    terminal_write(str, strlen(str));
}

void terminal_set_color(uint8_t fg, uint8_t bg)
{
    term.current_fg = fg;
    term.current_bg = bg;
}

void terminal_set_attrs(uint8_t attrs)
{
    term.current_attrs = attrs;
}

void terminal_clear(void)
{
    if (!term.initialized) return;
    color_t cfg = display_ansi_to_rgb565(term.current_fg);
    color_t cbg = display_ansi_to_rgb565(term.current_bg);
    for (int i = 0; i < term.cols * term.rows; i++) {
        term.buffer[i].cp       = 0x0020;
        term.buffer[i].fg_color = cfg;
        term.buffer[i].bg_color = cbg;
        term.buffer[i].attrs    = 0;
    }
    term.cursor_x = 0;
    term.cursor_y = 0;
    display_set_cursor(0, 0, term.cursor_mode);
}

void terminal_set_cursor(int x, int y)
{
    if (x >= 0 && x < term.cols) term.cursor_x = x;
    if (y >= 0 && y < term.rows) term.cursor_y = y;
    display_set_cursor(term.cursor_x, term.cursor_y, term.cursor_mode);
}

void terminal_get_cursor(int *x, int *y)
{
    if (x) *x = term.cursor_x;
    if (y) *y = term.cursor_y;
}

void terminal_scroll_up(int lines)
{
    if (!term.initialized || lines <= 0) return;
    int move = term.rows - lines;
    if (move > 0)
        memmove(term.buffer,
                term.buffer + lines * term.cols,
                (size_t)move * (size_t)term.cols * sizeof(terminal_cell_t));
    color_t cfg = display_ansi_to_rgb565(term.current_fg);
    color_t cbg = display_ansi_to_rgb565(term.current_bg);
    int start = move * term.cols;
    for (int i = start; i < term.cols * term.rows; i++) {
        term.buffer[i].cp       = 0x0020;
        term.buffer[i].fg_color = cfg;
        term.buffer[i].bg_color = cbg;
        term.buffer[i].attrs    = 0;
    }
}

void terminal_set_cursor_mode(cursor_mode_t mode)
{
    term.cursor_mode = mode;
    display_set_cursor(term.cursor_x, term.cursor_y, mode);
}

cursor_mode_t terminal_get_cursor_mode(void)
{
    return term.cursor_mode;
}

void terminal_input(char key)
{
    ESP_LOGI(TAG, "Key input: 0x%02X ('%c')", key, key);
}
