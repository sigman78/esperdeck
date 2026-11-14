/*
 * app_menu.c is the overlay menu screen. It renders generic pages and
 * dispatches input over the app_menu_defs.c item tables.
 */

#include "app_internal.h"

static struct {
    int      sel;
    int      screen;                /* menu_screen_t: page of the menu tree */
    /* root is the entry page. Back from here pops the screen: MS_MAIN
     * in-session, MS_CONFIG from HOME, deep links later. */
    int      root;
    bool     armed;                 /* a destructive item needs a 2nd hit   */
    bool     fresh;                 /* page switched, not drawn yet         */
    bool     note_wifi;             /* live-track wifi_status_str()         */
} s_menu;
#include "app_screens.h"
#include "app_settings.h"
#include "app_widgets.h"
#include "app_menu_defs.h"
#include "ssh_client.h"
#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

/* Card ids on a table page: slot + 1 (0 is "no region"). */
#define SLOT_ID(i)  ((ui_id_t)((i) + 1))

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
    return s_slot_count = n;
}

/* The header: the page title, the pages above it as a muted path (at
 * most two; the in-session MENU root drops off a depth-3 chain). The
 * whole bar is the back target. */
static void draw_header(ui_rect_t r, int sc)
{
    const char *seg[3];
    int n = 0, cur = sc;
    while (n < 3) {
        const menu_page_t *p = menu_page(cur);
        seg[n++] = p->title;
        if (cur == s_menu.root || p->back_to < 0) break;
        cur = p->back_to;
    }
    char path[48] = "";
    int len = 0;
    for (int i = n - 1; i >= 1 && len < (int)sizeof(path); i--)
        len += snprintf(path + len, sizeof(path) - len, "%s%s",
                        i == n - 1 ? "" : " / ", seg[i]);
    ui_header(r, path, seg[0], NULL, true);
}

static void render_menu(uint64_t now)
{
    (void)now;
    ui_dim();   /* dim the live session behind the menu so it pops */

    const int sc = s_menu.screen;
    const menu_page_t *p = menu_page(sc);

    const ui_metrics_t m = ui_metrics();
    const ui_page_t pg = ui_page();
    draw_header(pg.header, sc);
    s_menu.fresh = false;

    char vbuf[MENU_MAX_TILES][16];
    const int note_row = ui_rows() - 1;

    /* Two columns of one-line cards. The whole section fits one
     * screen at every grid (fit-one-screen contract, asserted in
     * app_menu_defs.c and tests/ui). */
    const int count = build_slots(p);
    const ui_grid_t g = ui_grid(pg.body, 2, m.unit, m.gutter, 0);
    app.grid = (tilegrid_t){ 0 };  /* taps route through ui_hot */
    if (s_menu.sel >= count) s_menu.sel = count ? count - 1 : 0;

    for (int i = 0; i < count; i++) {
        const menu_item_t *it = s_slot[i];
        const bool armed = i == s_menu.sel && s_menu.armed && it->confirm;
        ui_card_t c = {
            .title  = it->label_fn ? it->label_fn(it->arg) : it->label,
            .accent = it->color_fn ? it->color_fn(it->arg) : it->color,
        };
        vbuf[i][0] = '\0';
        if (armed) {
            /* The armed confirm reads as the one action on the page. */
            c.title  = it->confirm;
            c.accent = OVERLAY_COL_RED;
            c.flags  = UI_CARD_ACTION | UI_CARD_LIT;
        } else if (it->dim && it->dim(it->arg)) {
            /* Still a region: activating it explains why (dim_note). */
            c.flags = UI_CARD_DISABLED;
        } else if (it->value) {
            c.value = it->value(it->arg, vbuf[i], sizeof(vbuf[i]));
        }
        ui_card(ui_grid_cell(&g, i), SLOT_ID(i), &c);
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
    s_menu.fresh  = true;
    ui_hot_reset();
    const menu_page_t *p = menu_page(sc);
    if (p && p->on_open) p->on_open();
    if (p) build_slots(p);
    app_settings_hold(p ? p->hold : 0);
    menu_clear_note();
    nav_invalidate();
}

/* Back one level. Every back path (Esc, tap-outside, Back tile) funnels here
 * so an armed confirm can never leak across pages. Back at the entry root
 * leaves the menu screen; anywhere else follows the page's back_to. */
void menu_back(uint64_t now)
{
    const int to = menu_page(s_menu.screen)->back_to;
    if (s_menu.screen == s_menu.root || to == MENU_BACK_LEAVE) {
        s_menu.armed = false;
        nav_pop(now);
    } else {
        menu_goto(to);
    }
}

/* Generic activation: dim gates first, then the 2-step confirm arm, then
 * the item's own action. Replaces the per-page positional switch. */
static void menu_activate(uint64_t now)
{
    const int sel = s_menu.sel;
    const bool was_armed = s_menu.armed;
    s_menu.armed = false;

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

/* Any departure — a pop, or another screen pushed on top — releases
 * the settings hold. This lets deferred writes flush on the next tick.
 * It also drops a leftover STICKY note: an arm prompt must not follow
 * the user out. */
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
    /* Live feedback runs on the 10 fps gate. Wifi-tracking notes rewrite
     * from the real state, expired notes clear, and the UP/LINK clocks
     * tick. */
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

/* A table page: every card is a region, so touch and keys resolve the
 * same way. The tapped or focused slot becomes the selection that
 * menu_activate and the confirm arm work on. */
static void page_input(const cyberdeck_input_t *ev, ui_key_t k, uint64_t now)
{
    /* A tap that lands before the page's first frame has no regions to
     * hit. Drop it: it must not read as a tap outside. */
    if (s_menu.fresh) return;

    const ui_act_t a = app_hot_input(ev, k);
    const int slot = (a.id && a.id != UI_ID_BACK) ? (int)a.id - 1 : -1;

    if (a.kind == UI_ACT_PRESS && a.id == UI_ID_BACK) {
        menu_back(now);
        return;
    }
    if (ev->type == CYBERDECK_INPUT_TAP && !a.id) {   /* tap outside: back */
        menu_back(now);
        return;
    }
    /* Finger down or up only changes how a card looks. A long press does
     * nothing here, but it ends the press and the card must repaint. */
    if (ev->type == CYBERDECK_INPUT_PRESS ||
        ev->type == CYBERDECK_INPUT_RELEASE ||
        ev->type == CYBERDECK_INPUT_LONG_PRESS) {
        if (a.kind != UI_ACT_NONE) nav_invalidate();
        return;
    }
    if (a.kind != UI_ACT_PRESS && a.kind != UI_ACT_MOVE) return;
    if (slot < 0 || slot >= s_slot_count) {
        nav_invalidate();
        return;
    }

    /* Moving to, or hitting, a DIFFERENT card than the armed one disarms
     * it first. Otherwise the stale arm fires that card's destructive
     * action unconfirmed. */
    if (slot != s_menu.sel && s_menu.armed) {
        s_menu.armed = false;
        menu_clear_note();
    }
    s_menu.sel = slot;
    if (a.kind == UI_ACT_PRESS) menu_activate(now);    /* tap == Enter */
    else                        nav_invalidate();
}

static void menu_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                       uint64_t now)
{
    (void)ch;
    /* Esc / F12 / tap-outside all step back one level. */
    if (k == K_ESC || k == K_F12) {
        menu_back(now);
        return;
    }
    page_input(ev, k, now);
}

const nav_screen_t menu_screen = {
    .name = "menu", .enter = menu_enter, .resume = menu_resume,
    .exit = menu_exit, .tick = menu_tick, .input = menu_input,
    .render = render_menu, .chrome = NAV_CHROME_FULL,
};
