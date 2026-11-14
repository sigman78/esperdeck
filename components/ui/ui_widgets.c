/*
 * ui_widgets.c — the small helpers every part shares: the live grid's
 * metrics, a touch in pixels and clipped text (ui_widgets.h).
 */

#include "cyberdeck_ui.h"
#include "ui_pure.h"
#include "font.h"   /* font_width/height() — touch pixel→cell mapping */

ui_metrics_t ui_metrics(void)
{
    return ui_metrics_for(ui_cols(), ui_rows());
}

ui_act_t ui_touch_px(ui_touch_t kind, int px, int py)
{
    return ui_hot_touch(kind, px / font_width(), py / font_height());
}

void ui_text(ui_rect_t r, const char *s, uint8_t style, int align)
{
    if (ui_rect_empty(r) || !s) return;
    bool cut;
    const int n = ui_fit(s, r.w, &cut);
    int x = r.x;
    if (align == UI_CENTER)     x += (r.w - n) / 2;
    else if (align == UI_RIGHT) x += r.w - n;
    for (int i = 0; i < n; i++) {
        const uint16_t cp = ui_utf8_next(&s);
        ui_putch(x + i, r.y, cut && i == n - 1 ? UI_ELLIPSIS : cp, style);
    }
}
