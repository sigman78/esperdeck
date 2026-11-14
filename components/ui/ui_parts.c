/*
 * ui_parts.c — the kit's parts: tile grid, list, drag converter, button
 * bar and the shared chrome helpers. Pure drawing + geometry over the
 * primitives in ui.c; no shell state.
 */

#include "cyberdeck_ui.h"
#include "font.h"   /* font_width/height() — touch pixel→cell mapping */

#include <stdio.h>
#include <string.h>

#ifndef CONFIG_INPUT_TOUCH_SCROLL_SPEED_PCT
#define CONFIG_INPUT_TOUCH_SCROLL_SPEED_PCT 100
#endif

int ui_drag_rows(ui_drag_t *d, int dy, int row_px)
{
    if (row_px <= 0) return 0;
    d->accum += dy * CONFIG_INPUT_TOUCH_SCROLL_SPEED_PCT;
    const int per_row = row_px * 100;
    int rows = d->accum / per_row;
    d->accum -= rows * per_row;
    return rows;
}

void ui_drag_reset(ui_drag_t *d)
{
    d->accum = 0;
}

int ui_list_visible(const ui_list_t *l)
{
    if (l->row_h <= 0) return 0;
    /* The trailing gutter row is optional: N rows need N*row_h - 1 cells. */
    int n = (l->h + 1) / l->row_h;
    return n < 0 ? 0 : n;
}

void ui_list_clamp(ui_list_t *l)
{
    const int vis = ui_list_visible(l);
    if (l->sel >= l->count) l->sel = l->count ? l->count - 1 : 0;
    if (l->sel < 0)         l->sel = 0;
    const int max_top = l->count > vis ? l->count - vis : 0;
    if (l->top > max_top)   l->top = max_top;
    if (l->top < 0)         l->top = 0;
    if (l->sel < l->top)              l->top = l->sel;
    if (vis && l->sel >= l->top + vis) l->top = l->sel - vis + 1;
}

int ui_list_row_y(const ui_list_t *l, int idx)
{
    const int vis = ui_list_visible(l);
    if (idx < l->top || idx >= l->top + vis || idx >= l->count) return -1;
    return l->y + (idx - l->top) * l->row_h;
}

int ui_list_hit(const ui_list_t *l, int px, int py)
{
    const int cc = px / font_width();
    const int cr = py / font_height();
    if (cc < l->x || cc >= l->x + l->w || cr < l->y || l->row_h <= 0)
        return -1;
    const int rel = cr - l->y;
    if (l->row_h > 1 && rel % l->row_h == l->row_h - 1) return -1;  /* gutter */
    const int idx = l->top + rel / l->row_h;
    return (rel / l->row_h < ui_list_visible(l) && idx < l->count) ? idx : -1;
}

bool ui_list_nav(ui_list_t *l, ui_key_t k)
{
    if (l->count <= 0) return false;
    const int vis = ui_list_visible(l);
    int sel = l->sel;
    switch (k) {
    case K_UP:          sel -= 1;   break;
    case K_DOWN:        sel += 1;   break;
    case K_SCROLL_UP:   sel -= vis; break;   /* page */
    case K_SCROLL_DOWN: sel += vis; break;
    default: return false;
    }
    if (sel < 0)         sel = 0;
    if (sel >= l->count) sel = l->count - 1;
    if (sel == l->sel) return false;
    l->sel = sel;
    ui_list_clamp(l);
    return true;
}

int ui_list_scroll(ui_list_t *l, int dy_px)
{
    const int rows = ui_drag_rows(&l->drag, dy_px, font_height() * l->row_h);
    if (!rows) return 0;
    const int vis = ui_list_visible(l);
    const int max_top = l->count > vis ? l->count - vis : 0;
    int top = l->top + rows;
    if (top > max_top) top = max_top;
    if (top < 0)       top = 0;
    const int moved = top - l->top;
    if (!moved) return 0;
    l->top = top;
    /* Selection follows the view so keyboard focus is never off-screen. */
    if (l->sel < l->top)        l->sel = l->top;
    if (l->sel >= l->top + vis) l->sel = l->top + vis - 1;
    return moved;
}

void ui_list_draw_scroll(const ui_list_t *l)
{
    const int vis = ui_list_visible(l);
    if (l->count <= vis || l->h <= 0) return;
    /* Shade column on the rect's right edge: lit span = visible window. */
    const int x = l->x + l->w - 1;
    const int y0 = l->y + l->top * l->h / l->count;
    const int y1 = l->y + (l->top + vis) * l->h / l->count;
    for (int y = l->y; y < l->y + l->h; y++)
        ui_putch(x, y, (y >= y0 && y <= y1) ? UI_SHADE3 : UI_SHADE1, 0);
}

tilegrid_t ui_button_bar(int y0, int count, int tw, int th)
{
    tilegrid_t g = { .tw = tw, .th = th, .gx = 4, .gy = 0,
                     .ncols = count, .nrows = 1, .count = count };
    g.x0 = (ui_cols() - (count * tw + (count - 1) * g.gx)) / 2;
    g.y0 = y0;
    return g;
}

void ui_button(const tilegrid_t *g, int slot, const char *label,
               const char *body, bool sel)
{
    ui_tile(tile_x(g, slot), tile_y(g, slot), g->tw, g->th, label, body, sel);
}

/* Cell coordinates of tile @p slot's top-left corner. */
int tile_x(const tilegrid_t *g, int slot)
{
    return g->x0 + (slot % g->ncols) * (g->tw + g->gx);
}
int tile_y(const tilegrid_t *g, int slot)
{
    return g->y0 + (slot / g->ncols) * (g->th + g->gy);
}

/* Map a touch pixel to a tile slot, or -1 for a gutter / margin / empty cell.
 * This is the two-axis hit-test. A tap must land inside a tile on BOTH
 * axes, not merely on the right row. */
int tile_hit(const tilegrid_t *g, int px, int py)
{
    if (g->ncols <= 0 || g->nrows <= 0) return -1;
    int cc = px / font_width()  - g->x0;
    int cr = py / font_height() - g->y0;
    if (cc < 0 || cr < 0) return -1;
    int pitchx = g->tw + g->gx, pitchy = g->th + g->gy;
    int col = cc / pitchx, row = cr / pitchy;
    if (col >= g->ncols || row >= g->nrows) return -1;
    if (cc % pitchx >= g->tw || cr % pitchy >= g->th) return -1;  /* gutter */
    int slot = row * g->ncols + col;
    return slot < g->count ? slot : -1;
}

/* Keyboard navigation within the grid (arrow keys). */
int tile_nav(const tilegrid_t *g, int sel, ui_key_t k)
{
    if (g->count <= 0) return 0;
    int col = sel % g->ncols, row = sel / g->ncols;
    switch (k) {
    case K_LEFT:  if (col > 0)                                sel -= 1;       break;
    case K_RIGHT: if (col < g->ncols - 1 && sel + 1 < g->count) sel += 1;     break;
    case K_UP:    if (row > 0)                                sel -= g->ncols; break;
    case K_DOWN:  if (sel + g->ncols < g->count)              sel += g->ncols; break;
    default: break;
    }
    if (sel < 0)             sel = 0;
    if (sel >= g->count)     sel = g->count - 1;
    return sel;
}

/* Shared full-screen picker grid: 3 columns of solid tiles sized from the
 * character grid, capped above the footer (100x30 → 3x4, 80x24/66x20 → 3x3;
 * every tile stays a comfortable finger target). */
tilegrid_t picker_grid(int count)
{
    tilegrid_t g = { .ncols = 3, .gx = 2, .gy = 1, .y0 = 4 };
    g.x0 = ui_cols() >= 97 ? 3 : 1;
    g.tw = (ui_cols() - 2 * g.x0 - (g.ncols - 1) * g.gx) / g.ncols;
    g.th = ui_rows() >= 28 ? 5 : ui_rows() >= 22 ? 4 : 3;
    int avail = ui_rows() - 2 - g.y0;          /* keep clear of the footer */
    g.nrows = (avail + g.gy) / (g.th + g.gy);
    int cap = g.ncols * g.nrows;
    g.count = count < cap ? count : cap;
    return g;
}

/* Full-width horizontal rule under a header. */
void draw_rule(int row)
{
    const int W = ui_cols();
    ui_pen(OVERLAY_COL_BLUE);
    for (int x = 0; x < W; x++)
        ui_putch(x, row, UI_BOX_H, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
}

/* 8-frame braille spinner (U+2800 block — present in the font). */
uint16_t spinner_glyph(uint32_t frame)
{
    static const uint16_t sp[8] = {
        0x280B, 0x2819, 0x2839, 0x2838, 0x283C, 0x2834, 0x2826, 0x2827
    };
    return sp[frame % 8];
}

/* Braille "noise" glyph from a hash — the shared recipe for every static/
 * rain/decode effect. Skips the blank U+2800 pattern so a speck can never
 * be invisible. */
uint16_t braille_noise(uint32_t h)
{
    return (uint16_t)(0x2801 + h % 255u);
}

/* Title chip framed by a static shade gradient: ░▒▓█ TEXT █▓▒░ on row 0.
 * Total width strlen(text)+10. */
void draw_titlebar(int x0, const char *text)
{
    int x = x0;
    static const uint16_t lg[4] = { UI_SHADE1, UI_SHADE2, UI_SHADE3, UI_BLOCK };
    ui_pen(OVERLAY_COL_MAGENTA);
    for (int i = 0; i < 4; i++)
        ui_putch(x++, 0, lg[i], 0);
    ui_pen(OVERLAY_COL_CYAN);
    ui_putch(x++, 0, ' ', UI_BAR);
    ui_puts (x, 0, text, UI_BAR | UI_BOLD);
    x += (int)strlen(text);
    ui_putch(x++, 0, ' ', UI_BAR);
    static const uint16_t rg[4] = { UI_BLOCK, UI_SHADE3, UI_SHADE2, UI_SHADE1 };
    ui_pen(OVERLAY_COL_MAGENTA);
    for (int i = 0; i < 4; i++)
        ui_putch(x++, 0, rg[i], 0);
    ui_pen(OVERLAY_COL_DEFAULT);
}

/* Standard modal header: titlebar chip, a right-aligned "// tag" in blue,
 * and a rule on row 3. Shared by CONNECTING / NEW PROFILE. */
void draw_screen_header(const char *title, const char *tag)
{
    draw_titlebar(2, title);
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - (int)strlen(tag) - 1, 0, tag, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    draw_rule(3);
}

/* Stable per-profile accent from a djb2 hash of the name — profiles get a
 * visual identity on HOME and CONNECTING. RED (destructive) and WHITE are
 * deliberately excluded. */
uint8_t prof_accent(const char *name)
{
    static const uint8_t pal[] = {
        OVERLAY_COL_GREEN, OVERLAY_COL_CYAN, OVERLAY_COL_MAGENTA,
        OVERLAY_COL_AMBER, OVERLAY_COL_BLUE,
    };
    uint32_t h = 5381;
    while (*name) h = h * 33 + (uint8_t)*name++;
    return pal[h % (sizeof(pal) / sizeof(pal[0]))];
}

/* U+2581..U+2588 fill the bottom n/8 of a cell. Terminus has no
 * upper-eighth glyphs, so the fill anchors to the bottom. */
#define BLK_LOWER(n)  ((uint16_t)(0x2580u + (n)))

void draw_scrollbar(int offset, int total)
{
    if (total <= 0) return;

    /* Pin the pen; never inherit the caller's. TRACK bakes one look
     * for every accent, and the theme test locks that. */
    ui_pen(OVERLAY_COL_DEFAULT);

    const int col  = ui_cols() - 1;
    const int rows = ui_rows();
    if (col < 0 || rows < 1) return;

    if (offset < 0)     offset = 0;
    if (offset > total) offset = total;

    /* Fill height in eighths of a cell, measured up from the bottom. */
    int f = (int)(((long)offset * 8 * rows) / total);
    if (f > 8 * rows) f = 8 * rows;
    const int full = f / 8;
    const int rem  = f % 8;

    /* Every cell shares the one TRACK entry, so the column's background
     * is seamless and the fill always contrasts with it. */
    for (int y = 0; y < rows; y++) {
        const int from_bottom = rows - 1 - y;
        uint16_t  cp;
        if (from_bottom < full)                cp = UI_BLOCK;
        else if (from_bottom == full && rem)   cp = BLK_LOWER(rem);
        else                                   cp = ' ';
        ui_putch(col, y, cp, UI_TRACK);
    }

    /* Pen 0 BAR is neutral gray with dark text. Nothing at offset 0 —
     * the empty bar already says it. */
    if (offset > 0) {
        char label[16];
        int n  = snprintf(label, sizeof(label), " %d ", offset);
        int lx = col - 1 - n;
        if (lx >= 0)
            ui_puts(lx, 0, label, UI_BAR);
    }
}
