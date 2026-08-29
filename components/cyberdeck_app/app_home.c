/*
 * app_home.c — HOME profile picker (ST_HOME) + the CRT power-off interlude
 * (ST_POWEROFF) that precedes it after a session ends.
 */

#include "app_internal.h"
#include "session_guard.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "cyberdeck_plugin.h"
#include "display_fx.h"
#include "keystore.h"
#include "wifi_manager.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static struct {
    int      sel;                   /* selected HOME tile                */
    bool     kbd_bonded;            /* gates the "Pair keyboard" tile    */
    uint8_t  kon_idx;               /* Konami sequence progress          */
    uint64_t next_refresh;          /* next live-status re-render        */
    uint64_t poweroff_until;        /* POWEROFF: when the collapse ends  */
} s_home;

static const char *TAG = "app_home";

/* Keystore presence snapshot, refreshed on enter_home(): render_home runs
 * ~10 Hz and an ABSENT store would stat the filesystem every frame. Store
 * create/remove always route through other screens, so this stays true. */
static bool s_ks_present;

/* Trailing HOME tiles follow the profiles, as home_tile_t rows. They
 * dogfood the plugin seam's shape (extensibility item 5). "New profile"
 * shows only as a first-run shortcut. "Pair keyboard" shows only while
 * the deck holds no bond. "Lock deck" shows while a keystore exists;
 * the panic button belongs on HOME, not three taps deep. Plugin tiles
 * come next, and Configuration always comes last. */
static bool tile_new_visible(void)  { return app.stored_count == 0; }
static bool tile_pair_visible(void) { return app.ble && !s_home.kbd_bonded; }
static bool tile_lock_visible(void) { return s_ks_present; }
static void tile_new_act(uint64_t now)  { enter_profile(now, -1); }
static void tile_pair_act(uint64_t now) { enter_pairing(now); }
static void tile_lock_act(uint64_t now)
{
    keystore_lock();                /* panic: wipe MK, raise the gate */
    app_creds_wipe();               /* ...and the hydrated copies */
    unlock_open_gate(now);
}
static void tile_config_act(uint64_t now) { menu_open_config(now); }

static const home_tile_t CORE_TILES[] = {
    { "+ New profile",   "add SSH host",      OVERLAY_COL_GREEN,
      tile_new_visible,  tile_new_act },
    { "+ Pair keyboard", "tap or long-press", OVERLAY_COL_CYAN,
      tile_pair_visible, tile_pair_act },
    { "Lock deck",       "PIN to wake",       OVERLAY_COL_AMBER,
      tile_lock_visible, tile_lock_act },
};
static const home_tile_t CONFIG_TILE = {
    "Configuration", "wifi / profiles / more", OVERLAY_COL_CYAN,
    NULL, tile_config_act,
};

#define HOME_TILES_MAX 8

/* Visible trailing tiles in display order: core, plugins, Configuration.
 * Returns the count; @p out must hold HOME_TILES_MAX entries. */
static int home_tiles(const home_tile_t *out[])
{
    int n = 0;
    for (int i = 0; i < NELEM(CORE_TILES) && n < HOME_TILES_MAX - 1; i++)
        if (!CORE_TILES[i].visible || CORE_TILES[i].visible())
            out[n++] = &CORE_TILES[i];
    for (int p = 0; p < cyberdeck_plugin_count; p++) {
        const cyberdeck_plugin_t *pl = cyberdeck_plugins[p];
        for (int i = 0; i < pl->n_home_tiles && n < HOME_TILES_MAX - 1; i++)
            if (!pl->home_tiles[i].visible || pl->home_tiles[i].visible())
                out[n++] = &pl->home_tiles[i];
    }
    out[n++] = &CONFIG_TILE;
    return n;
}

/* Little ● / ○ LED then a label + value; returns the column just past the
 * value text so callers can append glyphs without layout knowledge. */
static int draw_status_led(int row, bool on, const char *label, const char *value)
{
    /* Live-link heartbeat: the dot contracts to ∙ (U+2219 — U+2022 is
     * byte-identical to ● in this font!) twice per ~1.6 s cycle. */
    uint32_t ph = app.anim_frame & 15;
    uint16_t cp = !on ? UI_LED_OFF
                : (ph == 0 || ph == 2) ? 0x2219 : UI_LED_ON;
    ui_pen(on ? OVERLAY_COL_GREEN : OVERLAY_COL_RED);
    ui_putch(2, row, cp, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(4, row, label, UI_BOLD);   /* bold key, regular value */
    ui_puts(9, row, value, 0);
    return 9 + (int)strlen(value);
}

static void render_home(uint64_t now)
{
    (void)now;
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

    /* The render clamps SSID to a fixed field, so the dBm suffix always
     * fits. This also keeps the line clear of the right-aligned wordmark
     * (starts at cols-10). */
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
        /* 4-step signal bar after the text; pen color tracks link quality. */
        uint16_t bar = rssi > -55 ? UI_BLOCK : rssi > -67 ? 0x2586
                     : rssi > -78 ? 0x2584  : 0x2582;
        ui_pen(rssi > -67 ? OVERLAY_COL_GREEN
             : rssi > -78 ? OVERLAY_COL_AMBER : OVERLAY_COL_RED);
        ui_putch(netend + 1, 0, bar, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    bool kbd = app.ble && app.ble->get_state &&
               app.ble->get_state() == CYBERDECK_BLE_CONNECTED;
    const char *kn = (kbd && app.ble->get_name) ? app.ble->get_name() : "";
    char kbdinfo[64];
    snprintf(kbdinfo, sizeof(kbdinfo), "%-11s %s", ble_status_str(), kn);
    int ke = draw_status_led(1, kbd, "KBD", kbdinfo);

    /* Phone-presence chip after the KBD status. COLOR alone carries the
     * state; a fill-vs-hollow LED on top would read as noise. Green
     * means near, with RSSI over the ~1-2 m gate. Blue means in range
     * but far. Red means enrolled but gone. Amber means enroll mode:
     * advertising, waiting for the phone to pair. */
    if (app.presence &&
        (app.presence->enrolled() ||
         app.presence->enroll_state() == CYBERDECK_ENROLL_ADVERTISING)) {
        const bool adv  = app.presence->enroll_state() == CYBERDECK_ENROLL_ADVERTISING;
        const bool ph   = !adv && app.presence->present();
        const bool nearby = ph && app.presence->is_near();
        ui_pen(adv    ? OVERLAY_COL_AMBER        /* enrolling            */
             : nearby ? OVERLAY_COL_GREEN        /* within arm's reach   */
             : ph     ? OVERLAY_COL_BLUE         /* in range, but far    */
                      : OVERLAY_COL_RED);        /* gone                 */
        ui_putch(ke + 2, 1, UI_LED_ON, 0);
        ui_puts(ke + 4, 1, "PHN", UI_BOLD);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    /* All systems go: a small amber smiley in the margin when net + keyboard up. */
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
    ui_puts(4, 2, "RAM", UI_BOLD);
    ui_puts(9, 2, ram, 0);

    /* ── Title + version on the RIGHT (rows 0-1) ────────────────────── */
    ui_pen(OVERLAY_COL_CYAN);
    ui_puts(ui_cols() - (int)strlen("CYBERDECK") - 1, 0, "CYBERDECK",
            UI_BOLD);
    char ver[24];
    snprintf(ver, sizeof(ver), "// %s", app.cfg.version ? app.cfg.version : "?");
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - (int)strlen(ver) - 1, 1, ver, 0);

    /* Wall clock under the version once SNTP delivers real time. */
    char clk[8];
    if (clock_str(clk, sizeof(clk)))
        ui_puts(ui_cols() - (int)strlen(clk) - 1, 2, clk, 0);
    ui_pen(OVERLAY_COL_DEFAULT);

    /* Tiles: one per profile, then the trailing rows (see home_tiles). */
    const home_tile_t *xt[HOME_TILES_MAX];
    int nx = home_tiles(xt);
    tilegrid_t g = picker_grid(app.profile_count + nx);
    app.grid = g;
    if (s_home.sel >= g.count) s_home.sel = g.count ? g.count - 1 : 0;
    if (app.profile_count + nx > g.ncols * g.nrows)
        ESP_LOGW(TAG, "%d profiles exceed one page; showing first %d",
                 app.profile_count, g.count - nx);

    for (int i = 0; i < g.count; i++) {
        int cx = tile_x(&g, i), cy = tile_y(&g, i);
        bool sel = (i == s_home.sel);
        if (i < app.profile_count) {
            const conn_profile_t *p = &app.profiles[i];
            char body[48];
            snprintf(body, sizeof(body), "%s@%s:%u%s",
                     p->user, p->host, (unsigned)p->port,
                     p->auth == STORAGE_AUTH_KEY ? "  [key]" : "");
            ui_pen(prof_accent(p->name));   /* stable per-name identity */
            ui_tile(cx, cy, g.tw, g.th, p->name, body, sel);
        } else {
            const home_tile_t *t = xt[i - app.profile_count];
            ui_pen(t->accent);
            ui_tile(cx, cy, g.tw, g.th, t->label, t->body, sel);
        }
    }

    /* Vacant tile sockets get a whisper of CRT static. Each empty slot
     * shows a few dim braille specks, re-hashed every ~0.8 s. */
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
        /* Below the two tiles that still render (pair + config). */
        ui_pen(OVERLAY_COL_AMBER);
        ui_puts(3, g.y0 + g.th + 1,
                "no profiles - edit profiles.ini in storage", 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    /* Plugin strips just above the StatusBar (pacman lives there).
     * (Hints and the toast both retired to the bar, ui-spec.) */
    for (int i = 0; i < cyberdeck_plugin_count; i++)
        if (cyberdeck_plugins[i]->home_strip)
            cyberdeck_plugins[i]->home_strip(ui_rows() - 2, now);
}

static void home_enter(intptr_t arg, uint64_t now)
{
    (void)arg;
    /* kbd_bonded gates the "Pair keyboard" tile; s_ks_present gates
     * "Lock deck". Landing here counts as activity: it keeps a
     * drop/provisioning toast alive before the rain paints over it. */
    s_home.kbd_bonded = ble_has_bond();
    s_ks_present = keystore_state() != KEYSTORE_ABSENT;
    s_home.next_refresh = 0;
    session_guard_activity(now);
}

void enter_home(uint64_t now)
{
    nav_reset(SCR_HOME, 0, now);
}

/* Act on a trailing HOME tile; returns true if @p slot was one. */
static bool home_activate_extra(int slot, uint64_t now)
{
    if (slot < app.profile_count) return false;
    const home_tile_t *xt[HOME_TILES_MAX];
    int nx = home_tiles(xt);
    int xi = slot - app.profile_count;
    if (xi < 0 || xi >= nx || !xt[xi]->activate) return false;
    xt[xi]->activate(now);
    return true;
}

/* On session teardown, poweroff_enter hides the overlay. This lets the
 * CRT collapse play over the last live terminal frame; HOME appears
 * only once it finishes. Toasts set by the caller survive — they are
 * state, rendered when HOME appears. */
static void poweroff_enter(intptr_t arg, uint64_t now)
{
    ui_hide();
    ui_no_cursor();
    display_fx_collapse();
    s_home.poweroff_until = now + (uint64_t)arg * 17 + 80;
}

void enter_home_after_collapse(uint64_t now)
{
    display_fx_cfg_t c;
    display_fx_get(&c);
    if (!c.collapse) {
        enter_home(now);
        return;
    }
    nav_reset(SCR_POWEROFF, c.collapse_frames, now);
}

static void home_tick(uint64_t now)
{
    /* Expire toasts regardless of the saver, so a wake never flashes
     * a long-dead message. */
    if (app.toast[0] && now >= app.toast_until) app.toast[0] = '\0';

    if (now >= s_home.next_refresh) {
        s_home.next_refresh = now + ANIM_PERIOD_MS;   /* animation cadence */
        nav_invalidate();   /* live wifi/ble status */
    }
}

static void poweroff_tick(uint64_t now)
{
    /* Collapse finished (or was cut short by input) — bring HOME up. */
    if (now >= s_home.poweroff_until) enter_home(now);
}

static void home_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                       uint64_t now)
{
    /* Long-press anywhere = open pairing (works without a keyboard). */
    if (ev->type == CYBERDECK_INPUT_LONG_PRESS) {
        if (app.ble) enter_pairing(now);
        return;
    }
    if (ev->type == CYBERDECK_INPUT_TAP) {
        int slot = tile_hit(&app.grid, ev->x, ev->y);
        if (slot < 0) return;                    /* gutter/margin: ignore */
        if (home_activate_extra(slot, now)) {    /* New / Pair / Config */
            /* handled */
        } else if (s_home.sel != slot) {              /* first tap: select + show */
            s_home.sel = slot;
            nav_invalidate();
        } else if (!wifi_manager_is_connected()) {
            toast(now, "wifi not connected yet");
            nav_invalidate();
        } else {                                 /* second tap on same tile */
            start_connect(slot, now, now);
        }
        return;
    }
    switch (k) {
    case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
        /* Konami progress rides along invisibly; EVERY arrow still
         * navigates. On mismatch, honor the overlapping UP-UP prefix
         * (an extra leading UP must not break the code). */
        static const ui_key_t KONAMI[8] = {
            K_UP, K_UP, K_DOWN, K_DOWN, K_LEFT, K_RIGHT, K_LEFT, K_RIGHT,
        };
        if (k == KONAMI[s_home.kon_idx])   s_home.kon_idx++;
        else if (k == K_UP)           s_home.kon_idx = (s_home.kon_idx == 2) ? 2 : 1;
        else                          s_home.kon_idx = 0;
        if (s_home.kon_idx == 8) {
            s_home.kon_idx = 0;
            display_bell();
            toast(now, "CHEAT ACCEPTED - RAM +30K (not really)");
        }
        int ns = tile_nav(&app.grid, s_home.sel, k);
        if (ns != s_home.sel) s_home.sel = ns;
        nav_invalidate();
        break;
    }
    case K_ENTER:
        if (home_activate_extra(s_home.sel, now)) {               /* New/Pair/Config */
            /* handled */
        } else if (app.profile_count > 0) {
            if (!wifi_manager_is_connected()) {
                toast(now, "wifi not connected yet");
                nav_invalidate();
            } else {
                start_connect(s_home.sel, now, now);
            }
        }
        break;
    case K_CHAR:
        if (ch == 'b' || ch == 'B') enter_pairing(now);
        else if (ch == 'n' || ch == 'N') {
            if (app.stored_count >= MAX_PROFILES - 1) {
                toast(now, "profile list full");
                nav_invalidate();
            } else {
                enter_profile(now, -1);
            }
        }
        else if (ch == 'r' || ch == 'R') { load_profiles(); nav_invalidate(); }
        else if (ch == 'w' || ch == 'W') { kick_wifi(); nav_invalidate(); }
        else if ((ch == 'l' || ch == 'L') && s_ks_present) {
            keystore_lock();                /* keyboard panic button */
            app_creds_wipe();
            unlock_open_gate(now);
        }
        else if ((ch == 'p' || ch == 'P') && app.presence) {
            /* Phone presence is a prototype. P starts or cancels enroll
             * mode when the deck holds no phone. With one phone enrolled,
             * P shows status. A second P within 2 s forgets the identity
             * and bond. It then drops straight back into enroll: the
             * clean re-pair gesture. The phone side still needs its own
             * "Forget This Device". */
            static uint64_t s_p_last;
            const cyberdeck_presence_ops_t *pr = app.presence;
            if (pr->enroll_state() == CYBERDECK_ENROLL_ADVERTISING) {
                pr->enroll_stop();
                toast(now, "phone enroll cancelled");
            } else if (!pr->enrolled()) {
                pr->enroll_start();
                toast(now, "pair 'CYBERDECK' from the phone now");
            } else if (now - s_p_last < 2000) {
                pr->forget();
                pr->enroll_start();
                toast(now, "phone forgotten - pair again now");
            } else if (pr->present()) {
                toast(now, "phone here (%d dBm) - P again to re-pair",
                      pr->rssi());
            } else {
                const uint32_t age = pr->age_ms();
                if (age == UINT32_MAX)
                    toast(now, "phone not seen yet - P again to re-pair");
                else
                    toast(now, "phone away (%us) - P again to re-pair",
                          (unsigned)(age / 1000));
            }
            s_p_last = now;
            nav_invalidate();
        }
        break;
    default:
        break;
    }
}

static void poweroff_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                           uint64_t now)
{
    (void)ev; (void)k; (void)ch;
    /* Any input skips the rest of the power-off animation. */
    enter_home(now);
}

const nav_screen_t home_screen = {
    .name = "home", .enter = home_enter, .tick = home_tick,
    .input = home_input, .render = render_home, .chrome = NAV_CHROME_FULL,
};

/* No render: the collapse fx plays over the dead terminal frame. */
const nav_screen_t poweroff_screen = {
    .name = "poweroff", .enter = poweroff_enter, .tick = poweroff_tick,
    .input = poweroff_input, .chrome = NAV_CHROME_NONE,
};
