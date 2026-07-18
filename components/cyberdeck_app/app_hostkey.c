/*
 * app_hostkey.c — trust-on-first-use host-key prompt (ST_HOSTKEY).
 *
 * Two flavors: first contact (calm, Enter trusts) and a CHANGED pinned key
 * (possible MITM: hazard tape, default Cancel, modality-matched two-step
 * arming before REPLACE fires).
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "ssh_client.h"

#include <stdio.h>
#include <string.h>

/* Hostkey prompt buttons: two side-by-side tiles (trust/replace + cancel).
 * Slot 0 = trust/replace, slot 1 = cancel. */
static tilegrid_t hostkey_grid(void)
{
    tilegrid_t g = { .gx = 4, .gy = 0, .ncols = 2, .nrows = 1, .count = 2 };
    g.th = ui_rows() >= 28 ? 4 : 3;
    g.tw = (ui_cols() - 6) / 2;
    if (g.tw > 36) g.tw = 36;
    g.y0 = ui_rows() - 3 - g.th;               /* one blank row above the rule */
    g.x0 = (ui_cols() - (g.tw * 2 + g.gx)) / 2;
    return g;
}

static void render_hostkey(void)
{
    ui_colors(app.fp_mismatch ? COLOR_WHITE : UI_FG,
              app.fp_mismatch ? RGB565(96, 0, 0) : UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    const conn_profile_t *p = &app.active;
    const char *fp = ssh_client_get_fingerprint();

    if (app.fp_mismatch) {
        /* Scrolling ╱╱╲╲ hazard tape framing the screen: the one modal that
         * must not look like a calm dialog. The titlebar chip below draws
         * over the top run, so the tape flanks it on both sides. */
        ui_pen(OVERLAY_COL_AMBER);
        for (int x = 0; x < ui_cols(); x++) {
            uint16_t cp = (((x + app.anim_frame / 2) & 3) < 2) ? 0x2571 : 0x2572;
            ui_putch(x, 0, cp, 0);
            ui_putch(x, ui_rows() - 3, cp, 0);
        }
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    /* Standard screen chrome — the security modal was the only full screen
     * without a title or rule, which made it read as a glitch, not a page. */
    draw_titlebar(2, app.fp_mismatch ? "HOST KEY ALERT" : "NEW HOST KEY",
                  app.anim_frame);

    if (app.fp_mismatch) {
        /* Blink via INVERSE (ui_puts emits Latin-1 bytes — no UTF-8 here). */
        uint8_t blink = ((app.anim_frame / 5) & 1) ? OVERLAY_ATTR_INVERSE : 0;
        ui_puts(4, 5, "!  HOST KEY CHANGED - possible attack  !", blink);
        ui_puts(4, 7, "The server's key DIFFERS from the pinned one.", 0);
        ui_puts(4, 8, "Only replace it if you KNOW the server was rekeyed.", 0);
    } else {
        ui_puts(4, 5, "Unknown host - first connection", 0);
        ui_printf(4, 7, 0, "First connection to %s:%u.", p->host, (unsigned)p->port);
        ui_puts(4, 8, "Verify the fingerprint before trusting it.", 0);
    }

    /* SHA256 fingerprint in 4-hex groups inside a box, so it can be read
     * against `ssh-keygen -lf` output group by group. On entry the digits
     * decode out of braille noise left-to-right (~0.8 s), then hold. */
    int bx = (ui_cols() - 43) / 2;
    ui_box(bx, 10, 43, 4, " SHA256 ");
    uint32_t shown = (app.anim_frame - app.hostkey_frame0) * 8;
    for (int i = 0; i < 64; i++) {
        int x = bx + 2 + (i % 32) + (i % 32) / 4;   /* 1-cell group gaps */
        int y = 11 + i / 32;
        if ((uint32_t)i < shown) {
            ui_pen(OVERLAY_COL_CYAN);
            ui_putch(x, y, (uint8_t)fp[i], 0);
        } else {
            ui_pen(OVERLAY_COL_GREEN);
            ui_putch(x, y, braille_noise((uint32_t)i * 37u
                                         + app.anim_frame * 51u), 0);
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    tilegrid_t g = hostkey_grid();
    app.grid = g;
    const char *trust = app.fp_mismatch
        ? (app.hostkey_armed ? "TAP AGAIN to REPLACE" : "Replace key")
        : "Trust & Connect";
    ui_pen(app.fp_mismatch ? OVERLAY_COL_AMBER : OVERLAY_COL_GREEN);
    ui_tile(tile_x(&g, 0), tile_y(&g, 0), g.tw, g.th, trust,
            app.fp_mismatch ? "danger" : "",
            app.hostkey_sel == 0 || app.hostkey_armed);
    /* Cancel is safe navigation — BLUE, matching Back on the menu pages. */
    ui_pen(OVERLAY_COL_BLUE);
    ui_tile(tile_x(&g, 1), tile_y(&g, 1), g.tw, g.th, "Cancel", "",
            app.hostkey_sel == 1);
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_footer(app.fp_mismatch
                ? "arrows+Enter \xB7 Y replace \xB7 Esc cancel"
                : "arrows+Enter trust \xB7 Esc cancel");
    ui_no_cursor();
    ui_present();
}

/* Pin the server's current fingerprint and (re)connect. Called from the
 * hostkey prompt once the user has confirmed — a plain Enter for a first-seen
 * host, but only an explicit 'Y' for a CHANGED key (possible MITM). */
static void hostkey_trust_and_connect(uint64_t now)
{
    const conn_profile_t *p = &app.active;
    storage_known_host_set(p->host, p->port, ssh_client_get_fingerprint());
    snprintf(app.pinned_fp, sizeof(app.pinned_fp), "%s", ssh_client_get_fingerprint());
    app.connect_attempt = 0;             /* explicit user action, not a retry */
    app.connect_armed = true;
    app.connect_at    = now;
    app.state         = ST_CONNECTING;
    render_connecting("Connecting to", now);
}

/* Enter the prompt for the fingerprint ssh_client just reported. A first-
 * seen host defaults to Trust (Enter pins + connects); a CHANGED key
 * defaults to Cancel. */
void hostkey_open(bool mismatch)
{
    app.fp_mismatch     = mismatch;
    app.hostkey_armed   = false;
    app.hostkey_arm_src = 0;
    app.hostkey_sel     = mismatch ? 1 : 0;
    app.hostkey_frame0  = app.anim_frame;   /* start the decode reveal */
    app.next_anim       = 0;
    app.state = ST_HOSTKEY;
    render_hostkey();
}

void hostkey_tick(uint64_t now)
{
    /* Full animated screen like every other: titlebar spark, decode
     * reveal, and (on mismatch) the hazard tape all stay alive.
     * Stopping after the reveal froze the spark mid-shimmer. */
    if (now >= app.next_anim) {
        app.next_anim = now + ANIM_PERIOD_MS;
        render_hostkey();
    }
}

void hostkey_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now)
{
    if (ev->type == CYBERDECK_INPUT_TAP) {
        int slot = tile_hit(&app.grid, ev->x, ev->y);
        if (slot < 0) {                          /* tap outside: back down */
            if (app.hostkey_armed) {
                app.hostkey_armed   = false;
                app.hostkey_arm_src = 0;
                render_hostkey();
            }
        } else if (slot == 1) {                  /* Cancel tile */
            enter_home(now);
        } else {                                 /* Trust / Replace tile */
            app.hostkey_sel = 0;
            if (!app.fp_mismatch) {
                hostkey_trust_and_connect(now);
            } else if (app.hostkey_armed && app.hostkey_arm_src == 1) {
                hostkey_trust_and_connect(now);  /* 2nd tap fires */
            } else {
                /* First tap arms. Arming is modality-matched: a tap can
                 * only be fired by a second TAP — never by Enter — so an
                 * accidental tap + habitual Enter cannot re-pin. */
                app.hostkey_armed   = true;
                app.hostkey_arm_src = 1;
                render_hostkey();
            }
        }
        return;
    }
    if (k == K_LEFT || k == K_RIGHT || k == K_UP || k == K_DOWN) {
        int ns = tile_nav(&app.grid, app.hostkey_sel, k);
        bool redraw = ns != app.hostkey_sel;
        app.hostkey_sel = ns;
        if (app.hostkey_armed) {                 /* any arrow backs down */
            app.hostkey_armed   = false;
            app.hostkey_arm_src = 0;
            redraw = true;
        }
        if (redraw) render_hostkey();
        return;
    }
    if (k == K_ESC) { enter_home(now); return; }
    if (!app.fp_mismatch) {
        /* First contact (TOFU): Enter activates the selected tile
         * (default = Trust, so a single Enter still pins + connects). */
        if (k == K_ENTER) {
            if (app.hostkey_sel == 0) hostkey_trust_and_connect(now);
            else                      enter_home(now);
        }
    } else {
        /* The pinned key CHANGED — possible MITM. 'Y' always replaces.
         * Enter follows the selection (default = Cancel) with two-step
         * arming that only Enter itself can fire (modality-matched, see
         * the tap branch). Every other input backs the arming down. */
        if (k == K_CHAR && (ch == 'y' || ch == 'Y')) {
            hostkey_trust_and_connect(now);
        } else if (k == K_ENTER) {
            if (app.hostkey_sel != 0) {
                enter_home(now);
            } else if (app.hostkey_armed && app.hostkey_arm_src == 2) {
                hostkey_trust_and_connect(now);
            } else if (app.hostkey_armed) {      /* tap-armed: back down */
                app.hostkey_armed   = false;
                app.hostkey_arm_src = 0;
                render_hostkey();
            } else {
                app.hostkey_armed   = true;
                app.hostkey_arm_src = 2;
                render_hostkey();
            }
        } else if (app.hostkey_armed) {          /* anything else backs down */
            app.hostkey_armed   = false;
            app.hostkey_arm_src = 0;
            render_hostkey();
        }
    }
}
