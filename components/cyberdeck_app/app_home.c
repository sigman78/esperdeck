/*
 * app_home.c — HOME profile picker (ST_HOME).
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "app_menu_defs.h"   /* Configuration tile opens MS_CONFIG */
#include "display_fx.h"
#include "wifi_manager.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "app_home";

/* Resolve the trailing HOME tiles for the current state, in display order.
 * Returns the count; @p out must hold at least HOME_EXTRA_MAX entries. */
static int home_extras(home_extra_t *out)
{
    int n = 0;
    if (app.stored_count == 0)            out[n++] = HX_NEW;   /* first-run help */
    if (app.cfg.ble && !app.kbd_bonded)     out[n++] = HX_PAIR;  /* not yet bonded */
    out[n++] = HX_CONFIG;                                    /* always, last   */
    return n;
}

/* Little ● / ○ LED then a label + value, cyberpunk status line.
 * Returns the column just past the value text (the "%-4s " puts the value
 * at column 9), so callers can append glyphs without layout knowledge. */
static int draw_status_led(int row, bool on, const char *label, const char *value)
{
    /* A live link gets a heartbeat: the dot contracts to ∙ (U+2219, a
     * genuinely smaller bitmap — U+2022 is byte-identical to ● in this
     * font!) twice per ~1.6 s cycle. Off stays a steady hollow ○. */
    uint32_t ph = app.anim_frame & 15;
    uint16_t cp = !on ? UI_LED_OFF
                : (ph == 0 || ph == 2) ? 0x2219 : UI_LED_ON;
    ui_pen(on ? OVERLAY_COL_GREEN : OVERLAY_COL_RED);
    ui_putch(2, row, cp, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(4, row, label, OVERLAY_ATTR_BOLD);   /* bold key, regular value */
    ui_puts(9, row, value, 0);
    return 9 + (int)strlen(value);
}

void render_home(void)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    /* ── Status HUD on the LEFT (rows 0-2) ─────────────────────────── */
    /* RSSI changes over seconds; don't hit the WiFi driver lock at the
     * 10 fps render cadence — refresh the cached value about once a second. */
    static bool     rssi_fresh = false;
    static uint32_t rssi_frame = 0;
    static int      rssi       = 0;
    if (!rssi_fresh || app.anim_frame - rssi_frame >= 10) {
        rssi_fresh = true;
        rssi_frame = app.anim_frame;
        rssi       = wifi_manager_get_rssi();
    }

    /* SSID clamped to a fixed field so the dBm suffix always fits the buffer
     * and the whole line stays clear of the right-aligned CYBERDECK wordmark
     * (which starts at cols-10). */
    int sw = ui_cols() >= 97 ? 16 : 10;
    char net[48];
    snprintf(net, sizeof(net), "%-*.*s %s", sw, sw,
             wifi_manager_get_ssid()[0] ? wifi_manager_get_ssid() : "-",
             wifi_status_str());
    if (rssi < 0) {
        size_t nl = strlen(net);
        snprintf(net + nl, sizeof(net) - nl, "  %ddBm", rssi);
    }
    int netend = draw_status_led(0, wifi_manager_is_connected(), "NET", net);
    if (rssi < 0) {
        /* 4-step signal bar after the text (glyphs can't ride in the
         * Latin-1 string); pen color tracks link quality. */
        uint16_t bar = rssi > -55 ? UI_BLOCK : rssi > -67 ? 0x2586
                     : rssi > -78 ? 0x2584  : 0x2582;
        ui_pen(rssi > -67 ? OVERLAY_COL_GREEN
             : rssi > -78 ? OVERLAY_COL_AMBER : OVERLAY_COL_RED);
        ui_putch(netend + 1, 0, bar, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    bool kbd = app.cfg.ble && app.cfg.ble->get_state && app.cfg.ble->get_state() == 4;
    const char *kn = (kbd && app.cfg.ble->get_name) ? app.cfg.ble->get_name() : "";
    char kbdinfo[64];
    snprintf(kbdinfo, sizeof(kbdinfo), "%-11s %s", ble_status_str(), kn);
    draw_status_led(1, kbd, "KBD", kbdinfo);

    /* All systems go: a small amber ☺ in the margin when net + keyboard
     * are both up. Blink-and-you-miss-it personality, zero clutter. */
    if (wifi_manager_is_connected() && kbd) {
        ui_pen(OVERLAY_COL_AMBER);
        ui_putch(0, 1, 0x263A, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    char ram[48];
    ram_stats(ram, sizeof(ram));
    ui_pen(OVERLAY_COL_BLUE);
    ui_putch(2, 2, UI_DIAMOND, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(4, 2, "RAM", OVERLAY_ATTR_BOLD);
    ui_puts(9, 2, ram, 0);

    /* ── Title + version on the RIGHT (rows 0-1) ────────────────────── */
    ui_pen(OVERLAY_COL_CYAN);
    ui_puts(ui_cols() - (int)strlen("CYBERDECK") - 1, 0, "CYBERDECK",
            OVERLAY_ATTR_BOLD);
    char ver[24];
    snprintf(ver, sizeof(ver), "// %s", app.cfg.version ? app.cfg.version : "?");
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - (int)strlen(ver) - 1, 1, ver, 0);

    /* Wall clock under the version once SNTP delivers real time. */
    char clk[8];
    if (clock_str(clk, sizeof(clk)))
        ui_puts(ui_cols() - (int)strlen(clk) - 1, 2, clk, 0);
    ui_pen(OVERLAY_COL_DEFAULT);

    /* Tiles: one per profile, then a conditional trailing set (New profile only
     * as a first-run shortcut, Pair keyboard only when none is bonded, and
     * Configuration always). See home_extras(). */
    home_extra_t xt[HOME_EXTRA_MAX];
    int nx = home_extras(xt);
    tilegrid_t g = picker_grid(app.profile_count + nx);
    app.grid = g;
    if (app.sel >= g.count) app.sel = g.count ? g.count - 1 : 0;
    if (app.profile_count + nx > g.ncols * g.nrows)
        ESP_LOGW(TAG, "%d profiles exceed one page; showing first %d",
                 app.profile_count, g.count - nx);

    for (int i = 0; i < g.count; i++) {
        int cx = tile_x(&g, i), cy = tile_y(&g, i);
        bool sel = (i == app.sel);
        if (i < app.profile_count) {
            const conn_profile_t *p = &app.profiles[i];
            char body[48];
            snprintf(body, sizeof(body), "%s@%s:%u%s",
                     p->user, p->host, (unsigned)p->port,
                     p->auth == STORAGE_AUTH_KEY ? "  [key]" : "");
            ui_pen(prof_accent(p->name));   /* stable per-name identity */
            ui_tile(cx, cy, g.tw, g.th, p->name, body, sel);
        } else {
            switch (xt[i - app.profile_count]) {
            case HX_NEW:
                ui_pen(OVERLAY_COL_GREEN);
                ui_tile(cx, cy, g.tw, g.th, "+ New profile", "add SSH host", sel);
                break;
            case HX_PAIR:
                ui_pen(OVERLAY_COL_CYAN);
                ui_tile(cx, cy, g.tw, g.th, "+ Pair keyboard",
                        "tap or long-press", sel);
                break;
            case HX_CONFIG:
                ui_pen(OVERLAY_COL_CYAN);
                ui_tile(cx, cy, g.tw, g.th, "Configuration",
                        "wifi / profiles / more", sel);
                break;
            }
        }
    }

    /* Vacant tile sockets get a whisper of CRT static: a few dim braille
     * specks per empty slot, re-hashed every ~0.8 s — unpowered bays on a
     * deck that is very much alive. */
    ui_pen(OVERLAY_COL_BLUE);
    for (int i = g.count; i < g.ncols * g.nrows; i++) {
        for (int k = 0; k < 5; k++) {
            uint32_t h = (uint32_t)i * 97u + (uint32_t)k * 61u
                       + (app.anim_frame >> 3) * 31u;
            ui_putch(tile_x(&g, i) + (int)(h % (uint32_t)g.tw),
                     tile_y(&g, i) + (int)((h / 7u) % (uint32_t)g.th),
                     braille_noise(h), 0);
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    if (app.profile_count == 0) {
        /* Below the two tiles that still render (pair + config) — row 5 is
         * their title row and the hint used to punch right through it. */
        ui_pen(OVERLAY_COL_AMBER);
        ui_puts(3, g.y0 + g.th + 1,
                "no profiles - edit profiles.ini in storage", 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    /* Footer: touch + keyboard legend (the B/R/W shortcuts were previously
     * undiscoverable). Pairing hints only appear when the build has BLE —
     * advertising a silent no-op reads as broken input. An active toast owns
     * the right edge; draw_footer_lim clips the hint clear of it. */
    const char *hint = app.cfg.ble
        ? "tap\xB7tap connect \xB7 hold pair \xB7 B pair \xB7 R reload \xB7 W wifi"
        : "tap\xB7tap connect \xB7 R reload \xB7 W wifi";
    if (app.toast[0]) {
        int tx = ui_cols() - ((int)strlen(app.toast) + 2) - 1;
        draw_footer_lim(hint, tx - 1);           /* -1: the taper cell */
        ui_pen(OVERLAY_COL_AMBER);
        ui_chip(tx - 1, ui_rows() - 1, UI_PL_L, app.toast, 0, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    } else {
        draw_footer(hint);
    }

    ui_no_cursor();
    ui_present();
}

void enter_home(uint64_t now)
{
    app.state = ST_HOME;
    app.kbd_bonded = ble_has_bond();   /* gate the "Pair keyboard" HOME tile */
    app.next_home_refresh = 0;
    /* Arriving on HOME counts as activity: a session drop or provisioning
     * result must show its toast for the full lifetime — the rain waiting
     * out a stale idle timer would paint over it on the next tick. */
    app.last_input = now;
    app.saver_on   = false;
    render_home();
}

/* Open the config hub directly from HOME (no session behind it). */
static void home_open_config(void)
{
    app.menu_from_home = true;
    app.state          = ST_MENU;
    menu_goto(MS_CONFIG);
}

/* If HOME tile @p slot is a trailing extra, return its home_extra_t, else -1. */
static int home_extra_kind(int slot)
{
    if (slot < app.profile_count) return -1;
    home_extra_t xt[HOME_EXTRA_MAX];
    int nx = home_extras(xt);
    int xi = slot - app.profile_count;
    return (xi >= 0 && xi < nx) ? (int)xt[xi] : -1;
}

/* Act on a trailing HOME extra tile; returns true if @p slot was one. */
static bool home_activate_extra(int slot, uint64_t now)
{
    switch (home_extra_kind(slot)) {
    case HX_NEW:    enter_profile(now, -1);            return true;
    case HX_PAIR:   if (app.cfg.ble) enter_pairing(now); return true;
    case HX_CONFIG: home_open_config();                return true;
    default:        return false;
    }
}

/* Session teardown, DOOM style: enter HOME right away and let its overlay
 * melt down OVER the dead terminal frame still in the vterm buffer (the
 * renderer shows the old frame wherever the sheet has not arrived). Purely
 * visual — no interlude state, input acts on HOME immediately. Toasts set
 * by the caller survive: they are state, rendered as HOME's chrome arrives.
 * With the melt disabled the arm is a no-op and HOME just appears. */
void enter_home_after_melt(uint64_t now)
{
    display_fx_melt_over();
    enter_home(now);
}

void home_tick(uint64_t now)
{
    /* Expire toasts regardless of the saver, so a wake never flashes
     * a long-dead message. */
    if (app.toast[0] && now >= app.toast_until) app.toast[0] = '\0';
    if (now - app.last_input > SAVER_IDLE_MS) {
        if (now >= app.next_anim) {          /* idle: let it rain */
            app.next_anim = now + ANIM_PERIOD_MS;
            if (!app.saver_on) {
                app.saver_on    = true;      /* input handling keys off
                                              * what is actually on screen */
                app.saver_since = now;
            }
            render_saver();
        }
        return;
    }
    app.saver_on = false;
    if (now >= app.next_home_refresh) {
        app.next_home_refresh = now + ANIM_PERIOD_MS;   /* animation cadence */
        render_home();   /* live wifi/ble status */
    }
}

void home_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now)
{
    /* Long-press anywhere = open pairing (works without a keyboard). */
    if (ev->type == CYBERDECK_INPUT_LONG_PRESS) {
        if (app.cfg.ble) enter_pairing(now);
        return;
    }
    if (ev->type == CYBERDECK_INPUT_TAP) {
        int slot = tile_hit(&app.grid, ev->x, ev->y);
        if (slot < 0) return;                    /* gutter/margin: ignore */
        if (home_activate_extra(slot, now)) {    /* New / Pair / Config */
            /* handled */
        } else if (app.sel != slot) {            /* first tap: select + show */
            app.sel = slot;
            render_home();
        } else if (!wifi_manager_is_connected()) {
            toast(now, "wifi not connected yet");
            render_home();
        } else {                                 /* second tap on same tile */
            start_connect(slot, now, now);
        }
        return;
    }
    switch (k) {
    case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
        /* Konami progress rides along invisibly; EVERY arrow still
         * navigates, including the one that completes the sequence.
         * On mismatch, fall back honoring the overlapping UP-UP prefix
         * (an extra leading UP must not break the code). */
        static const ui_key_t KONAMI[8] = {
            K_UP, K_UP, K_DOWN, K_DOWN, K_LEFT, K_RIGHT, K_LEFT, K_RIGHT,
        };
        if (k == KONAMI[app.kon_idx])   app.kon_idx++;
        else if (k == K_UP)             app.kon_idx = (app.kon_idx == 2) ? 2 : 1;
        else                            app.kon_idx = 0;
        if (app.kon_idx == 8) {
            app.kon_idx = 0;
            display_bell();
            toast(now, "CHEAT ACCEPTED - RAM +30K (not really)");
        }
        int ns = tile_nav(&app.grid, app.sel, k);
        if (ns != app.sel) app.sel = ns;
        render_home();
        break;
    }
    case K_ENTER:
        if (home_activate_extra(app.sel, now)) {             /* New/Pair/Config */
            /* handled */
        } else if (app.profile_count > 0) {
            if (!wifi_manager_is_connected()) {
                toast(now, "wifi not connected yet");
                render_home();
            } else {
                start_connect(app.sel, now, now);
            }
        }
        break;
    case K_CHAR:
        if (ch == 'b' || ch == 'B') enter_pairing(now);
        else if (ch == 'n' || ch == 'N') {
            if (app.stored_count >= MAX_PROFILES - 1) {
                toast(now, "profile list full");
                render_home();
            } else {
                enter_profile(now, -1);
            }
        }
        else if (ch == 'r' || ch == 'R') { load_profiles(); render_home(); }
        else if (ch == 'w' || ch == 'W') { kick_wifi(); render_home(); }
        break;
    default:
        break;
    }
}
