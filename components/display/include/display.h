/*
 * Display driver interface for Waveshare ESP32-S3-Touch-LCD-7
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_attr.h"
#include "font.h"
#ifndef BUILD_SIMULATOR
#include "esp_lcd_panel_ops.h"
#endif

// Display dimensions
#define DISPLAY_WIDTH   800
#define DISPLAY_HEIGHT  480

/*
 * Bounce band: one character row at 8x16, HALF a row for taller fonts —
 * full 20/24-scanline bounce buffers would cost up to +25.6 KB of scarce
 * internal DMA RAM. Boot picks the font size, so the band height is a
 * runtime value. esp_lcd then fixes the geometry at panel init; changing
 * size needs a reboot.
 */
int display_band_height(void);   /* scanlines in one bounce band */
int display_bounce_px(void);     /* DISPLAY_WIDTH * display_band_height() */

/* Publish the active cell size to the renderer. Call after font_init() and
 * BEFORE the panel comes up. Rejects a size that would not tile the panel. */
void display_render_set_font(int width, int height);

/* Upper bound over every selectable size. Static allocations use it
 * before the boot sequence picks the font. */
#define BOUNCE_BUFFER_MAX_PX  (DISPLAY_WIDTH * FONT_MAX_BAND)

/* Character grid implied by the panel and the ACTIVE font.
 * 12x24 leaves an 8 px right margin (66*12 = 792) — rendered black. */
int display_text_cols(void);
int display_text_rows(void);

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

/* Terminal cell — defined here so the display ISR can read cell data
 * without a circular dependency. Colors are pre-converted RGB565, so the
 * renderer needs no per-cell palette lookup. */
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

/*
 * Overlay cell — a second compositing layer above the terminal buffer;
 * cp == 0 is transparent. All cells share the fg/bg pair set via
 * display_set_overlay_colors().
 *   INVERSE  swaps fg/bg per cell (solid bars, selection).
 *   DIM      on a transparent cell = scrim: the terminal shows through at
 *            ~50% brightness (modal backdrop). On an opaque cell, DIM
 *            selects the muted variant. An INVERSE bar drops to the
 *            darker bar palette (value wells, ui-spec). Text renders its
 *            accent at half brightness (inactive indicators).
 *   BRIGHT   focus wash: bg 50% toward white — a focused bar turns pastel.
 *   BOLD     use the real bold face; falls back to the normal glyph when
 *            no bold form exists. Pure glyph swap, colors untouched.
 */
#define OVERLAY_ATTR_INVERSE  (1 << 0)
#define OVERLAY_ATTR_DIM      (1 << 1)
#define OVERLAY_ATTR_BRIGHT   (1 << 2)
#define OVERLAY_ATTR_BOLD     (1 << 3)

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

/** Trigger the visual bell (a speakerless BEL). Safe from any task. */
void display_bell(void);

/** Register the terminal cell buffer (cols*rows, DRAM) for the ISR to
 *  render from. Call once after vterm_init(); never free the buffer. */
void display_set_text_buffer(const terminal_cell_t *buf, int cols, int rows);

/**
 * Set backlight brightness (0-100).
 * Not implemented: the driver ignores brightness and always leaves the
 * backlight fully on. lcd_driver.c has the pending PWM implementation.
 */
esp_err_t display_set_backlight(uint8_t brightness);

/**
 * Update the cursor position and shape.
 * Called by the terminal whenever the cursor moves or its mode changes.
 */
void display_set_cursor(int x, int y, cursor_mode_t mode);

/**
 * Drain (read, do not reset) the render-ISR per-chunk cycle bench.
 * avg_cycles/max_cycles cover the samples since the last reset; chunks is
 * the sample count. All zero when CONFIG_DISPLAY_ISR_BENCH is off.
 * Safe to call from any task: internal SRAM has no cache on the S3.
 */
void display_render_bench_get(uint32_t *avg_cycles, uint32_t *max_cycles, uint32_t *chunks);

/** Clear the render-ISR cycle bench accumulators. */
void display_render_bench_reset(void);

#ifdef BUILD_SIMULATOR
/** Render one full frame to the SDL2 window; call once per event loop. */
void display_render_frame(void);

/** Toggle the SDL2 window between 1× and 2× scale. */
void display_toggle_scale(void);

/** Map window coordinates (SDL mouse) to framebuffer coordinates.
 *  SDL_RenderCopy stretches the display texture to fill the window. This
 *  function reverses that with a straight rescale, then clamps the result
 *  to the framebuffer. */
void display_window_to_fb(int wx, int wy, uint16_t *fx, uint16_t *fy);

/** Render the current frame off-screen and write it as a 24-bit BMP —
 *  the --drive `shot:` verb (ui-spec mockup rounds). */
esp_err_t display_screenshot_bmp(const char *path);
#endif /* BUILD_SIMULATOR */

#endif // DISPLAY_H
