/*
 * app_wifinet.c - the saved-network page (SCR_WIFINET): what a tap on a
 * Setup > Wi-Fi row opens. Facts of one network, Join now, the two moves,
 * and Forget set apart at the bottom. Forget asks first (app_ask.c).
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "app_wifi.h"
#include "wifi_manager.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

enum { ID_JOIN = 1, ID_EARLIER, ID_LATER, ID_FORGET };

static const char *TAG = "app_wifinet";

/* The list as the page last read it; idx follows the network through a move. */
static struct {
    int     idx, n;
    char    ssid[STORAGE_WIFI_MAX][33];
    bool    locked[STORAGE_WIFI_MAX], open[STORAGE_WIFI_MAX];
    ui_id_t saved_focus;
} s_net;

static void net_load(void)
{
    s_net.n = app_wifi_list(s_net.ssid, s_net.locked, s_net.open,
                            STORAGE_WIFI_MAX);
}

static bool net_valid(void)
{
    return s_net.idx >= 0 && s_net.idx < s_net.n;
}

static bool net_joined(void)
{
    return wifi_manager_get_state() == WIFI_MGR_CONNECTED &&
           strcmp(wifi_manager_get_ssid(), s_net.ssid[s_net.idx]) == 0;
}

static void render_wifinet(uint64_t now)
{
    (void)now;
    const int faults = ui_faults();
    ui_slate_t pg = ui_slate_page();
    if (!net_valid()) return;
    const char *ssid = s_net.ssid[s_net.idx];
    const bool joined = net_joined();
    const bool off[ID_FORGET + 1] = {
        /* Joining drops the link, so a live session keeps it off. */
        [ID_JOIN] = joined || s_net.locked[s_net.idx] ||
                    nav_on_stack(SCR_SESSION),
        [ID_EARLIER] = s_net.idx <= 0,
        [ID_LATER] = s_net.idx >= s_net.n - 1,
    };

    char pos[24];
    snprintf(pos, sizeof pos, "%d of %d", s_net.idx + 1, s_net.n);
    const ui_info_row_t rows[3] = {
        joined ? (ui_info_row_t){ .label = "Network", .value = ssid,
                                  .tail = UI_S_CHECK " joined",
                                  .tail_pen = OVERLAY_COL_GREEN }
               : (ui_info_row_t){ .label = "Network", .value = ssid },
        { .label = "Position", .value = pos },
        { .label = "Password",
          .value = s_net.locked[s_net.idx] ? "locked"
                 : s_net.open[s_net.idx] ? "none" : "saved" },
    };

    /* A button that went off cannot keep the focus: its twin takes it. */
    const ui_id_t first = !off[ID_JOIN] ? ID_JOIN : !off[ID_EARLIER] ? ID_EARLIER
                        : !off[ID_LATER] ? ID_LATER : ID_FORGET;
    const ui_id_t f = ui_focus_get();
    if (f >= ID_JOIN && f <= ID_FORGET && off[f]) {
        const ui_id_t twin = f == ID_LATER ? ID_EARLIER
                           : f == ID_EARLIER ? ID_LATER : first;
        ui_focus_set(off[twin] ? first : twin);
    }
    ui_focus_default(first);

    ui_title(pg.header, "Wi-Fi", ssid, NULL, true);
    ui_info(ui_cut_top(&pg.body, 5), rows, 3,
            joined ? OVERLAY_COL_GREEN : 0);
    ui_flow_t fl = ui_flow(pg.body, pg.m.gutter, 3);
#define BTN(id, label, tone, accent) \
    ui_push(ui_flow_next(&fl, ui_push_w(label)), id, label, tone, accent, \
            off[id] ? UI_PART_OFF : 0)
    BTN(ID_JOIN, "Join now", UI_TONE_SOLID, OVERLAY_COL_GREEN);
    ui_flow_break(&fl);
    BTN(ID_EARLIER, UI_S_UP " Move earlier", UI_TONE_CARD, 0);
    BTN(ID_LATER, UI_S_DOWN " Move later", UI_TONE_CARD, 0);
#undef BTN
    ui_push(ui_rect(pg.body.x, pg.body.y + pg.body.h - 3,
                    ui_push_w("Forget"), 3),
            ID_FORGET, "Forget", UI_TONE_TINT, OVERLAY_COL_RED, 0);

    if (ui_faults() != faults)
        ESP_LOGW(TAG, "layout faults +%d", ui_faults() - faults);
}

static void wifinet_enter(intptr_t arg, uint64_t now)
{
    (void)now;
    s_net.idx = (int)arg;
    s_net.saved_focus = 0;
    net_load();
}

/* Back from the question: keep the focus where it was. */
static void wifinet_resume(intptr_t arg, uint64_t now)
{
    (void)arg; (void)now;
    if (s_net.saved_focus) ui_focus_set(s_net.saved_focus);
}

static void wifinet_exit(uint64_t now)
{
    (void)now;
    s_net.saved_focus = ui_focus_get();
}

/* The page follows the link. Join now turns off when the link joins. */
static void wifinet_tick(uint64_t now)
{
    if (!net_valid()) {
        nav_pop(now);
        return;
    }
    if (now >= app.next_anim) {
        app.next_anim = now + ANIM_PERIOD_MS;
        nav_invalidate();
    }
}

static void say_failed(esp_err_t e, uint64_t now)
{
    toast(now, e == ESP_ERR_INVALID_STATE ? "unlock the deck first"
                                          : "save failed");
    nav_invalidate();
}

static void net_join(uint64_t now)
{
    const esp_err_t e = app_wifi_join(s_net.idx);
    if (e != ESP_OK) {
        say_failed(e, now);
        return;
    }
    s_net.idx = 0;
    net_load();
    toast(now, "joining %s", s_net.ssid[0]);
    nav_invalidate();
}

/* Swap the network with its neighbour in wifi.ini; the page follows it. */
static void net_move(int dir, uint64_t now)
{
    const int to = s_net.idx + dir;
    if (to < 0 || to >= s_net.n) return;
    const esp_err_t e = app_wifi_move(s_net.idx, dir);
    if (e == ESP_OK) s_net.idx = to;
    else say_failed(e, now);
    net_load();
    nav_invalidate();
}

static void net_forget_yes(intptr_t arg, uint64_t now)
{
    const esp_err_t e = app_wifi_forget((int)arg);
    if (e != ESP_OK) {
        say_failed(e, now);
        return;
    }
    toast(now, "network forgotten");
    nav_pop(now);
}

static void net_ask_forget(uint64_t now)
{
    char title[64];
    snprintf(title, sizeof title, "Forget %s?", s_net.ssid[s_net.idx]);
    ask_open(&(ask_t){
        .title = title,
        .line1 = net_joined() ? "The deck leaves this network."
                              : "The network and its password go.",
        .line2 = net_joined() && nav_on_stack(SCR_SESSION)
                     ? "The SSH session drops." : NULL,
        .verb = "Forget", .accent = OVERLAY_COL_RED,
        .yes = net_forget_yes, .arg = s_net.idx,
    }, now);
}

static void wifinet_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                          uint64_t now)
{
    (void)ch;
    if (k == K_ESC) {
        nav_pop(now);
        return;
    }
    const ui_act_t a = app_hot_input(ev, k);
    if (a.kind == UI_ACT_NONE) return;
    nav_invalidate();
    if (a.kind != UI_ACT_PRESS || !net_valid()) return;
    switch (a.id) {
    case UI_ID_BACK: nav_pop(now);           break;
    case ID_JOIN:    net_join(now);          break;
    case ID_EARLIER: net_move(-1, now);      break;
    case ID_LATER:   net_move(1, now);       break;
    case ID_FORGET:  net_ask_forget(now);    break;
    default:         break;
    }
}

void enter_wifinet(uint64_t now, int idx)
{
    nav_push(SCR_WIFINET, idx, now);
}

const nav_screen_t wifinet_screen = {
    .name = "wifinet", .enter = wifinet_enter, .resume = wifinet_resume,
    .exit = wifinet_exit, .tick = wifinet_tick, .input = wifinet_input,
    .render = render_wifinet, .chrome = NAV_CHROME_FULL,
};
