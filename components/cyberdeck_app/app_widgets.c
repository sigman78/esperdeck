/*
 * app_widgets.c — shared shell chrome + tile-grid layout.
 *
 * Pure drawing + geometry helpers used by every screen module; reads the
 * shared state only for the animation clock (app.anim_frame) and cfg.
 */

#include "app_widgets.h"
#include "font.h"   /* font_width/height() — touch pixel→cell mapping */
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"   /* free-RAM stats (idfsim stubs it) */

/* --------------------------------------------------------- drag → rows */

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

/* ------------------------------------------------------------ ListView */

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

/* ------------------------------------------------------ action buttons */

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
 * This is the two-axis hit-test: a tap must land inside a tile on BOTH axes,
 * not merely on the right row. */
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

const char *wifi_status_str(void)
{
    switch (wifi_manager_get_state()) {
    case WIFI_MGR_CONNECTED:  return wifi_manager_get_ip();
    case WIFI_MGR_CONNECTING: return "connecting...";
    case WIFI_MGR_LOST:       return "reconnecting...";
    case WIFI_MGR_FAILED:     return "failed (retrying)";
    default:                  return "off";
    }
}

const char *ble_status_str(void)
{
    if (!app.ble || !app.ble->get_state) return "n/a";
    switch (app.ble->get_state()) {
    case CYBERDECK_BLE_CONNECTED:    return "connected";
    case CYBERDECK_BLE_CONNECTING:   return "connecting...";
    case CYBERDECK_BLE_PAIRING_SCAN: return "pairing scan";
    case CYBERDECK_BLE_RECONNECT:    return "searching...";
    default:                         return "idle";
    }
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

/* Wall-clock "HH:MM": SNTP time on device (0 until synced — no RTC
 * battery), host clock in the simulator. */
bool clock_str(char *buf, size_t sz)
{
    time_t t = wifi_manager_time();
    if (t == 0) return false;
    struct tm tm;
#ifdef _WIN32
    if (localtime_s(&tm, &t) != 0) return false;  /* UCRT: no localtime_r */
#else
    if (!localtime_r(&t, &tm)) return false;
#endif
    snprintf(buf, sz, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return true;
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
    ui_putch(x++, 0, ' ', OVERLAY_ATTR_INVERSE);
    ui_puts (x, 0, text, OVERLAY_ATTR_INVERSE | OVERLAY_ATTR_BOLD);
    x += (int)strlen(text);
    ui_putch(x++, 0, ' ', OVERLAY_ATTR_INVERSE);
    static const uint16_t rg[4] = { UI_BLOCK, UI_SHADE3, UI_SHADE2, UI_SHADE1 };
    ui_pen(OVERLAY_COL_MAGENTA);
    for (int i = 0; i < 4; i++)
        ui_putch(x++, 0, rg[i], 0);
    ui_pen(OVERLAY_COL_DEFAULT);
}

/* One StatusBar patch. Lit = BRIGHT wash of its accent — the overlay
 * resolve gives BRIGHT bars BLACK text — plus the bold face; off = the
 * dimmed companion, receding. Patches sit adjacent, no gaps. */
static int sb_patch(int x, int row, const char *txt, uint8_t accent, bool lit)
{
    ui_pen(lit ? accent : OVERLAY_COL_BLUE);
    ui_puts(x, row, txt,
            OVERLAY_ATTR_INVERSE | OVERLAY_ATTR_BOLD |
            (lit ? OVERLAY_ATTR_BRIGHT : OVERLAY_ATTR_DIM));
    return x + (int)strlen(txt);
}

/* The StatusBar: lettered indicator patches left, clock right; a live
 * toast owns the indicator span. */
void ui_statusbar(uint64_t now)
{
    const int sr = ui_rows() - 1;
    ui_pen(OVERLAY_COL_BLUE);
    for (int c = 0; c < ui_cols(); c++)
        ui_putch(c, sr, ' ', OVERLAY_ATTR_INVERSE);

    if (app.toast[0] && now < app.toast_until) {
        char clip[96];
        snprintf(clip, sizeof(clip), " %.*s ", ui_cols() - 12, app.toast);
        ui_puts(1, sr, clip,
                OVERLAY_ATTR_INVERSE | OVERLAY_ATTR_BRIGHT |
                OVERLAY_ATTR_BOLD);
    } else {
        /* A keystore-lock indicator is deliberately absent — a locked
         * deck shows the PIN pad, the state is self-evident (user call,
         * 2026-08-27). Caps is a keyboard sub-state, not a peer of
         * NET/KBD: an amber chip fused to the KBD patch, present only
         * while the lock is ON (design round, 2026-08-27). Num lock is
         * unrendered — the keymap ignores it, an indicator would have
         * no referent (get_locks still reports the bit). */
        int x = sb_patch(1, sr, " NET ", OVERLAY_COL_GREEN,
                         wifi_manager_is_connected());
        const bool kbd = app.ble && app.ble->get_state &&
                         app.ble->get_state() == CYBERDECK_BLE_CONNECTED;
        x = sb_patch(x, sr, " KBD ", OVERLAY_COL_CYAN, kbd);
        if (kbd && app.ble->get_locks &&
            (app.ble->get_locks() & CYBERDECK_KBD_LOCK_CAPS))
            sb_patch(x, sr, " C ", OVERLAY_COL_AMBER, true);
    }

    char clk[10];
    if (clock_str(clk + 1, sizeof(clk) - 2)) {
        clk[0] = ' ';
        size_t n = strlen(clk);
        clk[n] = ' ';
        clk[n + 1] = '\0';
        ui_pen(OVERLAY_COL_BLUE);
        ui_puts(ui_cols() - (int)strlen(clk) - 1, sr, clk,
                OVERLAY_ATTR_INVERSE | OVERLAY_ATTR_BRIGHT |
                OVERLAY_ATTR_BOLD);
    }
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

/* Free-RAM summary for the header. */
void ram_stats(char *buf, size_t sz)
{
#ifdef BUILD_SIMULATOR
    snprintf(buf, sz, "host build");
#else
    unsigned in = (unsigned)(heap_caps_get_free_size(
                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);
    unsigned ps = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    snprintf(buf, sz, "int %uK  psram %uK", in, ps);
#endif
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

/* Draw a QR top-right as half-block cells (two QR rows per cell), dark-on-
 * white via INVERSE. Fits itself to the grid, dropping caption/QR when the
 * space runs out. Returns the QR's first column (the caller's text limit),
 * or ui_cols() when no QR was drawn. */
int draw_qr_panel(int qsz, qr_module_fn mod, const char *caption)
{
    if (qsz <= 0) return ui_cols();
    const int QZ = 2;                        /* quiet-zone modules */
    int span  = qsz + 2 * QZ;
    int crows = (span + 1) / 2;
    int qy = ui_rows() >= 28 ? 6 : 4;
    if (qy + crows > ui_rows() - 1) return ui_cols();
    int qx = ui_cols() - span - 2;
    ui_pen(OVERLAY_COL_WHITE);
    for (int cr = 0; cr < crows; cr++) {
        for (int cc = 0; cc < span; cc++) {
            bool top = mod(cc - QZ, 2 * cr - QZ);
            bool bot = mod(cc - QZ, 2 * cr - QZ + 1);
            uint16_t g = (top && bot) ? UI_BLOCK
                       : top ? 0x2580u              /* upper half block */
                       : bot ? 0x2584u              /* lower half block */
                       : ' ';
            ui_putch(qx + cc, qy + cr, g, OVERLAY_ATTR_INVERSE);
        }
    }
    if (caption && qy + crows <= ui_rows() - 3) {
        ui_pen(OVERLAY_COL_CYAN);
        ui_puts(qx + (span - (int)strlen(caption)) / 2, qy + crows, caption, 0);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
    return qx;
}

/* Numbered onboarding step at row @p y: number + label, then the value as an
 * INVERSE chip — inline after the label when it fits left of @p xlimit
 * (the QR column), else indented on the next row. Returns the next step row. */
int draw_step(int y, char num, const char *label,
                     const char *value, uint8_t value_pen, int xlimit)
{
    ui_pen(OVERLAY_COL_CYAN);
    ui_putch(4, y, (uint16_t)(uint8_t)num, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_printf(6, y, 0, "%.*s", xlimit > 6 ? xlimit - 6 : 0, label);
    if (!value || !*value) return y + 2;
    int vx = 6 + (int)strlen(label) + 1;
    ui_pen(value_pen);
    if (vx + (int)strlen(value) + 2 <= xlimit) {
        ui_printf(vx, y, OVERLAY_ATTR_INVERSE, " %s ", value);
        ui_pen(OVERLAY_COL_DEFAULT);
        return y + 2;
    }
    ui_printf(8, y + 1, OVERLAY_ATTR_INVERSE, " %s ", value);
    ui_pen(OVERLAY_COL_DEFAULT);
    return y + 3;
}

/* ------------------------------------------------- scrollback indicator */

/* U+2581..U+2588 fill the bottom n/8 of a cell. (No upper-eighth glyphs
 * exist in Terminus, which is why the fill is anchored to the bottom.) */
#define BLK_LOWER(n)  ((uint16_t)(0x2580u + (n)))

void draw_scrollbar(int offset, int total)
{
    if (total <= 0) return;

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

    /* Every cell needs the SAME attrs: mixing INVERSE with plain cells gives
     * them different backgrounds and leaves a black notch at the boundary.
     * BRIGHT then DIM lands a dark-gray bg under a medium-white fg without
     * adding palette entries. */
    const uint8_t bar_attrs = OVERLAY_ATTR_BRIGHT | OVERLAY_ATTR_DIM;
    ui_pen(OVERLAY_COL_WHITE);
    for (int y = 0; y < rows; y++) {
        const int from_bottom = rows - 1 - y;
        uint16_t  cp;
        if (from_bottom < full)                cp = UI_BLOCK;
        else if (from_bottom == full && rem)   cp = BLK_LOWER(rem);
        else                                   cp = ' ';
        ui_putch(col, y, cp, bar_attrs);
    }

    /* Pen 0 + INVERSE resolves to neutral gray with dark text. Nothing at
     * offset 0 — the empty bar already says it. */
    if (offset > 0) {
        char label[16];
        int n  = snprintf(label, sizeof(label), " %d ", offset);
        int lx = col - 1 - n;
        if (lx >= 0) {
            ui_pen(OVERLAY_COL_DEFAULT);
            ui_puts(lx, 0, label, OVERLAY_ATTR_INVERSE);
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}
