/*
 * ui_widgets.h — helpers every part shares: clipped text, the live
 * grid's metrics and a touch in pixels. The parts themselves are in
 * ui_part.h. Included through cyberdeck_ui.h. Design notes:
 * docs/ui-kit.md.
 */

#pragma once

#include "ui_hot.h"
#include "ui_layout.h"

/* Horizontal alignment for ui_text. */
enum { UI_LEFT = 0, UI_CENTER, UI_RIGHT };

/** Draw @p s on the first row of @p r, clipped to its width with a
 *  trailing ellipsis. @p style as for ui_putch; the pen is the caller's. */
void ui_text(ui_rect_t r, const char *s, uint8_t style, int align);

/** Metrics of the live grid. */
ui_metrics_t ui_metrics(void);

/** Feed a touch in framebuffer pixels (ui_hot_touch in cell units). */
ui_act_t ui_touch_px(ui_touch_t kind, int px, int py);
