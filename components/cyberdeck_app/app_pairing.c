/*
 * app_pairing.c — BLE keyboard pairing modal (ST_PAIRING).
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "ssh_client.h"

#include <string.h>

#define PAIR_TIMEOUT_MS  30000
#define PAIR_POLL_MS     250

/* Device tiles on the page; the last two slots are Forget bonds + Cancel. */
static int pairing_ndev(const tilegrid_t *g)
{
    return g->count - 2;
}

static void render_pairing(uint64_t now)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_titlebar(2, "PAIR KEYBOARD", app.anim_frame);
    ui_pen(app.pair.ndevs ? OVERLAY_COL_GREEN : OVERLAY_COL_AMBER);
    ui_putch(2, 1, app.pair.ndevs ? UI_LED_ON : spinner_glyph(app.anim_frame), 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    if (app.pair.ndevs)
        ui_printf(4, 1, 0, "%d found - select your keyboard", app.pair.ndevs);
    else
        ui_puts(4, 1, "scanning for keyboards...", 0);

    /* The scan self-dismisses after PAIR_TIMEOUT_MS of inactivity; give the
     * last 10 s a visible countdown instead of vanishing without warning. */
    uint64_t idle = now - app.pair.last_activity;
    if (idle > PAIR_TIMEOUT_MS - 10000) {
        uint32_t left = (uint32_t)((PAIR_TIMEOUT_MS - idle + 999) / 1000);
        ui_pen(OVERLAY_COL_AMBER);
        ui_printf(ui_cols() - 15, 1, 0, "closing in %2us", left);
        ui_pen(OVERLAY_COL_DEFAULT);
    }
    draw_rule_scan(3, app.anim_frame);

    /* Devices, then "Forget bonds" and Cancel (always the last two).
     * Cap devices so both special tiles fit on the page. */
    tilegrid_t g = picker_grid(0);
    int cap  = g.ncols * g.nrows;
    int ndev = app.pair.ndevs > cap - 2 ? cap - 2 : app.pair.ndevs;
    g.count  = ndev + 2;
    app.grid = g;
    if (app.pair.sel >= g.count) app.pair.sel = g.count - 1;

    if (ndev == 0) {
        /* Empty-scan theater: a cyan radar beam sweeps the tile field with
         * faint braille "contacts" blipping behind it — 30 s of scan reads
         * as a search, not a hang. */
        int y0 = 4, y1 = ui_rows() - 3;
        int bx = (int)((app.anim_frame * 2u) % (uint32_t)ui_cols());
        for (int y = y0; y <= y1; y++) {
            ui_pen(OVERLAY_COL_BLUE);
            uint32_t h = (uint32_t)y * 41u + (app.anim_frame >> 2) * 13u;
            if ((h & 7) == 0)
                ui_putch((int)(h % (uint32_t)ui_cols()), y,
                         braille_noise(h), 0);
            ui_pen(OVERLAY_COL_CYAN);
            ui_putch(bx, y, UI_SHADE3, 0);
            if (bx >= 1) ui_putch(bx - 1, y, UI_SHADE2, 0);
            if (bx >= 2) ui_putch(bx - 2, y, UI_SHADE1, 0);
        }
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    ui_pen(OVERLAY_COL_GREEN);
    for (int i = 0; i < ndev; i++) {
        ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th,
                app.pair.devs[i].name,
                app.pair.devs[i].addr_type ? "random addr" : "public addr",
                i == app.pair.sel);
    }
    /* Color law: RED = destructive (Forget), BLUE = safe navigation. */
    ui_pen(OVERLAY_COL_RED);
    ui_tile(tile_x(&g, ndev), tile_y(&g, ndev), g.tw, g.th,
            app.pair.forget_armed ? "TAP AGAIN to forget" : "Forget bonds",
            "clear + re-pair", ndev == app.pair.sel);
    ui_pen(OVERLAY_COL_BLUE);
    ui_tile(tile_x(&g, ndev + 1), tile_y(&g, ndev + 1), g.tw, g.th,
            "Cancel", "", (ndev + 1) == app.pair.sel);
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_footer("keyboard in pairing mode, then tap it \xB7 Esc cancel");
    ui_no_cursor();
    ui_present();
}

void enter_pairing(uint64_t now)
{
    if (!app.cfg.ble || !app.cfg.ble->enter_pairing) return;
    app.cfg.ble->enter_pairing();
    app.state = ST_PAIRING;
    app.pair.ndevs = 0;
    app.pair.sel = 0;
    app.pair.last_poll = 0;
    app.pair.last_activity = now;
    app.pair.forget_armed = false;
    render_pairing(now);
}

/* Leave pairing: back to the live session if one exists, else HOME. */
static void exit_pairing(uint64_t now)
{
    if (app.cfg.ble && app.cfg.ble->exit_pairing)
        app.cfg.ble->exit_pairing();
    if (ssh_client_is_connected()) {
        app.state = ST_SESSION;
        ui_hide();
    } else {
        enter_home(now);
    }
}

/* Act on a PAIRING tile: a device (pair it), "Forget bonds", or "Cancel". */
static void pairing_select(int slot, uint64_t now)
{
    int nd = pairing_ndev(&app.grid);
    if (slot < nd && app.cfg.ble) {                /* a discovered device */
        app.cfg.ble->select_device(app.pair.devs[slot].addr, app.pair.devs[slot].addr_type);
        toast(now, "pairing %.32s...", app.pair.devs[slot].name);
        exit_pairing(now);
    } else if (slot == nd) {                       /* Forget bonds */
        /* Destructive: arm on the first activation like hostkey REPLACE. */
        if (!app.pair.forget_armed) {
            app.pair.forget_armed = true;
            render_pairing(now);
        } else if (app.cfg.ble && app.cfg.ble->forget) {
            app.cfg.ble->forget();
            toast(now, "bonds cleared - re-scanning");
            enter_pairing(now);                    /* restart the scan fresh */
        }
    } else {                                       /* Cancel */
        exit_pairing(now);
    }
}

void pairing_tick(uint64_t now)
{
    if (now - app.pair.last_activity > PAIR_TIMEOUT_MS) {
        toast(now, "pairing timed out");
        exit_pairing(now);
        return;
    }
    if (now >= app.next_anim) {          /* advance spinner / comet */
        app.next_anim = now + ANIM_PERIOD_MS;
        render_pairing(now);
    }
    if (now - app.pair.last_poll >= PAIR_POLL_MS && app.cfg.ble) {
        app.pair.last_poll = now;
        ble_device_info_t fresh[PAIR_MAX];
        int n = app.cfg.ble->get_scan_results(fresh, PAIR_MAX);
        if (n != app.pair.ndevs ||
            memcmp(fresh, app.pair.devs, (size_t)n * sizeof(fresh[0])) != 0) {
            memcpy(app.pair.devs, fresh, sizeof(fresh));
            app.pair.ndevs = n;
            if (app.pair.sel >= n) app.pair.sel = n ? n - 1 : 0;
            app.pair.last_activity = now;   /* results still arriving */
            render_pairing(now);
        }
    }
}

void pairing_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now)
{
    (void)ch;
    app.pair.last_activity = now;
    if (ev->type == CYBERDECK_INPUT_TAP) {
        int slot = tile_hit(&app.grid, ev->x, ev->y);
        if (slot >= 0) pairing_select(slot, now);   /* gutter tap: ignore */
        return;
    }
    switch (k) {
    case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
        int ns = tile_nav(&app.grid, app.pair.sel, k);
        if (ns != app.pair.sel) {
            app.pair.sel = ns;
            app.pair.forget_armed = false;   /* moving away backs down */
            render_pairing(now);
        }
        break;
    }
    case K_ENTER:
        pairing_select(app.pair.sel, now);
        break;
    case K_ESC:
        exit_pairing(now);
        break;
    default:
        break;
    }
}
