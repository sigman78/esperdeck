/*
 * app_sshimport.c — HTTP SSH-profile import modal (ST_SSHIMPORT), over the
 * deck's own SoftAP or the existing LAN (Web/PC).
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "ssh_import.h"

#include <stdio.h>
#include <string.h>

/* Full-screen HTTP SSH-profile import modal (SoftAP or Web transport). */
static void render_sshimport(uint64_t now)
{
    (void)now;
    bool web = (ssh_import_mode() == SSH_IMPORT_WEB);
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_titlebar(2, "SSH IMPORT");
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - 10, 0, web ? "// Web/PC" : "// SoftAP", 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    draw_rule(3);

    /* QR first — its column bounds the step text on narrow grids. */
    int qx = draw_qr_panel(ssh_import_qr_size(), ssh_import_qr_module,
                           web ? "scan to open" : "scan to join");
    bool wide = ui_cols() >= 97;
    int y = ui_rows() >= 28 ? 6 : 4;

    if (web) {
        /* On the existing LAN — the PC opens the deck's IP directly. */
        y = draw_step(y, '1', wide ? "On your PC browser, open:"
                                   : "Open on your PC:",
                      ssh_import_url(), OVERLAY_COL_WHITE, qx - 1);
        if (ssh_import_pop_required()) {
            y = draw_step(y, '2', wide ? "Type this proof code in the form:"
                                       : "Proof code:",
                          ssh_import_pop(), OVERLAY_COL_AMBER, qx - 1);
        } else {
            ui_pen(OVERLAY_COL_CYAN);
            ui_putch(4, y, '2', 0);
            ui_pen(OVERLAY_COL_AMBER);
            ui_printf(6, y, 0, "%.*s", qx - 7,
                      "Proof code OFF (dev build) - page is open.");
            ui_pen(OVERLAY_COL_DEFAULT);
            y += 2;
        }
        y = draw_step(y, '3', wide ? "Fill the form + Save. Repeat for more."
                                   : "Fill the form + Save.",
                      NULL, 0, qx - 1);
        /* Honest caveat: plain HTTP over the LAN (no WPA2 wrapping us). */
        ui_pen(OVERLAY_COL_AMBER);
        ui_putch(4, y, UI_DIAMOND, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_printf(6, y, 0, "%.*s", qx - 7,
                  wide ? "LAN only - use on a network you trust."
                       : "trusted LAN only.");
        y += 2;
    } else {
        /* SoftAP — the phone joins the deck's own WPA2 network. */
        y = draw_step(y, '1', wide ? "Join this WiFi network:"
                                   : "Join this WiFi:",
                      ssh_import_service_name(), OVERLAY_COL_GREEN, qx - 1);
        y = draw_step(y, '2', wide ? "WiFi password / proof code:"
                                   : "WiFi password:",
                      ssh_import_pop(), OVERLAY_COL_AMBER, qx - 1);
        y = draw_step(y, '3', wide ? "Open in a browser:" : "Browse to:",
                      ssh_import_url(), OVERLAY_COL_WHITE, qx - 1);
        y = draw_step(y, '4', wide ? "Fill the form + Save. Repeat for more."
                                   : "Fill the form + Save.",
                      NULL, 0, qx - 1);
    }

    /* Status: running import/delete tally + last name, or a waiting spinner. */
    int cnt = ssh_import_count();
    int del = ssh_import_deleted();
    const char *err = ssh_import_err();
    char stat[96];
    if (err && err[0]) {
        snprintf(stat, sizeof(stat), "rejected: %s", err);
        ui_pen(OVERLAY_COL_RED);
        ui_putch(4, y, UI_DIAMOND, 0);
    } else if (cnt > 0 || del > 0) {
        if (del > 0 && cnt > 0)
            snprintf(stat, sizeof(stat), "last: '%s'  (%d saved, %d removed)",
                     app.imp.last, cnt, del);
        else if (del > 0)
            snprintf(stat, sizeof(stat), "removed '%s'  (%d removed)",
                     app.imp.last, del);
        else
            snprintf(stat, sizeof(stat), "imported '%s'  (%d saved)",
                     app.imp.last, cnt);
        ui_pen(OVERLAY_COL_GREEN);
        ui_putch(4, y, UI_LED_ON, 0);
    } else {
        snprintf(stat, sizeof(stat), "waiting for a browser...");
        ui_pen(OVERLAY_COL_GREEN);
        ui_putch(4, y, spinner_glyph(app.anim_frame), 0);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_printf(6, y, 0, "%.*s", qx - 7, stat);

    /* Live RAM readout — dropped when the short grid has no spare row. */
    if (y + 2 <= ui_rows() - 3) {
        char ram[48];
        ram_stats(ram, sizeof(ram));
        ui_pen(OVERLAY_COL_BLUE);
        ui_putch(4, y + 2, UI_DIAMOND, 0);
        ui_puts(6, y + 2, "RAM", OVERLAY_ATTR_BOLD);
        ui_puts(11, y + 2, ram, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    draw_footer(cnt > 0 || del > 0 ? "tap or Esc when done - changes are saved"
                                   : "tap or Esc to cancel");
}

static void sshimport_enter(intptr_t arg, uint64_t now)
{
    (void)arg; (void)now;
    app.imp.seen    = 0;
    app.imp.last[0] = '\0';
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
    nav_push(SCR_SSHIMPORT, (intptr_t)mode, now);
}

/* Tear down the import server, refresh the profile list, go home. Only SoftAP
 * parked wifi_manager, so only that mode needs to un-park it. */
static void exit_sshimport(uint64_t now)
{
    int cnt = ssh_import_count();
    int del = ssh_import_deleted();
    bool softap = (ssh_import_mode() == SSH_IMPORT_SOFTAP);
    ssh_import_stop();
    if (softap) kick_wifi();   /* resume STA auto-reconnect (web never parked) */
    load_profiles();           /* surface freshly imported profiles on HOME */
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
    if (ssh_import_count() + ssh_import_deleted() != app.imp.seen) {
        app.imp.seen = ssh_import_count() + ssh_import_deleted();
        snprintf(app.imp.last, sizeof(app.imp.last), "%s", ssh_import_last());
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
    /* Esc / tap / long-press finishes the session (any imports are already
     * on flash). There is no destructive in-flight state to protect. */
    if (k == K_ESC || ev->type == CYBERDECK_INPUT_TAP ||
        ev->type == CYBERDECK_INPUT_LONG_PRESS) {
        exit_sshimport(now);
    }
}

const nav_screen_t sshimport_screen = {
    .name = "sshimport", .enter = sshimport_enter, .tick = sshimport_tick,
    .input = sshimport_input, .render = render_sshimport,
    .chrome = NAV_CHROME_NONE,
};
