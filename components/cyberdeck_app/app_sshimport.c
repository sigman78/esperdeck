/*
 * app_sshimport.c — the host import page (SCR_SSHIMPORT).
 * Steps and a QR code lead a browser to the deck's form. It runs over
 * the deck's SoftAP or the LAN.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "ssh_import.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app_sshimport";

static struct {
    int      seen;                  /* count already acknowledged on screen */
    char     last[32];              /* snapshot of the last imported name   */
    bool     running;               /* the import server is up              */
} s_imp;

static void render_sshimport(uint64_t now)
{
    (void)now;
    const int faults = ui_faults();
    ui_slate_t pg = ui_slate_page();
    const bool web = ssh_import_mode() == SSH_IMPORT_WEB;
    char spin[8], state[96];

    ui_title(pg.header, "Hosts", web ? "Import from PC" : "Import from phone",
             NULL, true);
    ui_utf8_put(spin, spinner_glyph(app.anim_frame));

    const ui_step_t web_steps[4] = {
        { .mark = '1', .text = "Open on your PC:", .value = ssh_import_url(),
          .pen = OVERLAY_COL_WHITE },
        ssh_import_pop_required()
            ? (ui_step_t){ .mark = '2', .text = "Proof code:",
                           .value = ssh_import_pop(),
                           .pen = OVERLAY_COL_AMBER }
            : (ui_step_t){ .mark = '2', .pen = OVERLAY_COL_AMBER,
                           .text = "Proof code is off in this build. "
                                   "The page is open." },
        { .mark = '3', .text = "Fill the form and save." },
        /* Plain HTTP over the LAN: nothing wraps the page in WPA2. */
        { .mark = '!', .text = "Plain HTTP. Use it on a network you trust.",
          .pen = OVERLAY_COL_AMBER },
    };
    const ui_step_t softap_steps[4] = {
        { .mark = '1', .text = "Join this Wi-Fi:",
          .value = ssh_import_service_name(), .pen = OVERLAY_COL_GREEN },
        { .mark = '2', .text = "Wi-Fi password:", .value = ssh_import_pop(),
          .pen = OVERLAY_COL_AMBER },
        { .mark = '3', .text = "Open in a browser:", .value = ssh_import_url(),
          .pen = OVERLAY_COL_WHITE },
        { .mark = '4', .text = "Fill the form and save. Repeat for more." },
    };
    ui_onboard_t o = {
        .steps = web ? web_steps : softap_steps, .nsteps = 4,
        .qr_size = ssh_import_qr_size(), .qr_mod = ssh_import_qr_module,
        .qr_caption = web ? "scan to open" : "scan to join",
        .mark = UI_S_CHECK, .pen = OVERLAY_COL_GREEN, .state = state,
    };

    /* Status: the running tally and the last name, or a waiting spinner. */
    const int cnt = ssh_import_count();
    const int del = ssh_import_deleted();
    const char *err = ssh_import_err();
    if (err && err[0]) {
        o.mark = UI_S_CROSS;
        o.pen = OVERLAY_COL_RED;
        snprintf(state, sizeof state, "Rejected: %s", err);
    } else if (cnt > 0 && del > 0) {
        snprintf(state, sizeof state, "Last: %s (%d saved, %d removed)",
                 s_imp.last, cnt, del);
    } else if (del > 0) {
        snprintf(state, sizeof state, "Removed %s (%d removed)", s_imp.last, del);
    } else if (cnt > 0) {
        snprintf(state, sizeof state, "Imported %s (%d saved)", s_imp.last, cnt);
    } else {
        o.mark = spin;
        snprintf(state, sizeof state, "Waiting for a browser");
    }
    ui_onboard(pg.body, &o);

    if (ui_faults() != faults)
        ESP_LOGW(TAG, "layout faults +%d", ui_faults() - faults);
}

static void sshimport_enter(intptr_t arg, uint64_t now)
{
    (void)arg; (void)now;
    s_imp.seen    = 0;
    s_imp.last[0] = '\0';
    app.next_anim   = 0;
}

void enter_sshimport(uint64_t now, ssh_import_mode_t mode)
{
    esp_err_t e = ssh_import_start(mode);
    if (e != ESP_OK) {
        if (mode == SSH_IMPORT_WEB && e == ESP_ERR_INVALID_STATE)
            toast(now, "connect WiFi first");
        else
            toast(now, "import unavailable");
        enter_home(now);
        return;
    }
    s_imp.running = true;
    nav_push(SCR_SSHIMPORT, (intptr_t)mode, now);
}

/* Tear down the import server and surface the imported profiles. Only
 * SoftAP parked wifi_manager, so only that mode needs to un-park it. */
static void imp_stop(void)
{
    if (!s_imp.running) return;
    s_imp.running = false;
    bool softap = (ssh_import_mode() == SSH_IMPORT_SOFTAP);
    ssh_import_stop();
    if (softap) kick_wifi();   /* resume STA auto-reconnect (web never parked) */
    load_profiles();
}

/* Any other way off the page (the idle lock) stops the server too. */
static void sshimport_exit(uint64_t now)
{
    (void)now;
    imp_stop();
}

/* Stop the server, report what it did, go home. */
static void exit_sshimport(uint64_t now)
{
    int cnt = ssh_import_count();
    int del = ssh_import_deleted();
    imp_stop();
    if (cnt > 0 && del > 0) toast(now, "%d imported, %d removed", cnt, del);
    else if (cnt > 0)       toast(now, "imported %d profile(s)", cnt);
    else if (del > 0)       toast(now, "removed %d profile(s)", del);
    else                    toast(now, "import cancelled");
    enter_home(now);
}

static void sshimport_tick(uint64_t now)
{
    /* A submission lands on the httpd task; re-render on its activity bump
     * so the confirmation appears at once. Snapshot the name via the locked
     * getter once per bump so the render never samples a half-write. */
    if (ssh_import_count() + ssh_import_deleted() != s_imp.seen) {
        s_imp.seen = ssh_import_count() + ssh_import_deleted();
        snprintf(s_imp.last, sizeof(s_imp.last), "%s", ssh_import_last());
        app.next_anim = now + ANIM_PERIOD_MS;
        nav_invalidate();
    } else if (now >= app.next_anim) {
        app.next_anim = now + ANIM_PERIOD_MS;
        nav_invalidate();
    }
}

static void sshimport_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                            uint64_t now)
{
    (void)ch;
    /* Esc or the header back finishes the session. Any imports are already
     * on flash, so no in-flight state needs protecting. */
    const ui_act_t a = app_hot_input(ev, k);
    if (a.kind != UI_ACT_NONE) nav_invalidate();
    if (k == K_ESC || (a.kind == UI_ACT_PRESS && a.id == UI_ID_BACK))
        exit_sshimport(now);
}

const nav_screen_t sshimport_screen = {
    .name = "sshimport", .enter = sshimport_enter, .exit = sshimport_exit,
    .tick = sshimport_tick,
    .input = sshimport_input, .render = render_sshimport,
    .chrome = NAV_CHROME_FULL,
};
