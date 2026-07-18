/*
 * app_menu.c — the overlay menu (ST_MENU): page rendering, navigation,
 * activation, the EFFECTS tunables page, and the dynamic profile pickers
 * (delete / edit / reorder). Static page definitions live in app_menu_defs.c.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "app_menu_defs.h"
#include "display_fx.h"
#include "ssh_client.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * EFFECTS page — every runtime render-fx tunable as a value-cycling tile.
 * Tapping a tile steps its value (wrapping); the menu re-renders every frame
 * so the body text follows. Values are quantized presets here; fx.ini keeps
 * byte-granular control for anything in between.
 * ---------------------------------------------------------------------- */

/* Tile order for the EFFECTS page. The row-glow tiles exist only when the
 * effect is compiled in (DISPLAY_FX_ROW_GLOW) — the enum renumbers itself. */
enum {
    FXM_SCAN, FXM_MONO, FXM_BOLD, FXM_WOBBLE,
#if DISPLAY_FX_ROW_GLOW
    FXM_GLOW, FXM_DECAY,
#endif
    FXM_MELT, FXM_STATIC, FXM_BACK,
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
    fx_secs(t, sizeof(t), c.melt_frames);
    out[FXM_MELT] = "Melt";
    snprintf(buf[FXM_MELT], sizeof(buf[FXM_MELT]), "%s", !c.melt ? "off" : t);
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

/* Step the EFFECTS tunable at @p sel, persist, and (for the event effects)
 * arm a one-shot preview so the change is seen immediately. */
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
    case FXM_MELT:  /* off -> fast -> slow -> off (display_fx_set floors
                     * the fast value for the active font's grid height) */
        if (!c.melt)                  { c.melt = 1; c.melt_frames = 32; }
        else if (c.melt_frames <= 32)   c.melt_frames = 52;
        else                            c.melt = 0;
        break;
    case FXM_STATIC:  /* off -> light -> heavy -> off */
        if (!c.static_burst) { c.static_burst = 1; c.static_frames = 8;  c.static_lines = 1; }
        else if (c.static_lines <= 1) { c.static_frames = 14; c.static_lines = 3; }
        else c.static_burst = 0;
        break;
    default: return;
    }
    display_fx_set(&c);
    storage_fx_save(&c);
    if (sel == FXM_MELT && c.melt)             display_fx_melt_over();
    if (sel == FXM_STATIC && c.static_burst)   display_fx_static();
}

/* Build a stored-profile picker's tiles plus a trailing "Back". Titles are
 * the profile names; bodies "user@host" keep same-named entries tellable.
 * Names point into app.profiles, bodies into @p bodybuf — frame-local. */
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

    const int sc = app.menu_screen;
    const bool root = (sc == MS_MAIN);

    /* Resolve the page: static def, a dynamic stored-profile picker, or the
     * EFFECTS page (static titles, live value bodies). */
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
    const bool picker = menu_is_picker(sc);
    const bool fxpage = (sc == MS_EFFECTS);
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
        cols   = NULL;                      /* colored per-tile below */
    } else {
        menu_def_t d = menu_def(sc);
        title = d.title; items = d.items; cols = d.cols; count = d.count;
    }

    /* A picker can hold up to 8 profiles + Back — too many for one centered
     * column (it would run off-screen), so lay it out on the same
     * multi-column grid HOME uses. Everything below is grid-agnostic (tile_x/
     * tile_y/tile_nav/tile_hit). */
    tilegrid_t g;
    int title_row, ly, chrome_x;
    if (picker || fxpage) {
        g = picker_grid(count);
        title_row = 2;
        ly        = ui_rows() - 3;
        chrome_x  = (ui_cols() - 40) / 2;   /* center chrome over the screen */
    } else {
        /* Tile height shrinks with the grid; items always keep a 1-row gap
         * (glued bars read as one slab). The tallest page (6 tiles at
         * 20 rows) pins the title to row 0 and the legend to the last row. */
        int th = ui_rows() >= 28 ? (count >= 6 ? 3 : 4) : 2;
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
    if (app.menu_sel >= g.count) app.menu_sel = g.count ? g.count - 1 : 0;

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
        bool armed   = (i == app.menu_sel) && app.menu_armed && confirm;
        bool grabbed = (sc == MS_REORDER) && (i == app.reorder_grab);
        uint8_t col;
        if (armed)        col = OVERLAY_COL_RED;
        else if (picker)  col = i >= app.stored_count ? OVERLAY_COL_BLUE
                              : sc == MS_DELPROFILE ? OVERLAY_COL_AMBER
                              : grabbed             ? OVERLAY_COL_GREEN
                                                    : OVERLAY_COL_CYAN;
        else if (fxpage)  col = i == g.count - 1 ? OVERLAY_COL_BLUE
                                                 : OVERLAY_COL_CYAN;
        else              col = cols[i];
        ui_pen(col);
        /* Focus is carried entirely by the tile itself (washed bar + lit
         * left rail) — no marker glyphs, no animated cursor. A grabbed
         * REORDER tile reads by its green bar. EFFECTS values are right-
         * aligned on the title line, settings-table style. */
        const char *title = armed ? confirm : items[i];
        const char *body  = bodies ? bodies[i] : dim ? "(unavailable)" : "";
        bool sel = i == app.menu_sel || grabbed;
        if (fxpage && body[0] && !armed) {
            /* EFFECTS: value right-aligned on the title row, settings-table
             * style. Overdrawn after the tile in regular weight, so the bold
             * name carries the emphasis and the value reads as data. */
            ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th, title, "", sel);
            ui_puts(tile_x(&g, i) + g.tw - 2 - (int)strlen(body),
                    tile_y(&g, i) + (g.th - 1) / 2, body,
                    OVERLAY_ATTR_INVERSE | (sel ? OVERLAY_ATTR_BRIGHT : 0));
        } else {
            ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th, title, body, sel);
        }
    }

    /* Esc legend under the tile area — quiet blue, centered on the column. */
    const char *legend = root ? "Esc/F12 resume \xB7 tap outside closes"
                       : sc == MS_CONFIG
                           ? (app.menu_from_home ? "Esc \xB7 home" : "Esc \xB7 menu")
                           : "Esc \xB7 back";
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(chrome_x + (40 - (int)strlen(legend)) / 2, ly, legend, 0);
    ui_pen(OVERLAY_COL_DEFAULT);

    /* Empty-picker hint, just above the (Back-only) grid. */
    if (picker && app.stored_count == 0) {
        const char *m = "no stored profiles";
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_puts(chrome_x + (40 - (int)strlen(m)) / 2, title_row + 1, m, 0);
    }

    if (app.menu_msg[0]) {               /* action feedback */
        int mx = chrome_x + (40 - ((int)strlen(app.menu_msg) + 2)) / 2;
        ui_pen(OVERLAY_COL_AMBER);
        ui_putch(mx, ly + 1, UI_DIAMOND, 0);
        ui_puts(mx + 2, ly + 1, app.menu_msg, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    if (root) {
        /* Mainframe flex: deck uptime + link time behind the menu. */
        uint64_t up = (uint64_t)app.anim_frame * ANIM_PERIOD_MS / 1000;
        char flex[48];
        uint64_t now_ms = (uint64_t)app.anim_frame * ANIM_PERIOD_MS;
        uint64_t lk = now_ms > app.session_start
                    ? (now_ms - app.session_start) / 1000 : 0;
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
 * @p ms: lifetime; 0 = sticky (lives until explicitly cleared).
 * @p live_wifi: keep rewriting it from wifi_status_str() while shown. */
void menu_note(uint64_t now, uint32_t ms, bool live_wifi,
                      const char *text)
{
    snprintf(app.menu_msg, sizeof(app.menu_msg), "%s", text);
    app.menu_msg_until = ms ? now + ms : 0;
    app.menu_msg_wifi  = live_wifi;
}

static void menu_clear_note(void)
{
    app.menu_msg[0]    = '\0';
    app.menu_msg_until = 0;
    app.menu_msg_wifi  = false;
}

/* Switch to menu screen @p sc, resetting selection/arm/note. */
void menu_goto(int sc)
{
    app.menu_screen = sc;
    app.menu_sel    = 0;
    app.menu_armed  = false;
    menu_clear_note();
    render_menu();
}

/* Back one level. Every back path (Esc, tap-outside, Back tile) funnels here so
 * an armed confirm can never leak across pages. */
static void menu_back(uint64_t now)
{
    /* Backing out of a grabbed-but-not-dropped reorder discards the pending
     * moves (they only live in app.profiles until the drop saves them). */
    if (app.menu_screen == MS_REORDER && app.reorder_grab >= 0) {
        app.reorder_grab = -1;
        load_profiles();
        menu_clear_note();
        render_menu();
        return;
    }
    switch (app.menu_screen) {
    case MS_MAIN:                              /* resume the live session */
        app.menu_armed = false;
        app.state = ST_SESSION;
        ui_hide();
        break;
    case MS_CONFIG:
        if (app.menu_from_home) enter_home(now);
        else                  menu_goto(MS_MAIN);
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

/* Delete the stored profile at index @p idx, plus its key files and TOFU
 * host pin if no other profile still references them. Reloads the in-RAM
 * list. A live session is untouched: it runs on the app.active snapshot. */
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
     * re-added profile then gets a fresh TOFU prompt instead of silently
     * trusting a pin the user may have forgotten about. */
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
    app.reorder_grab = -1;
    if (storage_save_profiles(app.profiles, app.stored_count) == ESP_OK)
        menu_note(now, MENU_MSG_MS, false, "order saved");
    else
        menu_note(now, MENU_MSG_MS, false, "save failed");
    load_profiles();
}

static void menu_activate(uint64_t now)
{
    const int sc  = app.menu_screen;
    const int sel = app.menu_sel;
    const bool was_armed = app.menu_armed;
    app.menu_armed = false;   /* destructive branches re-arm on the first hit */

    switch (sc) {
    case MS_MAIN:
        switch (sel) {
        case 0: app.state = ST_SESSION; ui_hide();          return;  /* resume  */
        case 1:                                                    /* discon. */
            ssh_client_disconnect();
            enter_home_after_melt(now);       /* deliberate melt-off */
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
        case 3: menu_goto(MS_EFFECTS); return;
        case 4: menu_goto(MS_SYSTEM);  return;
        case 5: menu_back(now);        return;   /* Back */
        }
        break;

    case MS_EFFECTS:
        if (sel >= FX_MENU_TILES - 1) { menu_back(now); return; }  /* Back */
        fx_menu_cycle(sel);
        return;

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
            app.reorder_grab = -1;
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
                app.menu_armed = true;
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
                app.menu_armed = true;
                menu_note(now, 0, false, "activate again to clear");
            } else {
                esp_err_t e = storage_known_hosts_clear();
                menu_note(now, MENU_MSG_MS, false,
                          e == ESP_OK ? "host keys cleared" : "nothing to clear");
            }
            break;
        case 1:                                   /* factory reset (2-step) */
            if (!was_armed) {
                app.menu_armed = true;
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
            app.menu_armed = true;
            menu_note(now, 0, false, "activate again to delete");
        } else {
            delete_profile_at(sel);
            if (app.menu_sel >= app.stored_count && app.menu_sel > 0) app.menu_sel--;
            menu_note(now, MENU_MSG_MS, false, "profile deleted");
        }
        break;

    case MS_EDITPROFILE:
        if (sel >= app.stored_count) { menu_back(now); return; }   /* Back tile */
        enter_profile(now, sel);
        return;

    case MS_REORDER:
        if (sel >= app.stored_count) { menu_back(now); return; }   /* Back tile */
        if (app.reorder_grab < 0) {                                /* grab      */
            app.reorder_grab = sel;
            menu_note(now, 0, false, "arrows/tap move it - Enter drops");
        } else {                                                 /* drop      */
            if (sel != app.reorder_grab) {
                conn_profile_t t         = app.profiles[sel];
                app.profiles[sel]          = app.profiles[app.reorder_grab];
                app.profiles[app.reorder_grab] = t;
                app.menu_sel = sel;
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
    app.menu_sel       = 0;
    app.menu_screen    = MS_MAIN;
    app.menu_from_home = false;
    app.menu_armed     = false;
    menu_clear_note();
    app.state = ST_MENU;
    render_menu();
}

void menu_tick(uint64_t now)
{
    /* A menu opened from HOME has no session to monitor. */
    if (!app.menu_from_home && !ssh_client_is_connected()) {
        session_dropped(now);
        return;
    }
    /* Live feedback (inside the 10 fps gate — its output is only ever
     * seen by render_menu): wifi-tracking notes rewrite themselves from
     * the real state, expired notes clear, the marker pulses and the
     * UP/LINK clocks tick. */
    if (now >= app.next_anim) {
        app.next_anim = now + ANIM_PERIOD_MS;
        if (app.menu_msg[0] && app.menu_msg_wifi) {
            snprintf(app.menu_msg, sizeof(app.menu_msg), "wifi: %s",
                     wifi_status_str());
            /* Still in flight? Keep the note alive — expiring mid-
             * reconnect reads as the action silently dying. */
            wifi_mgr_state_t ws = wifi_manager_get_state();
            if (ws == WIFI_MGR_CONNECTING || ws == WIFI_MGR_LOST)
                app.menu_msg_until = now + MENU_MSG_MS;
        }
        if (app.menu_msg[0] && app.menu_msg_until && now >= app.menu_msg_until)
            menu_clear_note();
        render_menu();
    }
}

void menu_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now)
{
    (void)ch;
    /* Esc / F12 / tap-outside all step back one level (menu_back knows how
     * far: submenu -> config -> main/home -> resume). */
    if (k == K_ESC || (ev->type == CYBERDECK_INPUT_KEY && k == K_F12)) {
        menu_back(now);
        return;
    }
    if (ev->type == CYBERDECK_INPUT_TAP) {
        int slot = tile_hit(&app.grid, ev->x, ev->y);
        if (slot < 0) { menu_back(now); return; }   /* tap outside: back */
        /* Tapping a DIFFERENT tile than the armed one must disarm first, or
         * the stale arm fires this tile's destructive action unconfirmed
         * (the keyboard-nav path already disarms on move). */
        if (slot != app.menu_sel && app.menu_armed) {
            app.menu_armed = false;
            menu_clear_note();
        }
        app.menu_sel = slot;
        menu_activate(now);                        /* == Enter */
        return;
    }
    switch (k) {
    case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
        int ns = tile_nav(&app.grid, app.menu_sel, k);
        /* A grabbed reorder tile rides the arrows: each step swaps it
         * with the neighbour (never with the trailing Back tile). */
        if (app.menu_screen == MS_REORDER && app.reorder_grab >= 0) {
            if (ns != app.menu_sel && ns < app.stored_count &&
                app.menu_sel < app.stored_count) {
                conn_profile_t t           = app.profiles[ns];
                app.profiles[ns]           = app.profiles[app.menu_sel];
                app.profiles[app.menu_sel] = t;
                app.reorder_grab = ns;
                app.menu_sel     = ns;
                render_menu();
            }
            break;
        }
        if (ns != app.menu_sel) {
            app.menu_sel = ns;
            if (app.menu_armed) {            /* moving away backs the arm down */
                app.menu_armed = false;
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
