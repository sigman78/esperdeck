/*
 * ui_frame.h — frame composition: init, clear, present, colors. This is
 * for the ONE owner of the frame (the shell's nav pass, or a host tool).
 * Screens include cyberdeck_ui.h alone: they draw, never present.
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

/** Set the screen theme colors: rebakes the overlay style table
 *  (ui_theme.c) and publishes it to the display. */
void ui_colors(color_t fg, color_t bg);

/** Clear the whole overlay to transparent. */
void ui_clear(void);

/** Feed the ~10 fps animation frame counter (drives hot-region animation). */
void ui_frame(uint32_t frame);

#ifdef BUILD_SIMULATOR
/** True when one row of the currently published overlay contains @p text. */
bool ui_debug_contains(const char *text);
/** The published frame's cells (ui_cols() x ui_rows()), NULL when hidden. */
const display_overlay_cell_t *ui_debug_frame(bool *scrim);
/** The baked style table, UI_PAL_COUNT entries. */
const display_overlay_style_t *ui_debug_styles(void);
#endif
