/*
 * app_ui.h — overlay TUI internals (shell-only). The drawing surface
 * itself is public in cyberdeck_ui.h; what stays here is frame
 * COMPOSITION — init, clear, present, colors — which belongs to the
 * shell's nav pass alone: screens draw, never present.
 */

#pragma once

#include "cyberdeck_ui.h"
#include "esp_err.h"

/** Allocate the overlay buffer for the current display size. */
esp_err_t ui_init(void);

/** Publish the drawn frame (double-buffered, atomic) and swap. */
void ui_present(void);
/** Unregister the overlay so the terminal buffer shows through. */
void ui_hide(void);
bool ui_visible(void);

/** Park the terminal cursor off-screen (call in full-screen modals). */
void ui_no_cursor(void);

/** Set the two overlay colors (all cells share them; INVERSE swaps). */
void ui_colors(color_t fg, color_t bg);

/** Clear the whole overlay to transparent. */
void ui_clear(void);

/** Feed the ~10 fps animation frame counter (drives the tile marquee). */
void ui_frame(uint32_t frame);

#ifdef BUILD_SIMULATOR
/** True when one row of the currently published overlay contains @p text. */
bool ui_debug_contains(const char *text);
#endif
