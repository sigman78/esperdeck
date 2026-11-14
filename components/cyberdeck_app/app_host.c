/*
 * app_host.c — the host page (SCR_HOST): what a hold on a HOME tile opens.
 * Facts of one profile, then Connect, Edit, Delete and the two moves.
 * Delete asks first through the question page (app_ask.c).
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "keystore.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

enum { ID_CONNECT = 1, ID_EDIT, ID_DELETE, ID_EARLIER, ID_LATER };

static const char *TAG = "app_host";

static struct {
    int     idx;            /* profile shown; follows it through a move */
    ui_id_t saved_focus;    /* focus saved when another screen opens      */
} s_host;

static bool host_valid(void)
{
    return s_host.idx >= 0 && s_host.idx < app.profile_count;
}

/* "SHA256:abcd..wxyz" from the stored hex digest. */
static void short_print(const char *hex, char *out, size_t sz)
{
    const size_t n = strlen(hex);
    if (n > 8) snprintf(out, sz, "SHA256:%.4s..%.4s", hex, hex + n - 4);
    else       snprintf(out, sz, "SHA256:%.8s", hex);
}

static void render_host(uint64_t now)
{
    (void)now;
    const int faults = ui_faults();
    ui_slate_t pg = ui_slate_page();
    if (!host_valid()) return;
    const conn_profile_t *p = &app.profiles[s_host.idx];
    const bool stored = s_host.idx < app.stored_count;

    char addr[96], sign[64], pin[32], hex[128];
    snprintf(addr, sizeof addr, "%s  port %u", p->host, (unsigned)p->port);
    if (p->auth == STORAGE_AUTH_KEY)
        snprintf(sign, sizeof sign, "key %s", p->key_id);
    else
        /* A locked store holds the password; the list has no copy of it. */
        snprintf(sign, sizeof sign, "%s",
                 p->password[0] ? "password, stored"
                 : keystore_state() == KEYSTORE_LOCKED ? "password, locked"
                 : "password, none");
    const bool pinned = storage_known_host_get(p->host, p->port, hex,
                                               sizeof hex) == ESP_OK;
    if (pinned) short_print(hex, pin, sizeof pin);

    const ui_info_row_t rows[4] = {
        { .label = "Address", .value = addr },
        { .label = "User", .value = p->user },
        { .label = "Sign-in", .value = sign },
        pinned ? (ui_info_row_t){ .label = "Host key", .value = pin,
                                  .tail = UI_S_CHECK " pinned",
                                  .tail_pen = OVERLAY_COL_GREEN }
               : (ui_info_row_t){ .label = "Host key",
                                  .value = "not pinned yet" },
    };

    ui_focus_default(ID_CONNECT);
    ui_title(pg.header, "Hosts", p->name, NULL, true);
    ui_info(ui_cut_top(&pg.body, 6), rows, 4, prof_accent(p->name));
    ui_push(ui_cut_top(&pg.body, 3), ID_CONNECT, "Connect", UI_TONE_SOLID,
            OVERLAY_COL_GREEN, 0);
    const ui_grid_t g = ui_grid(pg.body, 2, 3, pg.m.gutter, 0);
    const uint8_t off = stored ? 0 : UI_PART_OFF;
    ui_push(ui_grid_cell(&g, 0), ID_EDIT, "Edit", UI_TONE_CARD, 0, off);
    ui_push(ui_grid_cell(&g, 1), ID_DELETE, "Delete", UI_TONE_TINT,
            OVERLAY_COL_RED, off);
    ui_push(ui_grid_cell(&g, 2), ID_EARLIER, UI_S_UP " Move earlier",
            UI_TONE_CARD, 0, off | (s_host.idx <= 0 ? UI_PART_OFF : 0));
    ui_push(ui_grid_cell(&g, 3), ID_LATER, UI_S_DOWN " Move later",
            UI_TONE_CARD, 0,
            off | (s_host.idx >= app.stored_count - 1 ? UI_PART_OFF : 0));

    if (ui_faults() != faults)
        ESP_LOGW(TAG, "layout faults +%d", ui_faults() - faults);
}

static void host_enter(intptr_t arg, uint64_t now)
{
    s_host.idx = (int)arg;
    s_host.saved_focus = 0;
    (void)now;
}

/* Back from the editor or the question: keep the focus where it was. */
static void host_resume(intptr_t arg, uint64_t now)
{
    (void)arg; (void)now;
    if (s_host.saved_focus) ui_focus_set(s_host.saved_focus);
}

static void host_exit(uint64_t now)
{
    (void)now;
    s_host.saved_focus = ui_focus_get();
}

static void host_tick(uint64_t now)
{
    if (app.toast[0] && now >= app.toast_until) app.toast[0] = '\0';
    if (!host_valid()) enter_home(now);     /* the entry is gone */
}

/* Swap the profile with its neighbour in storage; the page follows it. */
static void host_move(int dir, uint64_t now)
{
    const int a = s_host.idx, b = a + dir;
    if (a < 0 || b < 0 || a >= app.stored_count || b >= app.stored_count)
        return;
    /* A locked store wiped the passwords from the list. Do not save it. */
    if (keystore_state() == KEYSTORE_LOCKED) {
        toast(now, "unlock the deck first");
        nav_invalidate();
        return;
    }
    const conn_profile_t t = app.profiles[a];
    app.profiles[a] = app.profiles[b];
    app.profiles[b] = t;
    if (storage_save_profiles(app.profiles, app.stored_count) == ESP_OK) {
        s_host.idx = b;
    } else {
        toast(now, "save failed");
    }
    load_profiles();
    /* A button that went off cannot keep the focus: point at its twin. */
    if (s_host.idx == 0 && dir < 0) ui_focus_set(ID_LATER);
    if (s_host.idx == app.stored_count - 1 && dir > 0)
        ui_focus_set(ID_EARLIER);
    nav_invalidate();
}

/* Delete the stored profile at @p idx, plus its key files and TOFU
 * host pin. It deletes the files only if no other profile still
 * references them. @p keep_pin leaves the pin in place. This never
 * touches a live session, which runs on the connect module's snapshot
 * instead. */
void profile_delete_at(int idx, bool keep_pin)
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

    /* Drop the host's pin too when no surviving profile targets it; a
     * re-added profile then gets a fresh TOFU prompt. */
    bool host_shared = false;
    for (int i = 0; i < n; i++)
        if (set[i].port == doomed.port &&
            strcmp(set[i].host, doomed.host) == 0) { host_shared = true; break; }
    if (!host_shared && !keep_pin)
        storage_known_host_delete(doomed.host, doomed.port);

    load_profiles();
}

static void host_delete_yes(intptr_t arg, uint64_t now)
{
    profile_delete_at((int)arg, true);      /* the pinned host key stays */
    toast(now, "profile deleted");
    enter_home(now);
}

static void host_ask_delete(uint64_t now)
{
    char title[64];
    snprintf(title, sizeof title, "Delete %s?", app.profiles[s_host.idx].name);
    ask_open(&(ask_t){
        .title = title,
        .line1 = "The entry and its stored password go.",
        .line2 = "The pinned host key stays.",
        .verb = "Delete", .accent = OVERLAY_COL_RED,
        .yes = host_delete_yes, .arg = s_host.idx,
    }, now);
}

static void host_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
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
    if (a.kind != UI_ACT_PRESS) return;
    switch (a.id) {
    case UI_ID_BACK:  nav_pop(now);                       break;
    case ID_CONNECT:  connect_checked(s_host.idx, now);   break;
    case ID_EDIT:     enter_profile(now, s_host.idx);     break;
    case ID_DELETE:   host_ask_delete(now);               break;
    case ID_EARLIER:  host_move(-1, now);                 break;
    case ID_LATER:    host_move(1, now);                  break;
    default:          break;
    }
}

void enter_host(uint64_t now, int idx)
{
    nav_push(SCR_HOST, idx, now);
}

const nav_screen_t host_screen = {
    .name = "host", .enter = host_enter, .resume = host_resume,
    .exit = host_exit, .tick = host_tick, .input = host_input,
    .render = render_host, .chrome = NAV_CHROME_FULL,
};
