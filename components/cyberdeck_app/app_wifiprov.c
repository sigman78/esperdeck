/*
 * app_wifiprov.c — SoftAP WiFi onboarding modal (ST_WIFIPROV): QR + numbered
 * steps for the ESP SoftAP Provisioning app, live status, ack hold.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "wifi_manager.h"
#include "wifi_provision.h"

#include <stdio.h>
#include <string.h>

#define PROV_ACK_HOLD_MS 2500   /* success hold so the phone reads the ack */

/* Full-screen SoftAP onboarding modal. */
static void render_wifiprov(uint64_t now)
{
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_titlebar(2, "WIFI SETUP");
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - 10, 0, "// SoftAP", 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    draw_rule(3);

    int st = wifi_provision_state();

    if (st == WIFI_PROV_ST_SUCCESS) {
        ui_pen(OVERLAY_COL_GREEN);
        ui_putch(4, 6, UI_LED_ON, 0);
        ui_printf(6, 6, 0, "Connected to '%s' - saved!", wifi_provision_ssid());
        ui_puts(6, 8, "returning home", 0);
        /* Departure bar: ✓s fill toward the moment we head home, so the
         * 2.5 s ack hold reads as a countdown instead of a freeze. */
        if (app.prov.done_at) {
            const int BW = 20;
            uint64_t left = app.prov.done_at > now ? app.prov.done_at - now : 0;
            int fill = BW - (int)(left * BW / PROV_ACK_HOLD_MS);
            for (int i = 0; i < fill && i < BW; i++)
                ui_putch(22 + i, 8, 0x2713, 0);
        }
        ui_pen(OVERLAY_COL_DEFAULT);
        return;
    }
    if (st == WIFI_PROV_ST_FAILED) {
        ui_pen(OVERLAY_COL_RED);
        ui_putch(4, 6, UI_DIAMOND, 0);
        ui_puts(6, 6, "failed - wrong password or network not found", 0);
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_puts(6, 8, "retry from the app, or tap/Esc to cancel", 0);
        return;
    }

    /* ACTIVE / RECEIVED — QR first (its column bounds the step text),
     * then the numbered onboarding steps stacked on the left. */
    int qx  = draw_qr_panel(wifi_provision_qr_size(), wifi_provision_qr_module,
                            "scan with app");
    bool qr = qx < ui_cols();
    bool wide = ui_cols() >= 97;

    int y = ui_rows() >= 28 ? 6 : 4;
    y = draw_step(y, '1',
                  qr ? (wide ? "Get the \"ESP SoftAP Provisioning\" app or scan ->"
                             : "Get the \"ESP SoftAP Prov\" app")
                     : "Get the \"ESP SoftAP Provisioning\" app",
                  NULL, 0, qx - 1);
    y = draw_step(y, '2', wide ? "Join this WiFi network:" : "Join this WiFi:",
                  wifi_provision_service_name(), OVERLAY_COL_GREEN, qx - 1);
    y = draw_step(y, '3', wide ? "Enter this proof code:" : "Proof code:",
                  wifi_provision_pop(), OVERLAY_COL_AMBER, qx - 1);
    y = draw_step(y, '4', wide ? "Pick your WiFi + password in the app."
                               : "Pick WiFi + password in app.",
                  NULL, 0, qx - 1);

    bool recv = (st == WIFI_PROV_ST_RECEIVED);
    ui_pen(recv ? OVERLAY_COL_AMBER : OVERLAY_COL_GREEN);
    ui_putch(4, y, spinner_glyph(app.anim_frame), 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_printf(6, y, 0, "%.*s", qx - 7,
              recv ? "credentials received - testing..."
                   : "waiting for the phone...");

    /* Live RAM readout — watch the internal-DRAM peak while the AP+httpd
     * run. Dropped when the short grid has no spare row above the footer. */
    if (y + 2 <= ui_rows() - 3) {
        char ram[48];
        ram_stats(ram, sizeof(ram));
        ui_pen(OVERLAY_COL_BLUE);
        ui_putch(4, y + 2, UI_DIAMOND, 0);
        ui_puts(6, y + 2, "RAM", OVERLAY_ATTR_BOLD);
        ui_puts(11, y + 2, ram, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    draw_footer(recv ? "testing - long-press or Esc to abort"
                     : "tap or Esc to cancel");
}

static void wifiprov_enter(intptr_t arg, uint64_t now)
{
    (void)arg; (void)now;
    app.prov.done_at = 0;
    app.next_anim    = 0;
}

void enter_wifiprov(uint64_t now)
{
    if (wifi_provision_start() != ESP_OK) {
        toast(now, "wifi setup unavailable");
        enter_home(now);
        return;
    }
    nav_push(SCR_WIFIPROV, 0, now);
}

static void wifiprov_tick(uint64_t now)
{
    if (wifi_provision_state() == WIFI_PROV_ST_SUCCESS) {
        if (app.prov.done_at == 0) {
            app.prov.done_at = now + PROV_ACK_HOLD_MS;  /* phone reads the ack */
            nav_invalidate();
        } else if (now < app.prov.done_at) {        /* keep the check-bar filling */
            if (now >= app.next_anim) {
                app.next_anim = now + ANIM_PERIOD_MS;
                nav_invalidate();
            }
        } else if (now >= app.prov.done_at) {
            char ssid[33];
            snprintf(ssid, sizeof(ssid), "%s", wifi_provision_ssid());
            wifi_provision_stop();
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
    /* Esc or any tap cancels — except while the phone's credentials are
     * being tested (RECEIVED): killing the AP then leaves the phone app
     * hanging mid-handshake, so a stray screen touch must not do it.
     * Esc / long-press remain as the deliberate abort. */
    int pst = wifi_provision_state();
    bool deliberate = (k == K_ESC ||
                       ev->type == CYBERDECK_INPUT_LONG_PRESS);
    bool tap = (ev->type == CYBERDECK_INPUT_TAP);
    if (pst != WIFI_PROV_ST_SUCCESS &&
        (deliberate || (tap && pst != WIFI_PROV_ST_RECEIVED))) {
        wifi_provision_stop();
        kick_wifi();                       /* un-park wifi_manager */
        toast(now, "wifi setup cancelled");
        enter_home(now);
    }
}

const nav_screen_t wifiprov_screen = {
    .name = "wifiprov", .enter = wifiprov_enter, .tick = wifiprov_tick,
    .input = wifiprov_input, .render = render_wifiprov,
    .chrome = NAV_CHROME_NONE,
};
