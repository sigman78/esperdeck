/*
 * app_pairing.c — BLE keyboard pairing modal (ST_PAIRING).
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "ssh_client.h"

#include <string.h>

#define PAIR_MAX  STORAGE_BLE_MAX

static struct {
    ble_device_info_t devs[PAIR_MAX];
    int      ndevs;
    int      sel;
    uint64_t last_poll;
    uint64_t last_activity;
    bool     forget_armed;          /* "Forget bonds" needs a 2nd tap */
} s_pair;

#define PAIR_TIMEOUT_MS  30000
#define PAIR_POLL_MS     250

/* Device tiles on the page; the last two slots are Forget bonds + Cancel. */
static int pairing_ndev(const tilegrid_t *g)
{
    return g->count - 2;
}

static void render_pairing(uint64_t now)
{
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_titlebar(2, "PAIR KEYBOARD");
    ui_pen(s_pair.ndevs ? OVERLAY_COL_GREEN : OVERLAY_COL_AMBER);
    ui_putch(2, 1, s_pair.ndevs ? UI_LED_ON : spinner_glyph(app.anim_frame), 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    if (s_pair.ndevs)
        ui_printf(4, 1, 0, "%d found - select your keyboard", s_pair.ndevs);
    else
        ui_puts(4, 1, "scanning for keyboards...", 0);

    /* The scan self-dismisses after PAIR_TIMEOUT_MS of inactivity; give the
     * last 10 s a visible countdown instead of vanishing without warning. */
    uint64_t idle = now - s_pair.last_activity;
    if (idle > PAIR_TIMEOUT_MS - 10000) {
        uint32_t left = (uint32_t)((PAIR_TIMEOUT_MS - idle + 999) / 1000);
        ui_pen(OVERLAY_COL_AMBER);
        ui_printf(ui_cols() - 15, 1, 0, "closing in %2us", left);
        ui_pen(OVERLAY_COL_DEFAULT);
    }
    draw_rule(3);

    /* Devices, then "Forget bonds" and Cancel (always the last two).
     * Cap devices so both special tiles fit on the page. */
    tilegrid_t g = picker_grid(0);
    int cap  = g.ncols * g.nrows;
    int ndev = s_pair.ndevs > cap - 2 ? cap - 2 : s_pair.ndevs;
    g.count  = ndev + 2;
    app.grid = g;
    if (s_pair.sel >= g.count) s_pair.sel = g.count - 1;

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
                s_pair.devs[i].name,
                s_pair.devs[i].addr_type ? "random addr" : "public addr",
                i == s_pair.sel);
    }
    /* Color law: RED = destructive (Forget), BLUE = safe navigation. */
    ui_pen(OVERLAY_COL_RED);
    ui_tile(tile_x(&g, ndev), tile_y(&g, ndev), g.tw, g.th,
            s_pair.forget_armed ? "TAP AGAIN to forget" : "Forget bonds",
            "clear + re-pair", ndev == s_pair.sel);
    ui_pen(OVERLAY_COL_BLUE);
    ui_tile(tile_x(&g, ndev + 1), tile_y(&g, ndev + 1), g.tw, g.th,
            "Cancel", "", (ndev + 1) == s_pair.sel);
    ui_pen(OVERLAY_COL_DEFAULT);

    /* Instructional state, not a hint — body line above the StatusBar. */
    const char *st = "put the keyboard in pairing mode, then tap it";
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts((ui_cols() - (int)strlen(st)) / 2, ui_rows() - 2, st, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
}

static void pairing_enter(intptr_t arg, uint64_t now)
{
    (void)arg;
    app.ble->enter_pairing();
    s_pair.ndevs = 0;
    s_pair.sel = 0;
    s_pair.last_poll = 0;
    s_pair.last_activity = now;
    s_pair.forget_armed = false;
}

static void pairing_exit(uint64_t now)
{
    (void)now;
    if (app.ble && app.ble->exit_pairing)
        app.ble->exit_pairing();
}

void enter_pairing(uint64_t now)
{
    if (!app.ble || !app.ble->enter_pairing) return;
    nav_push(SCR_PAIRING, 0, now);
}

/* Act on a PAIRING tile: a device (pair it), "Forget bonds", or "Cancel". */
static void pairing_select(int slot, uint64_t now)
{
    int nd = pairing_ndev(&app.grid);
    if (slot < nd && app.ble) {                /* a discovered device */
        app.ble->select_device(s_pair.devs[slot].addr, s_pair.devs[slot].addr_type);
        toast(now, "pairing %.32s...", s_pair.devs[slot].name);
        nav_pop(now);
    } else if (slot == nd) {                       /* Forget bonds */
        /* Destructive: arm on the first activation like hostkey REPLACE. */
        if (!s_pair.forget_armed) {
            s_pair.forget_armed = true;
            nav_invalidate();
        } else if (app.ble && app.ble->forget) {
            app.ble->forget();
            toast(now, "bonds cleared - re-scanning");
            nav_replace(SCR_PAIRING, 0, now);      /* restart the scan fresh */
        }
    } else {                                       /* Cancel */
        nav_pop(now);
    }
}

static void pairing_tick(uint64_t now)
{
    if (now - s_pair.last_activity > PAIR_TIMEOUT_MS) {
        toast(now, "pairing timed out");
        nav_pop(now);
        return;
    }
    if (now >= app.next_anim) {          /* advance spinner / comet */
        app.next_anim = now + ANIM_PERIOD_MS;
        nav_invalidate();
    }
    if (now - s_pair.last_poll >= PAIR_POLL_MS && app.ble) {
        s_pair.last_poll = now;
        ble_device_info_t fresh[PAIR_MAX];
        int n = app.ble->get_scan_results(fresh, PAIR_MAX);
        if (n != s_pair.ndevs ||
            memcmp(fresh, s_pair.devs, (size_t)n * sizeof(fresh[0])) != 0) {
            memcpy(s_pair.devs, fresh, sizeof(fresh));
            s_pair.ndevs = n;
            if (s_pair.sel >= n) s_pair.sel = n ? n - 1 : 0;
            s_pair.last_activity = now;   /* results still arriving */
            nav_invalidate();
        }
    }
}

static void pairing_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                          uint64_t now)
{
    (void)ch;
    s_pair.last_activity = now;
    if (ev->type == CYBERDECK_INPUT_TAP) {
        int slot = tile_hit(&app.grid, ev->x, ev->y);
        if (slot >= 0) pairing_select(slot, now);   /* gutter tap: ignore */
        return;
    }
    switch (k) {
    case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
        int ns = tile_nav(&app.grid, s_pair.sel, k);
        if (ns != s_pair.sel) {
            s_pair.sel = ns;
            s_pair.forget_armed = false;   /* moving away backs down */
            nav_invalidate();
        }
        break;
    }
    case K_ENTER:
        pairing_select(s_pair.sel, now);
        break;
    case K_ESC:
        nav_pop(now);
        break;
    default:
        break;
    }
}

const nav_screen_t pairing_screen = {
    .name = "pairing", .enter = pairing_enter, .exit = pairing_exit,
    .tick = pairing_tick, .input = pairing_input, .render = render_pairing,
    .chrome = NAV_CHROME_FULL,
};
