/*
 * app_widgets.c — shared shell chrome + tile-grid layout.
 *
 * Pure drawing + geometry helpers used by every screen module; reads the
 * shared state only for the animation clock (app.anim_frame) and cfg.
 */

#include "app_widgets.h"
#include "font.h"   /* FONT_WIDTH/FONT_HEIGHT — touch pixel→cell mapping */
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"   /* free-RAM stats (idfsim stubs it) */

/* ---------------------------------------------------------- tile grid */

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
    int cc = px / FONT_WIDTH  - g->x0;
    int cr = py / FONT_HEIGHT - g->y0;
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
    if (!app.cfg.ble || !app.cfg.ble->get_state) return "n/a";
    switch (app.cfg.ble->get_state()) {
    case 4:  return "connected";        /* BLE_CONNECTED       */
    case 3:  return "connecting...";    /* BLE_CONNECTING      */
    case 2:  return "pairing scan";     /* BLE_PAIRING_SCAN    */
    case 1:  return "searching...";     /* BLE_RECONNECT       */
    default: return "idle";
    }
}

/* Shared full-screen picker grid (HOME + PAIRING + menu pickers + EFFECTS):
 * 3 columns of solid tiles sized from the character grid, capped above the
 * footer. 100x30 → 3x4 of 30x5 (240x80 px ≈ 15 mm — a comfortable finger
 * target); 80x24 → 3x3 of 24x4 (80 px); 66x20 → 3x3 of 20x3 (72 px). */
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

/* Animated scanner: a cyan ░▒▓█ "comet" sweeps left→right along an
 * otherwise empty row (the ═══ divider it used to ride is gone — the solid
 * tile bars carry the layout now). */
void draw_rule_scan(int row, uint32_t frame)
{
    int W = ui_cols();
    static const uint16_t comet[4] = { UI_SHADE1, UI_SHADE2, UI_SHADE3, UI_BLOCK };
    int head = (int)((frame * 2u) % (uint32_t)W);   /* 2 cells/frame */
    ui_pen(OVERLAY_COL_CYAN);
    for (int k = 0; k < 4; k++) {
        int x = head - (3 - k);
        if (x >= 0 && x < W) ui_putch(x, row, comet[k], 0);
    }
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

/* Wall-clock "HH:MM" once real time exists: wifi_manager's one-shot NTP
 * fetch on device (0 until synced — no RTC battery), host clock in the
 * simulator. TZ comes from CONFIG_CYBERDECK_TZ via localtime. */
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

/* Title chip framed by a shade gradient: ░▒▓█ TEXT █▓▒░, drawn at cell x0 on
 * row 0. The flanking blocks glow: a cyan "spark" travels through them each
 * frame for a subtle animated shimmer. Its total width is strlen(text)+10. */
void draw_titlebar(int x0, const char *text, uint32_t frame)
{
    int spark = (int)((frame / 3u) % 4u);
    int x = x0;
    static const uint16_t lg[4] = { UI_SHADE1, UI_SHADE2, UI_SHADE3, UI_BLOCK };
    for (int i = 0; i < 4; i++) {
        ui_pen(i == spark ? OVERLAY_COL_CYAN : OVERLAY_COL_MAGENTA);
        ui_putch(x++, 0, lg[i], 0);
    }
    ui_pen(OVERLAY_COL_CYAN);
    ui_putch(x++, 0, ' ', OVERLAY_ATTR_INVERSE);
    ui_puts (x, 0, text, OVERLAY_ATTR_INVERSE | OVERLAY_ATTR_BOLD);
    x += (int)strlen(text);
    ui_putch(x++, 0, ' ', OVERLAY_ATTR_INVERSE);
    static const uint16_t rg[4] = { UI_BLOCK, UI_SHADE3, UI_SHADE2, UI_SHADE1 };
    for (int i = 0; i < 4; i++) {
        ui_pen((3 - i) == spark ? OVERLAY_COL_CYAN : OVERLAY_COL_MAGENTA);
        ui_putch(x++, 0, rg[i], 0);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}

/* Footer strip shared by every full screen: a rule, then the hint riding a
 * cyan powerline segment — ▶ hint ▶ — that tapers off with UI_PL_R (its
 * first use). INVERSE puts the accent in the cell background, so the run
 * reads as a solid bar with dark text.
 * @p limit: first column the chip must stay clear of (a right-aligned toast
 * lives there), or -1 for the full width. Clipping is internal so callers
 * never depend on the chip geometry. */
void draw_footer_lim(const char *hint, int limit)
{
    int r = ui_rows() - 1;
    if (limit < 0) limit = ui_cols();
    int avail = limit - 6;    /* lead-in(3) + trail space + taper + 1 gap */
    if (avail <= 0) return;   /* no room: rule only, no orphaned chip stub */
    char clip[96];
    snprintf(clip, sizeof(clip), "%.*s", avail, hint);
    ui_pen(OVERLAY_COL_CYAN);
    ui_putch(0, r, ' ', OVERLAY_ATTR_INVERSE);
    ui_putch(1, r, UI_PLAY, OVERLAY_ATTR_INVERSE);
    ui_putch(2, r, ' ', OVERLAY_ATTR_INVERSE);
    ui_puts(3, r, clip, OVERLAY_ATTR_INVERSE);
    int end = 3 + (int)strlen(clip);
    ui_putch(end,     r, ' ', OVERLAY_ATTR_INVERSE);
    ui_putch(end + 1, r, UI_PL_R, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
}

void draw_footer(const char *hint) { draw_footer_lim(hint, -1); }

/* Standard modal header: animated titlebar chip and a right-aligned
 * "// tag" in blue. Shared by CONNECTING / NEW PROFILE. */
void draw_screen_header(const char *title, const char *tag)
{
    draw_titlebar(2, title, app.anim_frame);
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - (int)strlen(tag) - 1, 0, tag, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
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
 * white via INVERSE. Fits itself to the grid: starts at row 6 on tall grids,
 * row 4 on short ones (overdrawing the comet rule — the quiet zone keeps it
 * scannable), drops the caption when the bottom is tight, and draws nothing
 * when the modules cannot fit above the footer hint. Returns the QR's first
 * column (the caller's text limit), or ui_cols() when no QR was drawn. */
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
