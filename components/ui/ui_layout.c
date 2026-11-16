/*
 * ui_layout.c — rect-cut slicing, grids and per-grid metrics
 * (ui_layout.h). No drawing, no globals.
 */

#include "ui_layout.h"

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

ui_rect_t ui_cut_top(ui_rect_t *r, int n)
{
    n = clampi(n, 0, r->h > 0 ? r->h : 0);
    ui_rect_t s = { r->x, r->y, r->w, n };
    r->y += n;
    r->h -= n;
    return s;
}

ui_rect_t ui_cut_bottom(ui_rect_t *r, int n)
{
    n = clampi(n, 0, r->h > 0 ? r->h : 0);
    r->h -= n;
    return (ui_rect_t){ r->x, r->y + r->h, r->w, n };
}

ui_rect_t ui_cut_left(ui_rect_t *r, int n)
{
    n = clampi(n, 0, r->w > 0 ? r->w : 0);
    ui_rect_t s = { r->x, r->y, n, r->h };
    r->x += n;
    r->w -= n;
    return s;
}

ui_rect_t ui_cut_right(ui_rect_t *r, int n)
{
    n = clampi(n, 0, r->w > 0 ? r->w : 0);
    r->w -= n;
    return (ui_rect_t){ r->x + r->w, r->y, n, r->h };
}

ui_rect_t ui_inset(ui_rect_t r, int dx, int dy)
{
    ui_rect_t s = { r.x + dx, r.y + dy, r.w - 2 * dx, r.h - 2 * dy };
    if (s.w < 0) s.w = 0;
    if (s.h < 0) s.h = 0;
    return s;
}

ui_rect_t ui_center(ui_rect_t outer, int w, int h)
{
    if (w > outer.w) w = outer.w;
    if (h > outer.h) h = outer.h;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    return (ui_rect_t){ outer.x + (outer.w - w) / 2,
                        outer.y + (outer.h - h) / 2, w, h };
}

ui_grid_t ui_grid(ui_rect_t area, int ncols, int ch, int gx, int gy)
{
    ui_grid_t g = { .area = area, .ncols = ncols, .ch = ch,
                    .gx = gx, .gy = gy };
    if (ncols <= 0 || ch <= 0 || ui_rect_empty(area)) {
        g.ncols = g.nrows = 0;
        return g;
    }
    g.cw    = (area.w - (ncols - 1) * gx) / ncols;
    g.nrows = (area.h + gy) / (ch + gy);
    if (g.cw <= 0) g.ncols = g.nrows = 0;
    return g;
}

ui_rect_t ui_grid_cell(const ui_grid_t *g, int i)
{
    if (i < 0 || i >= g->ncols * g->nrows) return (ui_rect_t){ 0 };
    const int c = i % g->ncols, r = i / g->ncols;
    return (ui_rect_t){ g->area.x + c * (g->cw + g->gx),
                        g->area.y + r * (g->ch + g->gy), g->cw, g->ch };
}

int ui_cols_for(int w, int min, int gap)
{
    const int n = (w + gap) / (min + gap);
    return n < 2 ? 2 : n;
}

ui_form_cols_t ui_form_cols(ui_rect_t body, int cap_w, int cap2_w)
{
    int b = body.w / 4;
    b = b < 16 ? 16 : b > 24 ? 24 : b;
    const int x1 = body.x + cap_w, x2 = body.x + body.w - b;
    return (ui_form_cols_t){ .cap_w = cap_w, .x1 = x1, .a = x2 - cap2_w - x1,
                             .cap2_w = cap2_w, .x2 = x2, .b = b };
}

ui_flow_t ui_flow(ui_rect_t area, int gap, int h)
{
    return (ui_flow_t){ .x0 = area.x, .w = area.w, .gap = gap, .h = h,
                        .x = area.x, .y = area.y };
}

void ui_flow_break(ui_flow_t *f)
{
    if (f->x > f->x0) { f->x = f->x0; f->y += f->h; }
}

ui_rect_t ui_flow_next(ui_flow_t *f, int w)
{
    if (f->x > f->x0 && f->x + w > f->x0 + f->w) ui_flow_break(f);
    const ui_rect_t r = { f->x, f->y, w < f->w ? w : f->w, f->h };
    f->x += w + f->gap;
    return r;
}

ui_metrics_t ui_metrics_for(int cols, int rows)
{
    (void)rows;
    ui_metrics_t m = { .gutter = 2, .unit = 3, .header = 3 };
    if (cols >= 96)      { m.cls = UI_WIDE;    m.margin = 2; }
    else if (cols >= 76) { m.cls = UI_REGULAR; m.margin = 1; }
    else                 { m.cls = UI_COMPACT; m.margin = 1; }
    return m;
}

int ui_page_count(int count, int per)
{
    if (per <= 0 || count <= per) return 1;
    return (count + per - 1) / per;
}

int ui_page_clamp(int page, int count, int per)
{
    const int n = ui_page_count(count, per);
    return page < 0 ? 0 : page >= n ? n - 1 : page;
}
