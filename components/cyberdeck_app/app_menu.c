/*
 * app_menu.c — the overlay menu screen: generic page rendering + input
 * dispatch over the app_menu_defs.c item tables, plus the dynamic
 * profile pickers (delete / edit / reorder — the future ListView
 * consumers, extensibility item 4).
 */

#include "app_internal.h"

static struct {
    int      sel;
    int      screen;                /* menu_screen_t: page of the menu tree */
    bool     from_home;             /* config opened from HOME (no session) */
    bool     armed;                 /* a destructive item needs a 2nd hit   */
    char     msg[48];               /* action result, shown under the tiles */
    uint64_t msg_until;             /* auto-clear time; 0 = sticky          */
    bool     msg_wifi;              /* live-track wifi_status_str()         */
    int      reorder_grab;          /* grabbed stored index, -1 = none      */
} s_menu = { .reorder_grab = -1 };
#include "app_screens.h"
#include "app_settings.h"
#include "app_widgets.h"
#include "app_menu_defs.h"
#include "ssh_client.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

/* Rendered slot -> item, hidden items skipped (the KEYSTORE page is
 * contextual). Rebuilt by menu_goto and every render. */
static const menu_item_t *s_slot[MENU_MAX_TILES];
static int s_slot_count;

static int build_slots(const menu_page_t *p)
{
    int n = 0;
    for (int i = 0; i < p->count && n < MENU_MAX_TILES; i++) {
        const menu_item_t *it = &p->items[i];
        if (it->hidden && it->hidden(it->arg)) continue;
        s_slot[n++] = it;
    }
    return n;
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

static void render_menu(uint64_t now)
{
    (void)now;
    ui_dim();   /* dim the live session behind the menu so it pops */

    const int sc = s_menu.screen;
    const bool root = (sc == MS_MAIN);
    const bool picker = menu_is_picker(sc);
    const menu_page_t *p = picker ? NULL : menu_page(sc);
    const uint8_t pflags = p ? p->flags : 0;

    const char *pick_titles[MAX_PROFILES + 1];
    const char *pick_bodies[MAX_PROFILES + 1];
    char pick_buf[MAX_PROFILES][28];
    char vbuf[MENU_MAX_TILES][16];
    const char *title;
    int count;

    if (picker) {
        title = sc == MS_DELPROFILE  ? "DELETE PROFILE"
              : sc == MS_EDITPROFILE ? "EDIT PROFILE"
                                     : "REORDER PROFILES";
        count = picker_items(pick_titles, pick_bodies, pick_buf,
                             NELEM(pick_titles));
    } else {
        title = p->title;
        count = s_slot_count = build_slots(p);
    }

    /* Pickers can outgrow one centered column, so they use the multi-column
     * HOME grid; everything below is grid-agnostic. */
    tilegrid_t g;
    int title_row, ly, chrome_x;
    if (picker || (pflags & MENU_PAGE_WIDE)) {
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
    if (s_menu.sel >= g.count) s_menu.sel = g.count ? g.count - 1 : 0;

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
        const char *label, *body;
        uint8_t col;
        bool armed = false, grabbed = false;
        if (picker) {
            const char *confirm = (sc == MS_DELPROFILE && i < app.stored_count)
                                ? "CONFIRM delete?" : NULL;
            armed   = i == s_menu.sel && s_menu.armed && confirm;
            grabbed = (sc == MS_REORDER) && (i == s_menu.reorder_grab);
            col = armed                 ? OVERLAY_COL_RED
                : i >= app.stored_count ? OVERLAY_COL_BLUE
                : sc == MS_DELPROFILE   ? OVERLAY_COL_AMBER
                : grabbed               ? OVERLAY_COL_GREEN
                                        : OVERLAY_COL_CYAN;
            label = armed ? confirm : pick_titles[i];
            body  = pick_bodies[i];
        } else {
            const menu_item_t *it = s_slot[i];
            const bool dim = it->dim && it->dim(it->arg);
            armed = i == s_menu.sel && s_menu.armed && it->confirm;
            col = armed        ? OVERLAY_COL_RED
                : it->color_fn ? it->color_fn(it->arg)
                               : it->color;
            label = armed        ? it->confirm
                  : it->label_fn ? it->label_fn(it->arg)
                                 : it->label;
            vbuf[i][0] = '\0';
            body = it->value ? it->value(it->arg, vbuf[i], sizeof(vbuf[i]))
                 : dim       ? "(unavailable)"
                             : "";
        }
        ui_pen(col);
        /* Focus is carried by the tile itself (washed bar + lit rail);
         * a grabbed REORDER tile reads by its green bar. */
        const bool sel = i == s_menu.sel || grabbed;
        if ((pflags & MENU_PAGE_VALS) && body[0] && !armed) {
            /* Value right-aligned on the title row in regular weight,
             * settings-table style — the bold name carries the emphasis
             * and the value reads as data. */
            ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th, label, "", sel);
            ui_puts(tile_x(&g, i) + g.tw - 2 - (int)strlen(body),
                    tile_y(&g, i) + (g.th - 1) / 2, body,
                    OVERLAY_ATTR_INVERSE | (sel ? OVERLAY_ATTR_BRIGHT : 0));
        } else {
            ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th, label, body, sel);
        }
    }

    /* Esc legend under the tile area; suppressed while a note is up (the
     * two share this row). */
    if (!s_menu.msg[0]) {
        const char *legend = root ? "Esc/F12 resume \xB7 tap outside closes"
                           : sc == MS_CONFIG
                               ? (s_menu.from_home ? "Esc \xB7 home" : "Esc \xB7 menu")
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

    if (s_menu.msg[0]) {               /* action feedback */
        /* On the legend row, not below it: ly is clamped to the last grid
         * row, so ly+1 could be off-panel and the note would vanish. */
        int mx = chrome_x + (40 - ((int)strlen(s_menu.msg) + 2)) / 2;
        ui_pen(OVERLAY_COL_AMBER);
        ui_putch(mx, ly, UI_DIAMOND, 0);
        ui_puts(mx + 2, ly, s_menu.msg, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    if (root) {
        /* Mainframe flex: deck uptime + link time behind the menu. */
        uint64_t up = (uint64_t)app.anim_frame * ANIM_PERIOD_MS / 1000;
        char flex[48];
        uint64_t now_ms = (uint64_t)app.anim_frame * ANIM_PERIOD_MS;
        uint64_t ss = conn_session_start();
        uint64_t lk = now_ms > ss ? (now_ms - ss) / 1000 : 0;
        snprintf(flex, sizeof(flex), "UP %02u:%02u:%02u   LINK %02u:%02u",
                 (unsigned)(up / 3600), (unsigned)(up / 60 % 60),
                 (unsigned)(up % 60), (unsigned)(lk / 60), (unsigned)(lk % 60));
        ui_pen(OVERLAY_COL_BLUE);
        ui_puts(g.x0 + (g.tw - (int)strlen(flex)) / 2, ly + 2, flex, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }
}

/* Paint + present out of turn — for an action that blocks and reboots
 * (font apply), so its note is readable before the restart. */
void menu_present_now(uint64_t now)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    render_menu(now);
    ui_no_cursor();
    ui_present();
}

/* Post an action-feedback line under the menu tiles.
 * @p ms: lifetime; 0 = sticky. @p live_wifi: track wifi_status_str(). */
void menu_note(uint64_t now, uint32_t ms, bool live_wifi, const char *text)
{
    snprintf(s_menu.msg, sizeof(s_menu.msg), "%s", text);
    s_menu.msg_until = ms ? now + ms : 0;
    s_menu.msg_wifi  = live_wifi;
}

static void menu_clear_note(void)
{
    s_menu.msg[0]    = '\0';
    s_menu.msg_until = 0;
    s_menu.msg_wifi  = false;
}

/* Switch to menu screen @p sc, resetting selection/arm/note. Table pages
 * snapshot volatile state via on_open and prebuild the slot map (a tap
 * can land before the first render). The settings hold defers flash
 * writes while their page is open (see app_settings_idle_flush). */
void menu_goto(int sc)
{
    s_menu.screen = sc;
    s_menu.sel    = 0;
    s_menu.armed  = false;
    if (sc == MS_REORDER) s_menu.reorder_grab = -1;
    const menu_page_t *p = menu_page(sc);
    if (p && p->on_open) p->on_open();
    if (p) s_slot_count = build_slots(p);
    app_settings_hold(sc == MS_EFFECTS ? APP_SETTINGS_HOLD_FX
                    : sc == MS_SYSTEM  ? APP_SETTINGS_HOLD_SYS : 0);
    menu_clear_note();
    nav_invalidate();
}

/* Discard a grabbed-but-not-dropped reorder: the pending moves only live in
 * app.profiles until a drop saves them, and a session drop can yank the
 * user out mid-drag — a later save would silently persist them. */
void menu_abort_reorder(void)
{
    if (s_menu.screen == MS_REORDER && s_menu.reorder_grab >= 0) {
        s_menu.reorder_grab = -1;
        load_profiles();
    }
}

/* Back one level. Every back path (Esc, tap-outside, Back tile) funnels here
 * so an armed confirm can never leak across pages. */
void menu_back(uint64_t now)
{
    if (s_menu.screen == MS_REORDER && s_menu.reorder_grab >= 0) {
        menu_abort_reorder();
        menu_clear_note();
        nav_invalidate();
        return;
    }
    if (s_menu.screen == MS_CONFIG && s_menu.from_home) {
        nav_pop(now);                          /* opened from HOME */
        return;
    }
    const int to = menu_is_picker(s_menu.screen)
                 ? MS_PROFILES
                 : menu_page(s_menu.screen)->back_to;
    if (to == MENU_BACK_LEAVE) {               /* MAIN: resume the session */
        s_menu.armed = false;
        nav_pop(now);
    } else {
        menu_goto(to);
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
    s_menu.reorder_grab = -1;
    if (storage_save_profiles(app.profiles, app.stored_count) == ESP_OK)
        menu_note(now, MENU_MSG_MS, false, "order saved");
    else
        menu_note(now, MENU_MSG_MS, false, "save failed");
    load_profiles();
}

/* The three pickers share the "profile tile or trailing Back" shape but
 * differ in what a profile hit means. */
static void picker_activate(int sc, int sel, bool was_armed, uint64_t now)
{
    if (sel >= app.stored_count) { menu_back(now); return; }   /* Back tile */

    switch (sc) {
    case MS_DELPROFILE:
        if (!was_armed) {
            s_menu.armed = true;
            menu_note(now, 0, false, "activate again to delete");
        } else {
            delete_profile_at(sel);
            if (s_menu.sel >= app.stored_count && s_menu.sel > 0) s_menu.sel--;
            menu_note(now, MENU_MSG_MS, false, "profile deleted");
        }
        break;

    case MS_EDITPROFILE:
        enter_profile(now, sel);
        return;

    case MS_REORDER:
        if (s_menu.reorder_grab < 0) {                         /* grab */
            s_menu.reorder_grab = sel;
            menu_note(now, 0, false, "arrows/tap move it - Enter drops");
        } else {                                               /* drop */
            if (sel != s_menu.reorder_grab) {
                conn_profile_t t                  = app.profiles[sel];
                app.profiles[sel]                 = app.profiles[s_menu.reorder_grab];
                app.profiles[s_menu.reorder_grab] = t;
                s_menu.sel = sel;
            }
            commit_reorder(now);
        }
        break;
    }
    nav_invalidate();
}

/* Generic activation: dim gates first, then the 2-step confirm arm, then
 * the item's own action. Replaces the per-page positional switch. */
static void menu_activate(uint64_t now)
{
    const int sc  = s_menu.screen;
    const int sel = s_menu.sel;
    const bool was_armed = s_menu.armed;
    s_menu.armed = false;

    if (menu_is_picker(sc)) {
        picker_activate(sc, sel, was_armed, now);
        return;
    }

    const menu_item_t *it = (sel >= 0 && sel < s_slot_count) ? s_slot[sel]
                                                             : NULL;
    if (!it) return;
    if (it->dim && it->dim(it->arg)) {
        if (it->dim_note) menu_note(now, MENU_MSG_MS, false, it->dim_note);
        nav_invalidate();
        return;
    }
    if (it->confirm && !was_armed) {
        s_menu.armed = true;
        menu_note(now, 0, false, it->arm_note);
        nav_invalidate();
        return;
    }
    if (it->action) it->action(it->arg, now);
    nav_invalidate();
}

/* @p arg = the root page: MS_MAIN in-session, MS_CONFIG from HOME. */
static void menu_enter(intptr_t arg, uint64_t now)
{
    (void)now;
    s_menu.from_home = (arg == MS_CONFIG);
    menu_goto((int)arg);
}

/* Revealed by a pop (profile editor, keystore pad): re-land on the page
 * the user left, selection reset. */
static void menu_resume(intptr_t arg, uint64_t now)
{
    (void)arg; (void)now;
    menu_goto(s_menu.screen);
}

/* Any departure — pop, or another screen pushed on top — releases the
 * settings hold so deferred writes flush on the next tick. */
static void menu_exit(uint64_t now)
{
    (void)now;
    app_settings_hold(0);
}

void menu_open(uint64_t now)
{
    nav_push(SCR_MENU, MS_MAIN, now);
}

void menu_open_config(uint64_t now)
{
    nav_push(SCR_MENU, MS_CONFIG, now);
}

static void menu_tick(uint64_t now)
{
    /* A menu opened from HOME has no session to monitor. */
    if (!s_menu.from_home && !ssh_client_is_connected()) {
        session_dropped(now);
        return;
    }
    /* Live feedback on the 10 fps gate: wifi-tracking notes rewrite from
     * the real state, expired notes clear, the UP/LINK clocks tick. */
    if (now >= app.next_anim) {
        app.next_anim = now + ANIM_PERIOD_MS;
        if (s_menu.msg[0] && s_menu.msg_wifi) {
            snprintf(s_menu.msg, sizeof(s_menu.msg), "wifi: %s", wifi_status_str());
            /* Still in flight? Keep the note alive — expiring mid-
             * reconnect reads as the action silently dying. */
            wifi_mgr_state_t ws = wifi_manager_get_state();
            if (ws == WIFI_MGR_CONNECTING || ws == WIFI_MGR_LOST)
                s_menu.msg_until = now + MENU_MSG_MS;
        }
        if (s_menu.msg[0] && s_menu.msg_until && now >= s_menu.msg_until)
            menu_clear_note();
        nav_invalidate();
    }
}

static void menu_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                       uint64_t now)
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
        if (slot != s_menu.sel && s_menu.armed) {
            s_menu.armed = false;
            menu_clear_note();
        }
        s_menu.sel = slot;
        menu_activate(now);                        /* == Enter */
        return;
    }
    switch (k) {
    case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
        int ns = tile_nav(&app.grid, s_menu.sel, k);
        /* A grabbed reorder tile rides the arrows: each step swaps it
         * with the neighbour (never with the trailing Back tile). */
        if (s_menu.screen == MS_REORDER && s_menu.reorder_grab >= 0) {
            if (ns != s_menu.sel && ns < app.stored_count &&
                s_menu.sel < app.stored_count) {
                conn_profile_t t         = app.profiles[ns];
                app.profiles[ns]         = app.profiles[s_menu.sel];
                app.profiles[s_menu.sel] = t;
                s_menu.reorder_grab = ns;
                s_menu.sel          = ns;
                nav_invalidate();
            }
            break;
        }
        if (ns != s_menu.sel) {
            s_menu.sel = ns;
            if (s_menu.armed) {              /* moving away backs the arm down */
                s_menu.armed = false;
                menu_clear_note();
            }
            nav_invalidate();
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

const nav_screen_t menu_screen = {
    .name = "menu", .enter = menu_enter, .resume = menu_resume,
    .exit = menu_exit, .tick = menu_tick, .input = menu_input,
    .render = render_menu, .chrome = NAV_CHROME_NONE,
};
