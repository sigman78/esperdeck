/*
 * ui_layout.h — cell-grid geometry for the UI kit: rects, rect-cut
 * slicing, equal-cell grids and the per-grid metrics. Pure integer
 * math with no display dependency, so the host tests cover it at every
 * grid (100x30, 80x24, 66x20). Design notes: docs/ui-kit.md.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A rectangle in character cells. Empty when w or h is <= 0. */
typedef struct { int x, y, w, h; } ui_rect_t;

static inline ui_rect_t ui_rect(int x, int y, int w, int h)
{
    return (ui_rect_t){ x, y, w, h };
}

static inline bool ui_rect_empty(ui_rect_t r) { return r.w <= 0 || r.h <= 0; }

static inline bool ui_rect_has(ui_rect_t r, int col, int row)
{
    return col >= r.x && col < r.x + r.w && row >= r.y && row < r.y + r.h;
}

/* Rect-cut: slice @p n cells off one side of @p r and return the slice.
 * @p r shrinks to what remains. A cut larger than the rect takes all of
 * it; a negative cut takes nothing. */
ui_rect_t ui_cut_top(ui_rect_t *r, int n);
ui_rect_t ui_cut_bottom(ui_rect_t *r, int n);
ui_rect_t ui_cut_left(ui_rect_t *r, int n);
ui_rect_t ui_cut_right(ui_rect_t *r, int n);

/** Shrink by @p dx cells left and right, @p dy rows top and bottom. */
ui_rect_t ui_inset(ui_rect_t r, int dx, int dy);

/** Clamps to @p outer when the size does not fit. */
ui_rect_t ui_center(ui_rect_t outer, int w, int h);

/* Equal cells laid out row-major inside an area. The cell width absorbs
 * the area; spare columns go to the right edge, spare rows to the
 * bottom. */
typedef struct {
    ui_rect_t area;
    int ncols, nrows;      /* cells per row / rows that fit            */
    int cw, ch;            /* one cell, in character cells             */
    int gx, gy;            /* gutters                                  */
} ui_grid_t;

/** @p ncols columns of @p ch-row cells; nrows = whatever fits @p area. */
ui_grid_t ui_grid(ui_rect_t area, int ncols, int ch, int gx, int gy);

/** Rect of cell @p i (row-major); empty past the last fitting cell. */
ui_rect_t ui_grid_cell(const ui_grid_t *g, int i);

static inline int ui_grid_capacity(const ui_grid_t *g)
{
    return g->ncols * g->nrows;
}

/* Paging for content that outgrows one grid: whole pages, no partial
 * scroll. @p per is the grid capacity. */
int ui_page_count(int count, int per);

/* Keeps the shown page valid after the item count shrinks. */
int ui_page_clamp(int page, int count, int per);

/* Size class of the character grid — the one switch layouts branch on. */
typedef enum {
    UI_COMPACT = 0,        /* 66x20 (12x24 font) */
    UI_REGULAR,            /* 80x24 (10x20 font) */
    UI_WIDE,               /* 100x30 (8x16 font) */
} ui_class_t;

/* Spacing tokens for one grid. The touch unit is odd, so a one-line
 * label centers on a real row. */
typedef struct {
    uint8_t cls;           /* ui_class_t                                */
    uint8_t margin;        /* screen side margin, cells                 */
    uint8_t gutter;        /* between columns, cells                    */
    uint8_t unit;          /* touch unit: min interactive height, rows  */
    uint8_t header;        /* header bar height, rows                   */
} ui_metrics_t;

ui_metrics_t ui_metrics_for(int cols, int rows);
