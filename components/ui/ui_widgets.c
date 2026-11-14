/*
 * ui_widgets.c — the card surface and the widgets built on it
 * (ui_widgets.h). All colour decisions live in card_look(). A widget
 * asks for a look and paints cells; it never picks a style itself.
 */

#include "cyberdeck_ui.h"
#include "ui_pure.h"
#include "font.h"   /* font_width/height() — touch pixel→cell mapping */

#include <stdio.h>
#include <string.h>

ui_metrics_t ui_metrics(void)
{
    return ui_metrics_for(ui_cols(), ui_rows());
}

ui_page_t ui_page(void)
{
    const ui_metrics_t m = ui_metrics();
    ui_rect_t r = ui_rect(0, 0, ui_cols(), ui_rows());
    ui_page_t p;
    p.header = ui_cut_top(&r, m.header);
    p.status = ui_cut_bottom(&r, 1);
    p.body   = ui_inset(r, m.margin, 0);
    return p;
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

/* How one card paints: which styles its cells take, and its trim. */
typedef struct {
    uint8_t fill, edge;        /* interior cells; half-row edge cells   */
    uint8_t title, detail;     /* first line; second line + trailers    */
    uint8_t value;             /* the current-value text                */
    uint8_t pen;
    bool    ring;              /* white focus rail                      */
} look_t;

static look_t card_look(uint8_t accent, uint8_t flags, bool focus, bool down)
{
    const bool off = flags & UI_CARD_DISABLED;
    look_t k = { .pen = accent, .value = UI_WELL };
    if (flags & UI_CARD_LIT) down = true;

    if (off && !focus) {
        k.fill = k.title = k.detail = UI_WELL;
        k.edge = UI_WELL_EDGE;
    } else if (focus || down) {
        k.fill = k.detail = UI_FOCUS;
        k.title = UI_FOCUS | UI_BOLD;
        k.edge  = UI_FOCUS_EDGE;
        k.ring  = focus;
    } else {
        k.fill = k.detail = UI_BAR;
        k.title = UI_BAR | UI_BOLD;
        k.edge  = UI_BAR_EDGE;
    }
    return k;
}

/* Paint the surface and return the rect its content may use. */
static ui_rect_t card_paint(ui_rect_t r, const look_t *k)
{
    const bool edged = r.h >= 3;

    ui_pen(k->pen);
    for (int y = 0; y < r.h; y++) {
        uint16_t cp = ' ';
        uint8_t  st = k->fill;
        if (edged && y == 0)       { cp = UI_HALF_DN; st = k->edge; }
        if (edged && y == r.h - 1) { cp = UI_HALF_UP; st = k->edge; }
        for (int x = 0; x < r.w; x++)
            ui_putch(r.x + x, r.y + y, cp, st);
    }

    ui_rect_t in = edged ? ui_inset(r, 0, 1) : r;

    if (k->ring) {
        /* The focus mark: a white half-cell rail with a dark seam, as
         * tall as the visible card. Quadrants carry it over the
         * half-height edge rows. */
        ui_pen(OVERLAY_COL_WHITE);
        for (int y = 0; y < r.h; y++) {
            const uint16_t cp = !edged         ? UI_LHALF
                              : y == 0         ? UI_QUAD_LL
                              : y == r.h - 1   ? UI_QUAD_UL
                                               : UI_LHALF;
            ui_putch(r.x, r.y + y, cp, UI_BAR_EDGE);
        }
        ui_pen(k->pen);
    }
    /* A key-sized card keeps one cell of padding so a 2-cell label fits. */
    return ui_inset(in, r.w >= 8 ? 2 : 1, 0);
}

void ui_card(ui_rect_t r, ui_id_t id, const ui_card_t *c)
{
    if (ui_rect_empty(r)) return;
    const bool off = c->flags & UI_CARD_DISABLED;
    ui_hot(r, id);

    const look_t k = card_look(c->accent, c->flags, ui_focused(id),
                               !off && ui_pressed(id));
    ui_rect_t in = card_paint(r, &k);
    if (ui_rect_empty(in)) return;

    /* Trailers eat the right end first; the title gets the rest. */
    if (c->value && *c->value) {
        const int vl = (int)strlen(c->value);
        /* The title keeps its length; a long value gets the rest. */
        const int room = in.w + 1 - (int)strlen(c->title);
        int ww = vl + 4 < 8 ? 8 : vl + 4;
        if (ww > room) ww = room < 8 ? 8 : room;
        ui_rect_t w = ui_cut_right(&in, ww - 2);
        for (int y = r.y; y < r.y + r.h; y++) {
            const bool top = r.h >= 3 && y == r.y;
            const bool bot = r.h >= 3 && y == r.y + r.h - 1;
            for (int x = w.x; x < w.x + w.w + 2; x++)
                ui_putch(x, y, top ? UI_HALF_DN : bot ? UI_HALF_UP : ' ',
                         (top || bot) ? UI_WELL_EDGE : UI_WELL);
        }
        ui_text(ui_rect(w.x + 1, in.y + (in.h - 1) / 2, w.w - 1, 1),
                c->value, k.value, UI_RIGHT);
        ui_cut_right(&in, 1);
    } else if (c->flags & (UI_CARD_NAV | UI_CARD_CHECKED)) {
        ui_rect_t w = ui_cut_right(&in, 1);
        ui_putch(w.x, in.y + (in.h - 1) / 2,
                 (c->flags & UI_CARD_CHECKED) ? UI_CHECK : UI_CHEVRON,
                 (c->flags & UI_CARD_CHECKED) ? k.title : k.detail);
        ui_cut_right(&in, 1);
    }

    const bool two = c->detail && *c->detail && in.h >= 2;
    const int  ty  = in.y + (in.h - (two ? 2 : 1)) / 2;
    const int  al  = (c->flags & UI_CARD_CENTER) ? UI_CENTER : UI_LEFT;
    ui_text(ui_rect(in.x, ty, in.w, 1), c->title, k.title, al);
    if (two) {
        ui_text(ui_rect(in.x, ty + 1, in.w, 1), c->detail, k.detail, al);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}

void ui_btn(ui_rect_t r, ui_id_t id, const char *label, uint8_t accent)
{
    ui_card(r, id, &(ui_card_t){ .title = label, .accent = accent,
                                 .flags = UI_CARD_CENTER | UI_CARD_ACTION });
}

void ui_header(ui_rect_t r, const char *path, const char *title,
               const char *note, bool back)
{
    if (ui_rect_empty(r)) return;
    const int m   = ui_metrics().margin;
    const int row = r.y + r.h / 2;
    const bool lit = back && (ui_focused(UI_ID_BACK) || ui_pressed(UI_ID_BACK));

    /* The whole bar is the back target: one big, forgiving hit. */
    if (back) ui_hot(r, UI_ID_BACK);

    const look_t k = card_look(OVERLAY_COL_BLUE, 0, false, lit);
    ui_rect_t in = card_paint(ui_rect(r.x, r.y, r.w, r.h), &k);
    in = ui_inset(in, m - 1, 0);
    const uint8_t st_path = k.detail, st_note = k.detail, st_title = k.title;
    if (back) {
        ui_putch(in.x, row, UI_POINT_L, k.title);
        ui_cut_left(&in, 3);
    }

    if (note && *note) {
        const int nl = (int)strlen(note);
        ui_rect_t n = ui_cut_right(&in, nl + 1 < in.w ? nl + 1 : in.w);
        ui_text(ui_rect(n.x, row, n.w, 1), note, st_note, UI_RIGHT);
    }
    if (path && *path) {
        const int pl = (int)strlen(path);
        if (pl + 3 < in.w / 2 + 3) {
            ui_text(ui_rect(in.x, row, pl, 1), path, st_path, UI_LEFT);
            ui_putch(in.x + pl + 1, row, '/', st_path);
            ui_cut_left(&in, pl + 3);
        }
    }
    ui_text(ui_rect(in.x, row, in.w, 1), title, st_title, UI_LEFT);
    ui_pen(OVERLAY_COL_DEFAULT);
}

ui_rect_t ui_pager(ui_rect_t header, ui_id_t prev, ui_id_t next,
                   int page, int pages)
{
    if (pages <= 1 || header.w < UI_PAGER_W + 8) return header;
    ui_rect_t r = ui_cut_right(&header, UI_PAGER_W);
    ui_rect_t n = ui_cut_right(&r, 5);
    ui_rect_t p = ui_cut_left(&r, 5);
    char label[24];
    snprintf(label, sizeof label, "%d/%d", page + 1, pages);

    /* An end step stays a region: a tap there must not fall through to
     * the header behind it. */
    ui_card(p, prev, &(ui_card_t){ .title = "<", .accent = OVERLAY_COL_BLUE,
        .flags = UI_CARD_CENTER | (page > 0 ? 0 : UI_CARD_DISABLED) });
    ui_card(n, next, &(ui_card_t){ .title = ">", .accent = OVERLAY_COL_BLUE,
        .flags = UI_CARD_CENTER | (page < pages - 1 ? 0 : UI_CARD_DISABLED) });
    ui_pen(OVERLAY_COL_BLUE);
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++)
            ui_putch(x, y, ' ', UI_TEXT);
    ui_text(ui_rect(r.x, r.y + r.h / 2, r.w, 1), label, UI_TEXT | UI_BOLD,
            UI_CENTER);
    ui_pen(OVERLAY_COL_DEFAULT);
    return header;
}

void ui_input(ui_rect_t r, ui_id_t id, const char *label, const char *text,
              int cursor, uint8_t flags)
{
    if (ui_rect_empty(r)) return;
    ui_hot(r, id);
    const bool edit = flags & UI_INPUT_EDITING;
    /* An editing field holds the lit look of a focused item: the eye
     * finds the caret at once. */
    look_t k = card_look(OVERLAY_COL_CYAN, 0, edit || ui_focused(id),
                         ui_pressed(id));
    ui_rect_t in = card_paint(r, &k);
    if (ui_rect_empty(in)) return;
    const int row = in.y + (in.h - 1) / 2;

    if (flags & UI_INPUT_CYCLE) {
        ui_putch(in.x + in.w - 1, row, UI_CHEVRON, k.detail);
        ui_cut_right(&in, 2);
    }
    const int ll = (int)strlen(label);
    const int lw = ll + 2 < 7 ? 7 : ll + 2;
    ui_text(ui_rect(in.x, row, lw - 1, 1), label, k.detail, UI_LEFT);
    ui_cut_left(&in, lw);
    if (in.w < 2) return;

    const int len = (int)strlen(text);
    if (cursor < 0)   cursor = 0;
    if (cursor > len) cursor = len;
    const int start = (edit && cursor > in.w - 1) ? cursor - (in.w - 1) : 0;
    for (int i = 0; i < in.w; i++) {
        const int idx = start + i;
        uint16_t cp = ' ';
        if (idx < len) cp = (flags & UI_INPUT_MASK) ? '*' : (uint8_t)text[idx];
        uint8_t st = k.title;
        if (edit && idx == cursor) {
            /* Caret: the cell flips to the opposite surface. */
            ui_pen(OVERLAY_COL_WHITE);
            st = UI_BAR;
        }
        ui_putch(in.x + i, row, cp, st);
        ui_pen(k.pen);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}

void ui_meter(ui_rect_t r, int value, int max, uint8_t accent)
{
    if (ui_rect_empty(r) || max <= 0) return;
    if (value < 0)   value = 0;
    if (value > max) value = max;
    const int eighths = (int)(((long)value * r.w * 8) / max);
    ui_pen(accent);
    for (int i = 0; i < r.w; i++) {
        const int f = eighths - i * 8;
        /* U+2589..258F are the 7/8..1/8 left blocks. */
        const uint16_t cp = f >= 8 ? UI_BLOCK
                          : f > 0  ? (uint16_t)(0x2590u - (unsigned)f) : ' ';
        ui_putch(r.x + i, r.y, cp, UI_WELL);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}

ui_rect_t ui_dialog(int w, int h, const char *title, uint8_t accent)
{
    ui_dim();
    ui_rect_t r = ui_center(ui_rect(0, 0, ui_cols(), ui_rows()), w, h);
    ui_pen(accent);
    /* A window: panel title strip over a body on the screen
     * background, inside a half-cell panel frame. The body stays
     * the screen colour so any widget's edges blend in it. */
    const int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
    for (int y = r.y; y <= y1; y++)
        for (int x = r.x; x <= x1; x++) {
            uint16_t cp = ' ';
            uint8_t  st = UI_TEXT;
            if (y == r.y)           { cp = UI_HALF_DN; st = UI_PANEL_EDGE; }
            else if (y <= r.y + 2)  { st = UI_PANEL; }
            else if (y == y1)       { cp = x == r.x ? 0x2599u : x == x1 ? 0x259Fu
                                           : UI_HALF_DN; st = UI_PANEL_EDGE; }
            else if (x == r.x)      { cp = UI_LHALF; st = UI_PANEL_EDGE; }
            else if (x == x1)       { cp = UI_RHALF; st = UI_PANEL_EDGE; }
            ui_putch(x, y, cp, st);
        }
    ui_putch(r.x + 2, r.y + 1, UI_LHALF, UI_PANEL);
    ui_text(ui_rect(r.x + 4, r.y + 1, r.w - 6, 1), title,
            UI_PANEL | UI_BOLD, UI_LEFT);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_rect_t in = ui_inset(r, 2, 1);
    ui_cut_top(&in, 3);
    return in;
}
