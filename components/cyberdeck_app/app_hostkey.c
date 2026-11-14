/*
 * app_hostkey.c - the host-key page (SCR_HOSTKEY). First contact: Trust
 * and connect holds the focus. A changed pinned key: Cancel holds it, and
 * Replace key asks on the question page before it pins anything.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "ssh_client.h"
#include "ui_pure.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

enum { ID_TRUST = 1, ID_CANCEL, ID_REPLACE };

static const char *TAG = "app_hostkey";

static struct {
    bool    mismatch;               /* pinned key CHANGED (vs first contact) */
    ui_id_t saved_focus;            /* focus kept while the question is open */
} s_hostkey;

static void render_hostkey(uint64_t now)
{
    (void)now;
    const int faults = ui_faults();
    ui_slate_t pg = ui_slate_page();
    const conn_profile_t *p = conn_active();
    const char *fp = ssh_client_get_fingerprint();
    const bool changed = s_hostkey.mismatch;
    char l[3][128];
    int n;

    ui_title(pg.header, NULL, changed ? "Host key changed" : "New host key",
             NULL, true);
    if (changed) {
        snprintf(l[0], sizeof l[0], "The key of %s differs from the pinned one.",
                 p->host);
        snprintf(l[1], sizeof l[1],
                 "Replace it only if you know the server got a new key.");
        n = 2;
    } else {
        snprintf(l[0], sizeof l[0], "First connection to %s,", p->name);
        snprintf(l[1], sizeof l[1], "%s port %u.", p->host, (unsigned)p->port);
        snprintf(l[2], sizeof l[2], "Compare the fingerprint before you trust it.");
        n = 3;
    }
    const ui_rect_t card = ui_cut_top(&pg.body, n + 2);
    ui_panel(card, changed ? OVERLAY_COL_RED : 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    for (int i = 0; i < n; i++)
        ui_text(ui_rect(card.x + 2, card.y + 1 + i, card.w - 4, 1), l[i],
                i == n - 1 ? UI_PANEL_MUTED : UI_PANEL, UI_LEFT);

    /* The hex digest in two rows of eight groups, as ssh-keygen -lf reads. */
    const ui_rect_t box = ui_cut_top(&pg.body, 5);
    const int fl = (int)strlen(fp);
    ui_panel(box, 0);
    ui_pen(OVERLAY_COL_CYAN);
    ui_text(ui_rect(box.x + 2, box.y + 1, box.w - 4, 1), "SHA256",
            UI_PANEL_MUTED, UI_LEFT);
    ui_pen(OVERLAY_COL_WHITE);
    for (int row = 0; row < 2; row++) {
        char groups[48];
        ui_group4(fp + (fl > row * 32 ? row * 32 : fl), 32, groups,
                  sizeof groups);
        ui_text(ui_rect(box.x + 2, box.y + 2 + row, box.w - 4, 1), groups,
                UI_PANEL | UI_BOLD, UI_LEFT);
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    ui_focus_default(changed ? ID_CANCEL : ID_TRUST);
    ui_flow_t f = ui_flow(pg.body, pg.m.gutter, 3);
    if (changed) {
        ui_push(ui_flow_next(&f, ui_push_w("Cancel")), ID_CANCEL, "Cancel",
                UI_TONE_CARD, 0, 0);
        ui_push(ui_flow_next(&f, ui_push_w("Replace key")), ID_REPLACE,
                "Replace key", UI_TONE_TINT, OVERLAY_COL_RED, 0);
    } else {
        ui_push(ui_flow_next(&f, ui_push_w("Trust and connect")), ID_TRUST,
                "Trust and connect", UI_TONE_SOLID, OVERLAY_COL_GREEN, 0);
        ui_push(ui_flow_next(&f, ui_push_w("Cancel")), ID_CANCEL, "Cancel",
                UI_TONE_CARD, 0, 0);
    }

    if (ui_faults() != faults)
        ESP_LOGW(TAG, "layout faults +%d", ui_faults() - faults);
}

/* Pin the server's current fingerprint and (re)connect. */
static void hostkey_trust_and_connect(uint64_t now)
{
    const conn_profile_t *p = conn_active();
    const char *fp = ssh_client_get_fingerprint();
    storage_known_host_set(p->host, p->port, fp);
    connect_arm_pinned(fp, now);
}

/* The question page calls this after it popped itself. */
static void hostkey_replace_yes(intptr_t arg, uint64_t now)
{
    (void)arg;
    hostkey_trust_and_connect(now);
}

static void hostkey_ask_replace(uint64_t now)
{
    ask_open(&(ask_t){
        .title = "Replace the pinned key?",
        .line1 = "A changed key can mean an attack.",
        .line2 = "The old pin is lost.",
        .verb = "Replace", .accent = OVERLAY_COL_RED,
        .yes = hostkey_replace_yes,
    }, now);
}

static void hostkey_enter(intptr_t arg, uint64_t now)
{
    (void)now;
    s_hostkey.mismatch    = arg != 0;
    s_hostkey.saved_focus = 0;
}

/* Back from the question page: keep the focus where it was. */
static void hostkey_resume(intptr_t arg, uint64_t now)
{
    (void)arg; (void)now;
    if (s_hostkey.saved_focus) ui_focus_set(s_hostkey.saved_focus);
}

static void hostkey_exit(uint64_t now)
{
    (void)now;
    s_hostkey.saved_focus = ui_focus_get();
}

/* Nothing animates here. A toast that expires needs one more frame. */
static void hostkey_tick(uint64_t now)
{
    if (app.toast[0] && now >= app.toast_until) {
        app.toast[0] = '\0';
        nav_invalidate();
    }
}

void hostkey_open(bool mismatch, uint64_t now)
{
    nav_replace(SCR_HOSTKEY, mismatch, now);
}

static void hostkey_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                          uint64_t now)
{
    (void)ch;
    if (k == K_ESC) {
        enter_home(now);
        return;
    }
    const ui_act_t a = app_hot_input(ev, k);
    if (a.kind == UI_ACT_NONE) return;
    nav_invalidate();
    if (a.kind != UI_ACT_PRESS) return;
    switch (a.id) {
    case UI_ID_BACK:
    case ID_CANCEL:   enter_home(now);                  break;
    case ID_TRUST:    hostkey_trust_and_connect(now);   break;
    case ID_REPLACE:  hostkey_ask_replace(now);         break;
    default:          break;
    }
}

const nav_screen_t hostkey_screen = {
    .name = "hostkey", .enter = hostkey_enter, .resume = hostkey_resume,
    .exit = hostkey_exit, .tick = hostkey_tick, .input = hostkey_input,
    .render = render_hostkey, .chrome = NAV_CHROME_FULL,
};
