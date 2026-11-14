/*
 * ui_part.c — the kit's parts (ui_part.h): slabs on the theme
 * background. A part asks ui_look() for its styles and paints cells.
 * Each function ports its sketch in tools/uimock/sketches-mo.js.
 * ui_part honours UI_PART_ROOMY (pad 3); ui_switch passes its flags on.
 */

#include "cyberdeck_ui.h"
#include "ui_pure.h"

#include <stdio.h>
#include <string.h>

/* Key focus is one colour on every part. Labels beside values share a hue. */
#define PART_FOCUS OVERLAY_COL_AMBER
#define PART_LABEL OVERLAY_COL_CYAN

static const char *str(const char *s) { return s ? s : ""; }

static bool has(const char *s) { return s && *s; }

void ui_slate(void)
{
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_fill(0, 0, ui_cols(), ui_rows(), UI_TEXT);
}

ui_slate_t ui_slate_page(void)
{
    ui_slate();
    ui_slate_t p;
    p.m = ui_metrics();
    ui_rect_t r = ui_rect(0, 0, ui_cols(), ui_rows());
    p.status = ui_cut_bottom(&r, 1);
    p.body   = ui_inset(r, p.m.margin, 0);
    p.area   = p.body;
    p.header = ui_cut_top(&p.body, 3);
    return p;
}

ui_look_t ui_look(ui_id_t id, uint8_t tone, uint8_t accent, uint8_t flags)
{
    const bool focus = id && ui_focused(id);
    const bool down  = (flags & UI_PART_LIT) || (id && ui_pressed(id));
    ui_look_t k = { 0 };

    if (flags & UI_PART_OFF) {
        k.pen = OVERLAY_COL_DEFAULT;
        k.fill = UI_PANEL;
        k.edge = UI_PANEL_EDGE;
        k.title = k.detail = UI_PANEL_MUTED;
        k.card = k.off = true;
    } else if (focus || down) {
        k.pen = focus ? PART_FOCUS : accent;
        k.fill = k.detail = UI_FOCUS;
        k.edge = UI_FOCUS_EDGE;
        k.title = UI_FOCUS | UI_BOLD;
    } else if (tone == UI_TONE_SOLID) {
        k.pen = accent;
        k.fill = k.detail = UI_BAR;
        k.edge = UI_BAR_EDGE;
        k.title = UI_BAR | UI_BOLD;
    } else if (tone == UI_TONE_TINT) {
        k.pen = accent;
        k.fill = k.detail = UI_WELL;
        k.edge = UI_WELL_EDGE;
        k.title = UI_WELL | UI_BOLD;
    } else {
        k.pen = OVERLAY_COL_DEFAULT;
        k.fill = UI_PANEL;
        k.edge = UI_PANEL_EDGE;
        k.title = UI_PANEL | ((flags & UI_PART_PLAIN) ? 0 : UI_BOLD);
        k.detail = UI_PANEL_MUTED;
        k.card = true;
    }
    return k;
}

void ui_slab(ui_rect_t r, const ui_look_t *k)
{
    const int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
    ui_pen(k->pen);
    for (int x = r.x; x <= x1; x++) {
        ui_putch(x, r.y, UI_HALF_DN, k->edge);
        ui_putch(x, y1, UI_HALF_UP, k->edge);
    }
    ui_fill(r.x, r.y + 1, r.w, r.h - 2, k->fill);
}

void ui_stripe(ui_rect_t r, uint8_t accent)
{
    ui_pen(accent);
    ui_putch(r.x, r.y, UI_HALF_DN, UI_BAR_EDGE);
    for (int y = r.y + 1; y < r.y + r.h - 1; y++)
        ui_putch(r.x, y, ' ', UI_BAR);
    ui_putch(r.x, r.y + r.h - 1, UI_HALF_UP, UI_BAR_EDGE);
    ui_pen(OVERLAY_COL_DEFAULT);
}

void ui_panel(ui_rect_t r, uint8_t stripe)
{
    const ui_look_t k = ui_look(0, UI_TONE_CARD, 0, 0);
    ui_slab(r, &k);
    if (stripe) ui_stripe(r, stripe);
}

void ui_units(ui_rect_t r, const char *text, uint8_t value, uint8_t unit,
              int align)
{
    if (!text) return;
    ui_text(r, text, value, align);
    if (value == unit || ui_rect_empty(r)) return;

    const int len = ui_len(text);
    const int n = len < r.w ? len : r.w;
    const int x0 = r.x + (align == UI_CENTER ? (r.w - n) >> 1
                        : align == UI_RIGHT ? r.w - n : 0);
    const int last = len > r.w ? n - 1 : n;
    bool after_number = false;
    int i = 0;
    const char *p = text;

    while (i < len) {
        if (*p == ' ') { p++; i++; continue; }
        const char *w = p;
        int j = i;
        while (*p && *p != ' ') { ui_utf8_next(&p); j++; }
        const bool number = ui_is_number(w, (int)(p - w));
        if (!number && after_number) {
            const char *q = w;
            for (int c = i; c < j && c < last; c++)
                ui_putch(x0 + c, r.y, ui_utf8_next(&q), unit);
        }
        after_number = number;
        i = j;
    }
}

bool ui_part(ui_rect_t r, ui_id_t id, const ui_part_t *p, ui_part_box_t *box)
{
    if (r.w < 4 || r.h < 3) { ui_fault(); return false; }
    const bool off = p->flags & UI_PART_OFF;
    if (!off) ui_hot(r, id);
    const ui_look_t k = ui_look(id, p->tone, p->accent, p->flags);
    ui_slab(r, &k);
    if (!k.off && p->stripe) ui_stripe(r, p->stripe);
    ui_pen(k.pen);

    const int pad = p->pad ? p->pad
                  : (p->flags & UI_PART_ROOMY) ? 3 : (r.w >= 10 ? 2 : 1);
    const ui_rect_t inn = ui_rect(r.x + pad, r.y + 1, r.w - 2 * pad, r.h - 2);
    const bool two = has(p->detail);
    const int y = ui_centre(inn.y, inn.h, two ? 2 : 1);
    const int al = (p->flags & UI_PART_CENTER) ? UI_CENTER : UI_LEFT;
    int w = inn.w - p->reserve;

    if (has(p->right)) {
        const int n = ui_len(p->right);
        const ui_rect_t rr = ui_rect(inn.x + w - n, y, n, 1);
        if (p->flags & UI_PART_UNITS)
            ui_units(rr, p->right, (uint8_t)(k.title & ~UI_BOLD), k.detail,
                     UI_RIGHT);
        else
            ui_text(rr, p->right,
                    (p->flags & UI_PART_RIGHT_BOLD) ? k.title : k.detail,
                    UI_RIGHT);
        w -= n + 1;
    }
    ui_text(ui_rect(inn.x, y, w, 1), str(p->title), k.title, al);
    if (two)
        ui_text(ui_rect(inn.x, y + 1, inn.w - p->reserve, 1), p->detail,
                k.detail, al);
    ui_pen(OVERLAY_COL_DEFAULT);
    if (box) { box->k = k; box->y = y; box->inn = inn; }
    return true;
}

bool ui_push(ui_rect_t r, ui_id_t id, const char *label, uint8_t tone,
             uint8_t accent, uint8_t flags)
{
    const ui_part_t p = { .title = label, .tone = tone, .accent = accent,
                          .flags = (uint8_t)(flags | UI_PART_CENTER) };
    return ui_part(r, id, &p, NULL);
}

void ui_pill(ui_rect_t *header, ui_id_t id, const char *label, uint8_t tone,
             uint8_t accent, uint8_t flags)
{
    ui_push(ui_cut_right(header, ui_len(label) + 4), id, label, tone, accent,
            flags);
    ui_cut_right(header, 1);
}

void ui_switch(ui_rect_t r, ui_id_t id, const char *label, bool on,
               uint8_t flags)
{
    const ui_part_t pt = { .title = label, .reserve = 9, .flags = flags };
    ui_part_box_t b;
    if (!ui_part(r, id, &pt, &b)) return;

    const int x = b.inn.x + b.inn.w - 4;
    const bool live = on && !b.k.off;
    const uint8_t knob = live ? b.k.title : b.k.detail;
    ui_pen(live && b.k.card ? OVERLAY_COL_GREEN : b.k.pen);
    ui_text(ui_rect(x - 4, b.y, 3, 1), on ? "On" : "Off", knob, UI_RIGHT);
    /* The knob is two cells: a left and a right triangle make one diamond. */
    const int kx = on ? x + 2 : x, lx = on ? x : x + 2;
    ui_putch(lx, b.y, UI_HEAVY_H, b.k.detail);
    ui_putch(lx + 1, b.y, UI_HEAVY_H, b.k.detail);
    ui_putch(kx, b.y, UI_POINT_L, knob);
    ui_putch(kx + 1, b.y, UI_ARROW, knob);
    ui_pen(OVERLAY_COL_DEFAULT);
}

static int chip_w(const char *label)
{
    const int w = ui_len(label) + 4;
    return w < 7 ? 7 : w;
}

void ui_chips(ui_rect_t r, ui_id_t id_base, const char *caption,
              const char *const *labels, int n, int value, uint8_t accent,
              int cap_w)
{
    int need = -1;
    for (int i = 0; i < n; i++) need += chip_w(labels[i]) + 1;

    const int capw = cap_w ? cap_w : ui_caption_w(caption);
    const int y = ui_centre(r.y, r.h, 1);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_text(ui_rect(r.x + 2, y, capw - 4, 1), caption, UI_TEXT, UI_LEFT);
    if (capw + need > r.w) { ui_fault(); return; }

    int x = r.x + capw;
    for (int i = 0; i < n; i++) {
        const int w = chip_w(labels[i]);
        const bool chosen = i == value;
        char title[64];
        if (chosen) snprintf(title, sizeof title, UI_S_CHECK " %s", labels[i]);
        else        snprintf(title, sizeof title, "%s", labels[i]);
        const ui_part_t p = {
            .title = title, .pad = 1,
            .tone = chosen ? UI_TONE_SOLID : UI_TONE_CARD,
            .accent = chosen ? (accent ? accent : OVERLAY_COL_BLUE) : 0,
            .flags = (uint8_t)(UI_PART_CENTER | (chosen ? 0 : UI_PART_PLAIN)),
        };
        ui_part(ui_rect(x, r.y, w, r.h), (ui_id_t)(id_base + i), &p, NULL);
        x += w + 1;
    }
}

ui_rect_t ui_pages(ui_rect_t header, ui_id_t prev, ui_id_t next,
                   int page, int pages)
{
    if (pages <= 1) return header;
    ui_rect_t r = ui_cut_right(&header, UI_PAGES_W);
    const int y = ui_centre(r.y, r.h, 1);
    const ui_rect_t pr = ui_cut_left(&r, 6), nx = ui_cut_right(&r, 6);
    ui_push(pr, prev, UI_S_POINT_L, UI_TONE_CARD, 0,
            page <= 0 ? UI_PART_OFF : 0);
    ui_push(nx, next, UI_S_ARROW, UI_TONE_CARD, 0,
            page >= pages - 1 ? UI_PART_OFF : 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    char label[24];
    snprintf(label, sizeof label, "%d/%d", page + 1, pages);
    ui_text(ui_rect(r.x, y, r.w, 1), label, UI_TEXT | UI_BOLD, UI_CENTER);
    ui_cut_right(&header, 1);
    return header;
}

void ui_rail(ui_rect_t r, int page, int pages)
{
    if (pages <= 1 || r.h < 2) return;
    int at;
    const int thumb = ui_rail_thumb(r.h, page, pages, &at);
    ui_pen(OVERLAY_COL_DEFAULT);
    for (int i = 0; i < r.h; i++)
        ui_putch(r.x, r.y + i, UI_RHALF,
                 i >= at && i < at + thumb ? UI_MUTED : UI_PANEL_EDGE);
}

void ui_title(ui_rect_t r, const char *path, const char *title,
              const char *note, bool back)
{
    const int y = ui_centre(r.y, r.h, 1);
    if (!back) {
        const int tl = ui_len(title);
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_text(ui_rect(r.x + 1, y, tl, 1), title, UI_TEXT | UI_BOLD, UI_LEFT);
        if (has(note))
            ui_text(ui_rect(r.x + tl + 3, y, r.w - tl - 3, 1), note, UI_MUTED,
                    UI_LEFT);
        return;
    }
    ui_hot(r, UI_ID_BACK);
    const ui_look_t k = ui_look(UI_ID_BACK, UI_TONE_TINT, OVERLAY_COL_BLUE, 0);
    ui_slab(r, &k);
    int x = r.x + 2;
    ui_putch(x, y, UI_POINT_L, k.title);
    x += 3;
    if (has(path)) {
        const int pl = ui_len(path);
        ui_puts_u8(x, y, path, k.detail);
        ui_puts_u8(x + pl, y, " /", k.detail);
        x += pl + 3;
    }
    const int room = r.x + r.w - 2 - x;
    const int nl = has(note) ? ui_len(note) : 0;
    ui_text(ui_rect(x, y, room - (nl ? nl + 2 : 0), 1), title, k.title,
            UI_LEFT);
    if (nl)
        ui_text(ui_rect(r.x + r.w - 2 - nl, y, nl, 1), note, k.detail,
                UI_RIGHT);
    ui_pen(OVERLAY_COL_DEFAULT);
}

void ui_info(ui_rect_t r, const ui_info_row_t *rows, int n, uint8_t stripe)
{
    if (r.h != n + 2) { ui_fault(); return; }
    ui_panel(r, stripe);

    int lw = 0;
    for (int i = 0; i < n; i++) {
        const int l = ui_len(str(rows[i].label));
        if (l > lw) lw = l;
    }
    lw += 2;
    for (int i = 0; i < n; i++) {
        const ui_info_row_t *w = &rows[i];
        const int y = r.y + 1 + i, vx = r.x + 2 + lw, vw = r.w - 4 - lw;
        const int vl = ui_len(w->value);
        ui_pen(w->label_pen ? w->label_pen : PART_LABEL);
        ui_text(ui_rect(r.x + 2, y, lw, 1), str(w->label),
                w->label_pen ? UI_PANEL | UI_BOLD : UI_PANEL_MUTED, UI_LEFT);
        ui_pen(w->value_pen);
        if (w->value_pen)
            ui_text(ui_rect(vx, y, vw, 1), str(w->value), UI_PANEL | UI_BOLD,
                    UI_LEFT);
        else
            ui_units(ui_rect(vx, y, vw, 1), str(w->value), UI_PANEL,
                     UI_PANEL_MUTED, UI_LEFT);
        if (has(w->tail)) {
            ui_pen(w->tail_pen);
            ui_text(ui_rect(vx + vl + 2, y, vw - vl - 2, 1), w->tail,
                    UI_PANEL | UI_BOLD, UI_LEFT);
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}

void ui_entry(ui_rect_t r, ui_id_t id, const char *label, const char *value,
              int label_w, uint8_t flags)
{
    ui_entry_at(r, id, label, value, label_w, flags, ui_len(value));
}

void ui_entry_at(ui_rect_t r, ui_id_t id, const char *label, const char *value,
                 int label_w, uint8_t flags, int caret)
{
    const bool edit = flags & UI_ENTRY_EDIT, bad = flags & UI_ENTRY_BAD;
    const ui_part_t pt = { .title = "", .accent = OVERLAY_COL_CYAN,
                           .stripe = bad ? OVERLAY_COL_RED : 0,
                           .flags = edit ? UI_PART_LIT : 0 };
    ui_part_box_t b;
    if (!ui_part(r, id, &pt, &b)) return;

    const ui_look_t *k = &b.k;
    const int lw = label_w ? label_w : 10;
    const int vx = b.inn.x + lw, vw = b.inn.w - lw;
    const int n = ui_len(value);
    const int room = vw - (bad ? 2 : 0);
    if (caret > n) caret = n;
    if (caret < 0) caret = 0;
    const int first = edit && n > room - 1 ? ui_entry_first(n, caret, room) : 0;
    const int show = n - first;
    char dots[3 * 100 + 1];
    const char *shown = str(value);
    const char *under = NULL;       /* the character the caret sits on */

    if (flags & UI_ENTRY_MASK) {
        const int d = show < 100 ? show : 100;
        for (int i = 0; i < d; i++) memcpy(dots + 3 * i, UI_S_BULLET, 3);
        dots[3 * d] = 0;
        shown = dots;
        if (caret < n) under = UI_S_BULLET;
    } else {
        for (int i = first; i > 0; i--) ui_utf8_next(&shown);
        if (caret < n) {
            under = str(value);
            for (int i = caret; i > 0; i--) ui_utf8_next(&under);
        }
    }

    ui_pen(k->card ? PART_LABEL : k->pen);
    ui_text(ui_rect(b.inn.x, b.y, lw - 1, 1), str(label), k->detail, UI_LEFT);
    ui_pen(bad && k->card ? OVERLAY_COL_RED : k->pen);
    ui_text(ui_rect(vx, b.y, vw - (bad ? 2 : 0), 1), shown, k->title, UI_LEFT);
    if (bad) ui_putch(b.inn.x + b.inn.w - 1, b.y, '!', k->title);
    ui_pen(k->pen);
    if (edit) {
        const int cx = vx + caret - first, cmax = vx + vw - 1;
        const uint16_t cp = under ? ui_utf8_next(&under) : ' ';
        ui_pen(OVERLAY_COL_WHITE);
        ui_putch(cx < cmax ? cx : cmax, b.y, cp, UI_BAR);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}
