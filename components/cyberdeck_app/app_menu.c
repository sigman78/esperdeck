/*
 * app_menu.c — the overlay menu (ST_MENU): page rendering, navigation,
 * activation, the EFFECTS/FONT tunable pages, and the dynamic profile
 * pickers (delete / edit / reorder). Static page defs in app_menu_defs.c.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "app_menu_defs.h"
#include "display_fx.h"
#include "keystore.h"
#include "ssh_client.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#ifndef BUILD_SIMULATOR
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"     /* esp_restart() — applying a new font size */
#endif

/* -------------------------------------------------------------------------
 * EFFECTS page — every runtime render-fx tunable as a value-cycling tile.
 * Values are quantized presets; fx.ini keeps byte-granular control.
 * ---------------------------------------------------------------------- */

/* Tile order; the row-glow tiles exist only when compiled in. */
enum {
    FXM_SCAN, FXM_MONO, FXM_BOLD, FXM_WOBBLE,
#if DISPLAY_FX_ROW_GLOW
    FXM_GLOW, FXM_DECAY,
#endif
    FXM_WIPE, FXM_COLLAPSE, FXM_STATIC, FXM_BACK,
    FX_MENU_TILES,
};

/* Frames (~39/s) to a compact "0.5s" string. */
static void fx_secs(char *buf, size_t sz, unsigned frames)
{
    unsigned ms = frames * 26u;
    snprintf(buf, sz, "%u.%us", ms / 1000u, (ms % 1000u) / 100u);
}

/* Fill titles + current-value bodies for the EFFECTS page. */
static int fx_menu_items(const char *out[], const char *bodies[],
                         char (*buf)[16])
{
    display_fx_cfg_t c;
    display_fx_get(&c);
    char t[8];

    out[FXM_SCAN] = "Scanlines";
    snprintf(buf[FXM_SCAN], sizeof(buf[FXM_SCAN]), "%s",
             c.scanlines ? "on" : "off");
    out[FXM_MONO] = "Phosphor";
    snprintf(buf[FXM_MONO], sizeof(buf[FXM_MONO]), "%s",
             c.mono == 0 ? "color" : c.mono == 1 ? "green" : "amber");
    out[FXM_BOLD] = "Bold pop";
    snprintf(buf[FXM_BOLD], sizeof(buf[FXM_BOLD]), "%s",
             c.bold_pop ? "on" : "off");
    out[FXM_WOBBLE] = "Wobble";
    snprintf(buf[FXM_WOBBLE], sizeof(buf[FXM_WOBBLE]), "%s",
             c.wobble == 0 ? "off"    : c.wobble == 1 ? "subtle"
           : c.wobble == 2 ? "medium" : "deep");
#if DISPLAY_FX_ROW_GLOW
    out[FXM_GLOW] = "Row glow";
    snprintf(buf[FXM_GLOW], sizeof(buf[FXM_GLOW]), "%s",
             !c.glow ? "off" : c.glow_strength ? "strong" : "subtle");
    fx_secs(t, sizeof(t), c.glow_frames);
    out[FXM_DECAY] = "Glow decay";
    snprintf(buf[FXM_DECAY], sizeof(buf[FXM_DECAY]), "%s", t);
#endif
    fx_secs(t, sizeof(t), c.wipe_frames);
    out[FXM_WIPE] = "Wipe in";
    snprintf(buf[FXM_WIPE], sizeof(buf[FXM_WIPE]), "%s", !c.wipe ? "off" : t);
    fx_secs(t, sizeof(t), c.collapse_frames);
    out[FXM_COLLAPSE] = "Collapse";
    snprintf(buf[FXM_COLLAPSE], sizeof(buf[FXM_COLLAPSE]), "%s",
             !c.collapse ? "off" : t);
    out[FXM_STATIC] = "Static";
    snprintf(buf[FXM_STATIC], sizeof(buf[FXM_STATIC]), "%s",
             !c.static_burst      ? "off"
             : c.static_lines <= 1 ? "light"
             : c.static_lines == 2 ? "medium"
                                   : "heavy");
    out[FXM_BACK]    = "Back";
    buf[FXM_BACK][0] = '\0';
    for (int i = 0; i < FX_MENU_TILES; i++) bodies[i] = buf[i];
    return FX_MENU_TILES;
}

/* ---------------------------------------------------------------- FONT */

/* One tile per font size, plus Back. */
#define FONT_MENU_TILES  (FONT_SIZE_COUNT + 1)

/* What the next boot will use — the active size unless a tile tapped this
 * session already wrote font.ini. Resolved when the page OPENS, not per
 * render: render_menu runs ~10 Hz and font.ini lives on littlefs. */
static font_size_t s_font_pending = FONT_SIZE_COUNT;   /* COUNT = unresolved */

static void font_menu_refresh_pending(void)
{
    s_font_pending = font_active_size();
    char staged[16];
    if (storage_font_load(staged, sizeof(staged)) == ESP_OK) {
        for (int i = 0; i < FONT_SIZE_COUNT; i++)
            if (strcmp(staged, font_size_name((font_size_t)i)) == 0) {
                s_font_pending = (font_size_t)i;
                break;
            }
    }
}

static int font_menu_items(const char *out[], const char *bodies[])
{
    const font_size_t active = font_active_size();

    if (s_font_pending >= FONT_SIZE_COUNT) font_menu_refresh_pending();
    const font_size_t pending = s_font_pending;

    for (int i = 0; i < FONT_SIZE_COUNT; i++) {
        const font_size_t s = (font_size_t)i;
        out[i] = font_size_name(s);
        bodies[i] = !font_size_available(s) ? "not in this build"
                  : (s == active)           ? "in use"
                  : (s == pending)          ? "reboot to apply"
                                            : "";
    }
    out[FONT_SIZE_COUNT]    = "Back";
    bodies[FONT_SIZE_COUNT] = "";
    return FONT_MENU_TILES;
}

/* ------------------------------------------------------------- KEYSTORE */

#define KS_MENU_TILES 5

/* Store state resolved when the page OPENS, not per render: render_menu
 * runs ~10 Hz and an ABSENT store would stat the filesystem every frame. */
static keystore_state_t s_ks_snap;

/* The page is CONTEXTUAL — impossible actions are hidden, not no-op'd
 * (no "Lock deck" or "Remove code" without a store). Two-gates model: a
 * store on the deck means the deck locks at boot/wake; there is nothing
 * to configure. s_ks_act maps the rendered slot to its action. */
enum { KSA_LOCK, KSA_SETPIN, KSA_REMOVE, KSA_BACK };
static uint8_t s_ks_act[KS_MENU_TILES];

static int ks_menu_items(const char *out[], const char *bodies[])
{
    int n = 0;
    if (s_ks_snap != KEYSTORE_ABSENT) {
        out[n]      = "Lock deck";     /* panic button: wipe MK, raise pad */
        bodies[n]   = s_ks_snap == KEYSTORE_UNLOCKED ? "unlocked" : "locked";
        s_ks_act[n++] = KSA_LOCK;
    }
    out[n]      = s_ks_snap == KEYSTORE_ABSENT ? "Create keystore"
                                               : "Change code";
    bodies[n]   = s_ks_snap == KEYSTORE_ABSENT ? "locks the deck" : "";
    s_ks_act[n++] = KSA_SETPIN;
    if (s_ks_snap != KEYSTORE_ABSENT) {
        out[n]      = "Remove code";
        bodies[n]   = "keys to plain";
        s_ks_act[n++] = KSA_REMOVE;
    }
    out[n]      = "Back";
    bodies[n]   = "";
    s_ks_act[n++] = KSA_BACK;
    return n;
}

/* Step the EFFECTS tunable at @p sel, persist, and (for the event effects)
 * arm a one-shot preview so the change is seen immediately. */
/* fx changes apply live (display_fx_set) but the fx.ini flash write is
 * DEFERRED until the user leaves the EFFECTS page: a write per toggle
 * paused the render ISR for the flash-cache-off window, landing a visible
 * hiccup exactly on the keypress. menu_fx_flush() runs from the app tick,
 * so forced exits (session drop, home) still save. */
static bool s_fx_dirty = false;

void menu_fx_flush(void)
{
    if (!s_fx_dirty) return;
    if (app.state == ST_MENU && app.menu.screen == MS_EFFECTS) return;
    s_fx_dirty = false;
    display_fx_cfg_t c;
    display_fx_get(&c);
    storage_fx_save(&c);
}

static void fx_menu_cycle(int sel)
{
    display_fx_cfg_t c;
    display_fx_get(&c);
    switch (sel) {
    case FXM_SCAN: c.scanlines = !c.scanlines; break;
    case FXM_MONO: c.mono = (uint8_t)((c.mono + 1) % 3); break;
    case FXM_BOLD: c.bold_pop = !c.bold_pop;             break;
    case FXM_WOBBLE: c.wobble = (uint8_t)((c.wobble + 1) % 4); break;
#if DISPLAY_FX_ROW_GLOW
    case FXM_GLOW:  /* off -> subtle -> strong -> off */
        if (!c.glow)                { c.glow = 1; c.glow_strength = 0; }
        else if (!c.glow_strength)    c.glow_strength = 1;
        else                          c.glow = 0;
        break;
    case FXM_DECAY:  /* 0.3s -> 0.5s -> 1s -> 2s -> 0.3s */
        c.glow_frames = c.glow_frames < 16 ? 20
                      : c.glow_frames < 30 ? 39
                      : c.glow_frames < 60 ? 78 : 12;
        break;
#endif
    case FXM_WIPE:  /* off -> fast -> slow -> off */
        if (!c.wipe)                  { c.wipe = 1; c.wipe_frames = 12; }
        else if (c.wipe_frames <= 12)   c.wipe_frames = 24;
        else                            c.wipe = 0;
        break;
    case FXM_COLLAPSE:
        if (!c.collapse)                  { c.collapse = 1; c.collapse_frames = 8; }
        else if (c.collapse_frames <= 8)    c.collapse_frames = 20;
        else                                c.collapse = 0;
        break;
    case FXM_STATIC:  /* off -> light -> heavy -> off */
        if (!c.static_burst) { c.static_burst = 1; c.static_frames = 8;  c.static_lines = 1; }
        else if (c.static_lines <= 1) { c.static_frames = 14; c.static_lines = 3; }
        else c.static_burst = 0;
        break;
    default: return;
    }
    display_fx_set(&c);
    s_fx_dirty = true;   /* fx.ini write deferred — see menu_fx_flush() */
    if (sel == FXM_WIPE && c.wipe)             display_fx_wipe();
    if (sel == FXM_COLLAPSE && c.collapse)     display_fx_collapse();
    if (sel == FXM_STATIC && c.static_burst)   display_fx_static();
}

/* Build a stored-profile picker's tiles plus a trailing "Back". Bodies
 * "user@host" keep same-named entries tellable. */
static int picker_items(const char *out[], const char *bodies[],
                        char (*bodybuf)[28], int cap)
{
    int n = 0;
    for (int i = 0; i < app.stored_count && n < cap - 1; i++) {
        snprintf(bodybuf[i], sizeof(bodybuf[i]), "%s@%s",
                 app.profiles[i].user, app.profiles[i].host);
        bodies[n] = bodybuf[i];
        out[n++]  = app.profiles[i].name;
    }
    if (n < cap) { bodies[n] = ""; out[n++] = "Back"; }
    return n;
}

static void render_menu(void)
{
    ui_colors(UI_FG, UI_BG);
    ui_dim();   /* dim the live session behind the menu so it pops */

    const int sc = app.menu.screen;
    const bool root = (sc == MS_MAIN);

    /* Resolve the page: static def, a dynamic stored-profile picker, or the
     * EFFECTS/FONT pages (static titles, live value bodies). */
    const char *dyn[MAX_PROFILES + 1];
    const char *dynb[MAX_PROFILES + 1];
    char bodybuf[MAX_PROFILES][28];
    const char *fxi[FX_MENU_TILES];
    const char *fxb[FX_MENU_TILES];
    char fxbuf[FX_MENU_TILES][16];
    const char *title;
    const char *const *items;
    const char *const *bodies = NULL;
    const uint8_t *cols;
    int count;
    const char *fnti[FONT_MENU_TILES];
    const char *fntb[FONT_MENU_TILES];
    const char *ksi[KS_MENU_TILES];
    const char *ksb[KS_MENU_TILES];
    const bool picker = menu_is_picker(sc);
    const bool fxpage = (sc == MS_EFFECTS);
    const bool fntpage = (sc == MS_FONT);
    const bool kspage = (sc == MS_KEYSTORE);
    if (picker) {
        title = sc == MS_DELPROFILE  ? "DELETE PROFILE"
              : sc == MS_EDITPROFILE ? "EDIT PROFILE"
                                     : "REORDER PROFILES";
        count  = picker_items(dyn, dynb, bodybuf, NELEM(dyn));
        items  = dyn;
        bodies = dynb;
        cols   = NULL;                      /* colored per-tile below */
    } else if (fxpage) {
        title  = "EFFECTS";
        count  = fx_menu_items(fxi, fxb, fxbuf);
        items  = fxi;
        bodies = fxb;
        cols   = NULL;
    } else if (fntpage) {
        title  = "FONT";
        count  = font_menu_items(fnti, fntb);
        items  = fnti;
        bodies = fntb;
        cols   = NULL;
    } else if (kspage) {
        title  = "KEYSTORE";
        count  = ks_menu_items(ksi, ksb);
        items  = ksi;
        bodies = ksb;
        cols   = NULL;
    } else {
        menu_def_t d = menu_def(sc);
        title = d.title; items = d.items; cols = d.cols; count = d.count;
    }

    /* Pickers can outgrow one centered column, so they use the multi-column
     * HOME grid; everything below is grid-agnostic. */
    tilegrid_t g;
    int title_row, ly, chrome_x;
    if (picker || fxpage) {
        g = picker_grid(count);
        title_row = 2;
        ly        = ui_rows() - 3;
        chrome_x  = (ui_cols() - 40) / 2;   /* center chrome over the screen */
    } else {
        /* Shrink tile height until the column actually FITS: an off-grid
         * tile is not merely invisible — ui_putch clips it AND no touch y
         * maps to it, so the item (usually Back) becomes untappable.
         * Derived from count so a new menu item can't silently break it. */
        int th = ui_rows() >= 28 ? (count >= 6 ? 3 : 4) : 2;
        const int budget = ui_rows() - 3;   /* title chip above, legend below */
        while (th > 1 && count * (th + 1) - 1 > budget) th--;
        g = (tilegrid_t){ .tw = 40, .th = th, .gx = 0, .gy = 1,
                          .ncols = 1, .nrows = count, .count = count };
        int total = count * (g.th + 1) - 1;
        g.x0 = (ui_cols() - g.tw) / 2;
        g.y0 = (ui_rows() - total) / 2;
        if (g.y0 < 2) g.y0 = 2;             /* title chip needs row y0-2 */
        title_row = g.y0 - 2;
        ly        = g.y0 + total + 1;
        if (ly > ui_rows() - 1) ly = ui_rows() - 1;
        chrome_x  = g.x0;
    }
    app.grid = g;
    if (app.menu.sel >= g.count) app.menu.sel = g.count ? g.count - 1 : 0;

    /* Title as a magenta lozenge, centered over the chrome column. */
    int tl = (int)strlen(title);
    ui_pen(OVERLAY_COL_MAGENTA);
    ui_chip(chrome_x + (40 - tl - 4) / 2, title_row, UI_RHALF, title, UI_LHALF,
            OVERLAY_ATTR_BOLD);

    /* Wall clock, top-right, ticking live (menu re-renders every frame). */
    char clk[8];
    if (clock_str(clk, sizeof(clk))) {
        ui_pen(OVERLAY_COL_BLUE);
        ui_puts(ui_cols() - (int)strlen(clk) - 1, 0, clk, 0);
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    for (int i = 0; i < g.count; i++) {
        bool dim   = menu_item_dim(sc, i);
        const char *confirm = menu_confirm(sc, i);
        bool armed   = (i == app.menu.sel) && app.menu.armed && confirm;
        bool grabbed = (sc == MS_REORDER) && (i == app.menu.reorder_grab);
        uint8_t col;
        if (armed)        col = OVERLAY_COL_RED;
        else if (picker)  col = i >= app.stored_count ? OVERLAY_COL_BLUE
                              : sc == MS_DELPROFILE ? OVERLAY_COL_AMBER
                              : grabbed             ? OVERLAY_COL_GREEN
                                                    : OVERLAY_COL_CYAN;
        else if (fxpage)  col = i == g.count - 1 ? OVERLAY_COL_BLUE
                                                 : OVERLAY_COL_CYAN;
        else if (fntpage) col = i >= FONT_SIZE_COUNT          ? OVERLAY_COL_BLUE
                              : (font_size_t)i == font_active_size()
                                                             ? OVERLAY_COL_GREEN
                                                             : OVERLAY_COL_CYAN;
        else if (kspage)  col = i == g.count - 1 ? OVERLAY_COL_BLUE
                                                 : OVERLAY_COL_CYAN;
        else              col = cols[i];
        ui_pen(col);
        /* Focus is carried by the tile itself (washed bar + lit rail);
         * a grabbed REORDER tile reads by its green bar. */
        const char *title = armed ? confirm : items[i];
        const char *body  = bodies ? bodies[i] : dim ? "(unavailable)" : "";
        bool sel = i == app.menu.sel || grabbed;
        if (fxpage && body[0] && !armed) {
            /* EFFECTS: value right-aligned on the title row in regular
             * weight, settings-table style — the bold name carries the
             * emphasis and the value reads as data. */
            ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th, title, "", sel);
            ui_puts(tile_x(&g, i) + g.tw - 2 - (int)strlen(body),
                    tile_y(&g, i) + (g.th - 1) / 2, body,
                    OVERLAY_ATTR_INVERSE | (sel ? OVERLAY_ATTR_BRIGHT : 0));
        } else {
            ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th, title, body, sel);
        }
    }

    /* Esc legend under the tile area; suppressed while a note is up (the
     * two share this row). */
    if (!app.menu.msg[0]) {
        const char *legend = root ? "Esc/F12 resume \xB7 tap outside closes"
                           : sc == MS_CONFIG
                               ? (app.menu.from_home ? "Esc \xB7 home" : "Esc \xB7 menu")
                               : "Esc \xB7 back";
        ui_pen(OVERLAY_COL_BLUE);
        ui_puts(chrome_x + (40 - (int)strlen(legend)) / 2, ly, legend, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    /* Empty-picker hint, just above the (Back-only) grid. */
    if (picker && app.stored_count == 0) {
        const char *m = "no stored profiles";
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_puts(chrome_x + (40 - (int)strlen(m)) / 2, title_row + 1, m, 0);
    }

    if (app.menu.msg[0]) {               /* action feedback */
        /* On the legend row, not below it: ly is clamped to the last grid
         * row, so ly+1 could be off-panel and the note would vanish. */
        int mx = chrome_x + (40 - ((int)strlen(app.menu.msg) + 2)) / 2;
        ui_pen(OVERLAY_COL_AMBER);
        ui_putch(mx, ly, UI_DIAMOND, 0);
        ui_puts(mx + 2, ly, app.menu.msg, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    if (root) {
        /* Mainframe flex: deck uptime + link time behind the menu. */
        uint64_t up = (uint64_t)app.anim_frame * ANIM_PERIOD_MS / 1000;
        char flex[48];
        uint64_t now_ms = (uint64_t)app.anim_frame * ANIM_PERIOD_MS;
        uint64_t ss = app.conn.session_start;
        uint64_t lk = now_ms > ss ? (now_ms - ss) / 1000 : 0;
        snprintf(flex, sizeof(flex), "UP %02u:%02u:%02u   LINK %02u:%02u",
                 (unsigned)(up / 3600), (unsigned)(up / 60 % 60),
                 (unsigned)(up % 60), (unsigned)(lk / 60), (unsigned)(lk % 60));
        ui_pen(OVERLAY_COL_BLUE);
        ui_puts(g.x0 + (g.tw - (int)strlen(flex)) / 2, ly + 2, flex, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }
    ui_no_cursor();
    ui_present();
}

/* Post an action-feedback line under the menu tiles.
 * @p ms: lifetime; 0 = sticky. @p live_wifi: track wifi_status_str(). */
void menu_note(uint64_t now, uint32_t ms, bool live_wifi, const char *text)
{
    snprintf(app.menu.msg, sizeof(app.menu.msg), "%s", text);
    app.menu.msg_until = ms ? now + ms : 0;
    app.menu.msg_wifi  = live_wifi;
}

static void menu_clear_note(void)
{
    app.menu.msg[0]    = '\0';
    app.menu.msg_until = 0;
    app.menu.msg_wifi  = false;
}

/* Switch to menu screen @p sc, resetting selection/arm/note. */
void menu_goto(int sc)
{
    app.menu.screen = sc;
    app.menu.sel    = 0;
    app.menu.armed  = false;
    if (sc == MS_FONT) s_font_pending = FONT_SIZE_COUNT;  /* re-read on open */
    if (sc == MS_KEYSTORE) s_ks_snap = keystore_state();  /* see ks_menu_items */
    menu_clear_note();
    render_menu();
}

/* Discard a grabbed-but-not-dropped reorder: the pending moves only live in
 * app.profiles until a drop saves them, and a session drop can yank the
 * user out mid-drag — a later save would silently persist them. */
void menu_abort_reorder(void)
{
    if (app.menu.screen == MS_REORDER && app.menu.reorder_grab >= 0) {
        app.menu.reorder_grab = -1;
        load_profiles();
    }
}

/* Back one level. Every back path (Esc, tap-outside, Back tile) funnels here
 * so an armed confirm can never leak across pages. */
static void menu_back(uint64_t now)
{
    if (app.menu.screen == MS_REORDER && app.menu.reorder_grab >= 0) {
        menu_abort_reorder();
        menu_clear_note();
        render_menu();
        return;
    }
    switch (app.menu.screen) {
    case MS_MAIN:                              /* resume the live session */
        app.menu.armed = false;
        session_resume();
        break;
    case MS_CONFIG:
        if (app.menu.from_home) enter_home(now);
        else             menu_goto(MS_MAIN);
        break;
    case MS_DELPROFILE:
    case MS_EDITPROFILE:
    case MS_REORDER:
    case MS_IMPORT:
        menu_goto(MS_PROFILES);
        break;
    default:                                   /* PROFILES/WIFI/KEYBOARD/SYSTEM */
        menu_goto(MS_CONFIG);
        break;
    }
}

/* Delete the stored profile at @p idx, plus its key files and TOFU host pin
 * if no other profile still references them. A live session is untouched:
 * it runs on the connect module's snapshot. */
static void delete_profile_at(int idx)
{
    if (idx < 0 || idx >= app.stored_count) return;
    conn_profile_t doomed = app.profiles[idx];

    conn_profile_t set[MAX_PROFILES];
    int n = 0;
    for (int i = 0; i < app.stored_count && n < MAX_PROFILES; i++)
        if (i != idx) set[n++] = app.profiles[i];
    storage_save_profiles(set, n);

    if (doomed.auth == STORAGE_AUTH_KEY && doomed.key_id[0]) {
        bool shared = false;
        for (int i = 0; i < n; i++)
            if (set[i].auth == STORAGE_AUTH_KEY &&
                strcmp(set[i].key_id, doomed.key_id) == 0) { shared = true; break; }
        if (!shared) storage_delete_key(doomed.key_id);
    }

    /* Drop the host's pin too when no surviving profile targets it — a
     * re-added profile then gets a fresh TOFU prompt. */
    bool host_shared = false;
    for (int i = 0; i < n; i++)
        if (set[i].port == doomed.port &&
            strcmp(set[i].host, doomed.host) == 0) { host_shared = true; break; }
    if (!host_shared) storage_known_host_delete(doomed.host, doomed.port);

    load_profiles();
}

/* Persist the reorder picker's current in-RAM order and release the grab. */
static void commit_reorder(uint64_t now)
{
    app.menu.reorder_grab = -1;
    if (storage_save_profiles(app.profiles, app.stored_count) == ESP_OK)
        menu_note(now, MENU_MSG_MS, false, "order saved");
    else
        menu_note(now, MENU_MSG_MS, false, "save failed");
    load_profiles();
}

static void menu_activate(uint64_t now)
{
    const int sc  = app.menu.screen;
    const int sel = app.menu.sel;
    const bool was_armed = app.menu.armed;
    app.menu.armed = false;   /* destructive branches re-arm on the first hit */

    switch (sc) {
    case MS_MAIN:
        switch (sel) {
        case 0: session_resume();                         return;  /* resume  */
        case 1:                                                    /* discon. */
            ssh_client_disconnect();
            enter_home_after_collapse(now);   /* deliberate CRT power-off */
            return;
        case 2: menu_goto(MS_CONFIG);                     return;
        }
        return;

    case MS_CONFIG:
        switch (sel) {
        case 0: menu_goto(MS_PROFILES); return;
        case 1: menu_goto(MS_WIFI);     return;
        case CFG_KEYBOARD:
            if (!app.cfg.ble) { menu_note(now, MENU_MSG_MS, false,
                                        "no BLE keyboard support"); break; }
            menu_goto(MS_KEYBOARD); return;
        case 3: menu_goto(MS_EFFECTS);  return;
        case 4: menu_goto(MS_FONT);     return;
        case 5: menu_goto(MS_KEYSTORE); return;
        case 6: menu_goto(MS_SYSTEM);   return;
        case 7: menu_back(now);         return;   /* Back */
        }
        break;

    case MS_EFFECTS:
        if (sel >= FX_MENU_TILES - 1) { menu_back(now); return; }  /* Back */
        fx_menu_cycle(sel);
        return;

    case MS_KEYSTORE:
        if (sel < 0 || sel >= KS_MENU_TILES) break;
        switch (s_ks_act[sel]) {          /* slot -> action (contextual page) */
        case KSA_LOCK:                    /* panic button: park the deck */
            keystore_lock();
            unlock_open_gate(now);
            return;
        case KSA_SETPIN: unlock_open_setpin(now); return;  /* create/change */
        case KSA_REMOVE: unlock_open_remove(now); return;  /* proves code   */
        case KSA_BACK: menu_back(now);            return;  /* Back */
        }
        break;

    case MS_FONT: {
        if (sel >= FONT_SIZE_COUNT) { menu_back(now); return; }    /* Back */
        const font_size_t want = (font_size_t)sel;
        char note[40];

        if (!font_size_available(want)) {
            snprintf(note, sizeof(note), "%s not in this build",
                     font_size_name(want));
            menu_note(now, MENU_MSG_MS, false, note);
            return;
        }
        if (want == font_active_size()) {
            snprintf(note, sizeof(note), "%s already in use",
                     font_size_name(want));
            menu_note(now, MENU_MSG_MS, false, note);
            return;
        }
        if (storage_font_save(font_size_name(want)) != ESP_OK) {
            menu_note(now, MENU_MSG_MS, false, "could not save font");
            return;
        }
        /* The grid, DMA bounce geometry and DRAM glyph copy are fixed at
         * init, so a size change needs a restart. The setting is already
         * on disk — paint the note, hold it readable, reboot. */
#ifndef BUILD_SIMULATOR
        snprintf(note, sizeof(note), "%s set - rebooting", font_size_name(want));
        menu_note(now, MENU_MSG_MS, false, note);
        render_menu();
        vTaskDelay(pdMS_TO_TICKS(1200));
        esp_restart();
#else
        /* The simulator is single-size and cannot restart into another one;
         * the setting is saved so the device picks it up. */
        snprintf(note, sizeof(note), "%s saved (reboot)", font_size_name(want));
        menu_note(now, MENU_MSG_MS, false, note);
#endif
        return;
    }

    case MS_PROFILES:
        switch (sel) {
        case 0:                                           /* add     */
            if (app.stored_count >= MAX_PROFILES - 1) {
                menu_note(now, MENU_MSG_MS, false, "profile list full");
                break;
            }
            enter_profile(now, -1);
            return;
        case 1:                                           /* edit    */
            if (app.stored_count == 0) {
                menu_note(now, MENU_MSG_MS, false, "no stored profiles");
                break;
            }
            menu_goto(MS_EDITPROFILE);
            return;
        case 2:                                           /* reorder */
            if (app.stored_count < 2) {
                menu_note(now, MENU_MSG_MS, false, "nothing to reorder");
                break;
            }
            app.menu.reorder_grab = -1;
            menu_goto(MS_REORDER);
            return;
        case 3: menu_goto(MS_DELPROFILE);                 return;
        case 4: menu_goto(MS_IMPORT);                     return;
        case 5: menu_back(now);                           return;  /* Back    */
        }
        break;

    case MS_IMPORT:
        switch (sel) {
        case 0: enter_sshimport(now, SSH_IMPORT_SOFTAP);  return;
        case 1: enter_sshimport(now, SSH_IMPORT_WEB);     return;
        case 2: menu_back(now);                           return;  /* Back    */
        }
        break;

    case MS_WIFI:
        switch (sel) {
        case 0:                                   /* reconnect (live note) */
            kick_wifi();
            menu_note(now, MENU_MSG_MS, true, "wifi: ...");
            break;
        case 1: enter_wifiprov(now); return;      /* add network via phone */
        case 2: menu_back(now);      return;      /* Back */
        }
        break;

    case MS_KEYBOARD:
        if (!app.cfg.ble) { menu_note(now, MENU_MSG_MS, false,
                                    "no BLE keyboard support"); break; }
        switch (sel) {
        case 0: enter_pairing(now); return;       /* pair */
        case 1:                                   /* forget bonds (2-step) */
            if (!app.cfg.ble->forget) {
                menu_note(now, MENU_MSG_MS, false, "forget unavailable");
            } else if (!was_armed) {
                app.menu.armed = true;
                menu_note(now, 0, false, "activate again to forget");
            } else {
                app.cfg.ble->forget();
                menu_note(now, MENU_MSG_MS, false, "keyboard bonds cleared");
            }
            break;
        case 2: menu_back(now); return;           /* Back */
        }
        break;

    case MS_SYSTEM:
        switch (sel) {
        case 0:                                   /* clear host keys (2-step) */
            if (!was_armed) {
                app.menu.armed = true;
                menu_note(now, 0, false, "activate again to clear");
            } else {
                esp_err_t e = storage_known_hosts_clear();
                menu_note(now, MENU_MSG_MS, false,
                          e == ESP_OK ? "host keys cleared" : "nothing to clear");
            }
            break;
        case 1:                                   /* factory reset (2-step) */
            if (!was_armed) {
                app.menu.armed = true;
                menu_note(now, 0, false, "activate again to WIPE ALL");
            } else {
                storage_factory_reset();
                if (app.cfg.ble && app.cfg.ble->forget) app.cfg.ble->forget();
                load_profiles();
                menu_note(now, MENU_MSG_MS, false, "wiped - reboot advised");
            }
            break;
        case 2: menu_back(now); return;           /* Back */
        }
        break;

    case MS_DELPROFILE:
        if (sel >= app.stored_count) { menu_back(now); return; }   /* Back tile */
        if (!was_armed) {
            app.menu.armed = true;
            menu_note(now, 0, false, "activate again to delete");
        } else {
            delete_profile_at(sel);
            if (app.menu.sel >= app.stored_count && app.menu.sel > 0) app.menu.sel--;
            menu_note(now, MENU_MSG_MS, false, "profile deleted");
        }
        break;

    case MS_EDITPROFILE:
        if (sel >= app.stored_count) { menu_back(now); return; }   /* Back tile */
        enter_profile(now, sel);
        return;

    case MS_REORDER:
        if (sel >= app.stored_count) { menu_back(now); return; }   /* Back tile */
        if (app.menu.reorder_grab < 0) {                                  /* grab      */
            app.menu.reorder_grab = sel;
            menu_note(now, 0, false, "arrows/tap move it - Enter drops");
        } else {                                                   /* drop      */
            if (sel != app.menu.reorder_grab) {
                conn_profile_t t             = app.profiles[sel];
                app.profiles[sel]            = app.profiles[app.menu.reorder_grab];
                app.profiles[app.menu.reorder_grab] = t;
                app.menu.sel = sel;
            }
            commit_reorder(now);
        }
        break;
    }
    render_menu();
}

/* Open the in-session root menu (F12 / long-press). */
void menu_open(uint64_t now)
{
    (void)now;
    app.menu.sel       = 0;
    app.menu.screen    = MS_MAIN;
    app.menu.from_home = false;
    app.menu.armed     = false;
    menu_clear_note();
    app.state = ST_MENU;
    render_menu();
}

/* Open the config hub directly from HOME (no session behind it). */
void menu_open_config(void)
{
    app.menu.from_home = true;
    app.state   = ST_MENU;
    menu_goto(MS_CONFIG);
}

void menu_tick(uint64_t now)
{
    /* A menu opened from HOME has no session to monitor. */
    if (!app.menu.from_home && !ssh_client_is_connected()) {
        session_dropped(now);
        return;
    }
    /* Live feedback on the 10 fps gate: wifi-tracking notes rewrite from
     * the real state, expired notes clear, the UP/LINK clocks tick. */
    if (now >= app.next_anim) {
        app.next_anim = now + ANIM_PERIOD_MS;
        if (app.menu.msg[0] && app.menu.msg_wifi) {
            snprintf(app.menu.msg, sizeof(app.menu.msg), "wifi: %s", wifi_status_str());
            /* Still in flight? Keep the note alive — expiring mid-
             * reconnect reads as the action silently dying. */
            wifi_mgr_state_t ws = wifi_manager_get_state();
            if (ws == WIFI_MGR_CONNECTING || ws == WIFI_MGR_LOST)
                app.menu.msg_until = now + MENU_MSG_MS;
        }
        if (app.menu.msg[0] && app.menu.msg_until && now >= app.menu.msg_until)
            menu_clear_note();
        render_menu();
    }
}

void menu_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now)
{
    (void)ch;
    /* Esc / F12 / tap-outside all step back one level. */
    if (k == K_ESC || (ev->type == CYBERDECK_INPUT_KEY && k == K_F12)) {
        menu_back(now);
        return;
    }
    if (ev->type == CYBERDECK_INPUT_TAP) {
        int slot = tile_hit(&app.grid, ev->x, ev->y);
        if (slot < 0) { menu_back(now); return; }   /* tap outside: back */
        /* Tapping a DIFFERENT tile than the armed one must disarm first, or
         * the stale arm fires this tile's destructive action unconfirmed. */
        if (slot != app.menu.sel && app.menu.armed) {
            app.menu.armed = false;
            menu_clear_note();
        }
        app.menu.sel = slot;
        menu_activate(now);                        /* == Enter */
        return;
    }
    switch (k) {
    case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
        int ns = tile_nav(&app.grid, app.menu.sel, k);
        /* A grabbed reorder tile rides the arrows: each step swaps it
         * with the neighbour (never with the trailing Back tile). */
        if (app.menu.screen == MS_REORDER && app.menu.reorder_grab >= 0) {
            if (ns != app.menu.sel && ns < app.stored_count &&
                app.menu.sel < app.stored_count) {
                conn_profile_t t     = app.profiles[ns];
                app.profiles[ns]     = app.profiles[app.menu.sel];
                app.profiles[app.menu.sel]  = t;
                app.menu.reorder_grab = ns;
                app.menu.sel          = ns;
                render_menu();
            }
            break;
        }
        if (ns != app.menu.sel) {
            app.menu.sel = ns;
            if (app.menu.armed) {              /* moving away backs the arm down */
                app.menu.armed = false;
                menu_clear_note();
            }
            render_menu();
        }
        break;
    }
    case K_ENTER:
        menu_activate(now);
        break;
    default:
        break;
    }
}
