/*
 * app_wifiprov.c — the SoftAP onboarding page (SCR_WIFIPROV).
 * Steps and a QR code guide the phone app. A short hold shows the result.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "wifi_manager.h"
#include "wifi_provision.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app_wifiprov";

static struct {
    uint64_t done_at;               /* finish after CRED_SUCCESS (0 = unset) */
    bool     running;               /* the provisioning service is up        */
} s_prov;

static void prov_stop(void)
{
    s_prov.running = false;
    wifi_provision_stop();
}

#define PROV_ACK_HOLD_MS 2500   /* success hold so the phone reads the ack */

static void render_wifiprov(uint64_t now)
{
    const int faults = ui_faults();
    ui_slate_t pg = ui_slate_page();
    const int st = wifi_provision_state();
    char spin[8], joined[UI_STEP_TEXT], hold[32];

    ui_title(pg.header, "Wi-Fi", "Add network (phone)", NULL, true);
    ui_utf8_put(spin, spinner_glyph(app.anim_frame));
    const ui_step_t steps[4] = {
        { .mark = '1', .text = "Get the \"ESP SoftAP Provisioning\" app" },
        { .mark = '2', .text = "Join this Wi-Fi:",
          .value = wifi_provision_service_name(), .pen = OVERLAY_COL_GREEN },
        { .mark = '3', .text = "Proof code:", .value = wifi_provision_pop(),
          .pen = OVERLAY_COL_AMBER },
        { .mark = '4', .text = "Pick your Wi-Fi and password in the app." },
    };
    ui_onboard_t o = {
        .steps = steps, .nsteps = 4,
        .qr_size = wifi_provision_qr_size(),
        .qr_mod = wifi_provision_qr_module, .qr_caption = "scan with the app",
        .mark = spin, .pen = OVERLAY_COL_GREEN,
        .state = "Waiting for the phone",
    };
    if (st == WIFI_PROV_ST_SUCCESS) {
        /* The result card replaces the steps while the phone reads the ack. */
        uint64_t left_ms = PROV_ACK_HOLD_MS;
        if (s_prov.done_at) left_ms = s_prov.done_at > now ? s_prov.done_at - now : 0;
        unsigned secs = (unsigned)(left_ms / 1000);
        snprintf(joined, sizeof joined, "Joined %s. Saved.", wifi_provision_ssid());
        snprintf(hold, sizeof hold, "Back to HOME in %u s", secs ? secs : 1u);
        o.nsteps = 0;
        o.qr_size = 0;
        o.mark = UI_S_CHECK;
        o.state = joined;
        o.state2 = hold;
    } else if (st == WIFI_PROV_ST_FAILED) {
        o.mark = UI_S_CROSS;
        o.pen = OVERLAY_COL_RED;
        o.state = "Wrong password, or the network was not found.";
        o.state2 = "Try again from the app. Back cancels.";
    } else if (st == WIFI_PROV_ST_RECEIVED) {
        o.pen = OVERLAY_COL_AMBER;
        o.state = "Testing the network";
    }
    ui_onboard(pg.body, &o);

    if (ui_faults() != faults)
        ESP_LOGW(TAG, "layout faults +%d", ui_faults() - faults);
}

static void wifiprov_enter(intptr_t arg, uint64_t now)
{
    (void)arg; (void)now;
    s_prov.done_at = 0;
    app.next_anim    = 0;
}

void enter_wifiprov(uint64_t now)
{
    if (wifi_provision_start() != ESP_OK) {
        toast(now, "wifi setup unavailable");
        enter_home(now);
        return;
    }
    s_prov.running = true;
    nav_push(SCR_WIFIPROV, 0, now);
}

static void wifiprov_tick(uint64_t now)
{
    if (wifi_provision_state() == WIFI_PROV_ST_SUCCESS) {
        if (s_prov.done_at == 0) {
            s_prov.done_at = now + PROV_ACK_HOLD_MS;  /* phone reads the ack */
            nav_invalidate();
        } else if (now < s_prov.done_at) {        /* keep the countdown moving */
            if (now >= app.next_anim) {
                app.next_anim = now + ANIM_PERIOD_MS;
                nav_invalidate();
            }
        } else if (now >= s_prov.done_at) {
            char ssid[33];
            snprintf(ssid, sizeof(ssid), "%s", wifi_provision_ssid());
            prov_stop();
            /* The provisioning manager already associated + got an IP;
             * adopt that link instead of forcing a redundant reconnect
             * (which produced no GOT_IP and hung at "connecting..."). */
            wifi_profile_t nets[STORAGE_WIFI_MAX];
            int n = 0;
            storage_wifi_load(nets, &n, STORAGE_WIFI_MAX);
            if (n > 0) wifi_manager_adopt(nets, n, ssid);
            else       kick_wifi();
            toast(now, "wifi '%.18s' connected", ssid);
            enter_home(now);
        }
    } else if (now >= app.next_anim) {
        app.next_anim = now + ANIM_PERIOD_MS;
        nav_invalidate();
    }
}

static void wifiprov_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                           uint64_t now)
{
    (void)ch;
    /* Esc or the header back cancels. Testing the credentials is the
     * exception: a stray tap must not kill the AP under the phone app.
     * Esc and a hold on the bar still cancel. */
    const int pst = wifi_provision_state();
    const ui_act_t a = app_hot_input(ev, k);
    if (a.kind != UI_ACT_NONE) nav_invalidate();
    if (pst == WIFI_PROV_ST_SUCCESS) return;
    const bool deliberate = k == K_ESC ||
        (a.kind == UI_ACT_HOLD && a.id == UI_ID_BACK);
    const bool tap = a.kind == UI_ACT_PRESS && a.id == UI_ID_BACK;
    if (deliberate || (tap && pst != WIFI_PROV_ST_RECEIVED)) {
        prov_stop();
        kick_wifi();                       /* un-park wifi_manager */
        toast(now, "wifi setup cancelled");
        enter_home(now);
    }
}

/* Any other way off the page (the idle lock) stops the service too. */
static void wifiprov_exit(uint64_t now)
{
    (void)now;
    if (!s_prov.running) return;
    prov_stop();
    kick_wifi();                           /* un-park wifi_manager */
}

const nav_screen_t wifiprov_screen = {
    .name = "wifiprov", .enter = wifiprov_enter, .exit = wifiprov_exit,
    .tick = wifiprov_tick,
    .input = wifiprov_input, .render = render_wifiprov,
    .chrome = NAV_CHROME_FULL,
};
