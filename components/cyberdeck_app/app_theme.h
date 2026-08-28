/*
 * app_theme.h — overlay style-table builder (shell-internal). Pure
 * function of the screen theme colors, so the host test can assert
 * invariants over every entry (docs/overlay-style.md).
 */

#pragma once

#include "cyberdeck_ui.h"

/** Fill @p out[UI_PAL_COUNT] with every (style, accent) pair resolved
 *  against the screen theme @p fg / @p bg. */
void ui_theme_build(color_t fg, color_t bg, display_overlay_style_t *out);
