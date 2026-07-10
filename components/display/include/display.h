/*
 * Display driver interface for Waveshare ESP32-S3-Touch-LCD-7
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_attr.h"
#ifndef BUILD_SIMULATOR
#include "esp_lcd_panel_ops.h"
#endif

// Display dimensions
#define DISPLAY_WIDTH   800
#define DISPLAY_HEIGHT  480

// Bounce buffer configuration (one character row at a time)
#define BOUNCE_BUFFER_HEIGHT  16  // One character row (8x16 font)
#define BOUNCE_BUFFER_SIZE    (DISPLAY_WIDTH * BOUNCE_BUFFER_HEIGHT)

// Color format (RGB565)
typedef uint16_t color_t;

// RGB565 color macros
#define RGB565(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
#define COLOR_BLACK     RGB565(0, 0, 0)
#define COLOR_WHITE     RGB565(255, 255, 255)
#define COLOR_RED       RGB565(255, 0, 0)
#define COLOR_GREEN     RGB565(0, 255, 0)
#define COLOR_BLUE      RGB565(0, 0, 255)
#define COLOR_YELLOW    RGB565(255, 255, 0)
#define COLOR_CYAN      RGB565(0, 255, 255)
#define COLOR_MAGENTA   RGB565(255, 0, 255)

/*
 * Terminal cell — defined here (not in terminal.h) so the display ISR can
 * read cell data without creating a circular dependency.
 *
 * fg_color / bg_color store pre-converted RGB565 values so the renderer can
 * use them directly without a per-cell palette lookup.  Both terminal.c and
 * vterm.c call display_ansi_to_rgb565() when writing cells.
 */
typedef struct {
    uint16_t cp;        // Unicode codepoint (BMP, U+0000..U+FFFF)
    uint16_t fg_color;  // Foreground RGB565
    uint16_t bg_color;  // Background RGB565
    uint8_t  attrs;     // Attribute flags (see ATTR_* below)
} terminal_cell_t;

/* tsm_cell_t is cast to terminal_cell_t by the vterm bridge; the layouts
 * must stay byte-compatible (tsm uses the padding byte at offset 7). */
_Static_assert(sizeof(terminal_cell_t) == 8, "terminal_cell_t must be 8 bytes");

/**
 * Convert an ANSI-256 palette index to an RGB565 value.
 * Covers the full range: 0-15 named, 16-231 6×6×6 cube, 232-255 grayscale.
 * IRAM_ATTR — safe to call from the ESP32 bounce-buffer ISR.
 */
color_t display_ansi_to_rgb565(uint8_t ansi);

// Cell attribute flags
#define ATTR_BOLD       (1 << 0)
#define ATTR_UNDERLINE  (1 << 1)
#define ATTR_REVERSE    (1 << 2)
#define ATTR_BLINK      (1 << 3)

// Cursor shape
typedef enum {
    CURSOR_NONE = 0,    // invisible
    CURSOR_UNDERSCORE,  // horizontal bar — last 2 scanlines of cell
    CURSOR_BLOCK,       // full character cell, XOR
} cursor_mode_t;

/**
 * Initialize display driver
 */
esp_err_t display_init(void);

#ifndef BUILD_SIMULATOR
/**
 * Get LCD panel handle (hardware target only)
 */
esp_lcd_panel_handle_t display_get_panel(void);
#endif /* BUILD_SIMULATOR */

/**
 * Overlay cell — a second compositing layer rendered on top of the primary
 * terminal buffer.  cp == 0 means "transparent" (primary cell shows through).
 * All overlay cells share the same fg/bg colors set via
 * display_set_overlay_colors(); OVERLAY_ATTR_INVERSE swaps them per cell
 * (menu selection highlight).
 *
 * OVERLAY_ATTR_DIM on a TRANSPARENT cell (cp == 0) is a scrim: the primary
 * terminal still shows through, but at ~50% brightness — used to dim the live
 * session behind a modal so the modal pops.
 */
#define OVERLAY_ATTR_INVERSE  (1 << 0)
#define OVERLAY_ATTR_DIM      (1 << 1)

/* Per-cell accent color: index into the overlay palette. 0 = use the screen's
 * default overlay fg (set via display_set_overlay_colors); 1..N pick a fixed
 * accent so a single screen can mix colors without a per-screen repaint. */
#define OVERLAY_COL_DEFAULT   0
#define OVERLAY_COL_GREEN     1
#define OVERLAY_COL_CYAN      2
#define OVERLAY_COL_MAGENTA   3
#define OVERLAY_COL_AMBER     4
#define OVERLAY_COL_RED       5
#define OVERLAY_COL_BLUE      6
#define OVERLAY_COL_WHITE     7
#define OVERLAY_PAL_SIZE      8

typedef struct {
    uint16_t cp;     /* BMP codepoint; 0 = transparent */
    uint8_t  attrs;  /* OVERLAY_ATTR_* flags            */
    uint8_t  color;  /* OVERLAY_COL_* accent palette index */
} display_overlay_cell_t;

/** Register (or clear) the overlay buffer.  Pass NULL to disable the overlay.
 *  buf must reside in DRAM (DRAM_ATTR / static DRAM).
 *  cols/rows must match the registered terminal buffer dimensions. */
void display_set_overlay_buffer(display_overlay_cell_t *buf, int cols, int rows);

/** Set the fg/bg RGB565 colors used for all non-transparent overlay cells. */
void display_set_overlay_colors(color_t fg, color_t bg);

/** Query the currently registered terminal buffer dimensions (0,0 if not set). */
void display_get_text_size(int *cols, int *rows);

/** Trigger the visual bell: a brief decaying vertical screen shake (a speaker-
 *  less substitute for the terminal BEL). Safe to call from any task. */
void display_bell(void);

/**
 * Register the terminal cell buffer so the display ISR can render from it.
 *
 * Call once after vterm_init(). The pointer must remain valid for the
 * lifetime of the display (never free the cell buffer).
 *
 * @param buf   Pointer to cols*rows terminal_cell_t array (must be in DRAM)
 * @param cols  Number of character columns
 * @param rows  Number of character rows
 */
void display_set_text_buffer(const terminal_cell_t *buf, int cols, int rows);

/**
 * Set backlight brightness (0-100%)
 */
esp_err_t display_set_backlight(uint8_t brightness);

/**
 * Update the cursor position and shape.
 * Called by the terminal whenever the cursor moves or its mode changes.
 */
void display_set_cursor(int x, int y, cursor_mode_t mode);

#ifdef BUILD_SIMULATOR
/**
 * Render one full frame to the SDL2 window (simulator only).
 * Call once per iteration of the main event loop.
 */
void display_render_frame(void);

/**
 * Toggle the SDL2 window between 1× and 2× scale (simulator only).
 * The texture resolution stays fixed at DISPLAY_WIDTH × DISPLAY_HEIGHT;
 * SDL scales it to fill the window.
 */
void display_toggle_scale(void);
#endif /* BUILD_SIMULATOR */

#endif // DISPLAY_H
