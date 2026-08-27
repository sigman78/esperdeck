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
    int      root;                  /* entry page: back from here pops the
                                       screen (MS_MAIN in-session, MS_CONFIG
                                       from HOME, deep links later)         */
    bool     armed;                 /* a destructive item needs a 2nd hit   */
    bool     note_wifi;             /* live-track wifi_status_str()         */
    int      reorder_grab;          /* grabbed stored index, -1 = none      */
} s_menu = { .reorder_grab = -1 };
#include "app_screens.h"
#include "app_settings.h"
#include "app_widgets.h"
#include "app_menu_defs.h"
#include "font.h"        /* font_height() — breadcrumb tap zone */
#include "ssh_client.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

/* Rendered slot -> item, hidden items skipped (the KEYSTORE page is
 * contextual). Rebuilt by menu_goto and every render. */
static const menu_item_t *s_slot[MENU_MAX_TILES];
static int s_slot_count;

/* The profile pickers' scrolling list (geometry refreshed per render). */
static ui_list_t s_pick;

static int build_slots(const menu_page_t *p)
{
    int n = 0;
    for (int i = 0; i < p->count && n < MENU_MAX_TILES; i++) {
        const menu_item_t *it = &p->items[i];
        if (it->hidden && it->hidden(it->arg)) continue;
        s_slot[n++] = it;
    }
    return s_slot_count = n;
}

/* Build a stored-profile picker's rows. Bodies "user@host" keep
 * same-named entries tellable. (No Back row — the breadcrumb is back.) */
static int picker_items(const char *out[], const char *bodies[],
                        char (*bodybuf)[28], int cap)
{
    int n = 0;
    for (int i = 0; i < app.stored_count && n < cap; i++) {
        snprintf(bodybuf[i], sizeof(bodybuf[i]), "%s@%s",
                 app.profiles[i].user, app.profiles[i].host);
        bodies[n] = bodybuf[i];
        out[n++]  = app.profiles[i].name;
    }
    return n;
}

/* Breadcrumb text: root-to-leaf page titles, at most three segments
 * (the in-session MENU root drops off a depth-3 chain). */
static void crumb_build(char *out, size_t sz, int sc)
{
    const char *seg[3];
    int n = 0, cur = sc;
    while (n < 3) {
        if (menu_is_picker(cur)) {
            seg[n++] = cur == MS_DELPROFILE  ? "DELETE"
                     : cur == MS_EDITPROFILE ? "EDIT"
                                             : "REORDER";
            cur = MS_PROFILES;
            continue;
        }
        const menu_page_t *p = menu_page(cur);
        seg[n++] = p->title;
        if (cur == s_menu.root || p->back_to < 0) break;
        cur = p->back_to;
    }
    int len = snprintf(out, sz, "<");
    for (int i = n - 1; i >= 0 && len < (int)sz; i--)
        len += snprintf(out + len, sz - len, "%s %s",
                        i == n - 1 ? " " : " /", seg[i]);
}

/* The BreadcrumbBar: rows 0-2 full width — names the place and IS the
 * back target (menu_input routes any tap on it to menu_back). */
static void draw_crumb(int x0, int w, int sc)
{
    char crumb[64];
    crumb_build(crumb, sizeof(crumb), sc);
    ui_pen(OVERLAY_COL_BLUE);
    ui_tile(x0, 0, w, 3, crumb, "", false);
    /* ui_puts is Latin-1 only, so the ◄ back chevron overdraws the
     * placeholder '<' the crumb string carries. */
    ui_putch(x0 + 2, 1, UI_POINT_L, OVERLAY_ATTR_INVERSE);
    char clk[8];
    if (clock_str(clk, sizeof(clk)))
        ui_puts(x0 + w - 2 - (int)strlen(clk), 1, clk, OVERLAY_ATTR_INVERSE);
    ui_pen(OVERLAY_COL_DEFAULT);
}

/* One value tile: 3-row solid bar, the value on a dimmed-accent well
 * (ui-spec, locked). Armed confirms replace the label and drop the well. */
static void draw_value_tile(int x, int y, int tw, const char *label,
                            const char *body, bool well, bool sel)
{
    ui_tile(x, y, tw, 3, label, well ? "" : body, sel);
    if (!well || !body[0]) return;
    int ww = tw / 3 + 4;
    if (ww > 14) ww = 14;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < ww; c++)
            ui_putch(x + tw - ww + c, y + r, ' ',
                     OVERLAY_ATTR_INVERSE | OVERLAY_ATTR_DIM);
    ui_puts(x + tw - 2 - (int)strlen(body), y + 1, body,
            OVERLAY_ATTR_INVERSE | OVERLAY_ATTR_DIM |
            (sel ? OVERLAY_ATTR_BOLD : 0));
}

static void render_menu(uint64_t now)
{
    (void)now;
    ui_dim();   /* dim the live session behind the menu so it pops */

    const int sc = s_menu.screen;
    const bool picker = menu_is_picker(sc);
    const menu_page_t *p = picker ? NULL : menu_page(sc);

    const int w  = ui_cols() - 8;
    const int x0 = 4;
    draw_crumb(x0, w, sc);

    const char *pick_titles[MAX_PROFILES];
    const char *pick_bodies[MAX_PROFILES];
    char pick_buf[MAX_PROFILES][28];
    char vbuf[MENU_MAX_TILES][16];
    const int note_row = ui_rows() - 1;

    if (picker) {
        /* Scrolling two-line list under the breadcrumb (variable length —
         * the one menu surface that scrolls, per ui-spec ListView). */
        const int count = picker_items(pick_titles, pick_bodies, pick_buf,
                                       NELEM(pick_titles));
        s_pick.x     = (ui_cols() - 40) / 2;
        s_pick.y     = 4;
        s_pick.w     = 40;
        s_pick.h     = note_row - 1 - s_pick.y;
        s_pick.row_h = 3;              /* 2-row tile + 1 gutter */
        s_pick.count = count;
        s_pick.sel   = s_menu.sel;
        ui_list_clamp(&s_pick);
        s_menu.sel = s_pick.sel;
        app.grid = (tilegrid_t){ 0 };  /* taps route through the list */

        const int vis = ui_list_visible(&s_pick);
        for (int i = s_pick.top; i < s_pick.top + vis && i < count; i++) {
            const char *confirm = (sc == MS_DELPROFILE)
                                ? "CONFIRM delete?" : NULL;
            const bool armed   = i == s_menu.sel && s_menu.armed && confirm;
            const bool grabbed = (sc == MS_REORDER) &&
                                 (i == s_menu.reorder_grab);
            ui_pen(armed               ? OVERLAY_COL_RED
                 : sc == MS_DELPROFILE ? OVERLAY_COL_AMBER
                 : grabbed             ? OVERLAY_COL_GREEN
                                       : OVERLAY_COL_CYAN);
            ui_tile(s_pick.x, ui_list_row_y(&s_pick, i), s_pick.w - 2, 2,
                    armed ? confirm : pick_titles[i], pick_bodies[i],
                    i == s_menu.sel || grabbed);
        }
        ui_pen(OVERLAY_COL_BLUE);
        ui_list_draw_scroll(&s_pick);
        ui_pen(OVERLAY_COL_DEFAULT);

        if (app.stored_count == 0) {
            const char *m = "no stored profiles";
            ui_puts((ui_cols() - (int)strlen(m)) / 2, 5, m, 0);
        }
    } else {
        /* Two-column grid of 3-row tiles — the whole section fits one
         * screen at every grid (fit-one-screen contract, asserted in
         * app_menu_defs.c). */
        const int count = build_slots(p);
        tilegrid_t g = { .tw = (w - 2) / 2, .th = 3, .gx = 2, .gy = 1,
                         .ncols = 2, .x0 = x0, .y0 = 4, .count = count };
        g.nrows = (count + 1) / 2;
        app.grid = g;
        if (s_menu.sel >= count) s_menu.sel = count ? count - 1 : 0;

        for (int i = 0; i < count; i++) {
            const menu_item_t *it = s_slot[i];
            const bool dim = it->dim && it->dim(it->arg);
            const bool armed = i == s_menu.sel && s_menu.armed && it->confirm;
            const bool sel = i == s_menu.sel;
            const int tx = tile_x(&g, i), ty = tile_y(&g, i);
            const char *label = armed ? it->confirm
                       : it->label_fn ? it->label_fn(it->arg)
                                      : it->label;
            ui_pen(armed         ? OVERLAY_COL_RED
                 : it->color_fn  ? it->color_fn(it->arg)
                                 : it->color);
            if (dim && !armed) {
                /* Unavailable: the whole bar in the dimmed accent, label
                 * centered — the muted look IS the message (dim_note
                 * explains on activation). */
                for (int r = 0; r < 3; r++)
                    for (int c = 0; c < g.tw; c++)
                        ui_putch(tx + c, ty + r, ' ',
                                 OVERLAY_ATTR_INVERSE | OVERLAY_ATTR_DIM);
                ui_puts(tx + 2, ty + 1, label,
                        OVERLAY_ATTR_INVERSE | OVERLAY_ATTR_DIM |
                        OVERLAY_ATTR_BOLD);
                if (sel) {          /* focus rail, as ui_tile draws it */
                    ui_pen(OVERLAY_COL_WHITE);
                    for (int r = 0; r < 3; r++)
                        ui_putch(tx, ty + r, UI_RHALF, OVERLAY_ATTR_INVERSE);
                }
                continue;
            }
            vbuf[i][0] = '\0';
            const char *body =
                it->value ? it->value(it->arg, vbuf[i], sizeof(vbuf[i])) : "";
            /* Value items put the body on the dimmed well; armed confirms
             * drop it. Plain items (hub sections, actions) center alone. */
            draw_value_tile(tx, ty, g.tw, label, body,
                            !armed && it->value, sel);
        }
        ui_pen(OVERLAY_COL_DEFAULT);

        if (sc == MS_MAIN) {
            /* Mainframe flex: deck uptime + link time. */
            uint64_t up = (uint64_t)app.anim_frame * ANIM_PERIOD_MS / 1000;
            char flex[48];
            uint64_t now_ms = (uint64_t)app.anim_frame * ANIM_PERIOD_MS;
            uint64_t ss = conn_session_start();
            uint64_t lk = now_ms > ss ? (now_ms - ss) / 1000 : 0;
            snprintf(flex, sizeof(flex), "UP %02u:%02u:%02u   LINK %02u:%02u",
                     (unsigned)(up / 3600), (unsigned)(up / 60 % 60),
                     (unsigned)(up % 60), (unsigned)(lk / 60),
                     (unsigned)(lk % 60));
            ui_pen(OVERLAY_COL_BLUE);
            ui_puts((ui_cols() - (int)strlen(flex)) / 2, note_row - 1, flex, 0);
            ui_pen(OVERLAY_COL_DEFAULT);
        }
    }

    /* Feedback renders on the StatusBar (the shared toast) — the shell
     * composites it after this render. */
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

/* Menu feedback rides the shared toast on the StatusBar (ui-spec).
 * @p ms: lifetime; 0 = sticky (an armed confirm holds its prompt until
 * acted on or disarmed — menu_exit clears a leftover sticky so it can
 * never follow the user onto another screen). */
void menu_note(uint64_t now, uint32_t ms, bool live_wifi, const char *text)
{
    toast_for(now, 1, "%s", text);
    app.toast_until = ms ? now + ms : UINT64_MAX;   /* MAX = sticky */
    s_menu.note_wifi = live_wifi;
}

static void menu_clear_note(void)
{
    app.toast[0]     = '\0';
    s_menu.note_wifi = false;
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
    if (menu_is_picker(sc)) {
        s_pick.top = 0;
        ui_drag_reset(&s_pick.drag);
    }
    const menu_page_t *p = menu_page(sc);
    if (p && p->on_open) p->on_open();
    if (p) build_slots(p);
    app_settings_hold(p ? p->hold : 0);
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
 * so an armed confirm can never leak across pages. Back at the entry root
 * leaves the menu screen; anywhere else follows the page's back_to. */
void menu_back(uint64_t now)
{
    if (s_menu.screen == MS_REORDER && s_menu.reorder_grab >= 0) {
        menu_abort_reorder();
        menu_clear_note();
        nav_invalidate();
        return;
    }
    const int to = menu_is_picker(s_menu.screen)
                 ? MS_PROFILES
                 : menu_page(s_menu.screen)->back_to;
    if (s_menu.screen == s_menu.root || to == MENU_BACK_LEAVE) {
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

/* The three pickers share the profile-row shape but differ in what a
 * hit means. Back is the breadcrumb (no Back row). */
static void picker_activate(int sc, int sel, bool was_armed, uint64_t now)
{
    if (sel < 0 || sel >= app.stored_count) return;   /* empty picker */

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
    s_menu.root = (int)arg;
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
 * settings hold so deferred writes flush on the next tick, and drops a
 * leftover STICKY note (an arm prompt must not follow the user out). */
static void menu_exit(uint64_t now)
{
    (void)now;
    app_settings_hold(0);
    if (app.toast_until == UINT64_MAX) menu_clear_note();
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
    /* Only the in-session menu has a session to monitor. */
    if (s_menu.root == MS_MAIN && !ssh_client_is_connected()) {
        session_dropped(now);
        return;
    }
    /* Live feedback on the 10 fps gate: wifi-tracking notes rewrite from
     * the real state, expired notes clear, the UP/LINK clocks tick. */
    if (now >= app.next_anim) {
        app.next_anim = now + ANIM_PERIOD_MS;
        if (s_menu.note_wifi && app.toast[0]) {
            snprintf(app.toast, sizeof(app.toast), "wifi: %s",
                     wifi_status_str());
            /* Still in flight? Keep the note alive — expiring mid-
             * reconnect reads as the action silently dying. */
            wifi_mgr_state_t ws = wifi_manager_get_state();
            if (ws == WIFI_MGR_CONNECTING || ws == WIFI_MGR_LOST)
                app.toast_until = now + MENU_MSG_MS;
            else if (now >= app.toast_until)
                menu_clear_note();
        }
        nav_invalidate();
    }
}

static void menu_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                       uint64_t now)
{
    (void)ch;
    const bool picker = menu_is_picker(s_menu.screen);
    /* Esc / F12 / tap-outside all step back one level. */
    if (k == K_ESC || k == K_F12) {
        menu_back(now);
        return;
    }
    /* Right-edge drag scrolls a picker's list; selection follows the view. */
    if (ev->type == CYBERDECK_INPUT_SCROLL) {
        if (picker && ui_list_scroll(&s_pick, ev->dy)) {
            s_menu.sel = s_pick.sel;
            if (s_menu.armed) {
                s_menu.armed = false;
                menu_clear_note();
            }
            nav_invalidate();
        }
        return;
    }
    if (ev->type == CYBERDECK_INPUT_TAP) {
        /* The BreadcrumbBar (rows 0-2) is the back target. */
        if (ev->y / font_height() < 3) { menu_back(now); return; }
        int slot = picker ? ui_list_hit(&s_pick, ev->x, ev->y)
                          : tile_hit(&app.grid, ev->x, ev->y);
        if (slot < 0) { menu_back(now); return; }   /* tap outside: back */
        /* Tapping a DIFFERENT tile than the armed one must disarm first, or
         * the stale arm fires this tile's destructive action unconfirmed. */
        if (slot != s_menu.sel && s_menu.armed) {
            s_menu.armed = false;
            menu_clear_note();
        }
        s_menu.sel = slot;
        if (picker) s_pick.sel = slot;
        menu_activate(now);                        /* == Enter */
        return;
    }
    switch (k) {
    case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT:
    case K_SCROLL_UP: case K_SCROLL_DOWN: {
        /* A grabbed reorder row rides the arrows: each step swaps it
         * with the neighbour (never with the trailing Back row). */
        if (s_menu.screen == MS_REORDER && s_menu.reorder_grab >= 0) {
            int ns = k == K_UP   ? s_menu.sel - 1
                   : k == K_DOWN ? s_menu.sel + 1
                                 : s_menu.sel;
            if (ns != s_menu.sel && ns >= 0 && ns < app.stored_count &&
                s_menu.sel < app.stored_count) {
                conn_profile_t t         = app.profiles[ns];
                app.profiles[ns]         = app.profiles[s_menu.sel];
                app.profiles[s_menu.sel] = t;
                s_menu.reorder_grab = ns;
                s_menu.sel = s_pick.sel = ns;
                ui_list_clamp(&s_pick);
                nav_invalidate();
            }
            break;
        }
        int ns = s_menu.sel;
        if (picker) {
            s_pick.sel = s_menu.sel;
            if (ui_list_nav(&s_pick, k)) ns = s_pick.sel;
        } else {
            ns = tile_nav(&app.grid, s_menu.sel, k);
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
    .render = render_menu, .chrome = NAV_CHROME_FULL,
};
