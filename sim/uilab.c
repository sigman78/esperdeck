/*
 * uilab.c — the UI kit's lab driver. The mock screens (uilab_mo.c) use
 * only kit calls and render through the real display path. Headless,
 * it writes one BMP per screen (--out DIR). With a window, the mouse
 * is the finger and the arrow keys drive focus. Host-only tool;
 * nothing here ships.
 *
 *   cyberdeck_uilab [--font 8x16|10x20|12x24] [--out DIR] [--dump DIR]
 *                   [--screen NAME]
 *
 * --dump writes the font's glyphs, the baked style tables and every
 * posed frame's cells as JSON: the input of tools/uimock.
 */

#include "uilab.h"
#include "display.h"
#include "display_fx.h"
#include "font.h"
#include "ui_frame.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- fake terminal backdrop for the modal screens ------------------- */

static terminal_cell_t *s_term;

static void term_fill(int cols, int rows)
{
    static const char *LINES[] = {
        "deck@homelab:~$ journalctl -fu sshd",
        "Nov 12 14:02:11 homelab sshd[811]: Accepted publickey for deck",
        "Nov 12 14:02:11 homelab sshd[811]: pam_unix(sshd:session): opened",
        "Nov 12 14:03:40 homelab systemd[1]: Started Daily apt upgrade.",
        "Nov 12 14:04:02 homelab kernel: wlan0: associated",
        "deck@homelab:~$ htop",
        "  PID USER      PRI  NI  VIRT   RES   SHR S CPU% MEM%   TIME+",
        "  811 deck       20   0 16.2M 6304K 5120K S  0.7  0.3  0:00.42",
        "    1 root       20   0  167M 11.9M 8448K S  0.0  0.6  0:02.10",
    };
    for (int r = 0; r < rows; r++) {
        const char *l = LINES[r % NELEM(LINES)];
        const int n = (int)strlen(l);
        for (int c = 0; c < cols; c++)
            s_term[r * cols + c] = (terminal_cell_t){
                .cp = c < n ? (uint8_t)l[c] : ' ',
                .fg_color = LAB_FG, .bg_color = LAB_BG,
            };
    }
}

static void term_blank(int cols, int rows)
{
    for (int i = 0; i < cols * rows; i++)
        s_term[i] = (terminal_cell_t){ .cp = ' ', .fg_color = LAB_FG,
                                       .bg_color = LAB_BG };
}

const lab_profile_t PROFILES[LAB_NPROFILES] = {
    { "homelab",   "root@192.168.1.10:22" },
    { "build-box", "ci@build.lan:22  key" },
    { "pi-garage", "pi@10.0.0.42:22" },
    { "vps-fra",   "deploy@fra1.example.net:2222  key" },
    { "router",    "admin@192.168.1.1:22" },
    { "nas",       "backup@nas.lan:22  key" },
    { "printer",   "octo@10.0.0.77:22" },
    { "bastion",   "ops@bastion.example.org:22  key" },
};

static int s_cols, s_rows;
uint32_t lab_frame;
static bool s_faulted;        /* a screen counted a layout fault     */

static int screen_count(void)
{
    int n;
    lab_mo_screens(&n);
    return n;
}

static const lab_screen_t *screen_at(int i)
{
    int n;
    return &lab_mo_screens(&n)[i];
}

/* Every render bakes its own theme: the dump and other screens leave
 * another one active. */
static void render(const lab_screen_t *s, uint32_t frame)
{
    if (s->slate) ui_colors(LAB_SLATE_FG, LAB_SLATE_BG);
    else          ui_colors(LAB_FG, LAB_BG);
    if (s->term) term_fill(s_cols, s_rows);
    else         term_blank(s_cols, s_rows);
    lab_frame = frame;
    ui_frame(frame);
    ui_clear();
    s->draw();
    ui_present();
}

/* Cell at the center of region @p id in the frame just drawn. */
static bool region_center(ui_id_t id, int *col, int *row)
{
    for (int r = 0; r < s_rows; r++)
        for (int c = 0; c < s_cols; c++)
            if (ui_hot_at(c, r) == id) {
                int c1 = c, r1 = r;
                while (c1 + 1 < s_cols && ui_hot_at(c1 + 1, r) == id) c1++;
                while (r1 + 1 < s_rows && ui_hot_at(c, r1 + 1) == id) r1++;
                *col = (c + c1) / 2;
                *row = (r + r1) / 2;
                return true;
            }
    return false;
}

/* Draw a screen in its documented pose. The first pass lays out the
 * regions. The second draws with the focus and the held finger set. */
static void render_posed(const lab_screen_t *s)
{
    const int faults = ui_faults();
    ui_hot_reset();
    render(s, 0);
    ui_hot_touch(UI_TOUCH_UP, 0, 0);            /* touch mode: no ring */
    int c, r;
    if (s->down && region_center(s->down, &c, &r))
        ui_hot_touch(UI_TOUCH_DOWN, c, r);
    if (s->focus) {
        ui_focus_set(s->focus);
        ui_hot_key(K_ENTER);                     /* only reveals the ring */
    }
    render(s, 1);
    if (ui_faults() > faults) {
        fprintf(stderr, "%s: layout faults\n", s->name);
        s_faulted = true;
    }
}

/* ---- model dump for tools/uimock ------------------------------------- */

static const char *const STYLE_NAMES[] = {
    "TEXT", "MUTED", "BAR", "WELL", "FOCUS", "TRACK", "BAR_EDGE",
    "WELL_EDGE", "FOCUS_EDGE", "PANEL", "PANEL_MUTED", "PANEL_EDGE",
    "FOCUS_MUTED",
};
_Static_assert(NELEM(STYLE_NAMES) == UI_STYLE_COUNT, "name every style");

/* Every glyph the active font can draw, as decoded rows. A codepoint
 * that decodes to the '?' fallback is absent and left out. */
static void dump_font(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/font-%dx%d.json", dir, font_width(),
             font_height());
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }

    const int h = font_height(), rb = (int)FONT_ROW_BYTES(font_width());
    const size_t nb = font_glyph_bytes();
    _Alignas(4) uint8_t q[FONT_MAX_GLYPH_BYTES], g[FONT_MAX_GLYPH_BYTES],
                        b[FONT_MAX_GLYPH_BYTES];
    font_decode_glyph('?', false, q);

    fprintf(f, "{\"w\":%d,\"h\":%d,\"sprite_base\":%u,\"sprite_count\":%u",
            font_width(), h, FONT_SPRITE_BASE, FONT_SPRITE_COUNT);
    for (int bold = 0; bold < 2; bold++) {
        fprintf(f, ",\n\"%s\":{", bold ? "bold" : "regular");
        bool first = true;
        for (unsigned cp = 0x20; cp <= 0xFFFF; cp++) {
            if (cp - FONT_SPRITE_BASE < FONT_SPRITE_COUNT) continue;
            font_decode_glyph((uint16_t)cp, false, g);
            if (cp != '?' && !memcmp(g, q, nb)) continue;
            const uint8_t *src = g;
            if (bold) {
                font_decode_glyph((uint16_t)cp, true, b);
                if (!memcmp(g, b, nb)) continue;   /* no bold form */
                src = b;
            }
            fprintf(f, "%s\"%u\":\"", first ? "" : ",", cp);
            for (int r = 0; r < h; r++) {
                if (rb == 1) fprintf(f, "%02x", src[r]);
                else fprintf(f, "%04x", src[r * 2] | (src[r * 2 + 1] << 8));
            }
            fputc('"', f);
            first = false;
        }
        fputc('}', f);
    }
    fprintf(f, "}\n");
    fclose(f);
}

static void dump_theme(FILE *f, const char *name, color_t fg, color_t bg,
                       bool last)
{
    ui_colors(fg, bg);
    const display_overlay_style_t *st = ui_debug_styles();
    fprintf(f, "\"%s\":{\"fg\":%u,\"bg\":%u,\"table\":[", name, fg, bg);
    for (int i = 0; i < UI_PAL_COUNT; i++)
        fprintf(f, "%s%u,%u", i ? "," : "", st[i].fg, st[i].bg);
    fprintf(f, "]}%s\n", last ? "" : ",");
}

static FILE *dump_open(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/lab-%dx%d.json", dir, font_width(),
             font_height());
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }

    const ui_metrics_t m = ui_metrics();
    fprintf(f, "{\"cw\":%d,\"ch\":%d,\"cols\":%d,\"rows\":%d,"
            "\"width\":%d,\"height\":%d,\n",
            font_width(), font_height(), s_cols, s_rows,
            DISPLAY_WIDTH, DISPLAY_HEIGHT);
    fprintf(f, "\"metrics\":{\"cls\":%d,\"margin\":%d,\"gutter\":%d,"
            "\"unit\":%d,\"header\":%d},\n",
            m.cls, m.margin, m.gutter, m.unit, m.header);
    fprintf(f, "\"accents\":%d,\"styles\":[", OVERLAY_ACCENTS);
    for (int i = 0; i < UI_STYLE_COUNT; i++)
        fprintf(f, "%s\"%s\"", i ? "," : "", STYLE_NAMES[i]);
    fprintf(f, "],\n\"themes\":{\n");
    dump_theme(f, "hazard", COLOR_WHITE, RGB565(96, 0, 0), false);
    dump_theme(f, "default", LAB_FG, LAB_BG, false);
    dump_theme(f, "slate", LAB_SLATE_FG, LAB_SLATE_BG, true);
    fprintf(f, "},\n\"screens\":[\n");
    return f;
}

/* One posed frame: overlay cells, the terminal under it, hit regions. */
static void dump_screen(FILE *f, const lab_screen_t *s, bool first)
{
    bool scrim = false;
    const display_overlay_cell_t *ov = ui_debug_frame(&scrim);
    const int n = s_cols * s_rows;

    fprintf(f, "%s{\"name\":\"%s\",\"theme\":\"%s\",\"scrim\":%s,"
            "\"focus\":%u,\"down\":%u,\n\"ov\":[", first ? "" : ",\n",
            s->name, s->slate ? "slate" : "default",
            scrim ? "true" : "false", s->focus, s->down);
    for (int i = 0; i < n; i++)
        fprintf(f, "%s%u,%u,%u", i ? "," : "", ov[i].cp, ov[i].attrs,
                ov[i].pal);
    fprintf(f, "],\n\"term\":[");
    for (int i = 0; i < n; i++)
        fprintf(f, "%s%u,%u,%u,%u", i ? "," : "", s_term[i].cp,
                s_term[i].fg_color, s_term[i].bg_color, s_term[i].attrs);

    /* Regions as the finger meets them: the bounding box of each id. */
    struct { ui_id_t id; int x0, y0, x1, y1; } box[UI_HOT_MAX];
    int nbox = 0;
    for (int r = 0; r < s_rows; r++) {
        for (int c = 0; c < s_cols; c++) {
            const ui_id_t id = ui_hot_at(c, r);
            if (!id) continue;
            int k = 0;
            while (k < nbox && box[k].id != id) k++;
            if (k == nbox) {
                if (nbox == UI_HOT_MAX) continue;
                box[nbox].id = id;
                box[nbox].x0 = box[nbox].x1 = c;
                box[nbox].y0 = box[nbox].y1 = r;
                nbox++;
                continue;
            }
            if (c < box[k].x0) box[k].x0 = c;
            if (c > box[k].x1) box[k].x1 = c;
            if (r > box[k].y1) box[k].y1 = r;
        }
    }
    fprintf(f, "],\n\"hot\":[");
    for (int k = 0; k < nbox; k++)
        fprintf(f, "%s[%u,%d,%d,%d,%d]", k ? "," : "", box[k].id,
                box[k].x0, box[k].y0, box[k].x1 - box[k].x0 + 1,
                box[k].y1 - box[k].y0 + 1);
    fprintf(f, "]}");
}

static int find_screen(const char *name)
{
    for (int i = 0; i < screen_count(); i++)
        if (!strcmp(screen_at(i)->name, name)) return i;
    return -1;
}

int main(int argc, char *argv[])
{
    font_size_t font = FONT_SIZE_8X16;
    const char *out = NULL, *only = NULL, *dump = NULL;
    for (int i = 1; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(argv[i], "--font")) {
            font = !strcmp(v, "12x24") ? FONT_SIZE_12X24
                 : !strcmp(v, "10x20") ? FONT_SIZE_10X20 : FONT_SIZE_8X16;
            i++;
        } else if (!strcmp(argv[i], "--out"))    { out = v;  i++; }
        else if (!strcmp(argv[i], "--dump"))     { dump = v; i++; }
        else if (!strcmp(argv[i], "--screen"))   { only = v; i++; }
        else {
            fprintf(stderr, "usage: cyberdeck_uilab [--font WxH] "
                    "[--out DIR] [--dump DIR] [--screen NAME]\n");
            return 2;
        }
    }

    font_init(font);
    display_render_set_font(font_width(), font_height());
    s_cols = display_text_cols();
    s_rows = display_text_rows();
    s_term = calloc((size_t)s_cols * s_rows, sizeof *s_term);
    if (!s_term) return 1;
    term_blank(s_cols, s_rows);
    display_set_text_buffer(s_term, s_cols, s_rows);
    display_set_cursor(-1, -1, CURSOR_NONE);

    /* A shot must not depend on its capture time: keep the static CRT
     * look, drop every animated effect. */
    display_fx_cfg_t fx;
    display_fx_defaults(&fx);
    fx.glow = fx.wipe = fx.collapse = fx.static_burst = fx.wobble = 0;
    display_fx_set(&fx);

    if (ui_init() != ESP_OK) return 1;
    ui_colors(LAB_FG, LAB_BG);

    int cur = only ? find_screen(only) : 0;
    if (cur < 0) { fprintf(stderr, "no screen '%s'\n", only); return 2; }

    if (out || dump) {
        FILE *df = NULL;
        if (dump) {
            dump_font(dump);
            df = dump_open(dump);
        }
        bool first = true;
        for (int i = 0; i < screen_count(); i++) {
            if (only && i != cur) continue;
            char path[512];
            render_posed(screen_at(i));
            if (out) {
                snprintf(path, sizeof path, "%s/%s.bmp", out,
                         screen_at(i)->name);
                if (display_screenshot_bmp(path) != ESP_OK) {
                    fprintf(stderr, "cannot write %s\n", path);
                    return 1;
                }
            }
            if (df) dump_screen(df, screen_at(i), first);
            first = false;
        }
        if (df) { fprintf(df, "\n]}\n"); fclose(df); }
        return s_faulted ? 1 : 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0 || display_init() != ESP_OK) return 1;
    ui_hot_reset();
    bool run = true, dirty = true;
    uint32_t last_frame = 0;
    while (run) {
        const uint32_t frame = (uint32_t)(SDL_GetTicks64() / 100);
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            uint16_t fx_, fy_;
            switch (ev.type) {
            case SDL_QUIT: run = false; break;
            case SDL_MOUSEBUTTONDOWN:
                display_window_to_fb(ev.button.x, ev.button.y, &fx_, &fy_);
                (void)ui_touch_px(ev.button.button == SDL_BUTTON_RIGHT
                                  ? UI_TOUCH_HOLD : UI_TOUCH_DOWN, fx_, fy_);
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button != SDL_BUTTON_LEFT) break;
                display_window_to_fb(ev.button.x, ev.button.y, &fx_, &fy_);
                (void)ui_touch_px(UI_TOUCH_TAP, fx_, fy_);
                break;
            case SDL_KEYDOWN:
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: run = false; break;
                case SDLK_TAB:
                    cur = (cur + ((ev.key.keysym.mod & KMOD_SHIFT)
                                  ? screen_count() - 1 : 1)) % screen_count();
                    ui_hot_reset();
                    break;
                case SDLK_UP:     (void)ui_hot_key(K_UP);    break;
                case SDLK_DOWN:   (void)ui_hot_key(K_DOWN);  break;
                case SDLK_LEFT:   (void)ui_hot_key(K_LEFT);  break;
                case SDLK_RIGHT:  (void)ui_hot_key(K_RIGHT); break;
                case SDLK_RETURN: (void)ui_hot_key(K_ENTER); break;
                default: break;
                }
                break;
            default: break;
            }
            dirty = true;
        }
        if (dirty || frame != last_frame || ui_hot_animating()) {
            render(screen_at(cur), frame);
            last_frame = frame;
            dirty = false;
        }
        display_render_frame();
    }
    SDL_Quit();
    return 0;
}
