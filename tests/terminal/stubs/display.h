/*
 * Host stub for display.h — contains only the types and constants that
 * terminal.c / terminal.h actually depend on.  No ESP-IDF or LCD headers.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Mirror the real display dimensions. */
#define DISPLAY_WIDTH         800
#define DISPLAY_HEIGHT        480
#define BOUNCE_BUFFER_HEIGHT  16
#define BOUNCE_BUFFER_SIZE    (DISPLAY_WIDTH * BOUNCE_BUFFER_HEIGHT)

typedef uint16_t color_t;

#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define COLOR_BLACK  RGB565(0,   0,   0)
#define COLOR_WHITE  RGB565(255, 255, 255)

/*
 * Terminal cell — must match the definition in the real display.h exactly.
 */
typedef struct {
    uint16_t cp;        /* Unicode codepoint (BMP) */
    uint8_t  fg_color;  /* ANSI-256 foreground index */
    uint8_t  bg_color;  /* ANSI-256 background index */
    uint8_t  attrs;     /* ATTR_* bitmask */
} terminal_cell_t;

#define ATTR_BOLD       (1 << 0)
#define ATTR_UNDERLINE  (1 << 1)
#define ATTR_REVERSE    (1 << 2)
#define ATTR_BLINK      (1 << 3)

typedef enum {
    CURSOR_NONE = 0,
    CURSOR_UNDERSCORE,
    CURSOR_BLOCK,
} cursor_mode_t;

void display_set_text_buffer(const terminal_cell_t *buf, int cols, int rows);
void display_set_cursor(int x, int y, cursor_mode_t mode);
